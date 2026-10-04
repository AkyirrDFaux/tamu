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

// Sends a single-packet Device service reply back to the requester (src mirrored).
// `reply` must be a PacketFrame provided by the caller: allocating one here would nest a
// second full frame (~270 B) on top of the caller's own frame on the small node stack.
static inline void SendDeviceReply(const PacketFrame &frame, PacketFrame &reply,
                                   const void *payload, uint8_t len)
{
    PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.trid,
                     FLAG_TYPE | FLAG_START | FLAG_STOP,
                     (const uint8_t *)payload, len);
    DispatchPacket(reply);
}

// Device reply kind tag. The System/Log TRID is an incrementing counter, so a Device
// reply no longer carries the request's CID and the handler has to tell its replies
// apart. Length alone is ambiguous (a 16-byte assign reply shares its length with an
// SNDB read reply), so the assign reply is prefixed with this tag. Discover/TimeSync
// keep their documented byte layout: the app parses the TimeSync reply directly and
// never sends Discover, and the assign reply here is firmware-internal (no app change).
#define DEVICE_REPLY_KIND_ASSIGN 0xD1

#ifdef TYPE_CORE
// Dispatches SNDB requests: Read by ID/SN (0x11), Write (0x12) and Read All (0x13)
// (Docs/Command ID table.md 0x0011-0x0013).
void HandleSNDB(const PacketFrame &frame)
{
    uint8_t cid = GetServiceCID(frame.srv_tgt);
    bool is_response = (frame.flags & FLAG_TYPE);

    if (is_response) return; // Core only handles requests

    switch (cid) {
        case 0x13: { // SNDB Read All per docs 00.13
            // The recovered registry tracks the valid-entry count, so one streaming scan
            // is enough (no separate counting pass over flash).
            SNDB::IterReset();
            uint32_t count = (uint32_t)SNDB::ActiveCount();

            if (count == 0) {
                SendResponse(frame, nullptr, 0); // empty stop packet
                break;
            }

            // Stream every entry as a FRAG stream (Docs/Services/System Block and Device Commands.md:
            // "Fragmentation, SN + ID stream"). Each fragment carries up to 112 bytes
            // of 16-byte (SN + ID) entries; SendFragFragment writes the 4-byte frag info.
            uint32_t total = count * 16;
            uint16_t total_frags = (uint16_t)((total + MAX_FRAG_CONTENT_SIZE - 1) / MAX_FRAG_CONTENT_SIZE);
            RegistryEntry entry;
            uint32_t sent = 0;
            for (uint16_t f = 0; f < total_frags; f++) {
                uint16_t off = 4;
                while (off - 4 < MAX_FRAG_CONTENT_SIZE && sent < count && SNDB::IterNext(entry)) {
                    uint16_t wire_id = entry.shortID;
                    memcpy(tx_frame.payload + off, entry.uid.bytes, 14);
                    memcpy(tx_frame.payload + off + 14, &wire_id, 2);
                    off += 16;
                    sent++;
                }
                SendFragFragment(frame, f, total_frags, (uint16_t)(off - 4));
            }
            break;
        }

        case 0x11: { // SNDB Read per docs 00.11
            RegistryEntry entry;
            bool found = false;

            // Disambiguate by length (Docs: "ID or SN (based on length)"): an ID request is a
            // 2-byte uint16, an SN request the 14-byte serial. Payloads are not padded.
            if (PayloadBytes(frame) == 2) {
                // The request carries a full net.device. Local node entries are stored
                // device-only, while the core's own entry is net-qualified, so try the
                // exact ID first and then the device field.
                uint16_t lookup_id = *reinterpret_cast<const uint16_t *>(frame.payload);
                found = SNDB::GetEntry(lookup_id, entry) ||
                        SNDB::GetEntry((uint16_t)(lookup_id & 0x3FF), entry);
            } else if (PayloadBytes(frame) >= 14) {
                const SerialNumber *lookup_sn = reinterpret_cast<const SerialNumber *>(frame.payload);
                uint16_t lookup_id = SNDB::FindShortID(*lookup_sn);
                if (lookup_id != ADDR_INVALID) {
                    found = SNDB::GetEntry(lookup_id, entry);
                }
            }

            if (found) {
                uint8_t payload[16];
                uint16_t wire_id = entry.shortID;
                memcpy(payload, entry.uid.bytes, 14);
                memcpy(payload + 14, &wire_id, 2);
                PacketConstruct(&tx_frame, frame.id_src, frame.srv_src, frame.trid,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, payload, 16);
                DispatchPacket(tx_frame);
            } else {
                // Send an empty response to indicate not found
                DeviceLog("DEVICE", "SNDB lookup miss (id or sn)");
                PacketConstruct(&tx_frame, frame.id_src, frame.srv_src, frame.trid,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
                DispatchPacket(tx_frame);
            }
            break;
        }

        case 0x12: { // SNDB Write per docs 00.12
            if (PayloadBytes(frame) >= 16) {
                const SerialNumber *write_sn = reinterpret_cast<const SerialNumber *>(frame.payload);
                // Stored/allocated IDs are device-only (0..1023): strip any net bits the
                // app may have sent so the registry stays uniform.
                uint16_t write_id = (uint16_t)(LoadUnaligned<uint16_t>(frame.payload + 14) & 0x3FF);

                // Per Docs/Services/System Block and Device Commands.md: ID 0 = delete the
                // entry carrying this serial number.
                bool success = (write_id != 0)
                                   ? SNDB::AddDevice(*write_sn, write_id)
                                   : SNDB::RemoveDevice(
                                         SNDB::FindShortID(*write_sn));
                if (!success) {
                    DeviceLog("DEVICE", "SNDB write id %u failed", (unsigned)write_id);
                }
                // Always acknowledge (empty frame = failure, like the CID 13 not-found case).
                PacketConstruct(&tx_frame, frame.id_src, frame.srv_src, frame.trid,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP,
                                 success ? frame.payload : nullptr, success ? 16 : 0);
                DispatchPacket(tx_frame);
            }
            break;
        }

        default:
            break;
    }
}

// Bootloader passthrough (Docs/Services/Bootloader.md "Core bootloader passthrough
// commands", CIDs 0x20/0x21). The app sends these to the connected core; the core relays the
// raw frame onto its RSBus. The frame carries no address, so this is a broadcast: every node
// currently in bootloader mode on that bus receives it. A write has no acknowledgement from
// the node - the app paces and verifies with a read.
static void HandleBootloaderPassthrough(const PacketFrame &frame, uint8_t cid)
{
    if (PayloadBytes(frame) < 4)
        return;

    uint32_t offset = *reinterpret_cast<const uint32_t *>(frame.payload);

    if (cid == 0x20) { // Send write packet: Offset, Payload (32)
        if (PayloadBytes(frame) < 4 + Bootloader::PAYLOAD_SIZE)
            return;
        uint8_t raw[Bootloader::DATA_SIZE];
        Bootloader::EncodeWrite(offset, frame.payload + 4, raw);
        bool ok = RS485_SendRaw(raw, Bootloader::DATA_SIZE);
        PacketConstruct(&tx_frame, frame.id_src, frame.srv_src, frame.trid,
                         FLAG_TYPE | FLAG_START | FLAG_STOP | (ok ? FLAG_SUCCESS : FLAG_FAIL),
                         nullptr, 0);
        DispatchPacket(tx_frame);
        return;
    }

    // cid == 0x21: Request read -> Offset, Payload. The bootloader replies after the standard
    // CSMA silence window, but a request can still be lost, so retry a few times; the app
    // retries again if all fail.
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t req[Bootloader::READ_REQ_SIZE];
        Bootloader::EncodeReadRequest(offset, req);
        if (!RS485_SendRaw(req, Bootloader::READ_REQ_SIZE))
            continue;
        uint8_t resp[Bootloader::MAX_FRAME_SIZE];
        int n = RS485_ReceiveRaw(resp, sizeof(resp), 200);
        if (n == Bootloader::DATA_SIZE &&
            Bootloader::Decode(resp, (uint16_t)n) == Bootloader::CMD_READ_RESP) {
            uint8_t out[4 + Bootloader::PAYLOAD_SIZE];
            memcpy(out, &offset, 4);
            memcpy(out + 4, Bootloader::Payload(resp), Bootloader::PAYLOAD_SIZE);
            PacketConstruct(&tx_frame, frame.id_src, frame.srv_src, frame.trid,
                             FLAG_TYPE | FLAG_START | FLAG_STOP | FLAG_SUCCESS,
                             out, sizeof(out));
            DispatchPacket(tx_frame);
            return;
        }
    }
    PacketConstruct(&tx_frame, frame.id_src, frame.srv_src, frame.trid,
                     FLAG_TYPE | FLAG_START | FLAG_STOP | FLAG_FAIL, nullptr, 0);
    DispatchPacket(tx_frame);
}
#endif // TYPE_CORE

// Handles Device service requests (Discover, Ping, Identify, Type, SN, Version, Capability, Name, Uptime, Time sync/offset, SNDB).
void HandleDeviceService(const PacketFrame &frame)
{
    uint8_t cid = GetServiceCID(frame.srv_tgt);
    bool is_response = (frame.flags & FLAG_TYPE);

    if (is_response)
    {
        // The System/Log TRID is an incrementing counter, so a reply no longer carries the
        // request's CID; tell the replies apart by their explicit kind. The assign reply is
        // tagged (DEVICE_REPLY_KIND_ASSIGN) because its length is otherwise ambiguous; the
        // discover reply is SN(14)+uptime(4) and the TimeSync reply the exact 12-byte triple.
        if (PayloadBytes(frame) >= 18) // Core-discover response
        {
#ifdef TYPE_CORE
            CoreTimeSync.HandleDiscoverResponse(frame);
#endif
        }
        else if (PayloadBytes(frame) >= 1 + sizeof(AssignPayload) &&
                 frame.payload[0] == DEVICE_REPLY_KIND_ASSIGN) // Discover (assign) response
        {
#ifdef TYPE_CORE
            // A core is never assigned. The reply's source is the assigning core: if it
            // carries our net (and is not our own broadcast looped back) the net is
            // claimed twice - the same collision the Core-discover path detects.
            if (frame.id_src != DeviceStatus.ShortAddress)
                NoteCoreNet((uint8_t)((frame.id_src >> 10) & 0x3F), DeviceStatus.NetId);
            if (DeviceIsCore()) return; // a core is never assigned
#endif
            const AssignPayload *assign = reinterpret_cast<const AssignPayload *>(frame.payload + 1);
            if (assign->sn == GetSerialNumber())
            {
                // The core sends the net-qualified address; store it and adopt its net so
                // local net-0 targets resolve correctly.
                DeviceStatus.ShortAddress = assign->new_addr;
                DeviceStatus.NetId = (uint8_t)((assign->new_addr >> 10) & 0x3F);
            }
        }
        else if (PayloadBytes(frame) == 12) // Time sync response per docs 00.03 (exact triple)
        {
            // TimeSync is synchronized-device initiated: the INITIATOR (a node syncing to
            // a core, or a core syncing to the longest-running core) applies the offset to
            // its OWN clock. The core never pushes offsets to other devices.
            //
            // NTP-like with SYNCHRONIZED timestamps (Now()): t0/t3 are the initiator's
            // clock including its current offset, so the computed offset is a DELTA to
            // add. Using raw TimeFromBoot() here would make it an absolute value and
            // `+=` would accumulate it on every sync.
            uint32_t t0, t1, t2;
            // 4-byte aligned offsets in a packed+aligned(4) frame: word loads.
            t0 = *reinterpret_cast<const uint32_t *>(frame.payload);
            t1 = *reinterpret_cast<const uint32_t *>(frame.payload + 4);
            t2 = *reinterpret_cast<const uint32_t *>(frame.payload + 8);
            uint32_t t3 = Now();
#ifdef TYPE_CORE
            int32_t offset = (int32_t)(((int64_t)(int32_t)(t1 - t0) + (int64_t)(int32_t)(t2 - t3)) / 2);
#else
            int32_t offset = ((int32_t)(t1 - t0) + (int32_t)(t2 - t3)) / 2;
#endif
            // Step the offset AND update the drift estimate (ApplyTimeSync) so the clock
            // tracks the peer's rate between syncs, not just its value at this instant.
            ApplyTimeSync(offset);
        }
        return;
    }

    // Requests:
    switch (cid)
    {
        case 0: // Discover
        {
#ifdef TYPE_CORE
            // Per the docs, ID assignment is a *core capability*: only answer when we
            // report the CORE bit and hold the core role (device ID 1 in a valid net).
            if (!(kCapabilities & Capabilities::Core) || !DeviceIsCore())
                return;

            if (PayloadBytes(frame) < sizeof(SerialNumber))
                return;

            char sn_str[29];
            const SerialNumber *incoming_sn = reinterpret_cast<const SerialNumber *>(frame.payload);
            SerialNumberToString(*incoming_sn, sn_str, sizeof(sn_str));

            uint16_t NewAddr = SNDB::FindShortID(*incoming_sn);

            if (NewAddr != ADDR_INVALID)
            {
                DeviceLog("CORE", "Re-discovered device: %s -> Assigned ID: %d", sn_str, NewAddr);
            }
            else
            {
                NewAddr = SNDB::NewDevice(*incoming_sn);
                if (NewAddr != ADDR_INVALID)
                {
                    DeviceLog("CORE", "Registered new device: %s -> Assigned ID: %d", sn_str, NewAddr);
                }
                else
                {
                    DeviceLog("CORE", "Registry full! Cannot register: %s", sn_str);
                    return;
                }
            }

            // Reply with the kind tag + SN(14) + assigned address(2). The tag keeps the
            // 16-byte body from being confused with an SNDB read reply (both would otherwise
            // be told apart by length alone). The reply echoes the request's TRID (its high
            // byte is the Device service), per "responses echo the request's TRID".
            uint8_t response[1 + sizeof(AssignPayload)];
            response[0] = DEVICE_REPLY_KIND_ASSIGN;
            AssignPayload response_data;
            response_data.sn = *incoming_sn;
            // SNDB stores/allocates device-only IDs (0..1023); the core's NetID is
            // automatically added here so the node's address is net-qualified.
            response_data.new_addr = MakeId(DeviceStatus.NetId, NewAddr);
            memcpy(response + 1, &response_data, sizeof(AssignPayload));

#ifdef USE_SUB_REQUEST
            // The node just (re-)appeared with an empty provider table, so ask for this
            // device's subscriptions that point at it to be re-pushed - otherwise a node reboot
            // silently ends them. Only *requested* here: this runs inside packet dispatch and
            // must not delay the assignment reply below. Provider addresses are wire
            // addresses (net-qualified), so queue the qualified value.
            SubscriptionsRequestReRegister(response_data.new_addr);
#endif

            PacketConstruct(&tx_frame, ADDR_BROADCAST,
                             frame.srv_src, // originator's service tag (== its echoed TRID)
                             frame.trid,    // echo the request TRID
                             FLAG_TYPE | FLAG_START | FLAG_STOP,
                             response, sizeof(response));

            DispatchPacket(tx_frame);

#endif
            break;
        }

        case 1: // Ping
            SendDeviceReply(frame, tx_frame, nullptr, 0);
            break;

        case 2: // Identify (Bool: blink the red LED fast while true)
        {
            DeviceIdentifyStart((PayloadBytes(frame) > 0 && frame.payload[0] != 0),
                                DeviceStatus.UptimeMs);
            SendDeviceReply(frame, tx_frame, nullptr, 0);
            break;
        }

        case 3: // Time sync per docs 00.03
        {
            if (PayloadBytes(frame) >= 4)
            {
                uint32_t time_sent = *reinterpret_cast<const uint32_t *>(frame.payload);
                // The responder reports its SYNCHRONIZED time (Now()), matching the
                // initiator's t0/t3 (see the response handler above).
                uint32_t t1 = Now();
                uint32_t t2 = Now();

                // Response: echoed requester time, responder receive time, responder send
                // time (NTP-like; see the offset math in the response handler above).
                uint8_t rpl[12];
                memcpy(rpl + 0, &time_sent, 4);
                memcpy(rpl + 4, &t1, 4);
                memcpy(rpl + 8, &t2, 4);
                PacketConstruct(&tx_frame, frame.id_src, frame.srv_src, frame.trid,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, rpl, 12,
                                 PRIORITY_TIMESYNC);
                DispatchPacket(tx_frame);
            }
            break;
        }

        case 0x10: // Core discover per docs 00.10
        {
#ifdef TYPE_CORE
            // Core discover reply: SN(14) + uptime(4) to the requesting core (3F.1 is the
            // broadcast target). The SNDB is not involved; the responder only reports uptime.
            uint32_t uptime = TimeFromBoot();
            uint8_t rpl[14+4];
            memcpy(rpl, &GetSerialNumber(), 14);
            memcpy(rpl+14, &uptime, 4);
            PacketConstruct(&tx_frame, frame.id_src, frame.srv_src, frame.trid,
                             FLAG_TYPE | FLAG_START | FLAG_STOP, rpl, 18);
            DispatchPacket(tx_frame);
#endif
            break;
        }

#ifdef TYPE_CORE
        case 0x11: // SNDB Read per docs 00.11
        case 0x12: // SNDB Write per docs 00.12
        case 0x13: // SNDB Read All per docs 00.13
        {
            HandleSNDB(frame);
            break;
        }

        case 0x20: // Bootloader passthrough: send write packet
        case 0x21: // Bootloader passthrough: request read
        {
            HandleBootloaderPassthrough(frame, cid);
            break;
        }
#endif

        default:
            break;
    }
}

