#pragma once

// Shared memory subsystem. Holds the common block model (BlockIndex, BlockMeta), the
// runtime block descriptors/registries used by the Dynamic and Keyed Memory services, and
// the response helpers shared by every memory service. The per-service request handling
// lives in Core/Services/<Service>.h (one file per service).

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include "Core/Functions/SystemMemory.h"
#include "Core/Functions/Packet.h"
#include "Core/Functions/Storage.h"

void DispatchPacket(const PacketFrame &frame);

#define INVALID_BLOCK 0xFF
#define INVALID_INDEX 0xFF

// Capacity of a serialised service backup buffer (Dynamic/Keyed/System backup files).
// RAM-starved devices (DAS) build with a smaller value via the MEMORY_BACKUP_CAP build flag.
#ifndef MEMORY_BACKUP_CAP
#define MEMORY_BACKUP_CAP 2048
#endif

// Block numbers are local to each memory service: System, Dynamic and Keyed Memory each
// number their blocks independently from 0. The service a request targets is carried by the
// packet's SRV TGT (ServiceType), never by the block number itself.
struct BlockIndex
{
    uint8_t Block = INVALID_BLOCK;
    uint8_t Field = INVALID_INDEX;
    uint8_t Key = INVALID_INDEX;
    uint8_t Padding = 0;
};

// Sends a single response packet back to the requester (only if REQACK was set).
inline void SendResponse(const PacketFrame &frame, const uint8_t *payload, uint8_t len)
{
    if (!(frame.flags & FLAG_REQACK))
        return;
    PacketFrame reply;
    PacketConstruct(&reply, frame.id_src, frame.srv_src, frame.srv_tgt,
                     FLAG_TYPE | FLAG_START | FLAG_STOP, payload, len);
    DispatchPacket(reply);
}

// Sends a one-byte status response (0 = OK, otherwise a non-zero failure code).
inline void RespondStatus(const PacketFrame &frame, bool ok)
{
    uint8_t status = ok ? 0 : 0xFF;
    SendResponse(frame, &status, 1);
}

// Fills `out` with BlockIndex(block, invalid, invalid) + BlockMeta(flags_and_type,
// key = invalid, size = map_count) + name - the shared "block meta + name" reply
// payload of the Dynamic/Keyed/System memory services. Returns the payload length.
inline uint16_t MakeBlockMetaPayload(uint8_t block, uint16_t flags_and_type, uint16_t map_count,
                                     const char *name, uint8_t name_len,
                                     uint8_t *out, uint16_t cap)
{
    if ((uint32_t)sizeof(BlockIndex) + sizeof(BlockMeta) + name_len > cap)
        return 0;
    uint16_t cursor = 0;
    BlockIndex out_index = {block, INVALID_INDEX, INVALID_INDEX};
    memcpy(out + cursor, &out_index, sizeof(BlockIndex)); cursor += sizeof(BlockIndex);
    BlockMeta meta;
    meta.FlagsAndType = flags_and_type;
    meta.Key = INVALID_INDEX;
    meta.Size = (uint8_t)map_count;
    memcpy(out + cursor, &meta, sizeof(BlockMeta)); cursor += sizeof(BlockMeta);
    if (name_len) { memcpy(out + cursor, name, name_len); cursor += name_len; }
    return cursor;
}

// Writes `len` bytes to a backup file (creating/resizing it as needed).
inline bool WriteBackupFile(const char name[8], const uint8_t *data, uint16_t len)
{
    uint32_t off, sz;
    bool existed = Storage.GetFileInfo(name, &off, &sz);
    if (!existed)
    {
        if (!Storage.CreateFile(name, len))
            return false;
        if (!Storage.GetFileInfo(name, &off, &sz))
            return false;
    }
    else if (sz != len)
    {
        if (!Storage.ResizeFile(name, len))
            return false;
        if (!Storage.GetFileInfo(name, &off, &sz))
            return false;
    }

    // NOR flash can only program 1s to 0s, so rewriting a file that already holds
    // data (same size, a shrink that kept its blocks, or the copy left by a grow)
    // requires erasing its data area first; otherwise bits that must go back to 1
    // stay 0 and the backup corrupts.
    if (existed)
    {
        uint32_t blocks = (len + STORAGE_BLOCK_SIZE - 1) / STORAGE_BLOCK_SIZE;
        if (blocks == 0) blocks = 1;
        if (!Storage_FlashErase(off, blocks * STORAGE_BLOCK_SIZE))
            return false;
    }
    return Storage_FlashWrite(off, data, len);
}

// Reads a backup file into `out`; returns the byte count (0 if absent or too large).
inline uint16_t ReadBackupFile(const char name[8], uint8_t *out, uint16_t cap)
{
    uint32_t off, sz;
    if (!Storage.GetFileInfo(name, &off, &sz)) return 0;
    if (sz > cap) sz = cap;
    if (Storage_FlashRead(off, out, sz) != sz) return 0;
    return (uint16_t)sz;
}

//**********************************************************************
// Dynamic (non-keyed) memory block. Each block holds an ordered list of plain fields
// (BlockMeta + aligned value). Keyed storage is a separate type (KeyedBlockDescriptor);
// a dynamic block has no key access.
struct DynamicBlockDescriptor
{
    void *data_ptr = nullptr;
    BlockMeta *map = nullptr;
    BlockType type = BlockType::Unknown;
    uint16_t length = 0;
    uint16_t allocated = 0;
    uint16_t map_count = 0;
    uint16_t map_allocated = 0;
    char Name[BLOCK_NAME_LEN] = {};

    // Navigation always jumps by the aligned size, even if the stored size is not.
    size_t GetOffset(uint16_t index) const
    {
        size_t offset = 0;
        for (uint16_t i = 0; i < index; ++i)
            offset += AlignTo4(map[i].Size);
        return offset;
    }

    // Grows/shrinks the data buffer to accommodate `delta` more bytes; returns false on realloc failure.
    bool EnsureCapacity(int16_t delta)
    {
        if (delta < 0)
        {
            if ((uint16_t)(-delta) > length)
                return false;
            return true;
        }
        if (length + delta <= allocated)
            return true;
        uint16_t new_cap = length + delta + 32;
        void *new_data = realloc(data_ptr, new_cap);
        if (!new_data)
            return false;
        data_ptr = new_data;
        allocated = new_cap;
        return true;
    }

    // Inserts a new field at position `insert_at`, growing the map/data buffers and shifting existing entries.
    bool InsertField(uint16_t insert_at, BlockMeta new_field)
    {
        uint16_t aligned_size = AlignTo4(new_field.Size);
        if (map_count + 1 > map_allocated)
        {
            BlockMeta *new_map = (BlockMeta *)realloc(map, (map_allocated + 4) * sizeof(BlockMeta));
            if (!new_map)
                return false;
            map = new_map;
            map_allocated += 4;
        }
        if (!EnsureCapacity(aligned_size))
            return false;
        size_t offset = GetOffset(insert_at);
        memmove((uint8_t *)data_ptr + offset + aligned_size, (uint8_t *)data_ptr + offset, length - offset);
        memmove(&map[insert_at + 1], &map[insert_at], (map_count - insert_at) * sizeof(BlockMeta));
        map[insert_at] = new_field;
        map_count++;
        length += aligned_size;
        return true;
    }

    // Writes `input_len` bytes of `input` into field `index`, resizing/re-shifting the buffer when the aligned size changes.
    bool Set(uint16_t index, const void *input, uint16_t input_len, uint16_t input_type_and_flag)
    {
        if (index >= map_count || !input)
            return false;
        size_t offset = GetOffset(index);
        uint16_t old_aligned = AlignTo4(map[index].Size);
        uint16_t new_aligned = AlignTo4(input_len);
        if (new_aligned != old_aligned)
        {
            if (!EnsureCapacity((int16_t)new_aligned - (int16_t)old_aligned))
                return false;
            size_t after_offset = offset + old_aligned;
            memmove((uint8_t *)data_ptr + offset + new_aligned,
                    (uint8_t *)data_ptr + after_offset,
                    length - after_offset);
            length += ((int16_t)new_aligned - (int16_t)old_aligned);
        }
        map[index].Size = input_len;
        map[index].FlagsAndType = input_type_and_flag;
        memcpy((uint8_t *)data_ptr + offset, input, input_len);
        return true;
    }

    // Removes the field at `index`, compacting the data and map arrays.
    bool Remove(uint16_t index)
    {
        if (index >= map_count)
            return false;
        size_t offset = GetOffset(index);
        uint16_t size_to_remove = AlignTo4(map[index].Size);
        size_t after_offset = offset + size_to_remove;
        memmove((uint8_t *)data_ptr + offset, (uint8_t *)data_ptr + after_offset, length - after_offset);
        memmove(&map[index], &map[index + 1], (map_count - index - 1) * sizeof(BlockMeta));
        length -= size_to_remove;
        map_count--;
        if (allocated > length + 64)
        {
            void *new_data = realloc(data_ptr, length);
            if (new_data)
            {
                data_ptr = new_data;
                allocated = length;
            }
        }
        return true;
    }

    // Returns the descriptor and data pointer for the field at `index` (empty result if out of range).
    FieldResult Get(uint16_t index) const
    {
        FieldResult Output;
        if (index >= map_count)
            return Output;
        size_t offset = GetOffset(index);
        Output.Descriptor = map[index];
        Output.Data = (uint8_t *)data_ptr + offset;
        return Output;
    }

    // Frees all owned allocations and resets the block to empty.
    void Release()
    {
        if (data_ptr) free(data_ptr);
        if (map) free(map);
        data_ptr = nullptr;
        map = nullptr;
        length = allocated = map_count = map_allocated = 0;
        Name[0] = '\0';
    }
};

// Generic block registry holding `T` (DynamicBlockDescriptor or KeyedBlockDescriptor).
// Dynamic and keyed memory each own one registry so the two kinds of blocks can never mix.
template <typename T>
struct BlockRegistry
{
    T *blocks = nullptr;
    uint16_t block_count = 0;
    uint16_t block_allocated = 0;

    // Appends a new empty block of `type`, growing the descriptor array as needed.
    bool AddBlock(BlockType type)
    {
        if (block_count + 1 > block_allocated)
        {
            uint16_t new_cap = block_allocated + 4;
            T *new_blocks = (T *)realloc(blocks, new_cap * sizeof(T));
            if (!new_blocks)
                return false;
            memset((void *)(new_blocks + block_allocated), 0, (new_cap - block_allocated) * sizeof(T));
            blocks = new_blocks;
            block_allocated = new_cap;
        }
        blocks[block_count].type = type;
        block_count++;
        return true;
    }

    // Returns the block descriptor at `index`, or nullptr if out of range.
    T *GetBlock(uint16_t index)
    {
        if (index >= block_count)
            return nullptr;
        return &blocks[index];
    }

    // Fully deallocates a block and shifts the remaining ones down.
    void RemoveBlock(uint16_t index)
    {
        if (index >= block_count)
            return;
        blocks[index].Release();
        for (uint16_t i = index; i < block_count - 1; i++)
            blocks[i] = blocks[i + 1];
        block_count--;
    }
};

// Keyed entries reuse BlockMeta: Key = entry key, Size = value length.
struct KeyResult
{
    BlockMeta meta = {DataType::Unknown | FieldFlags::None, 0x00, 0};
    void *data_ptr = nullptr;
    uint16_t data_len = 0;
};

// True when a complete keyed entry (meta + value) starting at `start` fits inside a
// dictionary field of length `field_len`. Shared by every keyed-entry walker so one
// corrupt/truncated entry can never make the walk run into the next field's data.
static inline bool KeyedEntryFits(uint8_t val_size, uint16_t start, uint16_t field_len)
{
    return (uint32_t)start + sizeof(BlockMeta) + val_size <= (uint32_t)field_len;
}

// Keyed memory block. Storage layout is inherited from DynamicBlockDescriptor, but keyed
// fields additionally store (Key, Value) entries addressed through the block/field/key
// BlockIndex of the Keyed Memory service.
struct KeyedBlockDescriptor : public DynamicBlockDescriptor
{
    // Looks up a keyed entry by `target_key` inside field `field_idx`.
    KeyResult GetKey(uint16_t field_idx, uint8_t target_key)
    {
        KeyResult res;
        FieldResult field = this->Get(field_idx);
        if (!field.Data)
            return res;
        uint8_t *cursor = static_cast<uint8_t *>(field.Data);
        uint16_t processed = 0;
        while (processed + sizeof(BlockMeta) <= field.Descriptor.Size)
        {
            BlockMeta *m = reinterpret_cast<BlockMeta *>(cursor);
            if (m->Key == target_key)
            {
                res.meta = *m;
                res.data_ptr = cursor + sizeof(BlockMeta);
                res.data_len = m->Size;
                return res;
            }
            // Bounds check: a corrupt/truncated entry must end the walk, not step
            // into the next field's data (or past the allocation).
            if (!KeyedEntryFits(m->Size, processed, field.Descriptor.Size))
                break;
            uint16_t entry_size = sizeof(BlockMeta) + m->Size;
            uint16_t step = AlignTo4(entry_size);
            cursor += step;
            processed += step;
        }
        return res;
    }

    // Sets the value of keyed entry `key` in field `field_idx`, creating or resizing as needed.
    bool SetKey(uint16_t field_idx, uint8_t key, const void *val, uint8_t val_len, uint16_t type_and_flag)
    {
        if (field_idx >= map_count)
            return false;
        auto GetFieldBase = [&]() -> uint8_t *
        {
            size_t offset = GetOffset(field_idx);
            return data_ptr ? (uint8_t *)data_ptr + offset : nullptr;
        };

        uint16_t offset = 0;
        bool found = false;
        uint8_t *cursor = GetFieldBase();
        if (cursor)
        {
            uint16_t field_len = map[field_idx].Size;
            while (offset + sizeof(BlockMeta) <= field_len)
            {
                BlockMeta *m = reinterpret_cast<BlockMeta *>(cursor + offset);
                if (!KeyedEntryFits(m->Size, offset, field_len))
                    break;
                if (m->Key == key)
                {
                    found = true;
                    break;
                }
                offset += AlignTo4(sizeof(BlockMeta) + m->Size);
            }
        }

        uint16_t old_entry_size = found ? AlignTo4(sizeof(BlockMeta) + ((BlockMeta *)(cursor + offset))->Size) : 0;
        uint16_t new_entry_size = AlignTo4(sizeof(BlockMeta) + val_len);
        int16_t size_diff = (int16_t)new_entry_size - (int16_t)old_entry_size;

        // BlockMeta.Size is a single byte: refuse to grow the dictionary field past
        // its representable size instead of silently wrapping.
        if ((int32_t)map[field_idx].Size + size_diff > 0xFF)
            return false;

        if (!EnsureCapacity(size_diff))
            return false;

        cursor = GetFieldBase();
        if (found && size_diff != 0)
        {
            size_t tail_start = offset + old_entry_size;
            size_t tail_len = map[field_idx].Size - tail_start;
            if (tail_len > 0)
                memmove(cursor + offset + new_entry_size, cursor + tail_start, tail_len);
        }

        BlockMeta new_meta = {type_and_flag, key, val_len};
        memcpy(cursor + offset, &new_meta, sizeof(BlockMeta));
        if (val && val_len > 0)
            memcpy(cursor + offset + sizeof(BlockMeta), val, val_len);

        uint16_t total_written = sizeof(BlockMeta) + val_len;
        if (new_entry_size > total_written)
            memset(cursor + offset + total_written, 0, new_entry_size - total_written);

        if (size_diff != 0)
        {
            this->map[field_idx].Size += size_diff;
            this->length += size_diff;
        }
        return true;
    }

    // Reads a keyed value as type `T`, falling back to `defaultValue` when missing or of a different type.
    template <typename T>
    T GetKeyValue(uint16_t index, uint8_t key, DataType expectedType, T defaultValue = 0)
    {
        KeyResult res = this->GetKey(index, key);
        if (res.data_ptr && (BlockMetaType(res.meta.FlagsAndType) == (uint16_t)expectedType))
            return *(T *)res.data_ptr;
        return defaultValue;
    }

    // Copies the keys of dictionary field `field_idx` into `keys`; returns the key count.
    uint16_t ListKeys(uint16_t field_idx, uint8_t *keys, uint16_t cap)
    {
        uint16_t count = 0;
        FieldResult field = this->Get(field_idx);
        if (!field.Data)
            return 0;
        uint8_t *cursor = static_cast<uint8_t *>(field.Data);
        uint16_t offset = 0;
        while (offset + sizeof(BlockMeta) <= field.Descriptor.Size)
        {
            BlockMeta *m = reinterpret_cast<BlockMeta *>(cursor + offset);
            uint16_t entry_size = AlignTo4(sizeof(BlockMeta) + m->Size);
            if (!KeyedEntryFits(m->Size, offset, field.Descriptor.Size))
                break;
            if (count < cap)
                keys[count] = m->Key;
            count++;
            offset += entry_size;
        }
        return count;
    }

    // Removes the keyed entry `key` from dictionary field `field_idx`.
    bool RemoveKey(uint16_t field_idx, uint8_t key)
    {
        FieldResult field = this->Get(field_idx);
        if (!field.Data)
            return false;
        uint8_t *cursor = static_cast<uint8_t *>(field.Data);
        uint16_t offset = 0;
        while (offset + sizeof(BlockMeta) <= field.Descriptor.Size)
        {
            BlockMeta *m = reinterpret_cast<BlockMeta *>(cursor + offset);
            uint16_t entry_size = AlignTo4(sizeof(BlockMeta) + m->Size);
            if (!KeyedEntryFits(m->Size, offset, field.Descriptor.Size))
                break;
            if (m->Key == key)
            {
                uint16_t tail = field.Descriptor.Size - (offset + entry_size);
                if (tail > 0)
                    memmove(cursor + offset, cursor + offset + entry_size, tail);
                this->map[field_idx].Size -= entry_size;
                this->length -= entry_size;
                return true;
            }
            offset += entry_size;
        }
        return false;
    }
};

using DynamicRegistry = BlockRegistry<DynamicBlockDescriptor>;
using KeyedRegistry = BlockRegistry<KeyedBlockDescriptor>;

// One registry instance per dynamic/keyed memory service (defined in their service files).
extern DynamicRegistry dynamic_block_registry;
extern KeyedRegistry keyed_block_registry;

