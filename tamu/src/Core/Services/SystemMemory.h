#pragma once

#include "Core/Functions/Memory.h"

// System Memory service (static/compiled-in blocks, block numbers local to this service):
// Read (2), Write (3), Backup (4), Save (5), Recall (6).

// Number of writable (non-ReadOnly) fields in a block's schema (shared helper).
static uint16_t CountWritableFields(const BlockSchema *schema)
{
    uint16_t count = 0;
    for (uint16_t i = 0; i < schema->MapCount; i++)
        if (!(schema->Map[i].FlagsAndType & FieldFlags::ReadOnly))
            count++;
    return count;
}

// True if the block has at least one writable (non-ReadOnly) field.
static bool SystemBlockWritable(const StaticBlockDescriptor &block)
{
    return CountWritableFields(block.Schema) > 0;
}

// Encoded Storage file name holding this service's backup registry.
static const char *SystemBackupName()
{
    static constexpr char name[8] = {'S', 'Y', 'S', 'M', 'E', 'M', ' ', ' '};
    return name;
}

// Serialises the writable (non-ReadOnly) fields of every static block into `out`.
// Read-only entries are never saved (Docs/Services/System Memory.md).
// Returns the byte count (0 on overflow).
static uint16_t SerializeSystemBlocks(uint8_t *out, uint16_t cap)
{
    uint16_t cursor = 0;
    uint16_t writable_blocks = 0;
    for (uint16_t block_index = 0; block_index < static_block_num; block_index++)
        if (SystemBlockWritable(static_block_registry[block_index]))
            writable_blocks++;
    if (cursor + 2 > cap) return 0;
    memcpy(out + cursor, &writable_blocks, 2); cursor += 2;

    for (uint16_t block_index = 0; block_index < static_block_num; block_index++)
    {
        const StaticBlockDescriptor &block = static_block_registry[block_index];
        uint16_t writable_count = CountWritableFields(block.Schema);
        if (writable_count == 0) continue;

        if (cursor + 2 + 2 > cap) return 0;
        memcpy(out + cursor, &block_index, 2); cursor += 2;
        memcpy(out + cursor, &writable_count, 2); cursor += 2;

        for (uint16_t i = 0; i < block.Schema->MapCount; i++)
        {
            if (block.Schema->Map[i].FlagsAndType & FieldFlags::ReadOnly)
                continue;
            FieldResult field_result = block.Get(i);
            uint16_t vlen = block.Schema->Map[i].Size;
            if (cursor + 2 + 2 + vlen > cap) return 0;
            memcpy(out + cursor, &i, 2); cursor += 2;
            memcpy(out + cursor, &vlen, 2); cursor += 2;
            if (vlen && field_result.Data)
            {
                memcpy(out + cursor, field_result.Data, vlen);
                cursor += vlen;
            }
        }
    }
    return cursor;
}

// Restores the writable (non-ReadOnly) fields from a serialised buffer. Read-only entries
// keep their current RAM values. When `target_block` is not INVALID_BLOCK only that block's
// fields are restored (per-entry Recall).
static bool DeserializeSystemBlocks(const uint8_t *in, uint16_t len, uint16_t target_block = INVALID_BLOCK)
{
    uint16_t cursor = 0;
    if (len < 2) return false;
    uint16_t wblocks;
    memcpy(&wblocks, in + cursor, 2); cursor += 2;
    for (uint16_t w = 0; w < wblocks; w++)
    {
        if (cursor + 4 > len) return false;
        uint16_t block_index, writable_count;
        memcpy(&block_index, in + cursor, 2); cursor += 2;
        memcpy(&writable_count, in + cursor, 2); cursor += 2;
        if (block_index >= static_block_num) return false;
        const StaticBlockDescriptor &block = static_block_registry[block_index];
        bool apply = (target_block == INVALID_BLOCK) || (target_block == block_index);
        for (uint16_t f = 0; f < writable_count; f++)
        {
            if (cursor + 4 > len) return false;
            uint16_t field_index, vlen;
            memcpy(&field_index, in + cursor, 2); cursor += 2;
            memcpy(&vlen, in + cursor, 2); cursor += 2;
            if (cursor + vlen > len) return false;
            if (apply && field_index < block.Schema->MapCount &&
                !(block.Schema->Map[field_index].FlagsAndType & FieldFlags::ReadOnly) &&
                vlen == block.Schema->Map[field_index].Size)
            {
                FieldResult field_result = block.Get(field_index);
                if (field_result.Data) memcpy(field_result.Data, in + cursor, vlen);
            }
            cursor += vlen;
        }
    }
    return true;
}

// Serialises the writable (non-ReadOnly) fields of a single static block into `out`.
// Returns the byte count (0 on overflow or if the block has no writable fields).
static uint16_t SerializeSystemBlock(const StaticBlockDescriptor &block, uint16_t block_index,
                                     uint8_t *out, uint16_t cap)
{
    uint16_t cursor = 0;
    uint16_t writable_count = CountWritableFields(block.Schema);
    if (writable_count == 0) return 0;

    if (cursor + 2 + 2 > cap) return 0;
    memcpy(out + cursor, &block_index, 2); cursor += 2;
    memcpy(out + cursor, &writable_count, 2); cursor += 2;

    for (uint16_t i = 0; i < block.Schema->MapCount; i++)
    {
        if (block.Schema->Map[i].FlagsAndType & FieldFlags::ReadOnly)
            continue;
        FieldResult field_result = block.Get(i);
        uint16_t vlen = block.Schema->Map[i].Size;
        if (cursor + 2 + 2 + vlen > cap) return 0;
        memcpy(out + cursor, &i, 2); cursor += 2;
        memcpy(out + cursor, &vlen, 2); cursor += 2;
        if (vlen && field_result.Data) { memcpy(out + cursor, field_result.Data, vlen); cursor += vlen; }
    }
    return cursor;
}

// Builds the response payload for a System Memory backup-read request by parsing the backup
// file buffer directly (no heap). Returns the payload length (0 if the requested entry was
// not found).
static uint16_t SystemBackupPayload(const BlockIndex *idx, const uint8_t *buffer, uint16_t len,
                                    uint8_t *out, uint16_t cap)
{
    if (len < 2) return 0;
    uint16_t cursor = 2;
    uint16_t total_blocks;
    memcpy(&total_blocks, buffer, 2);

    if (idx->Block == INVALID_BLOCK) // summary: number of stored blocks
    {
        if (sizeof(BlockIndex) + 1 > cap) return 0;
        BlockIndex out_index = {INVALID_BLOCK, INVALID_INDEX, INVALID_INDEX};
        memcpy(out, &out_index, sizeof(BlockIndex));
        out[sizeof(BlockIndex)] = (uint8_t)total_blocks;
        return sizeof(BlockIndex) + 1;
    }

    for (uint16_t w = 0; w < total_blocks; w++)
    {
        if (cursor + 4 > len) return 0;
        uint16_t block_index, writable_count;
        memcpy(&block_index, buffer + cursor, 2); cursor += 2;
        memcpy(&writable_count, buffer + cursor, 2); cursor += 2;
        if (block_index >= static_block_num) return 0;
        const StaticBlockDescriptor &block = static_block_registry[block_index];

        if (block_index != idx->Block)
        {
            for (uint16_t f = 0; f < writable_count; f++) // skip this block's stored fields
            {
                if (cursor + 4 > len) return 0;
                uint16_t vlen;
                memcpy(&vlen, buffer + cursor + 2, 2);
                if (cursor + 4 + vlen > len) return 0;
                cursor += 4 + vlen;
            }
            continue;
        }

        if (idx->Field == INVALID_INDEX) // block meta + stored field count
        {
            if (sizeof(BlockIndex) + sizeof(BlockMeta) > cap) return 0;
            BlockIndex out_index = {(uint8_t)block_index, INVALID_INDEX, INVALID_INDEX};
            memcpy(out, &out_index, sizeof(BlockIndex));
            BlockMeta meta; meta.FlagsAndType = (uint16_t)block.Schema->Type;
            meta.Key = INVALID_INDEX; meta.Size = (uint8_t)writable_count;
            memcpy(out + sizeof(BlockIndex), &meta, sizeof(BlockMeta));
            return sizeof(BlockIndex) + sizeof(BlockMeta);
        }

        for (uint16_t f = 0; f < writable_count; f++)
        {
            if (cursor + 4 > len) return 0;
            uint16_t field_index, vlen;
            memcpy(&field_index, buffer + cursor, 2); cursor += 2;
            memcpy(&vlen, buffer + cursor, 2); cursor += 2;
            if (cursor + vlen > len) return 0;
            const uint8_t *value = buffer + cursor; cursor += vlen;

            if (field_index == idx->Field && field_index < block.Schema->MapCount)
            {
                if (sizeof(BlockIndex) + sizeof(BlockMeta) + vlen > cap) return 0;
                BlockIndex out_index = {(uint8_t)block_index, (uint8_t)field_index, INVALID_INDEX};
                memcpy(out, &out_index, sizeof(BlockIndex));
                memcpy(out + sizeof(BlockIndex), &block.Schema->Map[field_index], sizeof(BlockMeta));
                if (vlen) memcpy(out + sizeof(BlockIndex) + sizeof(BlockMeta), value, vlen);
                return sizeof(BlockIndex) + sizeof(BlockMeta) + vlen;
            }
        }
        return 0; // field not stored in the backup
    }
    return 0; // block not stored in the backup
}

// Writes the current writable fields of one static block into the backup file, keeping every
// other stored block untouched (per-entry Save).
//
// With large backup caps (core default 2048) the merge buffers are static instead of stack
// locals: 2 x MEMORY_BACKUP_CAP on the handler stack would be fatal. Small-cap node builds
// (DAS, 64) keep them on the stack - they are cheap there and do not eat scarce static RAM.
static bool SaveSystemBlockToFile(const StaticBlockDescriptor &block, uint16_t block_index,
                                  const char *fname)
{
#if MEMORY_BACKUP_CAP > 128
    static uint8_t file_buf[MEMORY_BACKUP_CAP];
    static uint8_t out[MEMORY_BACKUP_CAP];
#else
    uint8_t file_buf[MEMORY_BACKUP_CAP];
    uint8_t out[MEMORY_BACKUP_CAP];
#endif
    uint16_t count = ReadBackupFile(fname, file_buf, sizeof(file_buf));
    uint16_t op = 2; // block-count placeholder, written last
    uint16_t stored_blocks = 0;
    bool replaced = false;

    uint16_t cursor = 2;
    while (cursor + 4 <= count)
    {
        uint16_t record_index, writable_count;
        memcpy(&record_index, file_buf + cursor, 2);
        memcpy(&writable_count, file_buf + cursor + 2, 2);
        uint16_t rec_start = cursor;
        cursor += 4;
        for (uint16_t f = 0; f < writable_count; f++) // walk stored fields to find the record length
        {
            if (cursor + 4 > count) return false;
            uint16_t vlen;
            memcpy(&vlen, file_buf + cursor + 2, 2);
            if (cursor + 4 + vlen > count) return false;
            cursor += 4 + vlen;
        }
        uint16_t rec_len = cursor - rec_start;

        if (record_index == block_index)
        {
            uint16_t blen = SerializeSystemBlock(block, block_index, out + op, sizeof(out) - op);
            if (blen == 0) return false;
            op += blen;
            replaced = true;
        }
        else
        {
            if (op + rec_len > sizeof(out)) return false;
            memcpy(out + op, file_buf + rec_start, rec_len); op += rec_len;
        }
        stored_blocks++;
    }

    if (!replaced)
    {
        uint16_t blen = SerializeSystemBlock(block, block_index, out + op, sizeof(out) - op);
        if (blen == 0) return false;
        op += blen;
        stored_blocks++;
    }

    memcpy(out, &stored_blocks, 2);
    return WriteBackupFile(fname, out, op);
}

void HandleSystemMemory(const PacketFrame &frame)
{
    uint8_t cid = GetServiceCID(frame.srv_tgt);
    if (frame.flags & FLAG_TYPE) return; // Ignore responses

    if (PayloadBytes(frame) < sizeof(BlockIndex)) { DeviceLog("SYSMEM", "short payload (%u B)", (unsigned)PayloadBytes(frame)); return; }
    const BlockIndex *idx = reinterpret_cast<const BlockIndex *>(frame.payload);

    // Single scratch buffer shared by every reply-building case. Memory replies are small
    // (BlockIndex + BlockMeta + one field value / block name), so this is a fixed 64 bytes
    // rather than MAX_PAYLOAD_SIZE - a full-size buffer would stack-overflow the DAS (whose
    // deepest handler also carries SendResponse's 288-byte reply frame).
    uint8_t payload[64];

    switch (cid)
    {
    case 2: // Read
    {
        if (idx->Block == INVALID_BLOCK) // summary: count of blocks
        {
            BlockIndex out_index = {INVALID_BLOCK, INVALID_INDEX, INVALID_INDEX};
            memcpy(payload, &out_index, sizeof(BlockIndex));
            payload[sizeof(BlockIndex)] = (uint8_t)static_block_num;
            SendResponse(frame, payload, sizeof(BlockIndex) + 1);
            break;
        }
        if (idx->Block >= static_block_num) { RespondStatus(frame, false); break; }
        const StaticBlockDescriptor &block = static_block_registry[idx->Block];
        if (idx->Field == INVALID_INDEX) // block meta + name
        {
            uint16_t plen = MakeBlockMetaPayload(idx->Block, (uint16_t)block.Schema->Type,
                                                 block.Schema->MapCount, block.Name,
                                                 (uint8_t)strlen(block.Name),
                                                 payload, sizeof(payload));
            if (plen == 0) { RespondStatus(frame, false); break; }
            SendResponse(frame, payload, plen);
            break;
        }
        FieldResult field_result = block.Get(idx->Field);
        if (!field_result.Data) { RespondStatus(frame, false); break; }
        uint16_t cursor = 0;
        memcpy(payload + cursor, idx, sizeof(BlockIndex)); cursor += sizeof(BlockIndex);
        memcpy(payload + cursor, &field_result.Descriptor, sizeof(BlockMeta)); cursor += sizeof(BlockMeta);
        uint16_t size = field_result.Descriptor.Size;
        if (cursor + size > sizeof(payload)) size = sizeof(payload) - cursor;
        memcpy(payload + cursor, field_result.Data, size); cursor += size;
        SendResponse(frame, payload, cursor);
        break;
    }
    case 3: // Write
    {
        if (PayloadBytes(frame) < sizeof(BlockIndex) + sizeof(BlockMeta)) { RespondStatus(frame, false); break; }
        if (idx->Block >= static_block_num) { RespondStatus(frame, false); break; }
        const BlockMeta *desc = reinterpret_cast<const BlockMeta *>(frame.payload + sizeof(BlockIndex));
        const uint8_t *value = frame.payload + sizeof(BlockIndex) + sizeof(BlockMeta);
        // The real value length is carried by the descriptor's Size field; the wire
        // payload is padded to 4 bytes, so never derive lengths from payload_len.
        uint16_t value_len = desc->Size;
        uint16_t avail = PayloadBytes(frame) - sizeof(BlockIndex) - sizeof(BlockMeta);
        if (value_len > avail) value_len = avail;

        const StaticBlockDescriptor &block = static_block_registry[idx->Block];
        if (!block.Set(idx->Field, value, value_len, desc->FlagsAndType))
        {
            RespondStatus(frame, false);
            break;
        }
        // Success echo: the request payload already IS BlockIndex + BlockMeta + value,
        // so reply with it verbatim instead of re-assembling a copy.
        SendResponse(frame, frame.payload, PayloadBytes(frame));
        break;
    }
    case 4: // Read backup (writable fields as stored in the backup file)
    {
        uint8_t buffer[MEMORY_BACKUP_CAP];
        uint16_t count = ReadBackupFile(SystemBackupName(), buffer, sizeof(buffer));
        if (count == 0) { RespondStatus(frame, false); break; }
        uint16_t plen = SystemBackupPayload(idx, buffer, count, payload, sizeof(payload));
        if (plen == 0) { RespondStatus(frame, false); break; }
        SendResponse(frame, payload, plen);
        break;
    }
    case 5: // Save a block (or everything when the block is invalid) to its backup file
    {
        if (idx->Block == INVALID_BLOCK)
        {
            uint8_t buffer[MEMORY_BACKUP_CAP];
            uint16_t count = SerializeSystemBlocks(buffer, sizeof(buffer));
            RespondStatus(frame, count > 0 && WriteBackupFile(SystemBackupName(), buffer, count));
        }
        else if (idx->Block < static_block_num)
        {
            RespondStatus(frame, SaveSystemBlockToFile(static_block_registry[idx->Block], idx->Block, SystemBackupName()));
        }
        else
        {
            RespondStatus(frame, false);
        }
        break;
    }
    case 6: // Recall a block (or everything when the block is invalid) from its backup file
    {
        if (idx->Block != INVALID_BLOCK && idx->Block >= static_block_num)
        {
            RespondStatus(frame, false);
            break;
        }
        uint8_t buffer[MEMORY_BACKUP_CAP];
        uint16_t count = ReadBackupFile(SystemBackupName(), buffer, sizeof(buffer));
        RespondStatus(frame, count > 0 && DeserializeSystemBlocks(buffer, count, idx->Block));
        break;
    }
    default:
        break;
    }
}

