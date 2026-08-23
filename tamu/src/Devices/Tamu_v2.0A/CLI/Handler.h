#pragma once

#include "Core/Functions/Device.h"
#include "Core/Functions/Storage.h"

// Defined in Block.h (included after this file)
void PrintField(const BlockMeta &desc, const void *data_ptr, uint16_t index, bool is_key);

// Response handler: receives memory read responses from the network and prints them
void HandleCLIService(const PacketFrame &frame)
{
    if (!(frame.flags & FLAG_TYPE))
        return; // Only handle responses

    // A single status byte means the operation failed (RespondStatus); it is not a
    // BlockIndex payload, so print it directly instead of misparsing it as a read reply.
    if (frame.payload_len == 1)
    {
        printf("Device %d: Operation FAILED (status %d)\n", frame.id_src, frame.payload[0]);
        return;
    }
    if (frame.payload_len < sizeof(BlockIndex))
        return;

    if (frame.flags & FLAG_START)
        printf("--- Beginning Topology Dump from Device %d ---\n", frame.id_src);

    const BlockIndex *idx = reinterpret_cast<const BlockIndex *>(frame.payload);
    const uint8_t *data_ptr = frame.payload + sizeof(BlockIndex);

    if (idx->Block == INVALID_BLOCK)
    {
        if (frame.payload_len < sizeof(BlockIndex) + 1)
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
        if (frame.payload_len < sizeof(BlockIndex) + sizeof(BlockMeta))
            return; // truncated field reply: avoid underflowing the length math below
        const BlockMeta *desc = reinterpret_cast<const BlockMeta *>(data_ptr);
        const void *data = (const void *)(data_ptr + sizeof(BlockMeta));

        if (idx->Key != INVALID_INDEX)
        {
            PrintField(*desc, data, idx->Key, true);
        }
        else if (IsKeyedType((DataType)BlockMetaType(desc->FlagsAndType)))
        {
            uint16_t type_id = BlockMetaType(desc->FlagsAndType);
            printf("  |-- Field [%02d]: (Keyed Field, Type: 0x%03X)\n", idx->Field, type_id);
            const uint8_t *keys = static_cast<const uint8_t *>(data);
            uint16_t key_count = desc->Size;
            if (key_count > frame.payload_len - sizeof(BlockIndex) - sizeof(BlockMeta))
                key_count = frame.payload_len - sizeof(BlockIndex) - sizeof(BlockMeta);
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
            PrintField(*desc, data, idx->Field, false);
        }
    }
    else
    {
        if (frame.payload_len < sizeof(BlockIndex) + sizeof(BlockMeta))
            return; // truncated meta reply
        const BlockMeta *meta = reinterpret_cast<const BlockMeta *>(data_ptr);
        printf("Block [%02d] | Type: 0x%04X | Size: %d\n",
               idx->Block, BlockMetaType(meta->FlagsAndType), meta->Size);
    }

    if (frame.flags & FLAG_STOP)
        printf("--- Topology Dump Complete ---\n");
}

// Response handler: prints a single-byte status (Save/Recall/Delete results)
void HandleCLI_StatusResponse(const PacketFrame &frame)
{
    if (!(frame.flags & FLAG_TYPE))
        return; // Only handle responses

    if (frame.payload_len >= 1)
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

    if (frame.payload_len < sizeof(BlockIndex) + sizeof(BlockMeta))
    {
        printf("Device %d: Invalid create response\n", frame.id_src);
        return;
    }

    const BlockIndex *idx = reinterpret_cast<const BlockIndex *>(frame.payload);
    const BlockMeta *desc = reinterpret_cast<const BlockMeta *>(frame.payload + sizeof(BlockIndex));
    const uint8_t *value = frame.payload + sizeof(BlockIndex) + sizeof(BlockMeta);
    uint16_t value_len = desc->Size;
    if (value_len > frame.payload_len - sizeof(BlockIndex) - sizeof(BlockMeta))
        value_len = frame.payload_len - sizeof(BlockIndex) - sizeof(BlockMeta);

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

    uint8_t cid = GetServiceCID(frame.srv_src);

    switch (cid)
    {
        case 0: // Discover response (SN + ID)
            if (frame.payload_len >= sizeof(AssignPayload))
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

        case 2: // Type
            if (frame.payload_len >= 2)
            {
                uint16_t dev_type = *reinterpret_cast<const uint16_t *>(frame.payload);
                printf("Device %d: Type 0x%04X\n", frame.id_src, dev_type);
            }
            break;

        case 3: // Serial number
            if (frame.payload_len >= 14)
            {
                char sn_str[29] = {0};
                SerialNumberToString(*reinterpret_cast<const SerialNumber *>(frame.payload), sn_str, sizeof(sn_str));
                printf("Device %d: SN %s\n", frame.id_src, sn_str);
            }
            break;

        case 4: // Version
            printf("Device %d: Version %.*s\n", frame.id_src, frame.payload_len, (const char *)frame.payload);
            break;

        case 5: // Capability
            if (frame.payload_len >= 4)
            {
                uint32_t cap = *reinterpret_cast<const uint32_t *>(frame.payload);
                printf("Device %d: Capability 0x%08lX\n", frame.id_src, (unsigned long)cap);
            }
            break;

        case 6: // Read Name
            printf("Device %d: Name \"%.*s\"\n", frame.id_src, frame.payload_len, (const char *)frame.payload);
            break;

        case 7: // Set Name
            printf("Device %d: Name set to \"%.*s\"\n", frame.id_src, frame.payload_len, (const char *)frame.payload);
            break;

        case 8: // Uptime
            if (frame.payload_len >= 4)
            {
                uint32_t uptime = *reinterpret_cast<const uint32_t *>(frame.payload);
                printf("Device %d: Uptime %lu ms\n", frame.id_src, (unsigned long)uptime);
            }
            break;

        case 9: // Loop Time (avg, max as 2x Number)
            if (frame.payload_len >= 2 * sizeof(Number))
            {
                const Number *lp = reinterpret_cast<const Number *>(frame.payload);
                printf("Device %d: Loop avg %.2f ms, max %.2f ms\n",
                       frame.id_src, NumberToFloat(lp[0]), NumberToFloat(lp[1]));
            }
            break;

        case 10: // Time sync (t0,t1,t2) -> report round-trip and offset estimate
            if (frame.payload_len >= 12)
            {
                uint32_t t0, t1, t2;
                memcpy(&t0, frame.payload, 4);
                memcpy(&t1, frame.payload + 4, 4);
                memcpy(&t2, frame.payload + 8, 4);
                uint32_t t3 = DeviceStatus.UptimeMs; // local time the reply was received
                int32_t offset = (int32_t)(((int64_t)(t1 - t0) + (int64_t)(t2 - t3)) / 2);
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

    uint8_t cid = GetServiceCID(frame.srv_src);

    switch (cid)
    {
        case 0: // File table stream
            if (frame.flags & FLAG_START)
                printf("--- File Table from Device %d ---\n", frame.id_src);

            if (frame.payload_len >= sizeof(FileEntry))
            {
                const FileEntry *e = reinterpret_cast<const FileEntry *>(frame.payload);
                char name[9] = {0};
                memcpy(name, e->name, 8);
                printf("File \"%s\" | Offset %lu | Size %lu\n",
                       name, (unsigned long)e->offset, (unsigned long)e->size);
            }
            else if (frame.payload_len == 0)
            {
                printf("Device %d: No files stored.\n", frame.id_src);
            }

            if (frame.flags & FLAG_STOP)
                printf("--- File Table Complete ---\n");
            break;

        case 1: // Format filesystem
            printf("Device %d: Filesystem formatted\n", frame.id_src);
            break;

        case 5: // File data stream
            if (frame.flags & FLAG_START)
                printf("--- File Data from Device %d ---\n", frame.id_src);
            for (uint8_t i = 0; i < frame.payload_len; i++)
                printf("%02X ", frame.payload[i]);
            printf("\n");
            if (frame.flags & FLAG_STOP)
                printf("--- File Data Complete ---\n");
            break;

        case 2: // Create file
            if (frame.payload_len >= 1 && frame.payload[0] != 0)
                printf("Device %d: File created\n", frame.id_src);
            else
                printf("Device %d: File create failed\n", frame.id_src);
            break;

        case 3: // Delete file
            printf("Device %d: File deleted\n", frame.id_src);
            break;

        case 4: // Resize file
            if (frame.payload_len >= 1 && frame.payload[0] != 0)
                printf("Device %d: File resized\n", frame.id_src);
            else
                printf("Device %d: File resize failed\n", frame.id_src);
            break;

        default:
            printf("Device %d: Unknown Storage response CID %u\n", frame.id_src, cid);
            break;
    }
}
