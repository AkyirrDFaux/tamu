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
void SubscriptionsRequestReRegister(uint16_t addr);
#endif
#include "Core/Functions/TimeSync.h"
#endif

// Device service (Docs/Services/Device Commands.md): Discover, Ping, Identify, Type, SN,
// Version, Capability, Name, Uptime, Time sync/offset, SNDB and bootloader passthrough.
class DeviceService
{
public:
    void Init();                           // resets the service-owned state (none to date)
    bool Handle(const PacketFrame &Frame); // CID dispatch; true if consumed

private:
    // Dispatches SNDB requests: Read by ID/SN (0x11), Write (0x12) and Read All (0x13)
    // (Docs/Command ID table.md 0x0011-0x0013).
    void HandleSNDB(const PacketFrame &Frame);

    // Bootloader passthrough (Docs/Services/Bootloader.md "Core bootloader passthrough
    // commands", CIDs 0x20/0x21).
    void HandleBootloaderPassthrough(const PacketFrame &Frame, uint8_t Cid);

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
            for (uint16_t f = 0; f < TotalFrags; f++)
            {
                uint16_t Off = 4;
                while (Off - 4 < MAX_FRAG_CONTENT_SIZE && Sent < Count && SNDB::IterNext(Entry))
                {
                    uint16_t WireId = RegistryAddress(Entry);
                    memcpy(TxFrame.Payload + Off, Entry.uid.Bytes, 14);
                    memcpy(TxFrame.Payload + Off + 14, &WireId, 2);
                    Off += 16;
                    Sent++;
                }
                SendFragFragment(Frame, f, TotalFrags, (uint16_t)(Off - 4));
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
                memcpy(Payload, Entry.uid.Bytes, 14);
                memcpy(Payload + 14, &WireId, 2);
                PacketConstruct(&TxFrame, Frame.IdSrc, Frame.Cmd, Frame.Trid,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, Payload, 16);
                DispatchPacket(TxFrame);
            }
            else
            {
                // Send an empty response to indicate not found
                DeviceLog("DEVICE", "SNDB lookup miss (id or sn)");
                PacketConstruct(&TxFrame, Frame.IdSrc, Frame.Cmd, Frame.Trid,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
                DispatchPacket(TxFrame);
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
                PacketConstruct(&TxFrame, Frame.IdSrc, Frame.Cmd, Frame.Trid,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP,
                                 Success ? Frame.Payload : nullptr, Success ? 16 : 0);
                DispatchPacket(TxFrame);
            }
            break;
        }

        default:
            break;
    }
}

// The app sends these to the connected core; the core relays the raw frame onto its RSBus.
// The frame carries no address, so this is a broadcast: every node currently in bootloader
// mode on that bus receives it. A write has no acknowledgement from the node - the app
// paces and verifies with a read.
inline void DeviceService::HandleBootloaderPassthrough(const PacketFrame &Frame, uint8_t Cid)
{
    if (PayloadBytes(Frame) < 4)
        return;

    uint32_t Offset = *reinterpret_cast<const uint32_t *>(Frame.Payload);

    if (Cid == 0x20)
    { // Send write packet: Offset, Payload (32)
        if (PayloadBytes(Frame) < 4 + Bootloader::PAYLOAD_SIZE)
            return;
        uint8_t Raw[Bootloader::DATA_SIZE];
        Bootloader::EncodeWrite(Offset, Frame.Payload + 4, Raw);
        bool Ok = RS485_SendRaw(Raw, Bootloader::DATA_SIZE);
        PacketConstruct(&TxFrame, Frame.IdSrc, Frame.Cmd, Frame.Trid,
                         FLAG_TYPE | FLAG_START | FLAG_STOP | (Ok ? FLAG_SUCCESS : FLAG_FAIL),
                         nullptr, 0);
        DispatchPacket(TxFrame);
        return;
    }

    // cid == 0x21: Request read -> Offset, Payload. The bootloader replies after the standard
    // CSMA silence window, but a request can still be lost, so retry a few times; the app
    // retries again if all fail.
    for (int Attempt = 0; Attempt < 3; Attempt++)
    {
        uint8_t Req[Bootloader::READ_REQ_SIZE];
        Bootloader::EncodeReadRequest(Offset, Req);
        if (!RS485_SendRaw(Req, Bootloader::READ_REQ_SIZE))
            continue;
        uint8_t Resp[Bootloader::MAX_FRAME_SIZE];
        int N = RS485_ReceiveRaw(Resp, sizeof(Resp), 200);
        if (N == Bootloader::DATA_SIZE &&
            Bootloader::Decode(Resp, (uint16_t)N) == Bootloader::CMD_READ_RESP)
        {
            uint8_t Out[4 + Bootloader::PAYLOAD_SIZE];
            memcpy(Out, &Offset, 4);
            memcpy(Out + 4, Bootloader::Payload(Resp), Bootloader::PAYLOAD_SIZE);
            PacketConstruct(&TxFrame, Frame.IdSrc, Frame.Cmd, Frame.Trid,
                             FLAG_TYPE | FLAG_START | FLAG_STOP | FLAG_SUCCESS,
                             Out, sizeof(Out));
            DispatchPacket(TxFrame);
            return;
        }
    }
    PacketConstruct(&TxFrame, Frame.IdSrc, Frame.Cmd, Frame.Trid,
                     FLAG_TYPE | FLAG_START | FLAG_STOP | FLAG_FAIL, nullptr, 0);
    DispatchPacket(TxFrame);
}
#endif // TYPE_CORE

// Handles Device service requests (Discover, Ping, Identify, Type, SN, Version, Capability,
// Name, Uptime, Time sync/offset, SNDB).
bool DeviceService::Handle(const PacketFrame &Frame)
{
    uint8_t Cid = GetServiceCID(Frame.Cmd);
    bool IsResponse = (Frame.Flags & FLAG_TYPE);

    if (IsResponse)
    {
        // The System/Log TRID is an incrementing counter, so a reply no longer carries the
        // request's CID; tell the replies apart by payload length. Only device-originated
        // requests' replies reach here - Core-discover = SN(14)+uptime(4) (18 B), Discover
        // = SN(14)+addr(2) (16 B), TimeSync = the exact 12-byte triple; app-originated
        // replies route to the app by their TRID instead.
        if (PayloadBytes(Frame) >= 18) // Core-discover response
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
        else if (PayloadBytes(Frame) == sizeof(AssignPayload)) // Discover (assign) response
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
            if (Assign->sn == GetSerialNumber())
            {
                // The core sends the net-qualified address; store it and adopt its net so
                // local net-0 targets resolve correctly.
                DeviceStatus.ShortAddress = Assign->NewAddr;
                DeviceStatus.NetId = (uint8_t)((Assign->NewAddr >> 10) & 0x3F);
            }
        }
        else if (PayloadBytes(Frame) == 12) // Time sync response per docs 00.03 (exact triple)
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
            if (!(kCapabilities & Capabilities::Core) || !DeviceIsCore())
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

            // Reply with SN(14) + assigned address(2). The request/reply flag and the
            // 16-byte payload length tell it apart from the other Device-service replies.
            // The reply echoes the request's TRID (its high byte is the Device service),
            // per "responses echo the request's TRID".
            AssignPayload ResponseData;
            ResponseData.sn = *IncomingSn;
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
                             sizeof(ResponseData));

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

        case 0x10: // Core discover per docs 00.10
        {
#ifdef TYPE_CORE
            // Core discover reply: SN(14) + uptime(4) to the requesting core (3F.1 is the
            // broadcast target). The SNDB is not involved; the responder only reports uptime.
            uint32_t Uptime = TimeFromBoot();
            uint8_t Rpl[14+4];
            memcpy(Rpl, &GetSerialNumber(), 14);
            memcpy(Rpl+14, &Uptime, 4);
            PacketConstruct(&TxFrame, Frame.IdSrc, Frame.Cmd, Frame.Trid,
                             FLAG_TYPE | FLAG_START | FLAG_STOP, Rpl, 18);
            DispatchPacket(TxFrame);
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

        case 0x20: // Bootloader passthrough: send write packet
        case 0x21: // Bootloader passthrough: request read
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

inline void DeviceService::Init()
{
}
