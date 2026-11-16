#pragma once

#include "Core/Functions/Packet.h"
#include "Core/Functions/Device.h"
#include "Core/Functions/SysFunctions.h"
#include "Core/Functions/Log.h"

#ifdef TYPE_CORE
#include "Core/Functions/SNDB.h"
#include "Core/Functions/Bootloader.h"

#ifdef USE_SUB_REQUEST
// Defined in Core/Services/SubscriptionsPersist.h (included later in the same translation
// unit): ask for this device's requester entries for `addr` to be re-pushed. Deferred on
// purpose - see SubscriptionsRequestReRegister.
void SubscriptionsRequestReRegister(uint16_t Addr);
#endif
#include "Core/Functions/TimeSync.h"
#endif

// Device service (Docs/Services/System Block and Device Commands.md, Docs/Command ID Table.md):
// Discover 0x0000, Ping 0x0001, Identify 0x0002, TimeSync 0x0003, Core discover 0x0010, SNDB
// 0x0011-0x0013 and the bootloader passthrough 0x0020-0x0023 (which the CID table files under
// Bootloader). Type/SN/Version/Capability/Name/Uptime are System BLOCK fields read through the
// Register service - they are not Device service commands.
class DeviceService
{
public:
    bool Handle(const PacketFrame &Frame); // CID dispatch; true if consumed

#ifdef TYPE_CORE
    // Main-loop tick for the asynchronous bootloader passthrough (CIDs 0x20/0x21/0x22/0x23/0x24/0x25):
    // returns immediately unless a request is outstanding, settles the awaited reply into a
    // Device response, or fails it on the 1 s deadline. Call next to the other service ticks.
    void PollBootloaderPassthrough();
#endif

private:
    // Dispatches SNDB requests: Read by ID/SN (0x11), Write (0x12) and Read All (0x13)
    // (Docs/Command ID table.md 0x0011-0x0013).
    void HandleSNDB(const PacketFrame &Frame);

    // Bootloader passthrough (Docs/Services/Bootloader.md "Core bootloader passthrough
    // commands", CIDs 0x20 identify / 0x21 read / 0x22 erase / 0x23 write / 0x24 CRC / 0x25 jump -
    // the CID rank follows the bootloader command bits 1-3). Asynchronous:
    // Handle* encodes the raw frame, RS485SendRaw's it, records the pending request and returns;
    // the main loop's PollBootloaderPassthrough collects the reply.
    void HandleBootloaderPassthrough(const PacketFrame &Frame, uint8_t Cid);
    // Answers a passthrough request whose payload is too short to encode with FLAG_FAIL + reason
    // 0x02 instead of returning silently: an unanswered request is indistinguishable from a dead
    // node to the app, which then waits out its own timeout (card t_334a1347).
    void RefuseShortRequest(const PacketFrame &Frame, uint8_t Need);
    // One FLAG_FAIL reply carrying its one-byte reason (Docs/Services/Bootloader.md, the reason
    // table). Every passthrough failure goes through here, so the failure a client sees and the
    // reason byte it reads are built in the same place.
    void SendPassthroughFail(uint16_t Src, uint16_t Cmd, uint16_t Trid, uint8_t Reason);
    bool BootloaderRxFeed(uint8_t Byte);               // sink for raw bootloader reply bytes
    static inline DeviceService *RxInstance = nullptr; // the single instance, for that hook

#ifdef TYPE_CORE
    // One outstanding passthrough request at a time (issue-and-return). The state is only
    // meaningful while PtPending; PollBootloaderPassthrough's first test is `!PtPending`, so
    // the idle main loop pays one bool load and no bus access. The routing fields let the
    // poll rebuild the Device response for the original requester after Handle* has returned.
    static constexpr uint32_t KPtTimeoutMs = 1000; // reply deadline (ms)
    static constexpr uint32_t KPtDumpAfterMs = 10000; // idle before the totals line is printed
    // The one-byte reason every passthrough FAILURE carries (Docs/Services/Bootloader.md "Core
    // Bootloader Passthrough Commands (002x)"). Each failure path routes to its own constant, so the
    // byte on the wire, the counter and the log line cannot drift apart: a refusal the core decides
    // (0x01/0x02, decided without touching the bus) is not a node that stayed silent (0x03) and not
    // the node's own failure frame (0x04). Failures only - a SUCCESS reply is byte-identical to what
    // it was before this byte existed, so a client that ignores the byte still works.
    static constexpr uint8_t KPtReasonBusy      = 0x01; // a passthrough request is already outstanding
    static constexpr uint8_t KPtReasonMalformed = 0x02; // payload too short, or CID outside 0x20-0x25
    static constexpr uint8_t KPtReasonTimeout   = 0x03; // the node did not answer by the deadline
    static constexpr uint8_t KPtReasonNodeFail  = 0x04; // the node answered with a failure frame
    bool PtPending    = false;                    // a request is issued and not yet settled
    // The passthrough CID (0x20..0x25). 0x25 is the jump: the node ACKS it (0b1011) and then
    // leaves the bootloader, so it waits for that ack like every other command - a silent one is
    // the ordinary 0x03 deadline, and the node is not mistaken for lost when it did answer.
    uint8_t PtCid     = 0;
    uint16_t PtSrc    = 0;                        // reply routing: original request IdSrc
    uint16_t PtCmd    = 0;                        // reply routing: original request Cmd
    uint16_t PtTrid   = 0;                        // reply routing: original request Trid
    uint32_t PtOffset = 0;                        // requested offset (write match; read echo)
    uint32_t PtDeadline = 0;                      // DeviceStatus.UptimeMs deadline
    uint8_t PtRx[Bootloader::MAX_FRAME_SIZE];     // bootloader reply frame
    uint16_t PtRxLen = 0;                         // bytes accumulated in PtRx
    bool PtRxReady = false;                       // PtRx holds a complete, decoded reply frame

    // Passthrough diagnostics (card t_334a1347). A request that gets no reply at all is
    // indistinguishable from a dead node unless the core says where it stopped: refused short,
    // never sent, sent but the node silent, or settled and the reply lost on the way back. These
    // are 16-bit counters and one line; the passthrough is the ONLY way a node is flashed, so a
    // silent failure has to be diagnosable from the rig without a rebuild.
    uint16_t PtReqCount    = 0; // requests that reached the handler
    uint16_t PtBusyRefused = 0; // a request arrived while one was still outstanding
    uint16_t PtShortRefused = 0; // request payload too short to encode
    uint16_t PtSendFail    = 0; // RS485SendRaw never got the raw frame out
    uint16_t PtSinkBytes   = 0; // raw bytes the sink consumed
    uint16_t PtSinkFrames  = 0; // complete, valid 0xCA reply frames decoded
    uint16_t PtSinkBad     = 0; // 0xCA frames that failed the size/parity check
    uint16_t PtOkCount     = 0; // settled with a reply
    uint16_t PtTimeouts    = 0; // settled on the deadline (no reply) - reason 0x03
    uint16_t PtNodeFail    = 0; // the node answered with a failure frame - reason 0x04
    // Frames the sink met while a passthrough was outstanding that were NOT its answer (card
    // t_c9a37bb8): a valid frame of another command, or a read response echoing another offset.
    // The core's own header-only request echo is the common one - it is a valid 7-byte frame, and
    // accepting it forwarded its END marker (0xBC) as payload byte 0. `PtForeignCmd` is the last
    // one's command bits, so a rig run names what was on the bus instead of guessing (a read
    // request 0b0010 = our own echo; a response command = a late answer to an earlier request).
    uint16_t PtForeign     = 0;
    uint8_t PtForeignCmd   = 0;
    // The same deadline settles counted PER PASSTHROUGH COMMAND (index = CID - 0x20, so
    // 0 identify / 1 read / 2 erase / 3 write / 4 CRC / 5 jump). The card that added them
    // (t_5eb19a27) measures a 200-cycle run per command, and "the node answered 199 of 200
    // identifies and 196 of 200 reads" is a different finding from a total. A silent request has to
    // be attributable per command, and this is the device's own answer to that - the probe's table
    // is the app's side of it. The jump (0x25) is included so a rig run can distinguish a jump
    // timeout from the other commands; a jumped node that never acked is exactly the lost-node
    // case 0x03 reports, but having it in the table makes that explicit without a rebuild.
    uint16_t PtTimeoutById[6] = {0, 0, 0, 0, 0, 0};
    uint32_t PtLastMs      = 0; // UptimeMs of the last request handled
    uint32_t PtDumpedAt    = 0; // UptimeMs of the last totals line
    uint16_t PtDumpedReq   = 0; // PtReqCount at that dump: nothing new, nothing printed
    uint8_t PtShortLogged  = 0; // the short-payload refusal has been logged once since boot
#endif

    // Sends a single-packet Device service reply back to the requester (src mirrored).
    // `reply` must be a PacketFrame provided by the caller: allocating one here would nest a
    // second full frame (~270 B) on top of the caller's own frame on the small node stack.
    void SendDeviceReply(const PacketFrame &Frame, PacketFrame &Reply,
                         const void *Payload, uint8_t Len);
};

// One instance per binary, in Core/Services/.
DeviceService DeviceSvc;

inline void DeviceService::SendDeviceReply(const PacketFrame &Frame, PacketFrame &Reply,
                                           const void *Payload, uint8_t Len)
{
    PacketConstruct(&Reply, Frame.IdSrc, Frame.Cmd, Frame.Trid,
                     FLAG_TYPE | FLAG_START | FLAG_STOP,
                     (const uint8_t *)Payload, Len);
    DispatchPacket(Reply);
}

#ifdef TYPE_CORE
void DeviceService::HandleSNDB(const PacketFrame &Frame)
{
    uint8_t Cid = GetServiceCID(Frame.Cmd);
    bool IsResponse = (Frame.Flags & FLAG_TYPE);

    if (IsResponse) return; // Core only handles requests

    switch (Cid)
    {
        case 0x13:
        { // SNDB Read All per docs 00.13
            // The recovered registry tracks the valid-entry count, so one streaming scan
            // is enough (no separate counting pass over flash).
            SNDB::IterReset();
            uint32_t Count = (uint32_t)SNDB::ActiveCount();

            if (Count == 0)
            {
                SendResponse(Frame, nullptr, 0); // empty stop packet
                break;
            }

            // Stream every entry as a FRAG stream (Docs/Services/System Block and Device Commands.md:
            // "Fragmentation, SN + ID stream"). Each fragment carries up to 112 bytes
            // of 16-byte (SN + ID) entries; SendFragFragment writes the 4-byte frag info.
            uint32_t Total = Count * 16;
            uint16_t TotalFrags = (uint16_t)((Total + MAX_FRAG_CONTENT_SIZE - 1) / MAX_FRAG_CONTENT_SIZE);
            RegistryEntry Entry;
            uint32_t Sent = 0;
            for (uint16_t Frag = 0; Frag < TotalFrags; Frag++)
            {
                uint16_t Off = 4;
                while (Off - 4 < MAX_FRAG_CONTENT_SIZE && Sent < Count && SNDB::IterNext(Entry))
                {
                    uint16_t WireId = RegistryAddress(Entry);
                    memcpy(TxFrame.Payload + Off, Entry.Uid.Bytes, 14);
                    memcpy(TxFrame.Payload + Off + 14, &WireId, 2);
                    Off += 16;
                    Sent++;
                }
                SendFragFragment(Frame, Frag, TotalFrags, (uint16_t)(Off - 4));
            }
            break;
        }

        case 0x11:
        { // SNDB Read per docs 00.11
            RegistryEntry Entry;
            bool Found = false;

            // Disambiguate by length (Docs: "ID or SN (based on length)"): an ID request is a
            // 2-byte uint16, an SN request the 14-byte serial. Payloads are not padded.
            if (PayloadBytes(Frame) == 2)
            {
                // The request carries a full net.device. Local devices are stored with net
                // 0, so a net-qualified request for a local device also tries the net-0
                // (device-only) form.
                uint16_t LookupId = *reinterpret_cast<const uint16_t *>(Frame.Payload);
                Found = SNDB::GetEntry(LookupId, Entry) ||
                        (((LookupId >> 10) != 0) &&
                         SNDB::GetEntry((uint16_t)(LookupId & 0x3FF), Entry));
            }
            else if (PayloadBytes(Frame) >= 14)
            {
                const SerialNumber *LookupSn = reinterpret_cast<const SerialNumber *>(Frame.Payload);
                uint16_t LookupAddr = SNDB::FindAddress(*LookupSn);
                if (LookupAddr != ADDR_INVALID)
                {
                    Found = SNDB::GetEntry(LookupAddr, Entry);
                }
            }

            if (Found)
            {
                uint8_t Payload[16];
                uint16_t WireId = RegistryAddress(Entry);
                memcpy(Payload, Entry.Uid.Bytes, 14);
                memcpy(Payload + 14, &WireId, 2);
                SendDeviceReply(Frame, TxFrame, Payload, 16);
            }
            else
            {
                // Send an empty response to indicate not found
                DeviceLog("DEVICE", "SNDB lookup miss (id or sn)");
                SendDeviceReply(Frame, TxFrame, nullptr, 0);
            }
            break;
        }

        case 0x12:
        { // SNDB Write per docs 00.12
            if (PayloadBytes(Frame) >= 16)
            {
                const SerialNumber *WriteSn = reinterpret_cast<const SerialNumber *>(Frame.Payload);
                // The ID is net-qualified (net.device): local devices carry net 0, other
                // cores their NetID.1. AddDevice rejects a foreign-net address.
                uint16_t WriteId = LoadUnaligned<uint16_t>(Frame.Payload + 14);

                // Per Docs/Services/System Block and Device Commands.md: ID 0 = delete the
                // entry carrying this serial number.
                bool Success = (WriteId != 0)
                                   ? SNDB::AddDevice(*WriteSn, WriteId)
                                   : SNDB::RemoveDevice(
                                         SNDB::FindAddress(*WriteSn));
                if (!Success)
                {
                    DeviceLog("DEVICE", "SNDB write id %u failed", (unsigned)WriteId);
                }
                // Always acknowledge (empty frame = failure, like the CID 13 not-found case).
                SendDeviceReply(Frame, TxFrame, Success ? Frame.Payload : nullptr,
                                (uint8_t)(Success ? 16 : 0));
            }
            break;
        }

        default:
            break;
    }
}

// The app sends these to the connected core; the core relays the raw frame onto its RSBus.
// The frame carries no address, so this is a broadcast: every node currently in bootloader
// mode on that bus receives it. CIDs (Docs/Services/Bootloader.md, 3694c46): 0x20 identify,
// 0x21 read, 0x22 erase, 0x23 write, 0x24 CRC, 0x25 jump to the app - the CID and the frame
// carry the same rank. The handler only ISSUES the raw request and returns;
// PollBootloaderPassthrough (main loop) collects the reply and builds the Device response. One
// request is outstanding at a time - a second one while pending is refused, never queued.
// The jump is no exception: the node acks it before it leaves, so the core awaits that ack and
// reports the ordinary 0x03 deadline when it does not come.
inline void DeviceService::HandleBootloaderPassthrough(const PacketFrame &Frame, uint8_t Cid)
{
    PtReqCount = (uint16_t)(PtReqCount + 1);
    PtLastMs = DeviceStatus.UptimeMs;

    // Busy: refuse a second request rather than queue it, so the app must wait for the
    // outstanding one to settle (its reply or the 1 s timeout). Reason 0x01 - the app retries.
    if (PtPending)
    {
        PtBusyRefused = (uint16_t)(PtBusyRefused + 1);
        SendPassthroughFail(Frame.IdSrc, Frame.Cmd, Frame.Trid, KPtReasonBusy);
        return;
    }

    // Encode the raw bootloader request for this CID. The CIDs follow the bootloader command
    // order (bit 0 = direction, bits 1-3 = operation), so a CID and its frame carry the same rank:
    // 0x20 identify, 0x21 read, 0x22 erase, 0x23 write, 0x24 CRC, 0x25 jump.
    uint8_t Raw[Bootloader::DATA_SIZE];
    size_t Len;
    uint32_t Offset = 0;
    if (Cid == 0x20)
    { // Identification (0b000x): header-only request (no request payload).
        Bootloader::EncodeIdentifyRequest(Raw);
        Len = Bootloader::READ_REQ_SIZE;
    }
    else if (Cid == 0x21)
    { // Read (0b001x): Offset (4).
        if (PayloadBytes(Frame) < 4)
        {
            RefuseShortRequest(Frame, 4);
            return;
        }
        Offset = LoadUnaligned<uint32_t>(Frame.Payload);
        Bootloader::EncodeReadRequest(Offset, Raw);
        Len = Bootloader::READ_REQ_SIZE;
    }
    else if (Cid == 0x22)
    { // Erase (0b010x): Offset (4). The node blanks the page its offset names (its identify
      // response reports the page size), so the core forwards the offset and owns no geometry.
        if (PayloadBytes(Frame) < 4)
        {
            RefuseShortRequest(Frame, 4);
            return;
        }
        Offset = LoadUnaligned<uint32_t>(Frame.Payload);
        Bootloader::EncodeEraseRequest(Offset, Raw);
        Len = Bootloader::ERASE_REQ_SIZE;
    }
    else if (Cid == 0x23)
    { // Write (0b011x): Offset (4) + Payload (32).
        if (PayloadBytes(Frame) < 4 + Bootloader::PAYLOAD_SIZE)
        {
            RefuseShortRequest(Frame, 4 + Bootloader::PAYLOAD_SIZE);
            return;
        }
        Offset = LoadUnaligned<uint32_t>(Frame.Payload);
        Bootloader::EncodeWrite(Offset, Frame.Payload + 4, Raw);
        Len = Bootloader::DATA_SIZE;
    }
    else if (Cid == 0x24)
    { // CRC (0b100x): the request takes NO arguments - the CRC always covers the whole app
      // space - so it is relayed as the bootloader's 3-byte CRC request.
        Bootloader::EncodeDigestRequest(Raw);
        Len = Bootloader::DIGEST_REQ_SIZE;
    }
    else if (Cid == 0x25)
    { // Jump to app (0b101x): the request takes NO arguments - the offset slot is ignored - so it
      // is relayed as the bootloader's 7-byte jump request. The node ACKS it (0b1011) and only then
      // leaves for its application, so the core WAITS for that ack like it does for every other
      // command and settles on the ordinary 0x03 deadline when none comes: a node that never
      // answered is reported as the lost node it is. Relaying the ack (instead of acknowledging the
      // SEND and going quiet) is what makes the jump's outcome visible to the app.
        Bootloader::EncodeJumpRequest(Raw);
        Len = Bootloader::READ_REQ_SIZE;
    }
    else
    { // Not a passthrough CID: refuse rather than send something nobody asked for. Dispatch only
      // routes 0x20-0x25 here, so this is defensive - but it answers like the malformed request it
      // is (0x02, counted with the short-payload refusals), never with silence.
        PtShortRefused = (uint16_t)(PtShortRefused + 1);
        SendPassthroughFail(Frame.IdSrc, Frame.Cmd, Frame.Trid, KPtReasonMalformed);
        return;
    }

    // Issue and return. A send failure leaves nothing to await, so fail immediately; otherwise
    // record the pending request and let the poll collect the reply. A lost request settles as
    // FAIL on the deadline (no blocking, no retry loop in this handler).
    // Arm the sink and mark pending BEFORE the request goes out. The node answers within one
    // frame time (~700 us) of our last byte - inside RS485SendRaw's own echo read - so arming
    // afterwards races and loses the reply to the packet assembler as non-0xAA noise.
    RxInstance = this;
    BootloaderRxSink = [](uint8_t Byte) { return RxInstance->BootloaderRxFeed(Byte); };
    PtCid      = Cid;
    PtSrc      = Frame.IdSrc;
    PtCmd      = Frame.Cmd;
    PtTrid     = Frame.Trid;
    PtOffset   = Offset;
    PtRxLen    = 0;
    PtRxReady  = false;
    PtDeadline = DeviceStatus.UptimeMs + KPtTimeoutMs;
    PtPending  = true;

    if (!RS485SendRaw(Raw, Len))
    {
        PtSendFail = (uint16_t)(PtSendFail + 1);
        PtPending = false;
        BootloaderRxSink = nullptr;
        // The frame never left the core, so no node answered and none can. The reason table has no
        // code for the core's own send refusal, so it is reported in the class the app reacts to
        // when there is no node answer (0x03) and stays countable on its own through `sendfail=` in
        // the BLPT totals line - the app must never read a bus hold-off as a malformed request.
        SendPassthroughFail(Frame.IdSrc, Frame.Cmd, Frame.Trid, KPtReasonTimeout);
        return;
    }
}

// One FLAG_FAIL reply carrying its one-byte reason (Docs/Services/Bootloader.md, the reason table).
// Every passthrough failure is built here, so the wire byte and the reason constant cannot drift
// apart. The payload is the reason byte ALONE - the app classifies a FAIL by its one-byte payload -
// and the frame is otherwise the one SendDeviceReply builds, plus FLAG_FAIL.
inline void DeviceService::SendPassthroughFail(uint16_t Src, uint16_t Cmd, uint16_t Trid,
                                               uint8_t Reason)
{
    PacketConstruct(&TxFrame, Src, Cmd, Trid,
                    FLAG_TYPE | FLAG_START | FLAG_STOP | FLAG_FAIL, &Reason, 1);
    DispatchPacket(TxFrame);
}

// A passthrough request that cannot be encoded is REFUSED, not ignored: the app cannot tell "the
// core rejected this" from "the node is dead" when nothing comes back, and it then waits out its
// own timeout (card t_334a1347). One FAIL reply carrying reason 0x02 (malformed), and the check is
// logged once since boot.
inline void DeviceService::RefuseShortRequest(const PacketFrame &Frame, uint8_t Need)
{
    PtShortRefused = (uint16_t)(PtShortRefused + 1);
    if (!PtShortLogged)
    {
        PtShortLogged = 1;
        DeviceLog("BLPT", "refused a short passthrough request: cid=0x%02X payload=%u need=%u",
                  (unsigned)GetServiceCID(Frame.Cmd), (unsigned)PayloadBytes(Frame), (unsigned)Need);
    }
    SendPassthroughFail(Frame.IdSrc, Frame.Cmd, Frame.Trid, KPtReasonMalformed);
}

// Byte sink for the bootloader reply, installed on the shared bus reader (RSBus.h
// BootloaderRxFeed) while a passthrough is outstanding. It consumes 0xCA frame bytes into PtRx
// and returns true; any other byte returns false and falls through to the packet assembler, so
// one pass over the UART serves both packet types and neither is lost. A completed, valid frame
// sets PtRxReady for the poll.
inline bool DeviceService::BootloaderRxFeed(uint8_t Byte)
{
    if (!PtPending || PtRxReady)
        return false;
    PtSinkBytes = (uint16_t)(PtSinkBytes + 1);
    if (PtRxLen == 0)
    {
        if (Byte != Bootloader::START)
            return false;
        PtRx[0] = Byte;
        PtRxLen = 1;
        return true;
    }
    if (PtRxLen < sizeof(PtRx))
        PtRx[PtRxLen++] = Byte;
    if (PtRxLen >= 2)
    {
        uint16_t Need = Bootloader::FrameSize((uint8_t)(PtRx[1] & 0x0Fu));
        if (Need == 0 || Need > sizeof(PtRx))
        {
            PtRxLen = 0; // bad control: resync on the next 0xCA
            return true;
        }
        // Card t_c9a37bb8: a valid frame is not the same thing as OUR answer. The line is half
        // duplex, so the core hears its own raw request echo - for the header-only requests that
        // echo IS a valid 7-byte frame (0xCA, control, offset, 0xBC), and forwarding one as a read
        // reply handed the app its END marker as payload byte 0 (30 of 4480 chunk reads measured on
        // the rig). Reject it here, on the control byte, so nothing of the foreign frame is
        // assembled into PtRx and the real answer's 0xCA still starts a clean frame.
        if ((PtRx[1] & 0x0Fu) != Bootloader::PassthroughResponse(PtCid))
        {
            PtForeign = (uint16_t)(PtForeign + 1);
            PtForeignCmd = (uint8_t)(PtRx[1] & 0x0Fu);
            PtRxLen = 0; // not our answer: resync on the next 0xCA
            return true;
        }
        if (PtRxLen >= Need)
        {
            if (Bootloader::Decode(PtRx, Need) != Bootloader::INVALID)
            {
                PtRxReady = true;
                PtSinkFrames = (uint16_t)(PtSinkFrames + 1);
            }
            else
                PtSinkBad = (uint16_t)(PtSinkBad + 1);
            PtRxLen = 0; // corrupt frame: resync on the next 0xCA
        }
    }
    return true;
}

// Main-loop tick for the passthrough: settle the outstanding request without blocking the loop.
// Idle cost is a single bool test. The reply frame is assembled by BootloaderRxFeed, so this
// only builds the Device response (or FAILs on the deadline). TIMEOUT = 1 s.
inline void DeviceService::PollBootloaderPassthrough()
{
    // Idle: still settle the diagnostics. A request that got no reply has to be countable AFTER
    // the fact - the app only sees its own timeout, and "no reply" out of the core and "no reply"
    // out of the node are the same silence to it. Printed once per run of passthrough traffic,
    // 10 s after the last request, through DeviceLog (which itself keeps off a talking link).
    if (!PtPending)
    {
        if (PtReqCount != PtDumpedReq &&
            (uint32_t)(DeviceStatus.UptimeMs - PtLastMs) >= KPtDumpAfterMs)
        {
            PtDumpedReq = PtReqCount;
            PtDumpedAt = DeviceStatus.UptimeMs;
            // The one line a rig session needs after a passthrough run: how many requests arrived,
            // how many were refused/sent/failed, how many raw bytes the node actually put on the
            // wire (sinkbytes/frames - a silent request has sinkbytes for the requests that were
            // answered only), and whether the settle was a reply or the deadline. `timeout` above
            // zero with `sinkbytes` flat is a node that never answered; both flat is a request
            // that never left. Printed 10 s after the last request, and DeviceLog keeps it off a
            // link the app is still using.
            //
            // The four failure reasons the app is told are the four refusals this line counts:
            // `busy` (0x01) and `short` (0x02) are decided without the bus, `timeout` (0x03) is the
            // deadline, `nodefail` (0x04) is a failure frame from the node. `sendfail` is the core's
            // own send refusal, reported to the app in the 0x03 class (see the send path) and kept
            // apart here so a rig session can still tell it from a silent node.
            //
            // short/coll/reply are the raw send's own echo verdicts (RSBus.h): `reply` counts the
            // sends the node answered INSIDE the echo window - the case that used to be reported
            // as a collision and threw the answer away - and `coll` a byte that was neither our
            // echo nor the node's start marker (a genuine collision). A run with sendfail=0 and
            // reply>0 is the fixed path doing its job.
            DeviceLog("BLPT",
                      "total req=%u busy=%u short=%u sendfail=%u sinkbytes=%u frames=%u bad=%u "
                      "ok=%u timeout=%u nodefail=%u rawshort=%lu rawcoll=%lu rawreply=%lu",
                      (unsigned)PtReqCount, (unsigned)PtBusyRefused, (unsigned)PtShortRefused,
                      (unsigned)PtSendFail, (unsigned)PtSinkBytes, (unsigned)PtSinkFrames,
                      (unsigned)PtSinkBad, (unsigned)PtOkCount, (unsigned)PtTimeouts,
                      (unsigned)PtNodeFail,
                      (unsigned long)RS485RawShortEcho, (unsigned long)RS485RawCollisions,
                      (unsigned long)RS485RawEarlyReplies);
            // The second line is the card t_5eb19a27 attribution set, kept separate because the
            // line above is already at the console's 128-byte format buffer:
            //   kept/drop     bytes the keep ring accepted / refused (Bus.h BusRxKept,
            //                 BusRxDestroyed) - "the answer arrived while a send blocked" vs "the
            //                 ring could not hold it", which used to be one silence;
            //   rawstint      attempts the raw send's one deadline refused to start;
            //   rawstall      a raw send whose last byte never left the UART in
            //                 RS485_TX_DONE_TIMEOUT_MS - a SEND timeout, not a node's silence;
            //   nocid i/r/e/w/c/j deadline settles per passthrough command in CID order.
            DeviceLog("BLPT",
                      "counters kept=%lu drop=%lu rawstint=%lu rawstall=%lu "
                      "nocid i/r/e/w/c/j=%u/%u/%u/%u/%u/%u",
                      (unsigned long)BusRxKept, (unsigned long)BusRxDestroyed,
                      (unsigned long)RS485RawStintCuts, (unsigned long)RS485RawTxStalls,
                      (unsigned)PtTimeoutById[0], (unsigned)PtTimeoutById[1],
                      (unsigned)PtTimeoutById[2], (unsigned)PtTimeoutById[3],
                      (unsigned)PtTimeoutById[4], (unsigned)PtTimeoutById[5]);
            // Third line, card t_c9a37bb8: the frames met while a passthrough was outstanding that
            // were NOT its answer, and the command bits of the most recent one. This is what names
            // the defect from the rig without a rebuild: `last=0x2` is a READ REQUEST - the core
            // hearing its own raw request echo - while a response command (0x3/0x5/0x7) is a late
            // answer to an earlier request. Printed only when there were any.
            if (PtForeign != 0)
                DeviceLog("BLPT", "foreign=%u last=0x%02X", (unsigned)PtForeign,
                          (unsigned)PtForeignCmd);
        }
        return; // not expecting a reply: one bool test, no bus access
    }

    // The reply frame is assembled by BootloaderRxFeed on the shared bus reader (RSBus.h);
    // there is no bus access here. Build the Device response from it, or FAIL on the deadline.
    if (PtRxReady)
    {
        uint8_t Out[Bootloader::IDENTIFY_RESP_SIZE - 3]; // the frame body: size | name | page
        uint8_t Flags = FLAG_TYPE | FLAG_START | FLAG_STOP;
        uint8_t OutLen = 0;
        uint8_t Fail = 0; // non-zero: the one-byte reason this reply fails with (0x04)

        // The half of the answer check only the poll can make (card t_c9a37bb8; the sink rejects a
        // frame of the wrong COMMAND, BootloaderRxFeed): a read response must echo the offset we
        // asked for. One that does not is a late answer to an earlier request - a valid frame of the
        // right command, and forwarding it would hand this chunk another chunk's bytes. Dropped, so
        // the request keeps waiting for its own answer (and settles on the deadline if none comes).
        if (PtCid == 0x21 && !Bootloader::ReadAnswersOffset(PtRx, PtOffset))
        {
            PtForeign = (uint16_t)(PtForeign + 1);
            PtForeignCmd = (uint8_t)(PtRx[1] & 0x0Fu);
            PtRxReady = false;
            PtRxLen = 0;
            return;
        }

        if (PtCid == 0x20)
        { // Identify (0b000x): the node's identify-response body forwarded verbatim - app-space
          // size (u32), the page size (u32), the 28-byte null-padded name. The page size is the
          // node's ERASE unit, so the app asks the node how big a page is instead of assuming one
          // device's geometry. Forwarding the body is what keeps the app's layout and the
          // bootloader's one layout (Bootloader.h).
            memcpy(Out, PtRx + 2, sizeof(Out));
            OutLen = sizeof(Out);
            Flags |= FLAG_SUCCESS;
        }
        else if (PtCid == 0x21)
        { // Read (0b001x): echo the requested offset + the 32-byte payload.
            memcpy(Out, &PtOffset, 4);
            memcpy(Out + 4, Bootloader::Payload(PtRx), Bootloader::PAYLOAD_SIZE);
            OutLen = sizeof(Out);
            Flags |= FLAG_SUCCESS;
        }
        else if (PtCid == 0x22)
        { // Erase (0b010x): the node answers the page it blanked, with bit 0 set when the page is
          // NOT blank. Bit 0 is the protocol's failure flag (every real page offset is aligned), so
          // the core forwards the offset and reads the flag - no page-size arithmetic here.
            uint32_t Ack = Bootloader::Offset(PtRx);
            memcpy(Out, &Ack, 4);
            OutLen = 4;
            if (Ack & 1u) Fail = KPtReasonNodeFail;
            else          Flags |= FLAG_SUCCESS;
        }
        else if (PtCid == 0x23)
        { // Write (0b011x): SUCCESS only when the node reports the exact offset we asked it to
          // write; otherwise FAIL. Either way carry the node's last-written offset so the app can
          // resynchronise after a rejected chunk.
            uint32_t Last = Bootloader::Offset(PtRx);
            memcpy(Out, &Last, 4);
            OutLen = 4;
            if (Last == PtOffset) Flags |= FLAG_SUCCESS;
            else                  Fail = KPtReasonNodeFail;
        }
        else if (PtCid == 0x24)
        { // CRC (0b100x): the node answers with the CRC32 of the whole app space (7-byte frame,
          // no offset). The core forwards the four CRC bytes and owns no comparison; the app
          // compares them against its own CRC of the image padded to the app size. A frame of any
          // other command cannot reach here any more - the sink rejects one against this CID's
          // response command (BootloaderRxFeed, card t_c9a37bb8) - so the value is this request's.
            uint32_t Crc = Bootloader::DigestCrc(PtRx);
            memcpy(Out, &Crc, 4);
            OutLen = 4;
            Flags |= FLAG_SUCCESS;
        }
        else if (PtCid == 0x25)
        { // Jump to app (0b101x): the node answered, and it did so immediately before leaving the
          // bootloader. The reply's offset slot is the doc's "Offset" (ignored), forwarded verbatim;
          // FLAG_SUCCESS is the whole answer - the node took the jump.
            uint32_t Ack = Bootloader::Offset(PtRx);
            memcpy(Out, &Ack, 4);
            OutLen = 4;
            Flags |= FLAG_SUCCESS;
        }
        else
        { // Not a passthrough CID: FAIL rather than answer with another command's payload.
          // Defensive (PtCid is always 0x20-0x25 while pending) and reported as the malformed
          // request it would be.
            OutLen = 0;
            Fail = KPtReasonMalformed;
        }

        // A node that answered with a failure frame: the reply carries the one-byte reason, not the
        // node's 4-byte ack (Docs/Services/Bootloader.md - every failure answer carries a one-byte
        // reason). 0x04 still tells the app the node's own semantics are what refused it - an erase
        // at a misaligned offset, a write into a page that was not erased - and its recovery path
        // for those is unchanged.
        if (Fail != 0)
        {
            PtNodeFail = (uint16_t)(PtNodeFail + 1);
            Out[0] = Fail;
            OutLen = 1;
            Flags = (uint8_t)((Flags & ~FLAG_SUCCESS) | FLAG_FAIL);
        }

        PtPending = false;
        PtOkCount = (uint16_t)(PtOkCount + 1);
        BootloaderRxSink = nullptr;
        PacketConstruct(&TxFrame, PtSrc, PtCmd, PtTrid, Flags, Out, OutLen);
        DispatchPacket(TxFrame);
        return;
    }

    // No reply (or an unrelated frame) yet: settle as FAIL once the deadline passes.
    if ((int32_t)(DeviceStatus.UptimeMs - PtDeadline) >= 0)
    {
        PtPending = false;
        PtTimeouts = (uint16_t)(PtTimeouts + 1);
        // Per-command attribution covers all six passthrough commands (identify, read, erase,
        // write, CRC, jump). The jump (0x25) is included so a rig run can distinguish a jump
        // timeout from the other commands; a jumped node that never acked is exactly the lost-node
        // case 0x03 reports, but having it in the table makes that explicit without a rebuild.
        if (PtCid >= 0x20 && PtCid <= 0x25)
            PtTimeoutById[PtCid - 0x20] = (uint16_t)(PtTimeoutById[PtCid - 0x20] + 1);
        BootloaderRxSink = nullptr;
        // The deadline is its own reason: the request went out and the node never answered, which
        // the app retries differently from a refusal of its own.
        SendPassthroughFail(PtSrc, PtCmd, PtTrid, KPtReasonTimeout);
    }
}
#endif // TYPE_CORE

// Handles Device service requests (Discover, Ping, Identify, TimeSync, Core discover, SNDB, and
// the 0x20-0x25 bootloader passthrough that the CID table files under Bootloader).
bool DeviceService::Handle(const PacketFrame &Frame)
{
    uint8_t Cid = GetServiceCID(Frame.Cmd);
    bool IsResponse = (Frame.Flags & FLAG_TYPE);

    if (IsResponse)
    {
        // A reply echoes the request's CID and TRID, so the CID identifies the operation the
        // way the request side does. Only device-originated requests' replies reach here;
        // app-originated replies route to the app by their TRID instead.
        if (Cid == 0x10) // Core-discover response
        {
#ifdef TYPE_CORE
            // Another core answered Core discover: store it as its NetID.1 (Docs: "Other
            // cores are stored only with their NetID.1"). A net matching ours is the
            // collision case (latched in HandleDiscoverResponse); it is not stored. Skip the
            // write when the entry is already present so the periodic round does not churn.
            const SerialNumber *PeerSn = reinterpret_cast<const SerialNumber *>(Frame.Payload);
            uint8_t PeerNet = (uint8_t)((Frame.IdSrc >> 10) & 0x3F);
            uint16_t PeerAddr = MakeId(PeerNet, 1);
            if (PeerNet != 0 && PeerNet != DeviceStatus.NetId &&
                !(*PeerSn == GetSerialNumber()) &&
                SNDB::FindAddress(*PeerSn) != PeerAddr)
                SNDB::AddDevice(*PeerSn, PeerAddr);
            CoreTimeSync.HandleDiscoverResponse(Frame);
#endif
        }
        else if (Cid == 0) // Discover (assign) response
        {
#ifdef TYPE_CORE
            // A core is never assigned. The reply's source is the assigning core: if it
            // carries our net (and is not our own broadcast looped back) the net is
            // claimed twice - the same collision the Core-discover path detects.
            if (Frame.IdSrc != DeviceStatus.ShortAddress)
                NoteCoreNet((uint8_t)((Frame.IdSrc >> 10) & 0x3F), DeviceStatus.NetId);
            if (DeviceIsCore()) return true; // a core is never assigned
#endif
            const AssignPayload *Assign = reinterpret_cast<const AssignPayload *>(Frame.Payload);
            if (Assign->Sn == GetSerialNumber())
            {
                // The core sends the net-qualified address; store it and adopt its net so
                // local net-0 targets resolve correctly.
                DeviceStatus.ShortAddress = Assign->NewAddr;
                DeviceStatus.NetId = (uint8_t)((Assign->NewAddr >> 10) & 0x3F);
            }
        }
        else if (Cid == 3) // Time sync response per docs 00.03
        {
            // TimeSync is synchronized-device initiated: the INITIATOR (a node syncing to
            // a core, or a core syncing to the longest-running core) applies the offset to
            // its OWN clock. The core never pushes offsets to other devices.
            //
            // NTP-like with SYNCHRONIZED timestamps (Now()): t0/t3 are the initiator's
            // clock including its current offset, so the computed offset is a DELTA to
            // add. Using raw TimeFromBoot() here would make it an absolute value and
            // `+=` would accumulate it on every sync.
            uint32_t T0, T1, T2;
            // 4-byte aligned offsets in a packed+aligned(4) frame: word loads.
            T0 = *reinterpret_cast<const uint32_t *>(Frame.Payload);
            T1 = *reinterpret_cast<const uint32_t *>(Frame.Payload + 4);
            T2 = *reinterpret_cast<const uint32_t *>(Frame.Payload + 8);
            uint32_t T3 = Now();
#ifdef TYPE_CORE
            int32_t Offset = (int32_t)(((int64_t)(int32_t)(T1 - T0) + (int64_t)(int32_t)(T2 - T3)) / 2);
#else
            int32_t Offset = ((int32_t)(T1 - T0) + (int32_t)(T2 - T3)) / 2;
#endif
            // Step the offset AND update the drift estimate (ApplyTimeSync) so the clock
            // tracks the peer's rate between syncs, not just its value at this instant.
            ApplyTimeSync(Offset);
        }
        return true;
    }

    // Requests:
    switch (Cid)
    {
        case 0: // Discover
        {
#ifdef TYPE_CORE
            // Per the docs, ID assignment is a *core capability*: only answer when we
            // report the CORE bit and hold the core role (device ID 1 in a valid net).
            if (!(KCapabilities & Capabilities::Core) || !DeviceIsCore())
                return true;

            if (PayloadBytes(Frame) < sizeof(SerialNumber))
                return true;

            char SnStr[29];
            const SerialNumber *IncomingSn = reinterpret_cast<const SerialNumber *>(Frame.Payload);
            SerialNumberToString(*IncomingSn, SnStr, sizeof(SnStr));

            // The core never registers itself, so a Discover carrying our own serial (a
            // looped-back broadcast could deliver one) is ignored rather than allocating a
            // bogus device ID.
            if (*IncomingSn == GetSerialNumber())
                return true;

            // Foreign nets: a device broadcasting from the local net (net 0 while unassigned,
            // or our net) gets an ID and is stored; a device already addressed in another net
            // is dropped - not stored, not assigned (Docs: "Devices from foreign nets are not
            // stored").
            uint8_t SrcNet = (uint8_t)((Frame.IdSrc >> 10) & 0x3F);
            if (SrcNet != 0 && SrcNet != DeviceStatus.NetId)
            {
                DeviceLog("CORE", "Discover from foreign net %u ignored: %s",
                          (unsigned)SrcNet, SnStr);
                return true;
            }

            uint16_t NewAddr = SNDB::FindAddress(*IncomingSn);

            if (NewAddr != ADDR_INVALID)
            {
                DeviceLog("CORE", "Re-discovered device: %s -> Assigned ID: %d", SnStr,
                          (int)(NewAddr & 0x3FF));
            }
            else
            {
                NewAddr = SNDB::NewDevice(*IncomingSn);
                if (NewAddr != ADDR_INVALID)
                {
                    DeviceLog("CORE", "Registered new device: %s -> Assigned ID: %d", SnStr,
                              (int)(NewAddr & 0x3FF));
                }
                else
                {
                    DeviceLog("CORE", "Registry full! Cannot register: %s", SnStr);
                    return true;
                }
            }

            // Reply with SN(14) + assigned address(2). The reply echoes the request's CID and
            // TRID (per "responses echo the request's TRID"), which is how the requester tells
            // it apart from the other Device-service replies.
            AssignPayload ResponseData;
            ResponseData.Sn = *IncomingSn;
            // Local devices are stored with net 0; the core's NetID is added here so the
            // node's address is net-qualified.
            ResponseData.NewAddr = MakeId(DeviceStatus.NetId, (uint16_t)(NewAddr & 0x3FF));

#ifdef USE_SUB_REQUEST
            // The node just (re-)appeared with an empty provider table, so ask for this
            // device's subscriptions that point at it to be re-pushed - otherwise a node reboot
            // silently ends them. Only *requested* here: this runs inside packet dispatch and
            // must not delay the assignment reply below. Provider addresses are wire
            // addresses (net-qualified), so queue the qualified value.
            SubscriptionsRequestReRegister(ResponseData.NewAddr);
#endif

            PacketConstruct(&TxFrame, ADDR_BROADCAST,
                             Frame.Cmd, // the request CMD (Device service)
                             Frame.Trid,    // echo the request TRID
                             FLAG_TYPE | FLAG_START | FLAG_STOP,
                             reinterpret_cast<const uint8_t *>(&ResponseData),
                             sizeof(ResponseData),
                             PRIORITY_DISCOVER);

            DispatchPacket(TxFrame);

#endif
            break;
        }

        case 1: // Ping
            SendDeviceReply(Frame, TxFrame, nullptr, 0);
            break;

        case 2: // Identify (Bool: blink the red LED fast while true)
        {
            DeviceIdentifyStart((PayloadBytes(Frame) > 0 && Frame.Payload[0] != 0),
                                DeviceStatus.UptimeMs);
            SendDeviceReply(Frame, TxFrame, nullptr, 0);
            break;
        }

        case 3: // Time sync per docs 00.03
        {
            if (PayloadBytes(Frame) >= 4)
            {
                uint32_t TimeSent = *reinterpret_cast<const uint32_t *>(Frame.Payload);
                // The responder reports its SYNCHRONIZED time (Now()), matching the
                // initiator's t0/t3 (see the response handler above).
                uint32_t T1 = Now();
                uint32_t T2 = Now();

                // Response: echoed requester time, responder receive time, responder send
                // time (NTP-like; see the offset math in the response handler above).
                uint8_t Rpl[12];
                memcpy(Rpl + 0, &TimeSent, 4);
                memcpy(Rpl + 4, &T1, 4);
                memcpy(Rpl + 8, &T2, 4);
                PacketConstruct(&TxFrame, Frame.IdSrc, Frame.Cmd, Frame.Trid,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, Rpl, 12,
                                 PRIORITY_TIMESYNC);
                DispatchPacket(TxFrame);
            }
            break;
        }

        // Enter bootloader (Device service CID 0x0004, "Bootloader"): the inverse of the
        // bootloader's own jump-to-app (0b1010). 0x0004 is free after Discover/Ping/Identify/
        // TimeSync.
        //
        // SIZE-GATED (2026-11-15): this is compiled only for a device that defines
        // DEVICE_HAS_BOOTLOADER_ENTRY. The DAS app is the only device with a bootloader to return
        // to, and its code space is nearly full - [0x800, 0x3E00) is 13824 B. The handler below
        // costs 16 B of .text in the DAS build (measured on a clean build; the ack's own LTO clone
        // of SendDeviceReply plus the call), which is paid for by the storage diet in
        // Core/Functions/StorageBlockFS.h: the app image ends at 0x3DF8, 8 B below the .fixed_data
        // reservation at 0x3E00. The earlier latch-and-main-loop shape cost 24-28 B (the
        // per-iteration call to the entry makes the main loop's register allocation pay), so the
        // jump stays on this path, right after the ack.
#ifdef DEVICE_HAS_BOOTLOADER_ENTRY
        case 4:
        {
            // ADDRESSED like every other Device request, so exactly one node leaves the app and
            // the rest of the bus keeps running.
            //
            // The reply goes out FIRST and it is a COMPLETE send, not a queued hand-off:
            // SendDeviceReply -> DispatchPacket -> the device's bus send (SendAndVerifyPacket on
            // the DAS) blocks until the frame's TC flag, drops the RS485 direction pin back to RX
            // and consumes its own echo. When the call below is reached the ack is on the wire and
            // the bus is idle, which is what makes this the safe point to leave the app from -
            // there is no half-written frame to lose and nothing in the app's own RAM is needed
            // afterwards (the bootloader re-initialises its statics and its UART).
            SendDeviceReply(Frame, TxFrame, nullptr, 0);
            DeviceEnterBootloaderJump(); // does not return
            break;
        }
#endif

        case 0x10: // Core discover per docs 00.10
        {
#ifdef TYPE_CORE
            // Core discover reply: SN(14) + uptime(4) to the requesting core (3F.1 is the
            // broadcast target). The SNDB is not involved; the responder only reports uptime.
            uint32_t Uptime = TimeFromBoot();
            uint8_t Rpl[14+4];
            memcpy(Rpl, &GetSerialNumber(), 14);
            memcpy(Rpl+14, &Uptime, 4);
            SendDeviceReply(Frame, TxFrame, Rpl, 18);
#endif
            break;
        }

#ifdef TYPE_CORE
        case 0x11: // SNDB Read per docs 00.11
        case 0x12: // SNDB Write per docs 00.12
        case 0x13: // SNDB Read All per docs 00.13
        {
            HandleSNDB(Frame);
            break;
        }

        case 0x20: // Bootloader passthrough: identify
        case 0x21: // Bootloader passthrough: read
        case 0x22: // Bootloader passthrough: erase one page
        case 0x23: // Bootloader passthrough: write
        case 0x24: // Bootloader passthrough: CRC of the whole app space
        case 0x25: // Bootloader passthrough: jump to the app (the node acks, then leaves)
        {
            HandleBootloaderPassthrough(Frame, Cid);
            break;
        }
#endif

        default:
            break;
    }
    return true;
}
