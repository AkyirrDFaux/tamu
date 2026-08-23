#pragma once

#include "Core/Functions/Packet.h"
#include "Core/Functions/Device.h"
#include "Core/Functions/SysFunctions.h"
#include "Core/Functions/Log.h"

#ifdef TYPE_CORE
#include "Core/Functions/SNDB.h"
#include "Core/Functions/TimeSync.h"
#endif

// Sends a single-packet Device service reply back to the requester (src mirrored).
// `reply` must be a PacketFrame provided by the caller: allocating one here would nest a
// second full frame (~270 B) on top of the caller's own frame on the small node stack.
static inline void SendDeviceReply(const PacketFrame &frame, PacketFrame &reply,
                                   const void *payload, uint8_t len)
{
    PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                     FLAG_TYPE | FLAG_START | FLAG_STOP,
                     (const uint8_t *)payload, len);
    DispatchPacket(reply);
}

#ifdef TYPE_CORE
// Dispatches SNDB requests: Read All (12), Read by ID/SN (13) and Write (14).
void HandleSNDB(const PacketFrame &frame)
{
    uint8_t cid = GetServiceCID(frame.srv_tgt);
    bool is_response = (frame.flags & FLAG_TYPE);

    if (is_response) return; // Core only handles requests

    PacketFrame response;

    switch (cid) {
        case 12: { // SNDB Read All
            // Count the entries IterNext will actually yield so the stream reliably
            // terminates with STOP even if ActiveCount() ever desyncs from the scan.
            SNDB::IterReset();
            RegistryEntry entry;
            int32_t count = 0;
            while (SNDB::IterNext(entry)) count++;

            if (count == 0) {
                // Send an empty stop packet
                PacketConstruct(&response, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
                DispatchPacket(response);
                break;
            }

            SNDB::IterReset();
            int32_t sent = 0;
            while (SNDB::IterNext(entry)) {
                sent++;
                uint8_t flags = FLAG_TYPE;
                if (sent == 1) flags |= FLAG_START;
                if (sent == count) flags |= FLAG_STOP;

                // Payload is SN (14 bytes) + ID (2 bytes) = 16 bytes
                uint8_t payload[16];
                memcpy(payload, entry.uid.bytes, 14);
                memcpy(payload + 14, &entry.shortID, 2);

                PacketConstruct(&response, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 flags, payload, 16);
                response.frag_id = NextFragmentId(response.flags);
                DispatchPacket(response);
            }
            break;
        }

        case 13: { // SNDB Read (by ID or SN)
            RegistryEntry entry;
            bool found = false;

            if (frame.payload_len == 2) {
                uint16_t lookup_id = *reinterpret_cast<const uint16_t *>(frame.payload);
                found = SNDB::GetEntry(lookup_id, entry);
            } else if (frame.payload_len == 14) {
                const SerialNumber *lookup_sn = reinterpret_cast<const SerialNumber *>(frame.payload);
                uint16_t lookup_id = SNDB::FindShortID(*lookup_sn);
                if (lookup_id != ADDR_INVALID) {
                    found = SNDB::GetEntry(lookup_id, entry);
                }
            }

            if (found) {
                uint8_t payload[16];
                memcpy(payload, entry.uid.bytes, 14);
                memcpy(payload + 14, &entry.shortID, 2);
                PacketConstruct(&response, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, payload, 16);
                DispatchPacket(response);
            } else {
                // Send an empty response to indicate not found
                PacketConstruct(&response, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
                DispatchPacket(response);
            }
            break;
        }

        case 14: { // SNDB Write (SN + ID)
            if (frame.payload_len >= 16) {
                const SerialNumber *write_sn = reinterpret_cast<const SerialNumber *>(frame.payload);
                uint16_t write_id = *reinterpret_cast<const uint16_t *>(frame.payload + 14);

                bool success = SNDB::AddDevice(*write_sn, write_id);
                // Always acknowledge (empty frame = failure, like the CID 13 not-found case).
                PacketConstruct(&response, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP,
                                 success ? frame.payload : nullptr, success ? 16 : 0);
                DispatchPacket(response);
            }
            break;
        }

        default:
            break;
    }
}
#endif // TYPE_CORE

// Handles Device service requests (Discover, Ping, Type, SN, Version, Capability, Name, Uptime, Time sync/offset, SNDB).
void HandleDeviceService(const PacketFrame &frame)
{
    uint8_t cid = GetServiceCID(frame.srv_tgt);
    bool is_response = (frame.flags & FLAG_TYPE);

    if (is_response)
    {
        if (cid == 0) // Discover response
        {
#ifdef TYPE_CORE
            if (DeviceStatus.ShortAddress == 1) // Is core
                return;
#endif
            if (frame.payload_len >= sizeof(AssignPayload))
            {
                const AssignPayload *assign = reinterpret_cast<const AssignPayload *>(frame.payload);

                if (assign->sn == GetSerialNumber())
                {
                    DeviceStatus.ShortAddress = assign->new_addr;
                }
            }
        }
        else if (cid == 10) // Time sync response -> core averages samples and pushes the offset to the node
        {
#ifdef TYPE_CORE
            if (frame.payload_len >= 12)
            {
                uint32_t t0, t1, t2;
                memcpy(&t0, frame.payload, 4);
                memcpy(&t1, frame.payload + 4, 4);
                memcpy(&t2, frame.payload + 8, 4);
                uint32_t t3 = DeviceStatus.UptimeMs;

                // Standard NTP-style offset: ((t1 - t0) + (t2 - t3)) / 2
                int32_t offset = (int32_t)(((int64_t)(t1 - t0) + (int64_t)(t2 - t3)) / 2);

                TimeSync.HandleResponse(frame.id_src, offset);
            }
#endif
        }
        return;
    }

    // Requests:
    PacketFrame reply;
    switch (cid)
    {
        case 0: // Discover
        {
#ifdef TYPE_CORE
            // Per the docs, ID assignment is a *core capability*: only answer when we
            // report the CORE bit and hold a valid short address.
            if (!(kCapabilities & Capabilities::Core) || DeviceStatus.ShortAddress != 1)
                return;

            if (frame.payload_len < sizeof(SerialNumber))
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

            AssignPayload response_data;
            response_data.sn = *incoming_sn;
            response_data.new_addr = NewAddr;

            PacketConstruct(&reply, ADDR_BROADCAST,
                             MakeService(ServiceType::Device, 0),
                             MakeService(ServiceType::Device, 0),
                             FLAG_TYPE | FLAG_START | FLAG_STOP,
                             (uint8_t *)&response_data, sizeof(AssignPayload));

            DispatchPacket(reply);
#endif
            break;
        }

        case 1: // Ping
            SendDeviceReply(frame, reply, nullptr, 0);
            break;

        case 2: // Device type
        {
            uint16_t dev_type = (uint16_t)kDeviceType;
            SendDeviceReply(frame, reply, &dev_type, sizeof(dev_type));
            break;
        }

        case 3: // Serial number
            SendDeviceReply(frame, reply, &GetSerialNumber(), sizeof(SerialNumber));
            break;

        case 4: // Software version
            SendDeviceReply(frame, reply, DeviceVersion, (uint8_t)strlen(DeviceVersion));
            break;

        case 5: // Capability
        {
            uint32_t cap = kCapabilities;
            SendDeviceReply(frame, reply, &cap, sizeof(cap));
            break;
        }

        case 6: // Read Name
            SendDeviceReply(frame, reply, DeviceName, (uint8_t)strlen(DeviceName));
            break;

        case 7: // Set Name (respond only if requested)
        {
            if (frame.payload_len > 0)
            {
                uint16_t len = (frame.payload_len > 23) ? 23 : frame.payload_len;
                memcpy(DeviceNameBuffer, frame.payload, len);
                DeviceNameBuffer[len] = '\0';
            }
            if (frame.flags & FLAG_REQACK)
                SendDeviceReply(frame, reply, DeviceName, (uint8_t)strlen(DeviceName));
            break;
        }

        case 8: // Uptime
        {
            uint32_t uptime = DeviceStatus.UptimeMs;
            SendDeviceReply(frame, reply, &uptime, sizeof(uptime));
            break;
        }

        case 9: // Loop Time: average + maximum loop time (2x Number)
        {
            Number loop[2] = {DeviceStatus.AvgLoopTimeMs, DeviceStatus.MaxLoopTimeMs};
            SendDeviceReply(frame, reply, loop, sizeof(loop));
            break;
        }

        case 10: // Time sync: reply with { time sent, local time received, local time reply sent }
        {
            if (frame.payload_len >= 4)
            {
                uint32_t time_sent = *reinterpret_cast<const uint32_t *>(frame.payload);
                uint32_t t1 = DeviceStatus.UptimeMs; // local time the request was received
                uint32_t reply_payload[3] = { time_sent, t1, 0 };

                PacketConstruct(&reply, frame.id_src,
                                 frame.srv_src,
                                 frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP,
                                 (const uint8_t *)reply_payload, sizeof(reply_payload));

                // Sample t2 as close to the actual send as possible, patched after construction.
                uint32_t t2 = DeviceStatus.UptimeMs;
                memcpy(reply.payload + 8, &t2, sizeof(t2));
                reply.crc8 = Crc8(&reply.flags, 11 + reply.payload_len);

                DispatchPacket(reply);
            }
            break;
        }

        case 11: // Set time offset (core -> node: int32 offset ms)
        {
            if (frame.payload_len >= 4)
            {
                int32_t theta;
                memcpy(&theta, frame.payload, sizeof(theta));
                // The core pushes the NTP-style offset theta = node_time - core_time
                // measured on the node's *displayed* time (which already includes the old
                // offset). The correction therefore accumulates: subtracting theta from
                // the current offset converges to zero error; assigning -theta would keep
                // the previous round's residual alive forever.
                TimeOffsetMs -= theta;
            }
            break;
        }

#ifdef TYPE_CORE
        case 12: // SNDB Read All
        case 13: // SNDB Read
        case 14: // SNDB Write
        {
            HandleSNDB(frame);
            break;
        }
#endif

        default:
            break;
    }
}

