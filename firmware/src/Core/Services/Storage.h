#pragma once

#include "Core/Functions/Packet.h"
#include "Core/Functions/Storage.h"

// Storage service (Docs/Services/Storage.md): the file commands (format, create, delete,
// resize, rename, read/write streams). The filesystem itself is StorageBlockFS behind the
// `Storage` global; this class owns the service's own stream state.
class StorageService
{
public:
    bool Handle(const PacketFrame &Frame); // CID dispatch; true if consumed

private:
    // The in-flight fragmented write: the name travels in every fragment and must not change
    // mid-stream; the sequence and offset resume across fragments.
    char WriteName[8] = {0};
    bool WriteActive = false;
    uint16_t WriteSeq = 0;
    uint32_t WriteOff = 0;
};

// One instance per binary, in Core/Services/ (the FS global `Storage` is a different object).
StorageService StorageSvc;

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
                    // fragment count matching neither the file nor this loop, ContentOff runs past
                    // the end, and ContentLen underflows into a large read of whatever is beyond
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
