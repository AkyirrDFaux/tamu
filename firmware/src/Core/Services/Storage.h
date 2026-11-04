#pragma once

#include "Core/Functions/Packet.h"
#include "Core/Functions/Storage.h"

// App-link hooks the streamed read asks about (Core/Functions/AppInterface.h). Declared inline
// here because a service header is parsed before the link's own header; a target without an app
// link (the DAS) never references them, so they are only declared where the link is built.
#ifdef USE_APP_INTERFACE
inline bool AppTxRoomFor(uint16_t Bytes); // one whole fragment fits on a live link
inline bool AppTxIdle();                  // the ring owes the app nothing
inline uint32_t AppTxDropped();           // frames the ring refused since boot
#endif

// How many fragments one app-task pass may emit. A fragment is 88 B on the wire, and the BLE
// notify path carries one notification per AppBLETick, so the pass is capped rather than run to
// the ring's edge: eight fragments per pass keeps a whole-file reply flowing at the loop rate
// (~85 kB/s at the measured ~6 ms pass) without holding the pass open.
#define APP_STREAM_FRAGS_PER_PASS 8

// Storage service (Docs/Services/Storage.md): the file commands (format, create, delete,
// resize, rename, read/write streams). The filesystem itself is StorageBlockFS behind the
// `Storage` global; this class owns the service's own stream state.
class StorageService
{
public:
    bool Handle(const PacketFrame &Frame); // CID dispatch; true if consumed

#ifdef USE_APP_INTERFACE
    // Advances an open file read by up to APP_STREAM_FRAGS_PER_PASS fragments (called once per
    // app-task pass from AppInterface.Pump). A fragment is only produced when the link can take
    // it, so the remainder waits in the cursor instead of being dropped.
    void StreamTick();

    // Abandons an open read: the link went away, so its remaining fragments have no reader.
    void StreamAbort();
#endif

private:
    // The in-flight fragmented write: the name travels in every fragment and must not change
    // mid-stream; the sequence and offset resume across fragments.
    char WriteName[8] = {0};
    bool WriteActive = false;
    uint16_t WriteSeq = 0;
    uint32_t WriteOff = 0;

#ifdef USE_APP_INTERFACE
    // The in-flight fragmented READ (Docs 03.05): a resumable cursor.
    //
    // A whole-file reply used to be emitted from one handler call, back to back. When the reply
    // was bigger than the app link's TX ring, Send() refused the frames that did not fit and the
    // tail - including the STOP fragment - was dropped, so the app's reassembler never completed
    // a whole-`.TABLE` read. The read now keeps its position here and emits fragments a few per
    // app-task pass; the current fragment is built only when the ring has room for it, and
    // nothing is ever refused.
    //
    // One cursor, one file: a new Read request supersedes an open stream (the old cursor is
    // dropped - its remaining fragments are never sent - and the new reply starts at fragment 0
    // under its own TRID, so the app cannot mix the two). A link disconnect abandons the stream
    // through StreamAbort(); a later request re-reads the file from the start.
    //
    // The request frame itself is not kept (a PacketFrame is 128 B): only the three fields a reply
    // echoes are, and SendFragTo() builds the frame from them.
    //
    // Only an app target compiles this: a bus-only node has no ring to overrun, so it streams from
    // the dispatch as before and must not spend RAM on a cursor it cannot use (the DAS sits at
    // statics + stack = its whole 2 kB).
    struct ReadStream
    {
        bool Active = false;
        bool Ack = false;    // the request set REQACK (a failure answer may be sent)
        bool Report = false; // a completion line is owed once the reply has left the link
        uint8_t Pad = 0;
        char Name[8] = {0};
        uint32_t Start = 0;   // flash offset of content byte 0
        uint32_t Size = 0;    // content bytes
        uint16_t Cap = 0;     // content bytes per fragment
        uint16_t Frag = 0;    // next fragment index to emit
        uint16_t Total = 1;   // fragment count
        uint16_t Tgt = 0;     // the requester's address (echoed into every reply)
        uint16_t Cmd = 0;     // its CMD
        uint16_t Trid = 0;    // its TRID
        uint32_t ReportAt = 0; // UptimeMs after which the completion line may go out
    } Read;

    bool BeginRead(const PacketFrame &Frame);
    bool EmitReadFragment(); // false: the pass must yield, or the stream ended
#endif
};

// One instance per binary, in Core/Services/ (the FS global `Storage` is a different object).
StorageService StorageSvc;

#ifdef USE_APP_INTERFACE
inline bool StorageService::BeginRead(const PacketFrame &Frame)
{
    const char *Name = reinterpret_cast<const char *>(Frame.Payload);
    uint32_t FileOffset, FileSize;
    if (!Storage.GetFileInfo(Name, &FileOffset, &FileSize))
    {
        DeviceLog("STORAGE", "read '%.8s' failed", Name);
        SendResponse(Frame, nullptr, 0, FLAG_FAIL);
        return false;
    }

    uint32_t TotalContent = FileSize;
    // Docs 03.05: "maximum 64 byte stream fragment". The name travels in EVERY fragment (B12):
    // each fragment is [frag info][name echo (8)][content].
    uint16_t ContentCap = MAX_FRAG_WORD_BYTES;
    // Counted without the `+ Cap - 1` form: an untrusted FileSize near 0xFFFFFFFF overflows it,
    // and the uint16 cast then wraps the total. The app is handed a fragment count matching
    // neither the file nor this stream, the offset runs past the end, and the length underflows
    // into a large read of whatever is beyond the file. A size the wire's fragment count cannot
    // express is a failed read, never a silently truncated one.
    uint32_t Frags = TotalContent / ContentCap + ((TotalContent % ContentCap) ? 1u : 0u);
    if (Frags > 0xFFFFu)
    {
        DeviceLog("STORAGE", "read '%.8s': %u bytes needs %u fragments", Name,
                  (unsigned)TotalContent, (unsigned)Frags);
        SendResponse(Frame, nullptr, 0, FLAG_FAIL);
        return false;
    }

    if (Read.Active)
        DeviceLog("STORAGE", "read '%.8s' supersedes an open stream ('%.8s' at %u/%u)", Name,
                  Read.Name, (unsigned)Read.Frag, (unsigned)Read.Total);

    Read.Active = true;
    Read.Ack = (Frame.Flags & FLAG_REQACK) != 0;
    Read.Report = false;
    memcpy(Read.Name, Name, 8);
    Read.Start = FileOffset;
    Read.Size = TotalContent;
    Read.Cap = ContentCap;
    Read.Frag = 0;
    Read.Total = (uint16_t)(Frags == 0 ? 1 : Frags);
    Read.Tgt = Frame.IdSrc;
    Read.Cmd = Frame.Cmd;
    Read.Trid = Frame.Trid;
    return true;
}

inline bool StorageService::EmitReadFragment()
{
    if (!Read.Active)
    {
#ifdef USE_APP_INTERFACE
        // The completion line is owed once, after the reply has actually left the link AND the
        // app has had time to read it: this line goes into the same USB byte stream the app is
        // reading (Core/Functions/Log.h), and a log line written while the tail of a reply is
        // still in the driver's FIFO corrupts it. The ring being empty is not enough - the ring's
        // bytes have been handed to the driver, not necessarily to the host. Measured on the rig:
        // a line written the moment the last fragment left the ring spliced itself into fragment
        // ~252 of a 256-fragment `.TABLE` read.
        if (Read.Report && AppTxIdle() &&
            (int32_t)(DeviceStatus.UptimeMs - Read.ReportAt) >= 0)
        {
            DeviceLog("STORAGE", "read '%.8s' %u frag(s) out, tx dropped %u", Read.Name,
                      (unsigned)Read.Total, (unsigned)AppTxDropped());
            Read.Report = false;
        }
#endif
        return false;
    }

#ifdef USE_APP_INTERFACE
    // Room for the WHOLE fragment (header + frag info + name echo + content) before it is built:
    // Send() then cannot refuse it, which is what makes "no fragment is dropped" true rather
    // than best-effort. A full ring - a stalled app, a notify the stack would not take - simply
    // ends this pass, and the cursor resumes on the next one.
    if (!AppTxRoomFor((uint16_t)(12 + 4 + 8 + Read.Cap)))
        return false;
#endif

    uint32_t ContentOff = (uint32_t)Read.Frag * Read.Cap;
    uint16_t ContentLen = (Read.Size - ContentOff > Read.Cap) ? Read.Cap
                                                             : (uint16_t)(Read.Size - ContentOff);
    // This frame is reused for every fragment, so an unchecked short read would answer with
    // whatever the previous fragment left in the payload - a wrong result with no error signal.
    // A failed read is a failure.
    if (ContentLen &&
        StorageFlash.Read(Read.Start + ContentOff, TxFrame.Payload + 12, ContentLen) != ContentLen)
    {
        DeviceLog("STORAGE", "read '%.8s' failed at offset %u", Read.Name, (unsigned)ContentOff);
        Read.Active = false;
        if (Read.Ack)
            SendStatusTo(Read.Tgt, Read.Cmd, Read.Trid, FLAG_FAIL);
        return false;
    }
    memcpy(TxFrame.Payload + 4, Read.Name, 8);
    SendFragTo(Read.Tgt, Read.Cmd, Read.Trid, Read.Frag, Read.Total, (uint16_t)(8 + ContentLen));
    Read.Frag = (uint16_t)(Read.Frag + 1);
    if (Read.Frag >= Read.Total)
    {
        Read.Active = false;
        Read.Report = true;
        // The completion line waits until the tail of the reply is out of the driver too; the
        // measured USB FIFO drain is milliseconds, 300 ms is a wide margin that costs nothing
        // (nothing else reads it).
        Read.ReportAt = DeviceStatus.UptimeMs + 300;
    }
    return true;
}

inline void StorageService::StreamTick()
{
    if (!Read.Active)
    {
        (void)EmitReadFragment(); // drains the owed completion line
        return;
    }
    for (uint8_t i = 0; i < APP_STREAM_FRAGS_PER_PASS; i++)
        if (!EmitReadFragment())
            break;
}

inline void StorageService::StreamAbort()
{
    if (!Read.Active)
        return;
    DeviceLog("STORAGE", "read '%.8s' abandoned at fragment %u/%u: the link went away", Read.Name,
              (unsigned)Read.Frag, (unsigned)Read.Total);
    Read.Active = false;
    Read.Report = false;
    Read.Frag = 0;
}

// App-link entry points (declared in Core/Functions/AppInterface.h): the pump drives the cursor
// and a dismissed app session abandons the stream.
inline void StorageStreamTick() { StorageSvc.StreamTick(); }
inline void StorageStreamAbort() { StorageSvc.StreamAbort(); }
#endif // USE_APP_INTERFACE

inline bool StorageService::Handle(const PacketFrame &Frame)
{
    uint8_t Cid = GetServiceCID(Frame.Cmd);
    bool IsResponse = (Frame.Flags & FLAG_TYPE);

    if (IsResponse) return true;

    switch (Cid) {
        case 0: { // Format per docs 03.00
            bool Ok = Storage.Format();
            SendResponse(Frame, nullptr, 0, (uint8_t)(Ok ? FLAG_SUCCESS : FLAG_FAIL));
            break;
        }

        case 1: { // Create per docs 03.01
            if (PayloadBytes(Frame) >= 12) {
                const char *Name = reinterpret_cast<const char *>(Frame.Payload);
                uint32_t Size; memcpy(&Size, Frame.Payload + 8, sizeof(Size));
                // Docs 03.01: Size (>0). A zero-size file is pointless and rejected (B11).
                if (Size == 0) {
                    DeviceLog("STORAGE", "create '%.8s' rejected: zero size", Name);
                    SendResponse(Frame, nullptr, 0, FLAG_FAIL);
                    break;
                }
                bool Ok = Storage.CreateFile(Name, Size);
                if (!Ok) DeviceLog("STORAGE", "create '%.8s' size %u failed", Name, (unsigned)Size);
                SendResponse(Frame, nullptr, 0, (uint8_t)(Ok ? FLAG_SUCCESS : FLAG_FAIL));
            } else {
                DeviceLog("STORAGE", "create short Payload (%u B)", (unsigned)PayloadBytes(Frame));
            }
            break;
        }

        case 2: { // Delete per docs 03.02
            if (PayloadBytes(Frame) >= 8) {
                const char *Name = reinterpret_cast<const char *>(Frame.Payload);
                bool Ok = Storage.DeleteFile(Name);
                SendResponse(Frame, nullptr, 0, (uint8_t)(Ok ? FLAG_SUCCESS : FLAG_FAIL));
            }
            break;
        }

        case 3: { // Resize per docs 03.03
            if (PayloadBytes(Frame) >= 12) {
                const char *Name = reinterpret_cast<const char *>(Frame.Payload);
                uint32_t NewSize; memcpy(&NewSize, Frame.Payload + 8, sizeof(NewSize));
                // Docs 03.03: New Size (>0). A zero-size file is pointless and rejected (B11).
                if (NewSize == 0) {
                    DeviceLog("STORAGE", "resize '%.8s' rejected: zero size", Name);
                    SendResponse(Frame, nullptr, 0, FLAG_FAIL);
                    break;
                }
                bool Ok = Storage.ResizeFile(Name, NewSize);
                if (!Ok) DeviceLog("STORAGE", "resize '%.8s' -> %u failed", Name, (unsigned)NewSize);
                SendResponse(Frame, nullptr, 0, (uint8_t)(Ok ? FLAG_SUCCESS : FLAG_FAIL));
            } else {
                DeviceLog("STORAGE", "resize short Payload (%u B)", (unsigned)PayloadBytes(Frame));
            }
            break;
        }

        case 4: { // Rename per docs 03.04
            if (PayloadBytes(Frame) >= 16) {
                const char *OldName = reinterpret_cast<const char *>(Frame.Payload);
                const char *NewName = reinterpret_cast<const char *>(Frame.Payload + 8);
                bool Ok = Storage.RenameFile(OldName, NewName);
                if (!Ok) DeviceLog("STORAGE", "rename '%.8s' -> '%.8s' failed", OldName, NewName);
                SendResponse(Frame, nullptr, 0, (uint8_t)(Ok ? FLAG_SUCCESS : FLAG_FAIL));
            } else {
                DeviceLog("STORAGE", "rename short Payload (%u B)", (unsigned)PayloadBytes(Frame));
            }
            break;
        }

        case 5: { // Read per docs 03.05
#ifdef USE_APP_INTERFACE
            // Resumable: the cursor is started here and driven from AppInterface.Pump()
            // (StorageStreamTick), which emits a few fragments per pass and yields. Emitting the
            // first pass here as well only shortens a short read's latency - a whole file still
            // never runs inside one dispatch.
            if (PayloadBytes(Frame) >= 8 && BeginRead(Frame))
                StreamTick();
#else
            // No app link on this target: there is no ring to overrun, so the reply is streamed
            // onto the bus from this same dispatch, exactly as before.
            if (PayloadBytes(Frame) >= 8) {
                const char *Name = reinterpret_cast<const char *>(Frame.Payload);
                uint32_t FileOffset, FileSize;
                if (Storage.GetFileInfo(Name, &FileOffset, &FileSize)) {
                    uint32_t TotalContent = FileSize;
                    // Docs 03.05: "maximum 64 byte stream fragment". The name travels in EVERY
                    // fragment (B12): each fragment is [frag info][name echo (8)][content].
                    uint16_t ContentCap = MAX_FRAG_WORD_BYTES;
                    // Counted without the `+ Cap - 1` form: an untrusted FileSize near 0xFFFFFFFF
                    // overflows it, and the uint16 cast then wraps the total. The app is handed a
                    // fragment count matching neither the file nor this loop, the offset runs past
                    // the end, and the length underflows into a large read of whatever is beyond
                    // the file. A size the wire's fragment count cannot express is a failed read,
                    // never a silently truncated one.
                    uint32_t Frags = TotalContent / ContentCap + ((TotalContent % ContentCap) ? 1u : 0u);
                    if (Frags > 0xFFFFu) {
                        DeviceLog("STORAGE", "read '%.8s': %u bytes needs %u fragments", Name,
                                  (unsigned)TotalContent, (unsigned)Frags);
                        SendResponse(Frame, nullptr, 0, FLAG_FAIL);
                        break;
                    }
                    uint16_t TotalFrags = (uint16_t)(Frags == 0 ? 1 : Frags);
                    bool ReadOk = true;
                    for (uint16_t Frag = 0; Frag < TotalFrags; Frag++) {
                        memcpy(TxFrame.Payload + 4, Name, 8);
                        uint32_t ContentOff = (uint32_t)Frag * ContentCap;
                        uint16_t ContentLen = (TotalContent - ContentOff > ContentCap)
                                                   ? ContentCap
                                                   : (uint16_t)(TotalContent - ContentOff);
                        // This frame is reused for every fragment, so an unchecked short read would
                        // answer with whatever the previous fragment left in the payload - a wrong
                        // result with no error signal. A failed read is a failure.
                        if (ContentLen &&
                            StorageFlash.Read(FileOffset + ContentOff, TxFrame.Payload + 12, ContentLen) != ContentLen) {
                            DeviceLog("STORAGE", "read '%.8s' failed at offset %u", Name, (unsigned)ContentOff);
                            ReadOk = false;
                            break;
                        }
                        SendFragFragment(Frame, Frag, TotalFrags, (uint16_t)(8 + ContentLen));
                    }
                    if (!ReadOk) SendResponse(Frame, nullptr, 0, FLAG_FAIL);
                } else {
                    DeviceLog("STORAGE", "read '%.8s' failed", Name);
                    SendResponse(Frame, nullptr, 0, FLAG_FAIL);
                }
            }
#endif
            break;
        }

        case 6: { // Write per docs 03.06
            if (!(Frame.Flags & FLAG_FRAG)) break;
            PacketFragInfo Frag = PacketGetFrag(Frame);
            uint16_t Plen = PayloadBytes(Frame);
            // Docs 03.06 (B12): every fragment is [frag info][name (8)][content].
            if (Plen < 12) break;
            const uint8_t *Name = Frame.Payload + 4;
            const uint8_t *Contents = Frame.Payload + 12;
            uint16_t ContentLen = Plen - 12;
            if (Frag.Current == 0) {
                WriteActive = true;
                memcpy(WriteName, Name, 8);
                WriteSeq = 0xFFFF;
                WriteOff = 0;
            } else {
                if (!WriteActive) {
                    // A fragment of a stream that was never opened. Answering with the sequence
                    // below would let the app advance over bytes no file ever received.
                    DeviceLog("STORAGE", "write fragment with no stream open");
                    SendResponse(Frame, nullptr, 0, FLAG_FAIL);
                    break;
                }
                // The name is carried in every fragment and must not change mid-stream.
                if (memcmp(WriteName, Name, 8) != 0) {
                    DeviceLog("STORAGE", "write stream name changed mid-stream");
                    SendResponse(Frame, nullptr, 0, FLAG_FAIL);
                    break;
                }
            }
            uint32_t FileOffset, FileSize;
            // A fragment over the documented stream limit is refused, not clipped: the ack carries
            // the sequence and the app advances by what it sent, so a clipped write leaves this
            // offset behind and every later fragment lands short of where it belongs.
            if (ContentLen > 64) {
                DeviceLog("STORAGE", "write '%.8s' fragment %u over the stream limit", Name, (unsigned)ContentLen);
                SendResponse(Frame, nullptr, 0, FLAG_FAIL);
                break;
            }
            if (!Storage.GetFileInfo((const char *)Name, &FileOffset, &FileSize)) {
                DeviceLog("STORAGE", "write stream target '%.8s' missing", Name);
                SendResponse(Frame, nullptr, 0, FLAG_FAIL);
                break;
            }
            // Every rejection above and below must be visible to the sender: the ack carries the
            // sequence and the app advances its offset on a reply, so a fragment that is dropped
            // silently but still acknowledged writes every LATER fragment at the wrong offset - a
            // lost or out-of-order fragment turns into a corrupt file (B13).
            if (Frag.Current != (uint16_t)(WriteSeq + 1)) {
                DeviceLog("STORAGE", "write '%.8s' fragment %u out of order (want %u)", Name,
                          (unsigned)Frag.Current, (unsigned)(uint16_t)(WriteSeq + 1));
                SendResponse(Frame, nullptr, 0, FLAG_FAIL);
                break;
            }
            // A fragment reaching past the end of the file is a size disagreement, not a short
            // write: refuse it rather than clipping, because a clipped write advances this offset by
            // less than the app's and every later fragment lands short of where it belongs.
            if (ContentLen > (WriteOff < FileSize ? FileSize - WriteOff : 0)) {
                DeviceLog("STORAGE", "write '%.8s' fragment past the end at %u", Name, (unsigned)WriteOff);
                SendResponse(Frame, nullptr, 0, FLAG_FAIL);
                break;
            }
            if (ContentLen > 0) {
                if (!StorageFlash.Write(FileOffset + WriteOff, Contents, ContentLen)) {
                    DeviceLog("STORAGE", "write file '%.8s' flash write failed", Name);
                    SendResponse(Frame, nullptr, 0, FLAG_FAIL);
                    break;
                }
                WriteOff += ContentLen;
            }
            WriteSeq = Frag.Current;
            if (Frame.Flags & FLAG_REQACK) {
                uint8_t Ack[2] = {(uint8_t)(WriteSeq & 0xFF), (uint8_t)(WriteSeq >> 8)};
                SendResponse(Frame, Ack, 2);
            }
            break;
        }

        default:
            break;
    }
    return true;
}
