#pragma once

// Shared memory subsystem. Holds the common block model (BlockIndex, BlockMeta), the
// runtime block descriptors/registries used by the Dynamic and Keyed Memory services, and
// the response helpers shared by every memory service. The per-service request handling
// lives in Core/Services/<Service>.h (one file per service).

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include "Core/Functions/MemoryTypes.h"
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

// Block numbers are local to each memory service: Dynamic and Keyed Memory each
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
__attribute__((noinline)) void SendResponse(const PacketFrame &frame, const uint8_t *payload, uint16_t len)
{
    if (!(frame.flags & FLAG_REQACK))
        return;
    PacketConstruct(&tx_frame, frame.id_src, frame.srv_src, frame.srv_tgt,
                     FLAG_TYPE | FLAG_START | FLAG_STOP, payload, len);
    DispatchPacket(tx_frame);
}

// Sends a one-byte status response (0 = OK, otherwise a non-zero failure code).
// Failures are logged on the core (DeviceLog is a no-op on textless nodes): the
// service tag, CID and the request's block/field/key give a full audit trail for
// every rejected memory operation without per-call-site logging.
__attribute__((noinline)) void RespondStatus(const PacketFrame &frame, bool ok)
{
    if (!ok)
    {
#ifndef DEVICE_LOG_TEXTLESS
        const char *tag = "MEM";
        switch (GetServiceType(frame.srv_tgt))
        {
            case ServiceType::Storage:        tag = "STORAGE"; break;
            default: break;
        }
        uint16_t block = INVALID_INDEX, field = INVALID_INDEX, key = INVALID_INDEX;
        if (PayloadBytes(frame) >= sizeof(BlockIndex))
        {
            const BlockIndex *idx = reinterpret_cast<const BlockIndex *>(frame.payload);
            block = idx->Block; field = idx->Field; key = idx->Key;
        }
        DeviceLog(tag, "CID %u failed block=%u field=%u key=%u",
                  (unsigned)GetServiceCID(frame.srv_tgt),
                  (unsigned)block, (unsigned)field, (unsigned)key);
#endif
        // Structured report (LogHandler CID 0): reaches the core's log DB even
        // from textless nodes; code = CID so failures dedup per service+op.
        ReportLog(MakeLog(false, (uint8_t)GetServiceType(frame.srv_tgt), GetServiceCID(frame.srv_tgt), 0));
    }
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

// Derives the staging-file name for an atomic backup update: the last character of the
// padded 8-byte name becomes '~' ("SYSMEM " -> "SYSMEM~"). Backup names never end in '~'.
inline void BackupTempName(const char name[8], char out[8])
{
    memcpy(out, name, 8);
    out[7] = '~';
}

// Writes `data` to the backup file `name` atomically (NOR-safe copy-and-rename):
inline bool WriteBackupFile(const char name[8], const uint8_t *data, uint16_t len)
{
    char tmp[8];
    BackupTempName(name, tmp);
    if (memcmp(name, tmp, 8) == 0)
        return false; // naming convention violation guard

    // Remove a staging file left over from an interrupted update.
    if (Storage.FileExists(tmp) != 0xFFFFFFFF)
        Storage.DeleteFile(tmp);

    // Stage the new generation in its own file (CreateFile provides freshly erased blocks).
    if (!Storage.CreateFile(tmp, len))
        return false;
    if (!Storage.WriteToFile(tmp, 0, len, (const char *)data))
        return false;

    // Commit atomically: the staging file becomes the live backup under its final name.
    if (!Storage.RenameFile(tmp, name))
    {
        Storage.DeleteFile(tmp);
        return false;
    }
    return true;
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
// Keyed entry support types (used by DynamicBlockDescriptor for dictionaries)
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

// Iterates keyed entries in a dictionary field. Calls `fn(m, offset, field_len, ctx)` for each
// valid entry. Returns true if iteration completed, false if bounds check failed.
template <typename Fn, typename Ctx>
static bool WalkKeyedEntries(uint8_t *field_data, uint16_t field_len, Fn &&fn, Ctx &ctx)
{
    uint16_t offset = 0;
    while (offset + sizeof(BlockMeta) <= field_len)
    {
        BlockMeta *m = reinterpret_cast<BlockMeta *>(field_data + offset);
        if (!KeyedEntryFits(m->Size, offset, field_len))
            return false;
        if (!fn(m, offset, field_len, ctx))
            break; // callback signaled stop
        offset += AlignTo4(sizeof(BlockMeta) + m->Size);
    }
    return true;
}

//**********************************************************************
// Dynamic memory block. Each block holds an ordered list of fields
// (BlockMeta + aligned value). Fields can be either plain values (key = 0xFFFF)
// or keyed entries (dictionary entries with key = 0..254, None = 255 placeholder).
// The Field&Key in BlockMeta (uint16) encodes: field index in upper 8 bits, key in lower 8 bits.
// A key of 0xFFFF means "plain field" (no key), 0xFF = None placeholder.
struct DynamicBlockDescriptor
{
    void *data_ptr = nullptr;
    BlockMeta *map = nullptr;
    BlockType type = BlockType::Undefined;
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
            if ((int32_t)length + delta < 0)
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

    // ======== Keyed Entry Support (Dictionaries) ========
    // A field acts as a dictionary when its entries have key != 0xFFFF.
    // Entry format: BlockMeta(FlagsAndType, Key, Size) + Value[Size] (aligned to 4 bytes).
    // None-marked entries (DataType::None) are invisible placeholders.

    // Returns the keyed entry `target_key` in field `field_idx`.
    KeyResult GetKey(uint16_t field_idx, uint8_t target_key)
    {
        KeyResult res;
        FieldResult field = this->Get(field_idx);
        if (!field.Data)
            return res;
        
        WalkKeyedEntries(static_cast<uint8_t *>(field.Data), field.Descriptor.Size,
            [&](BlockMeta *m, uint16_t offset, uint16_t field_len, auto &ctx) {
                if (m->Key == target_key) {
                    if (((uint16_t)m->FlagsAndType & 0x03FF) == (uint16_t)DataType::None)
                        return false; // None placeholder = not found
                    res.meta = *m;
                    res.data_ptr = static_cast<uint8_t *>(field.Data) + offset + sizeof(BlockMeta);
                    res.data_len = m->Size;
                    return false; // stop
                }
                return true; // continue
            }, res);
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
            WalkKeyedEntries(cursor, field_len,
                [&](BlockMeta *m, uint16_t off, uint16_t field_len, auto &ctx) {
                    if (m->Key == key) {
                        found = true;
                        offset = off;
                        return false; // stop
                    }
                    return true; // continue
                }, found);
        }

        uint16_t old_entry_size = found ? AlignTo4(sizeof(BlockMeta) + ((BlockMeta *)(cursor + offset))->Size) : 0;
        uint16_t new_entry_size = AlignTo4(sizeof(BlockMeta) + val_len);
        int16_t size_diff = (int16_t)new_entry_size - (int16_t)old_entry_size;

        if (!EnsureCapacity(size_diff))
            return false;

        cursor = GetFieldBase();
        if (size_diff != 0)
        {
            // Shift the WHOLE tail of the block after this entry, not just the
            // current field's: later fields' data lives after this field too.
            // Covers replaced entries (found) AND newly-added entries at the
            // end of a non-last field (offset = end of the field's data).
            size_t tail_start = offset + old_entry_size;
            uint8_t *data_end = static_cast<uint8_t *>(data_ptr) + length;
            size_t tail_len = data_end - (cursor + tail_start);
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

    // Returns the keys of dictionary field `field_idx` into `keys`; returns the key count.
    uint16_t ListKeys(uint16_t field_idx, uint8_t *keys, uint16_t cap)
    {
        uint16_t count = 0;
        FieldResult field = this->Get(field_idx);
        if (!field.Data)
            return 0;
        
        WalkKeyedEntries(static_cast<uint8_t *>(field.Data), field.Descriptor.Size,
            [&](BlockMeta *m, uint16_t offset, uint16_t field_len, auto &ctx) {
                if (((uint16_t)m->FlagsAndType & 0x03FF) != (uint16_t)DataType::None) {
                    if (count < cap)
                        keys[count] = m->Key;
                    count++;
                }
                return true; // continue
            }, count);
        return count;
    }

    // Marks the keyed entry `key` as None IN PLACE without resizing: the slot
    // stays (key identity and size preserved), but ListKeys/GetKey ignore it.
    bool MarkKey(uint16_t field_idx, uint8_t key)
    {
        FieldResult field = this->Get(field_idx);
        if (!field.Data)
            return false;
        
        bool found = false;
        WalkKeyedEntries(static_cast<uint8_t *>(field.Data), field.Descriptor.Size,
            [&](BlockMeta *m, uint16_t offset, uint16_t field_len, auto &ctx) {
                if (m->Key == key) {
                    m->FlagsAndType = (m->FlagsAndType & BLOCK_META_FLAGS_MASK) |
                                      (uint16_t)DataType::None;
                    found = true;
                    return false; // stop
                }
                return true; // continue
            }, found);
        return found;
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

    // ======== End Keyed Entry Support ========
};

// Generic block registry holding `T` (DynamicBlockDescriptor).
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

using DynamicRegistry = BlockRegistry<DynamicBlockDescriptor>;

#ifndef DISABLE_DYNAMIC_MEMORY
// One registry instance per dynamic memory service (defined in their service files).
extern DynamicRegistry dynamic_block_registry;
DynamicRegistry dynamic_block_registry;

// Creates a new dynamic block of `type` named by `name` (name_len bytes). Returns the block or nullptr.
static DynamicBlockDescriptor *CreateDynamicBlock(BlockType type, const uint8_t *name, uint16_t name_len)
{
    if (!dynamic_block_registry.AddBlock(type))
        return nullptr;
    DynamicBlockDescriptor &block = *dynamic_block_registry.GetBlock(dynamic_block_registry.block_count - 1);
    uint16_t len = name_len;
    if (len > BLOCK_NAME_LEN - 1) len = BLOCK_NAME_LEN - 1;
    if (len && name) memcpy(block.Name, name, len);
    block.Name[len] = '\0';
    return &block;
}

// Encoded Storage file name holding the dynamic memory backup registry.
static const char *DynamicBackupName()
{
    static constexpr char name[8] = {'D', 'Y', 'N', 'M', 'E', 'M', ' ', ' '};
    return name;
}
#endif

// Common backup file iteration: reads one block's serialized data from backup buffer.
// Returns true on success and updates cursor.
template <typename T>
static bool ReadBlockFromBackup(const uint8_t *buf, uint16_t len, uint16_t &cursor, T &out_block_info, const uint8_t *&out_data, uint16_t &out_data_len)
{
    if (cursor + 1 > len) return false;
    uint8_t name_length = buf[cursor++];
    if (cursor + name_length > len) return false;
    const uint8_t *name = buf + cursor; cursor += name_length;

    if (cursor + 2 > len) return false;
    uint16_t type; memcpy(&type, buf + cursor, 2); cursor += 2;

    if (cursor + 2 > len) return false;
    uint16_t map_count; memcpy(&map_count, buf + cursor, 2); cursor += 2;

    uint16_t map_bytes = map_count * sizeof(BlockMeta);
    if (cursor + map_bytes > len) return false;
    const BlockMeta *map = (const BlockMeta *)(buf + cursor); cursor += map_bytes;

    if (cursor + 2 > len) return false;
    uint16_t data_length; memcpy(&data_length, buf + cursor, 2); cursor += 2;
    if (cursor + data_length > len) return false;
    const uint8_t *data = buf + cursor; cursor += data_length;

    out_block_info.name = name;
    out_block_info.name_len = name_length;
    out_block_info.type = type;
    out_block_info.map_count = map_count;
    out_block_info.map = map;
    out_data = data;
    out_data_len = data_length;
    return true;
}

struct BackupBlockInfo {
    const uint8_t *name = nullptr;
    uint8_t name_len = 0;
    uint16_t type = 0;
    uint16_t map_count = 0;
    const BlockMeta *map = nullptr;
};

// Rebuilds the registry from a serialised buffer, replacing any existing blocks.
template <typename T>
static bool DeserializeRegistry(BlockRegistry<T> &registry, const uint8_t *in, uint16_t len)
{
    uint16_t cursor = 0;
    if (len < 2) return false;
    uint16_t block_count;
    memcpy(&block_count, in + cursor, 2); cursor += 2;

    // Free any existing blocks first.
    while (registry.block_count > 0)
        registry.RemoveBlock(registry.block_count - 1);

    for (uint16_t index = 0; index < block_count; index++)
    {
        BackupBlockInfo info;
        const uint8_t *data = nullptr;
        uint16_t data_len = 0;
        if (!ReadBlockFromBackup(in, len, cursor, info, data, data_len))
            return false;

        if (!registry.AddBlock((BlockType)info.type))
            return false;
        T &block = *registry.GetBlock(registry.block_count - 1);

        uint16_t nn = info.name_len;
        if (nn > BLOCK_NAME_LEN - 1) nn = BLOCK_NAME_LEN - 1;
        memcpy(block.Name, info.name, nn); block.Name[nn] = '\0';

        if (info.map_count)
        {
            block.map = (BlockMeta *)malloc(info.map_count * sizeof(BlockMeta));
            if (!block.map) { registry.RemoveBlock(registry.block_count - 1); return false; }
            memcpy(block.map, info.map, info.map_count * sizeof(BlockMeta));
            block.map_allocated = block.map_count = info.map_count;
        }
        if (data_len)
        {
            block.data_ptr = malloc(data_len);
            if (!block.data_ptr) { registry.RemoveBlock(registry.block_count - 1); return false; }
            memcpy(block.data_ptr, data, data_len);
            block.allocated = block.length = data_len;
        }
    }
    return true;
}

// Serialises `registry` into `out` (max `cap` bytes). Returns the serialised length (0 on error).
template <typename T>
static uint16_t SerializeRegistry(const BlockRegistry<T> &registry, uint8_t *out, uint16_t cap)
{
    uint16_t cursor = 0;
    if (cap < 2)
        return 0;
    memcpy(out + cursor, &registry.block_count, 2); cursor += 2;

    for (uint16_t index = 0; index < registry.block_count; index++)
    {
        const T &block = registry.blocks[index];

        uint8_t name_length = (uint8_t)strlen(block.Name);
        if (cursor + 1 > cap) return 0;
        out[cursor++] = name_length;
        if (cursor + name_length > cap) return 0;
        memcpy(out + cursor, block.Name, name_length); cursor += name_length;

        if (cursor + 2 > cap) return 0;
        memcpy(out + cursor, &block.type, 2); cursor += 2;

        if (cursor + 2 > cap) return 0;
        memcpy(out + cursor, &block.map_count, 2); cursor += 2;

        uint16_t map_bytes = block.map_count * sizeof(BlockMeta);
        if (cursor + map_bytes > cap) return 0;
        if (block.map_count)
            memcpy(out + cursor, block.map, map_bytes);
        cursor += map_bytes;

        if (cursor + 2 > cap) return 0;
        memcpy(out + cursor, &block.length, 2); cursor += 2;

        if (cursor + block.length > cap) return 0;
        if (block.length)
            memcpy(out + cursor, block.data_ptr, block.length);
        cursor += block.length;
    }
    return cursor;
}

// Restores a single block `block_idx` from the backup file `backup_name` into `registry`.
// Returns true on success.
template <typename T>
static bool RecallRegistryBlock(BlockRegistry<T> &registry, uint16_t block_idx, const char backup_name[8])
{
    uint8_t buf[MEMORY_BACKUP_CAP];
    uint16_t cnt = ReadBackupFile(backup_name, buf, sizeof(buf));
    if (cnt == 0)
        return false;

    uint16_t cursor = 0;
    if (cnt < 2)
        return false;
    uint16_t block_count;
    memcpy(&block_count, buf + cursor, 2); cursor += 2;

    if (block_idx >= block_count)
        return false;

    for (uint16_t index = 0; index < block_count; index++)
    {
        BackupBlockInfo info;
        const uint8_t *data = nullptr;
        uint16_t data_len = 0;
        if (!ReadBlockFromBackup(buf, cnt, cursor, info, data, data_len))
            return false;

        if (index == block_idx)
        {
            // Free existing block if any
            if (block_idx < registry.block_count)
                registry.RemoveBlock(block_idx);
            // Add new block at the correct index
            if (!registry.AddBlock((BlockType)info.type))
                return false;
            // Need to shift blocks to make room at block_idx
            if (block_idx < registry.block_count - 1)
            {
                for (uint16_t i = registry.block_count - 1; i > block_idx; i--)
                    registry.blocks[i] = registry.blocks[i - 1];
                registry.blocks[block_idx].Release();
            }
            T &block = registry.blocks[block_idx];

            uint16_t nn = info.name_len;
            if (nn > BLOCK_NAME_LEN - 1) nn = BLOCK_NAME_LEN - 1;
            memcpy(block.Name, info.name, nn); block.Name[nn] = '\0';

            if (info.map_count)
            {
                block.map = (BlockMeta *)malloc(info.map_count * sizeof(BlockMeta));
                if (!block.map) { registry.RemoveBlock(block_idx); return false; }
                memcpy(block.map, info.map, info.map_count * sizeof(BlockMeta));
                block.map_allocated = block.map_count = info.map_count;
            }
            if (data_len)
            {
                block.data_ptr = malloc(data_len);
                if (!block.data_ptr) { registry.RemoveBlock(block_idx); return false; }
                memcpy(block.data_ptr, data, data_len);
                block.allocated = block.length = data_len;
            }
            return true;
        }
    }
    return false;
}