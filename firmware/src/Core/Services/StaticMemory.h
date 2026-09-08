#pragma once

#include "Core/Functions/Memory.h"

static const char *StaticLogName()
{
    static constexpr char name[8] = {'S', 'T', 'A', 'T', 'L', 'O', 'G', ' '};
    return name;
}


struct CountBlocksContext { uint8_t *seen; uint16_t block_count; };
struct InvalidateContext {
    uint16_t start_block;
    uint16_t end_block;
    uint8_t *buf;
};
struct ReadFieldContext { uint8_t target_block; uint8_t target_field; uint8_t *value_size; uint16_t *value_cursor; };
struct SaveAppendContext {
    uint16_t start_block;
    uint16_t end_block;
    uint8_t *log_buf;
    uint16_t log_cap;
    uint16_t write_pos;
    bool ok;
};

// BlockLog: sequential log file for persistent field-level storage.
// Caller provides buffer, zero internal allocations.
// Entry format: BlockIndex[4B] + BlockMeta[4B] + Value[NB, 4B-padded]
// BlockIndex.block == 0xFF  => unwritten (end of log)
// BlockIndex.block == 0x00  => invalidated entry

static constexpr uint16_t kLogEntryHeaderSize = sizeof(BlockMeta) + sizeof(BlockIndex);

static inline uint16_t LogEntrySize(uint8_t value_size)
{
    return kLogEntryHeaderSize + ((value_size + 3) & ~3);
}

struct DefragCountContext { uint16_t valid; };
struct DefragCompactContext { uint8_t *buf; uint16_t write_pos; uint16_t new_size; };

// Walk the log and call callback for each valid entry.
// callback(cursor, block_id, field_id, value_size, entry_size, context)
// Returns bytes scanned (position of first unwritten/invalid byte).
static uint16_t LogWalk(const uint8_t *buf, uint16_t len,
                 void (*callback)(uint16_t, uint8_t, uint8_t, uint8_t, uint16_t, void *),
                 void *context)
{
    uint16_t cursor = 0;
    while (cursor + kLogEntryHeaderSize <= len)
    {
        uint8_t block_id = buf[cursor];
        if (block_id == 0xFF) break;
        uint8_t value_size = buf[cursor + sizeof(BlockIndex) + 1];
        uint16_t entry_size = LogEntrySize(value_size);
        if (entry_size > len - cursor) break;
        callback(cursor, block_id, buf[cursor + 1], value_size, entry_size, context);
        cursor += entry_size;
    }
    return cursor;
}

static void CountBlocksCallback(uint16_t, uint8_t block_id, uint8_t, uint8_t, uint16_t, void *ctx)
{
    CountBlocksContext *c = static_cast<CountBlocksContext *>(ctx);
    if (block_id != 0x00 && block_id < c->block_count) c->seen[block_id] = 1;
}

static void InvalidateCallback(uint16_t cursor, uint8_t block_id, uint8_t field_id, uint8_t, uint16_t, void *ctx)
{
    InvalidateContext *c = static_cast<InvalidateContext *>(ctx);
    if (block_id >= c->start_block && block_id < c->end_block && block_id < 255)
    {
        const StaticBlockDescriptor &blk = static_block_registry[block_id];
        if (field_id < blk.Schema->MapCount &&
            !(blk.Schema->Map[field_id].FlagsAndType & FieldFlags::ReadOnly) &&
            !(blk.Schema->Map[field_id].FlagsAndType & FieldFlags::NotSaved))
            c->buf[cursor] = 0x00;
    }
}

static void FindEndSimpleCallback(uint16_t cursor, uint8_t block_id, uint8_t, uint8_t, uint16_t entry_size, void *ctx)
{
    if (block_id != 0xFF && block_id != 0x00)
        *static_cast<uint16_t *>(ctx) = cursor + entry_size;
}

static void ReadFieldCallback(uint16_t cursor, uint8_t block_id, uint8_t field_id, uint8_t value_size, uint16_t, void *ctx)
{
    ReadFieldContext *c = static_cast<ReadFieldContext *>(ctx);
    if (block_id == c->target_block && field_id == c->target_field)
    {
        *c->value_size = value_size;
        *c->value_cursor = cursor;
    }
}

static void AppendFieldCallback(uint16_t, uint8_t block_id, uint8_t field_id, uint8_t, uint16_t, void *ctx)
{
    SaveAppendContext *c = static_cast<SaveAppendContext *>(ctx);
    if (!c->ok) return;
    const StaticBlockDescriptor &blk = static_block_registry[block_id];
    if (blk.Schema->Map[field_id].FlagsAndType & FieldFlags::ReadOnly) return;
    if (blk.Schema->Map[field_id].FlagsAndType & FieldFlags::NotSaved) return;

    FieldResult fr = blk.Get(field_id);
    if (!fr.Data) return;

    uint8_t need = sizeof(BlockIndex) + sizeof(BlockMeta) + ((blk.Schema->Map[field_id].Size + 3) & ~3);
    if (c->write_pos + need > c->log_cap) { c->ok = false; return; }

    BlockIndex ei = {(uint8_t)block_id, (uint8_t)field_id, INVALID_INDEX, 0};
    memcpy(c->log_buf + c->write_pos, &ei, sizeof(BlockIndex)); c->write_pos += sizeof(BlockIndex);
    memcpy(c->log_buf + c->write_pos, &blk.Schema->Map[field_id], sizeof(BlockMeta)); c->write_pos += sizeof(BlockMeta);
    uint8_t vl = blk.Schema->Map[field_id].Size;
    memcpy(c->log_buf + c->write_pos, fr.Data, vl); c->write_pos += vl;
    uint16_t pad = (c->write_pos + 3) & ~3;
    while (c->write_pos < pad) c->log_buf[c->write_pos++] = 0;
}