#pragma once

#include "Core/Functions/Device.h"
#include "Core/Functions/Storage.h"

// Defined in Block.h (included after this file)
void PrintField(const BlockMeta &desc, const void *data_ptr, uint16_t index, bool is_key);

// Set to true by every CLI response handler when a reply arrives. Command functions clear
// it right before dispatching and wait briefly afterwards, so a missing reply (dead
// address, service not compiled into the node) produces a timeout error instead of
// silence.
bool g_cli_response_seen = false;

// Response handler: receives memory read responses from the network and prints them
void HandleCLIService(const PacketFrame &frame)
{
    if (!(frame.flags & FLAG_TYPE))
        return; // Only handle responses
    g_cli_response_seen = true;

    // A single status byte (padded to 4 on the wire) means the operation failed
    // (RespondStatus); it is not a BlockIndex payload, so print it directly instead
    // of misparsing it as a read reply.
    if (PayloadBytes(frame) == 4)
    {
        printf("Device %d: Operation FAILED (status %d)\n", frame.id_src, frame.payload[0]);
        return;
    }
    if (PayloadBytes(frame) < sizeof(BlockIndex))
        return;

    if (frame.flags & FLAG_START)
        printf("--- Beginning Topology Dump from Device %d ---\n", frame.id_src);

    const BlockIndex *idx = reinterpret_cast<const BlockIndex *>(frame.payload);
    const uint8_t *data_ptr = frame.payload + sizeof(BlockIndex);

    if (idx->Block == INVALID_BLOCK)
    {
        if (PayloadBytes(frame) < sizeof(BlockIndex) + 1)
            return; // truncated summary reply
        const char *svc_name = "?";
        switch (GetServiceType(frame.srv_src))
        {
            case ServiceType::SystemMemory:  svc_name = "System";  break;
            case ServiceType::DynamicMemory: svc_name = "Dynamic"; break;
            case ServiceType::KeyedMemory:   svc_name = "Keyed";   break;
            default: break;
        }
        printf("Registry Summary [%s]: %d blocks.\n", svc_name, data_ptr[0]);
    }
    else if (idx->Field != INVALID_INDEX)
    {
        if (PayloadBytes(frame) < sizeof(BlockIndex) + sizeof(BlockMeta))
            return; // truncated field reply: avoid underflowing the length math below
        const BlockMeta *desc = reinterpret_cast<const BlockMeta *>(data_ptr);
        const void *data = (const void *)(data_ptr + sizeof(BlockMeta));
        // Clamp the descriptor size to the bytes actually present so a truncated
        // reply cannot make the printer read past the payload.
        BlockMeta clamped = *desc;
        uint16_t avail = PayloadBytes(frame) - sizeof(BlockIndex) - sizeof(BlockMeta);
        if (clamped.Size > avail) clamped.Size = avail;

        if (idx->Key != INVALID_INDEX)
        {
            PrintField(clamped, data, idx->Key, true);
        }
        else if (IsKeyedType((DataType)BlockMetaType(desc->FlagsAndType)))
        {
            uint16_t type_id = BlockMetaType(desc->FlagsAndType);
            printf("  |-- Field [%02d]: (Keyed Field, Type: 0x%03X)\n", idx->Field, type_id);
            const uint8_t *keys = static_cast<const uint8_t *>(data);
            uint16_t key_count = clamped.Size;
            if (key_count > avail)
                key_count = avail;
            if (key_count > 0)
            {
                printf("       Keys:");
                for (uint16_t i = 0; i < key_count; i++)
                    printf(" %d", keys[i]);
                printf("\n");
            }
        }
        else
        {
            PrintField(clamped, data, idx->Field, false);
        }
    }
    else
    {
        if (PayloadBytes(frame) < sizeof(BlockIndex) + sizeof(BlockMeta))
            return; // truncated meta reply
        const BlockMeta *meta = reinterpret_cast<const BlockMeta *>(data_ptr);
        printf("Block [%02d] | Type: 0x%04X | Size: %d\n",
               idx->Block, BlockMetaType(meta->FlagsAndType), meta->Size);
    }

    if (frame.flags & FLAG_STOP)
        printf("--- Topology Dump Complete ---\n");
}

// Response handler: prints LogHandler service replies (GetLogs record stream from a
// remote device, ClearReadLogs status).
void HandleCLI_LogResponse(const PacketFrame &frame)
{
    if (!(frame.flags & FLAG_TYPE))
        return; // Only handle responses
    g_cli_response_seen = true;

    if (PayloadBytes(frame) == 0)
    {
        if (frame.flags & FLAG_START)
            printf("No logs on device %d.\n", frame.id_src);
        else
            printf("--- Log Dump Complete ---\n");
        return;
    }

    if (PayloadBytes(frame) == 4)
    {
        printf("Device %d: Log clear %s (status %d)\n",
               frame.id_src, frame.payload[0] == 0 ? "OK" : "FAILED", frame.payload[0]);
        return;
    }

    // GetLogs streams LogRecords as FRAG packets: the first 4 payload bytes are the
    // fragmentation info, followed by one or more 12-byte records.
    const uint8_t *data = frame.payload;
    uint16_t avail = PayloadBytes(frame);
    if (frame.flags & FLAG_FRAG)
    {
        if (avail <= 4) return;
        data += 4;
        avail -= 4;
    }

    if (avail >= sizeof(LogRecord) && (frame.flags & FLAG_START))
        printf("--- Logs from Device %d ---\n", frame.id_src);

    while (avail >= sizeof(LogRecord))
    {
        const LogRecord *r = reinterpret_cast<const LogRecord *>(data);
        const LogMessage &m = r->msg;
        printf("Dev %d | %s | Src 0x%04X | Code 0x%04X | Count %u | t=%lums\n",
               r->device_id, LogIsBlock(m) ? "Block" : "Svc",
               LogSourceId(m), LogCode(m), r->count, (unsigned long)m.timestamp);
        data += sizeof(LogRecord);
        avail -= sizeof(LogRecord);
    }
}

// Response handler: prints Script service replies (Docs/Services/Script.md).
void HandleCLI_ScriptResponse(const PacketFrame &frame)
{
    if (!(frame.flags & FLAG_TYPE))
        return; // Only handle responses
    g_cli_response_seen = true;

    uint8_t cid = GetServiceCID(frame.srv_src);

    switch (cid)
    {
        case 0: // Get number of scripts
            if (PayloadBytes(frame) >= 1)
                printf("Device %d: %u script(s)\n", frame.id_src, frame.payload[0]);
            break;
        case 1: // Read Name
            printf("Device %d: Script name \"%.*s\"\n", frame.id_src, (int)PayloadBytes(frame),
                   (const char *)frame.payload);
            break;
        case 3: // Read state
            if (PayloadBytes(frame) >= 1)
            {
                static const char *states[] = {"Stopped", "Running", "Paused", "Waiting",
                                               "Finished", "Error"};
                uint8_t s = frame.payload[0];
                printf("Device %d: State %s\n", frame.id_src,
                       s < 6 ? states[s] : "?");
            }
            break;
        case 4: // Set state
        case 10:
        case 12:
        case 14:
            if (PayloadBytes(frame) >= 1)
                printf("Device %d: Script op %s (status %d)\n", frame.id_src,
                       frame.payload[0] == 0 ? "OK" : "FAILED", frame.payload[0]);
            break;
        case 13: // Create script -> assigned id
            if (PayloadBytes(frame) >= 1 && frame.payload[0] != 0)
                printf("Device %d: Script %u created\n", frame.id_src, frame.payload[0]);
            else
                printf("Device %d: Script create FAILED\n", frame.id_src);
            break;
        default:
            printf("Device %d: Unknown Script response CID %u\n", frame.id_src, cid);
            break;
    }
}

// Response handler: prints a single-byte status (Save/Recall/Delete results)
void HandleCLI_StatusResponse(const PacketFrame &frame)
{
    if (!(frame.flags & FLAG_TYPE))
        return; // Only handle responses
    g_cli_response_seen = true;

    if (PayloadBytes(frame) >= 1)
    {
        uint8_t status = frame.payload[0];
        if (status == 0)
            printf("Device %d: Operation OK\n", frame.id_src);
        else
            printf("Device %d: Operation FAILED (status %d)\n", frame.id_src, status);
    }
    else
    {
        printf("Device %d: Empty status response\n", frame.id_src);
    }
}

// Response handler: prints a Create response (BlockIndex + BlockMeta + Value)
void HandleCLI_CreateResponse(const PacketFrame &frame)
{
    if (!(frame.flags & FLAG_TYPE))
        return; // Only handle responses
    g_cli_response_seen = true;

    if (PayloadBytes(frame) < sizeof(BlockIndex) + sizeof(BlockMeta))
    {
        printf("Device %d: Invalid create response\n", frame.id_src);
        return;
    }

    const BlockIndex *idx = reinterpret_cast<const BlockIndex *>(frame.payload);
    const BlockMeta *desc = reinterpret_cast<const BlockMeta *>(frame.payload + sizeof(BlockIndex));
    const uint8_t *value = frame.payload + sizeof(BlockIndex) + sizeof(BlockMeta);
    uint16_t value_len = desc->Size;
    uint16_t avail = PayloadBytes(frame) - sizeof(BlockIndex) - sizeof(BlockMeta);
    if (value_len > avail)
        value_len = avail;

    if (idx->Block == INVALID_BLOCK)
    {
        printf("Device %d: Create FAILED\n", frame.id_src);
        return;
    }

    printf("Device %d: Block %d created | Type 0x%03X | Name \"%.*s\"\n",
           frame.id_src, idx->Block, BlockMetaType(desc->FlagsAndType),
           value_len, (const char *)value);
}

// Response handler: prints Device service replies (the command is identified by
// the CID of the response's SRV SRC).
void HandleCLI_DeviceResponse(const PacketFrame &frame)
{
    if (!(frame.flags & FLAG_TYPE))
        return; // Only handle responses
    g_cli_response_seen = true;

    uint8_t cid = GetServiceCID(frame.srv_src);

    switch (cid)
    {
        case 0: // Discover response (SN + ID)
            if (PayloadBytes(frame) >= sizeof(AssignPayload))
            {
                const AssignPayload *p = reinterpret_cast<const AssignPayload *>(frame.payload);
                char sn_str[29];
                SerialNumberToString(p->sn, sn_str, sizeof(sn_str));
                printf("Device %d: Discover -> SN %s, ID 0x%04X\n", frame.id_src, sn_str, p->new_addr);
            }
            break;

        case 1: // Ping
            printf("Device %d: PONG\n", frame.id_src);
            break;

        case 2: // Identify
            printf("Device %d: Identify sent\n", frame.id_src);
            break;

        case 3: // Type
            if (PayloadBytes(frame) >= 2)
            {
                uint16_t dev_type = *reinterpret_cast<const uint16_t *>(frame.payload);
                printf("Device %d: Type 0x%04X\n", frame.id_src, dev_type);
            }
            break;

        case 4: // Serial number
            if (PayloadBytes(frame) >= 14)
            {
                char sn_str[29] = {0};
                SerialNumberToString(*reinterpret_cast<const SerialNumber *>(frame.payload), sn_str, sizeof(sn_str));
                printf("Device %d: SN %s\n", frame.id_src, sn_str);
            }
            break;

        case 5: // Version
            printf("Device %d: Version %.*s\n", frame.id_src, (int)PayloadBytes(frame), (const char *)frame.payload);
            break;

        case 6: // Capability
            if (PayloadBytes(frame) >= 4)
            {
                uint32_t cap = *reinterpret_cast<const uint32_t *>(frame.payload);
                printf("Device %d: Capability 0x%08lX\n", frame.id_src, (unsigned long)cap);
            }
            break;

        case 7: // Read Name
            printf("Device %d: Name \"%.*s\"\n", frame.id_src, (int)PayloadBytes(frame), (const char *)frame.payload);
            break;

        case 8: // Set Name
            printf("Device %d: Name set to \"%.*s\"\n", frame.id_src, (int)PayloadBytes(frame), (const char *)frame.payload);
            break;

        case 9: // Uptime
            if (PayloadBytes(frame) >= 4)
            {
                uint32_t uptime = *reinterpret_cast<const uint32_t *>(frame.payload);
                printf("Device %d: Uptime %lu ms\n", frame.id_src, (unsigned long)uptime);
            }
            break;

        case 10: // Loop Time (avg, max as 2x Number)
            if (PayloadBytes(frame) >= 2 * sizeof(Number))
            {
                const Number *lp = reinterpret_cast<const Number *>(frame.payload);
                printf("Device %d: Loop avg %.2f ms, max %.2f ms\n",
                       frame.id_src, NumberToFloat(lp[0]), NumberToFloat(lp[1]));
            }
            break;

        case 11: // Time sync (t0,t1,t2) -> report round-trip and offset estimate
            if (PayloadBytes(frame) >= 12)
            {
                uint32_t t0, t1, t2;
                memcpy(&t0, frame.payload, 4);
                memcpy(&t1, frame.payload + 4, 4);
                memcpy(&t2, frame.payload + 8, 4);
                uint32_t t3 = DeviceStatus.UptimeMs; // local time the reply was received
                // Counters wrap at 2^32 (~49.7 days); take signed deltas BEFORE widening so a
                // wrap is interpreted as a small negative interval, not a huge positive one.
                int32_t offset = (int32_t)(((int64_t)(int32_t)(t1 - t0) + (int64_t)(int32_t)(t2 - t3)) / 2);
                printf("Device %d: Time sync t0=%lu t1=%lu t2=%lu (est. offset %ld ms)\n",
                       frame.id_src, (unsigned long)t0, (unsigned long)t1, (unsigned long)t2, (long)offset);
            }
            break;

        default:
            printf("Device %d: Unknown Device response CID %u\n", frame.id_src, cid);
            break;
    }
}

// Response handler: prints Storage service replies (file table stream, file data
// stream or single-packet create/resize/delete results).
void HandleCLI_StorageResponse(const PacketFrame &frame)
{
    if (!(frame.flags & FLAG_TYPE))
        return; // Only handle responses
    g_cli_response_seen = true;

    uint8_t cid = GetServiceCID(frame.srv_src);

    switch (cid)
    {
        case 0: // File table stream (FRAG packets: info (4) + one or more entries)
            if (frame.flags & FLAG_START)
                printf("--- File Table from Device %d ---\n", frame.id_src);

            if (PayloadBytes(frame) == 0)
                printf("Device %d: No files stored.\n", frame.id_src);

            {
                const uint8_t *data = frame.payload;
                uint16_t avail = PayloadBytes(frame);
                if (frame.flags & FLAG_FRAG)
                {
                    if (avail <= 4) break;
                    data += 4;
                    avail -= 4;
                }
                while (avail >= sizeof(FileEntry))
                {
                    const FileEntry *e = reinterpret_cast<const FileEntry *>(data);
                    char name[9] = {0};
                    memcpy(name, e->name, 8);
                    printf("File \"%s\" | Offset %lu | Size %lu\n",
                           name, (unsigned long)e->offset, (unsigned long)e->size);
                    data += sizeof(FileEntry);
                    avail -= sizeof(FileEntry);
                }
            }

            if (frame.flags & FLAG_STOP)
                printf("--- File Table Complete ---\n");
            break;

        case 1: // Format filesystem
            printf("Device %d: Filesystem formatted\n", frame.id_src);
            break;

        case 6: // File data stream (Read File, Docs/Services/Storage.md CID 6)
            if (frame.flags & FLAG_START)
                printf("--- File Data from Device %d ---\n", frame.id_src);
            {
                // FRAG packets: info (4), plus the echoed name on fragment 0.
                const uint8_t *data = frame.payload;
                uint16_t avail = PayloadBytes(frame);
                if (frame.flags & FLAG_FRAG)
                {
                    if (avail <= 4) break;
                    data += 4;
                    avail -= 4;
                    if (frame.payload[0] == 0 && frame.payload[1] == 0 && avail >= 8)
                    {
                        // fragment 0: skip the echoed 8-byte name
                        data += 8;
                        avail -= 8;
                    }
                }
                for (uint16_t i = 0; i < avail; i++)
                    printf("%02X ", data[i]);
                printf("\n");
            }
            if (frame.flags & FLAG_STOP)
                printf("--- File Data Complete ---\n");
            break;

        case 2: // Create file
            if (PayloadBytes(frame) >= 1 && frame.payload[0] != 0)
                printf("Device %d: File created\n", frame.id_src);
            else
                printf("Device %d: File create failed\n", frame.id_src);
            break;

        case 3: // Delete file
            printf("Device %d: File deleted\n", frame.id_src);
            break;

        case 4: // Resize file
            if (PayloadBytes(frame) >= 1 && frame.payload[0] != 0)
                printf("Device %d: File resized\n", frame.id_src);
            else
                printf("Device %d: File resize failed\n", frame.id_src);
            break;

        case 5: // Rename file (status byte)
            if (PayloadBytes(frame) >= 1 && frame.payload[0] != 0)
                printf("Device %d: File renamed\n", frame.id_src);
            else
                printf("Device %d: File rename failed\n", frame.id_src);
            break;

        default:
            printf("Device %d: Unknown Storage response CID %u\n", frame.id_src, cid);
            break;
    }
}
