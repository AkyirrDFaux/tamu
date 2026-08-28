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
        case 13: { // SNDB Read All
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

            // Stream every entry as a FRAG stream (Docs/Services/Device service.md:
            // "Fragmentation, SN + ID stream"). Each fragment carries up to 256 bytes
            // of 16-byte (SN + ID) entries; the fragmentation info is the first 4
            // payload bytes (u16 current + u16 total fragments).
            SNDB::IterReset();
            uint32_t total = (uint32_t)count * 16;
            uint16_t total_frags = (uint16_t)((total + 255) / 256);
            uint8_t buf[MAX_PAYLOAD_SIZE];
            int32_t sent = 0;
            for (uint16_t f = 0; f < total_frags; f++) {
                uint8_t flags = FLAG_TYPE | FLAG_FRAG;
                if (f == 0) flags |= FLAG_START;
                WriteFragInfo(buf, f, total_frags);
                uint16_t off = 4;
                while (off - 4 < 256 && sent < count && SNDB::IterNext(entry)) {
                    memcpy(buf + off, entry.uid.bytes, 14);
                    memcpy(buf + off + 14, &entry.shortID, 2);
                    off += 16;
                    sent++;
                }
                if (sent >= count || f == total_frags - 1) flags |= FLAG_STOP;
                PacketConstruct(&response, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 flags, buf, off);
                DispatchPacket(response);
                if (sent >= count) break;
            }
            break;
        }

        case 14: { // SNDB Read (by ID or SN)
            RegistryEntry entry;
            bool found = false;

            // The wire pads payloads to 4 bytes: an ID request (2 B) arrives as 4,
            // an SN request (14 B) as 16. Disambiguate by the padded byte count.
            if (PayloadBytes(frame) == 4) {
                uint16_t lookup_id = *reinterpret_cast<const uint16_t *>(frame.payload);
                found = SNDB::GetEntry(lookup_id, entry);
            } else if (PayloadBytes(frame) == 16) {
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
                DeviceLog("DEVICE", "SNDB lookup miss (id or sn)");
                PacketConstruct(&response, frame.id_src, frame.srv_src, frame.srv_tgt,
                                 FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
                DispatchPacket(response);
            }
            break;
        }

        case 15: { // SNDB Write (SN + ID)
            if (PayloadBytes(frame) >= 16) {
                const SerialNumber *write_sn = reinterpret_cast<const SerialNumber *>(frame.payload);
                uint16_t write_id = *reinterpret_cast<const uint16_t *>(frame.payload + 14);

                // Per Docs/Services/Device service.md: ID 0 = delete the
                // entry carrying this serial number.
                bool success = (write_id != 0)
                                   ? SNDB::AddDevice(*write_sn, write_id)
                                   : SNDB::RemoveDevice(
                                         SNDB::FindShortID(*write_sn));
                if (!success) {
                    DeviceLog("DEVICE", "SNDB write id %u failed", (unsigned)write_id);
                }
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

// --- Device name persistence ---
// Docs/Services/Device service.md: "Device name is stored in standalone file to allow
// persistence." The name lives in a dedicated storage file ("DEVNAME ") updated
// NOR-safely (stage in a temp file, rename into place) so a power cut never leaves a
// torn name. Loading is idempotent and safe before storage is ready (FileExists
// reports not-found while the table is uninitialized).
#define DEVICE_NAME_FILE      "DEVNAME "
#define DEVICE_NAME_FILE_SIZE 24 // max name (23) + NUL

static bool s_device_name_loaded = false;

void LoadPersistedDeviceName()
{
    if (s_device_name_loaded) return;
    s_device_name_loaded = true;

    uint32_t sz = Storage.FileExists(DEVICE_NAME_FILE);
    if (sz == 0xFFFFFFFF || sz == 0) return; // none persisted: keep the built-in default
    uint8_t buf[DEVICE_NAME_FILE_SIZE];
    uint32_t n = Storage.ReadFromFile(DEVICE_NAME_FILE, 0, sizeof(buf), (char *)buf);
    if (n == 0 || buf[0] == '\0') return;
    uint32_t len = n < sizeof(DeviceNameBuffer) - 1 ? n : sizeof(DeviceNameBuffer) - 1;
    memcpy(DeviceNameBuffer, buf, len);
    DeviceNameBuffer[len] = '\0';
}

bool PersistDeviceName()
{
    uint8_t buf[DEVICE_NAME_FILE_SIZE] = {0};
    uint16_t len = (uint16_t)strlen(DeviceName);
    if (len >= DEVICE_NAME_FILE_SIZE) len = DEVICE_NAME_FILE_SIZE - 1;
    memcpy(buf, DeviceName, len);

    char tmp[8];
    memcpy(tmp, DEVICE_NAME_FILE, 8);
    tmp[7] = '~'; // staging name ("DEVNAME~")
    if (Storage.FileExists(tmp) != 0xFFFFFFFF)
        Storage.DeleteFile(tmp); // clear a stale staging file from an interrupted update
    if (!Storage.CreateFile(tmp, DEVICE_NAME_FILE_SIZE))
        return false;
    if (!Storage.WriteToFile(tmp, 0, DEVICE_NAME_FILE_SIZE, (const char *)buf))
    {
        Storage.DeleteFile(tmp);
        return false;
    }
    if (!Storage.RenameFile(tmp, DEVICE_NAME_FILE))
    {
        Storage.DeleteFile(tmp);
        return false;
    }
    return true;
}

// Handles Device service requests (Discover, Ping, Identify, Type, SN, Version, Capability, Name, Uptime, Time sync/offset, SNDB).
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
            if (PayloadBytes(frame) >= sizeof(AssignPayload))
            {
                const AssignPayload *assign = reinterpret_cast<const AssignPayload *>(frame.payload);

                if (assign->sn == GetSerialNumber())
                {
                    DeviceStatus.ShortAddress = assign->new_addr;
                }
            }
        }
        else if (cid == 11) // Time sync response -> core averages samples and pushes the offset to the node
        {
#ifdef TYPE_CORE
            if (PayloadBytes(frame) >= 12)
            {
                uint32_t t0, t1, t2;
                memcpy(&t0, frame.payload, 4);
                memcpy(&t1, frame.payload + 4, 4);
                memcpy(&t2, frame.payload + 8, 4);
                uint32_t t3 = DeviceStatus.UptimeMs;

                // Standard NTP-style offset: ((t1 - t0) + (t2 - t3)) / 2
                // Counters wrap at 2^32 (~49.7 days); take signed deltas BEFORE widening so a
                // wrap is interpreted as a small negative interval, not a huge positive one.
                int32_t offset = (int32_t)(((int64_t)(int32_t)(t1 - t0) + (int64_t)(int32_t)(t2 - t3)) / 2);

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

            AssignPayload response_data;
            response_data.sn = *incoming_sn;
            response_data.new_addr = NewAddr;

            PacketConstruct(&reply, ADDR_BROADCAST,
                             MakeService(ServiceType::Device, 0),
                             MakeService(ServiceType::Device, 0),
                             FLAG_TYPE | FLAG_START | FLAG_STOP,
                             (uint8_t *)&response_data, sizeof(AssignPayload));

            DispatchPacket(reply);

            // The CLI's `dev 1 discover` asks the CORE to register this SN and show
            // the assigned ID. The broadcast above is consumed by the target node,
            // and the core's own response handler drops it, so answer the CLI
            // directly (srv_tgt = CLI CID 3 -> HandleCLI_DeviceResponse).
            if (GetServiceType(frame.srv_src) == ServiceType::CLI)
            {
                PacketFrame cli_reply;
                PacketConstruct(&cli_reply, DeviceStatus.ShortAddress,
                                 MakeService(ServiceType::CLI, GetServiceCID(frame.srv_src)),
                                 MakeService(ServiceType::Device, 0),
                                 FLAG_TYPE | FLAG_START | FLAG_STOP,
                                 (uint8_t *)&response_data, sizeof(AssignPayload));
                cli_reply.id_src = NewAddr; // the CLI prints id_src as the device
                DispatchPacket(cli_reply);
            }
#endif
            break;
        }

        case 1: // Ping
            SendDeviceReply(frame, reply, nullptr, 0);
            break;

        case 2: // Identify (Bool: blink the red LED fast while true)
        {
            DeviceIdentifyStart((PayloadBytes(frame) > 0 && frame.payload[0] != 0),
                                DeviceStatus.UptimeMs);
            SendDeviceReply(frame, reply, nullptr, 0);
            break;
        }

        case 3: // Device type
        {
            uint16_t dev_type = (uint16_t)kDeviceType;
            SendDeviceReply(frame, reply, &dev_type, sizeof(dev_type));
            break;
        }

        case 4: // Serial number
            SendDeviceReply(frame, reply, &GetSerialNumber(), sizeof(SerialNumber));
            break;

        case 5: // Software version
            SendDeviceReply(frame, reply, DeviceVersion, (uint8_t)strlen(DeviceVersion));
            break;

        case 6: // Capability
        {
            uint32_t cap = kCapabilities;
            SendDeviceReply(frame, reply, &cap, sizeof(cap));
            break;
        }

        case 7: // Read Name
            LoadPersistedDeviceName();
            SendDeviceReply(frame, reply, DeviceName, (uint8_t)strlen(DeviceName));
            break;

        case 8: // Set Name (respond only if requested)
        {
            if (PayloadBytes(frame) > 0)
            {
                uint16_t len = (PayloadBytes(frame) > 23) ? 23 : PayloadBytes(frame);
                memcpy(DeviceNameBuffer, frame.payload, len);
                DeviceNameBuffer[len] = '\0';
            }
            // Persist the new name (standalone file) so it survives reboots.
            PersistDeviceName();
            if (frame.flags & FLAG_REQACK)
                SendDeviceReply(frame, reply, DeviceName, (uint8_t)strlen(DeviceName));
            break;
        }

        case 9: // Uptime: raw time since boot (Device service doc semantics), NOT the
                // synchronized clock that Now()/DeviceStatus.UptimeMs carry.
        {
            uint32_t uptime = TimeFromBoot();
            SendDeviceReply(frame, reply, &uptime, sizeof(uptime));
            break;
        }

        case 10: // Loop Time: average + maximum loop time (2x Number)
        {
            Number loop[2] = {DeviceStatus.AvgLoopTimeMs, DeviceStatus.MaxLoopTimeMs};
            SendDeviceReply(frame, reply, loop, sizeof(loop));
            break;
        }

        case 11: // Time sync: reply with { time sent, local time received, local time reply sent }
        {
            if (PayloadBytes(frame) >= 4)
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
                reply.crc8 = Crc8(&reply.flags, (uint16_t)(11 + PayloadBytes(reply)));

                DispatchPacket(reply);
            }
            break;
        }

        case 12: // Set time offset (core -> node: int32 offset ms)
        {
            if (PayloadBytes(frame) >= 4)
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
        case 13: // SNDB Read All
        case 14: // SNDB Read
        case 15: // SNDB Write
        {
            HandleSNDB(frame);
            break;
        }
#endif

        default:
            break;
    }
}

