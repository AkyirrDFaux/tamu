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

// Derives the staging-file name for an atomic backup update: the last character of the
// padded 8-byte name becomes '~' ("STATLOG " -> "STATLOG~"). Backup names never end in '~'.
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
//**********************************************************************
// Dynamic memory block (Docs/Services/Register.md "Dynamic blocks").
//
// A block's contents are a FLAT table of entries, strictly ascending by Field&Key
// (u16 = (field<<8)|key); fields and keys are equal entry types. Each entry carries a
// ValueInfo (FlagsAndType u16 + Size u8) and a MemoryOffset into one of the block's two
// value spaces (volatile / persistent, chosen by the entry's Persistent flag).
// Deleting an entry removes it from the table (the sequential record compacts);
// a deleted/skipped BLOCK keeps a tombstone slot in the registry.

// Combines a field index and a key into the 16-bit Field&Key sort key.
inline uint16_t MakeFieldKey(uint8_t field, uint8_t key) { return (uint16_t)(((uint16_t)field << 8) | key); }
inline uint8_t FieldOf(uint16_t fieldKey) { return (uint8_t)(fieldKey >> 8); }
inline uint8_t KeyOf(uint16_t fieldKey) { return (uint8_t)fieldKey; }

// One entry in a dynamic block's table.
struct DynamicEntry
{
    uint16_t fieldKey;     // (field << 8) | key
    uint16_t flagsAndType; // ValueInfo: type (10b) + flags (6b)
    uint8_t size;          // value size in bytes
    uint8_t pad = 0;
    uint16_t memoryOffset; // offset into the entry's volatile/persistent value space
};

// Lookup result compatible with the older FieldResult/KeyResult consumers: the meta
// (BlockMeta) + a pointer to the value bytes.
struct KeyResult
{
    BlockMeta meta = {DataType::Unknown | FieldFlags::None, 0x00, 0};
    void *data_ptr = nullptr;
    uint16_t data_len = 0;
    bool exists = false; // entry present (a size-0 entry exists with no value)
};

struct DynamicBlockDescriptor
{
    DynamicEntry *table = nullptr;
    uint16_t entry_count = 0;
    uint16_t entry_allocated = 0;
    uint8_t *volatile_data = nullptr;
    uint16_t volatile_len = 0, volatile_allocated = 0;
    uint8_t *persistent_data = nullptr;
    uint16_t persistent_len = 0, persistent_allocated = 0;
    BlockType type = BlockType::Undefined;
    char Name[BLOCK_NAME_LEN] = {};
    uint32_t generation = 0;

    bool IsPersistent(const DynamicEntry &e) const { return (e.flagsAndType & FieldFlags::Persistent) != 0; }
    uint8_t *Space(bool persistent) const { return persistent ? persistent_data : volatile_data; }
    uint16_t &SpaceLen(bool persistent) { return persistent ? persistent_len : volatile_len; }
    uint16_t &SpaceAlloc(bool persistent) { return persistent ? persistent_allocated : volatile_allocated; }
    uint8_t *&SpaceData(bool persistent) { return persistent ? persistent_data : volatile_data; }

    // Binary search for `fieldKey`; returns the insert position (first >= fieldKey).
    uint16_t FindEntry(uint16_t fieldKey) const
    {
        uint16_t lo = 0, hi = entry_count;
        while (lo < hi)
        {
            uint16_t mid = (lo + hi) >> 1;
            if (table[mid].fieldKey < fieldKey) lo = mid + 1; else hi = mid;
        }
        return lo;
    }

    bool EnsureTable(uint16_t count)
    {
        if (count <= entry_allocated) return true;
        uint16_t new_cap = count + 4;
        DynamicEntry *nt = (DynamicEntry *)realloc(table, new_cap * sizeof(DynamicEntry));
        if (!nt) return false;
        table = nt;
        entry_allocated = new_cap;
        return true;
    }

    // Appends `value` to the value space chosen by `persistent`; returns the offset
    // (0xFFFF on allocation failure).
    uint16_t AppendValue(bool persistent, const void *value, uint8_t len)
    {
        uint16_t &used = SpaceLen(persistent);
        uint16_t &alloc = SpaceAlloc(persistent);
        uint8_t *&sp = SpaceData(persistent);
        if (used + len > alloc)
        {
            uint16_t new_cap = used + len + 16;
            uint8_t *nb = (uint8_t *)realloc(sp, new_cap);
            if (!nb) return 0xFFFF;
            sp = nb;
            alloc = new_cap;
        }
        uint16_t offset = used;
        if (len) memcpy(sp + offset, value, len);
        used += len;
        return offset;
    }

    // Rebuilds the value spaces (compaction): copies every entry's value from its
    // current space into fresh compacted buffers and updates memoryOffset. Also moves
    // values between spaces when the Persistent flag changed. Called after any change.
    bool RebuildSpaces()
    {
        uint16_t v_needed = 0, p_needed = 0;
        for (uint16_t i = 0; i < entry_count; i++)
            (IsPersistent(table[i]) ? p_needed : v_needed) += table[i].size;

        uint8_t *nv = v_needed ? (uint8_t *)malloc(v_needed) : nullptr;
        uint8_t *np = p_needed ? (uint8_t *)malloc(p_needed) : nullptr;
        if ((v_needed && !nv) || (p_needed && !np)) {
            // Out of memory: skip compaction. The value just written already sits at its
            // entry's memoryOffset in the right space and the other entries are untouched,
            // so the block stays consistent (just less compact) instead of dereferencing
            // a null buffer.
            if (nv) free(nv);
            if (np) free(np);
            return true;
        }
        uint16_t vo = 0, po = 0;
        for (uint16_t i = 0; i < entry_count; i++)
        {
            DynamicEntry &e = table[i];
            bool pers = IsPersistent(e);
            uint8_t *src = pers ? persistent_data : volatile_data;
            uint8_t *dst = pers ? np : nv;
            uint16_t &off = pers ? po : vo;
            if (e.size && src)
                memcpy(dst + off, src + e.memoryOffset, e.size);
            e.memoryOffset = off;
            off += e.size;
        }
        if (volatile_data) free(volatile_data);
        if (persistent_data) free(persistent_data);
        volatile_data = nv;
        persistent_data = np;
        volatile_len = vo;
        persistent_len = po;
        volatile_allocated = v_needed;
        persistent_allocated = p_needed;
        return true;
    }

    // Writes the value at (field, key), creating/updating the sorted entry.
    // Writing type None deletes the entry (docs "Setting the type to None deletes").
    bool SetEntry(uint8_t field, uint8_t key, const void *value, uint8_t len, uint16_t type_and_flag)
    {
        if (BlockMetaType(type_and_flag) == (uint16_t)DataType::None)
            return DeleteEntry(field, key);

        uint16_t fk = MakeFieldKey(field, key);
        uint16_t i = FindEntry(fk);
        bool pers = (type_and_flag & FieldFlags::Persistent) != 0;

        if (i < entry_count && table[i].fieldKey == fk)
        {
            // Fast path: same size and persistence - overwrite the value in place. This is
            // the hot path (e.g. a script rewriting the same render matrix every tick) and
            // avoids the append + full-space compaction.
            if (table[i].size == len && IsPersistent(table[i]) == pers)
            {
                uint8_t *sp = pers ? persistent_data : volatile_data;
                if (len && sp) memcpy(sp + table[i].memoryOffset, value, len);
                table[i].flagsAndType = type_and_flag;
                generation++;
                return true;
            }
            // Size/persistence changed: stash the new value at the end of its space first,
            // then RebuildSpaces compacts (reads the new value from the tail).
            uint16_t off = AppendValue(pers, value, len);
            if (off == 0xFFFF) return false;
            table[i].flagsAndType = type_and_flag;
            table[i].size = len;
            table[i].memoryOffset = off;
        }
        else
        {
            uint16_t off = AppendValue(pers, value, len);
            if (off == 0xFFFF) return false;
            if (!EnsureTable(entry_count + 1)) return false;
            for (uint16_t j = entry_count; j > i; j--)
                table[j] = table[j - 1];
            DynamicEntry &e = table[i];
            e.fieldKey = fk;
            e.flagsAndType = type_and_flag;
            e.size = len;
            e.pad = 0;
            e.memoryOffset = off;
            entry_count++;
        }
        generation++;
        return RebuildSpaces();
    }

    // Removes the entry at (field, key) from the table (the record compacts).
    bool DeleteEntry(uint8_t field, uint8_t key)
    {
        uint16_t fk = MakeFieldKey(field, key);
        uint16_t i = FindEntry(fk);
        if (i >= entry_count || table[i].fieldKey != fk) return false;
        for (uint16_t j = i; j + 1 < entry_count; j++)
            table[j] = table[j + 1];
        entry_count--;
        generation++;
        return RebuildSpaces();
    }

    // Returns the entry at (field, key) as a KeyResult (meta + value pointer).
    KeyResult GetKey(uint8_t field, uint8_t key)
    {
        KeyResult res;
        uint16_t i = FindEntry(MakeFieldKey(field, key));
        if (i >= entry_count || table[i].fieldKey != MakeFieldKey(field, key))
            return res;
        const DynamicEntry &e = table[i];
        res.meta = {(uint16_t)e.flagsAndType, key, e.size};
        res.data_len = e.size;
        res.exists = true;
        res.data_ptr = e.size ? (Space(IsPersistent(e)) + e.memoryOffset) : nullptr;
        return res;
    }

    // The (field, key 0) entry - a field's "head" (its plain value or dictionary
    // marker). Convenience for consumers that address whole fields.
    KeyResult Get(uint8_t field)
    {
        return GetKey(field, 0);
    }

    // Reads a keyed value as type `T`, falling back to `defaultValue` when missing or
    // of a different type.
    template <typename T>
    T GetKeyValue(uint16_t field, uint8_t key, DataType expectedType, T defaultValue = 0)
    {
        KeyResult res = GetKey((uint8_t)field, key);
        if (res.data_ptr && (BlockMetaType(res.meta.FlagsAndType) == (uint16_t)expectedType))
            return *(T *)res.data_ptr;
        return defaultValue;
    }

    // Distinct field indexes present among the entries.
    uint16_t ListFields(uint8_t *fields, uint16_t cap)
    {
        uint16_t count = 0, prev = 0xFFFF;
        for (uint16_t i = 0; i < entry_count; i++)
        {
            uint8_t f = FieldOf(table[i].fieldKey);
            if (f != prev)
            {
                if (count < cap) fields[count] = f;
                count++;
                prev = f;
            }
        }
        return count;
    }

    // Keys present at `field`.
    uint16_t ListKeys(uint8_t field, uint8_t *keys, uint16_t cap)
    {
        uint16_t count = 0;
        uint16_t i = FindEntry(MakeFieldKey(field, 0));
        for (; i < entry_count && FieldOf(table[i].fieldKey) == field; i++)
        {
            if (count < cap) keys[count] = KeyOf(table[i].fieldKey);
            count++;
        }
        return count;
    }

    // Number of distinct fields (for enumeration / UI display).
    uint16_t FieldCount()
    {
        uint8_t scratch[256];
        return ListFields(scratch, 256);
    }

    // Frees all owned allocations and resets the block to a tombstone-like empty state.
    void Release()
    {
        if (table) free(table);
        if (volatile_data) free(volatile_data);
        if (persistent_data) free(persistent_data);
        table = nullptr;
        volatile_data = nullptr;
        persistent_data = nullptr;
        entry_count = entry_allocated = 0;
        volatile_len = volatile_allocated = 0;
        persistent_len = persistent_allocated = 0;
        Name[0] = '\0';
    }
};

// Generic block registry holding `T` (DynamicBlockDescriptor).
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

    // Creates a block at a specified position (docs "create/write is in specified
    // place, not an append necessarily"): fills a tombstone slot, pads with tombstones
    // beyond, or inserts (shifting later blocks) when the slot is occupied.
    bool AddBlockAt(uint16_t index, BlockType type)
    {
        if (index >= block_count)
        {
            while (block_count < index)
                if (!AddBlock(BlockType::None)) return false;
            return AddBlock(type);
        }
        if (blocks[index].type == BlockType::None)
        {
            blocks[index] = T();
            blocks[index].type = type;
            return true;
        }
        // Insert: shift the occupied slot and everything after it right by one.
        if (!AddBlock(BlockType::None)) return false;
        for (uint16_t i = block_count - 1; i > index; i--)
            blocks[i] = blocks[i - 1];
        blocks[index] = T();
        blocks[index].type = type;
        return true;
    }

    // Marks the block at `index` as a tombstone (keeps the slot; no compaction).
    void TombstoneBlock(uint16_t index)
    {
        if (index >= block_count) return;
        blocks[index].Release();
        blocks[index] = T();
        blocks[index].type = BlockType::None;
    }

    // Returns the block descriptor at `index`, or nullptr if out of range.
    T *GetBlock(uint16_t index)
    {
        if (index >= block_count)
            return nullptr;
        return &blocks[index];
    }

};

using DynamicRegistry = BlockRegistry<DynamicBlockDescriptor>;

#ifndef DISABLE_DYNAMIC_MEMORY
// One registry instance per dynamic memory service (defined in their service files).
extern DynamicRegistry dynamic_block_registry;
DynamicRegistry dynamic_block_registry;

// Creates a dynamic block at position `index` (see AddBlockAt). Returns the block or nullptr.
static DynamicBlockDescriptor *CreateDynamicBlock(BlockType type, const uint8_t *name, uint16_t name_len, uint16_t index)
{
    if (!dynamic_block_registry.AddBlockAt(index, type))
        return nullptr;
    DynamicBlockDescriptor &block = *dynamic_block_registry.GetBlock(index);
    uint16_t len = name_len;
    if (len > BLOCK_NAME_LEN - 1) len = BLOCK_NAME_LEN - 1;
    if (len && name) memcpy(block.Name, name, len);
    block.Name[len] = '\0';
    return &block;
}

// Per-block dynamic persistence (Docs/Services/Register.md: DT_XXX / DV_XXX files).
// A block's table lives in "DT_<hex2>", its persistent value space in "DV_<hex2>".
#define MAX_DYNAMIC_BLOCKS 64 // 6-bit BlockInfo instance range (0..63)

// "D<kind>_<hex2>" space padded to 8 chars (kind = T or V).
static void HexIndexName(char kind, uint16_t idx, char out[8])
{
    const char *hex = "0123456789ABCDEF";
    out[0] = 'D';
    out[1] = kind;
    out[2] = '_';
    out[3] = hex[(idx >> 4) & 0xF];
    out[4] = hex[idx & 0xF];
    out[5] = out[6] = out[7] = ' ';
}
static void DynamicTableName(uint16_t idx, char out[8]) { HexIndexName('T', idx, out); }
static void DynamicValuesName(uint16_t idx, char out[8]) { HexIndexName('V', idx, out); }

// Removes a block's DT/DV files (tombstoned slots must not leave files behind).
static void DeleteDynamicBlockFiles(uint16_t idx)
{
    char tn[8], vn[8];
    DynamicTableName(idx, tn);
    DynamicValuesName(idx, vn);
    if (Storage.FileExists(tn) != 0xFFFFFFFF) Storage.DeleteFile(tn);
    if (Storage.FileExists(vn) != 0xFFFFFFFF) Storage.DeleteFile(vn);
}

// Cleans orphaned DT/DV files for tombstoned or beyond-registry slots. Called before
// every save. File cleanup never changes registry positions.
static void CleanupDynamicFiles()
{
    for (uint16_t i = 0; i < MAX_DYNAMIC_BLOCKS; i++)
    {
        bool live = (i < dynamic_block_registry.block_count &&
                     dynamic_block_registry.blocks[i].type != BlockType::None);
        if (!live)
            DeleteDynamicBlockFiles(i);
    }
}

// Writes the block's DT (table) + DV (persistent value space) files atomically via the
// staging/rename helper. Volatile values are NOT persisted (docs: only the Persistent
// space is saved; the table keeps all entries).
static bool SaveDynamicBlockFiles(const DynamicBlockDescriptor &b, uint16_t idx)
{
    uint8_t buf[MEMORY_BACKUP_CAP];
    uint16_t cursor = 0;
    uint8_t name_len = (uint8_t)strlen(b.Name);
    uint16_t need = (uint16_t)(1 + name_len + 2 + 2 + (uint16_t)b.entry_count * 6);
    if (need > sizeof(buf))
        return false;

    buf[cursor++] = name_len;
    if (name_len) { memcpy(buf + cursor, b.Name, name_len); cursor += name_len; }
    memcpy(buf + cursor, &b.type, 2); cursor += 2;
    memcpy(buf + cursor, &b.entry_count, 2); cursor += 2;
    for (uint16_t i = 0; i < b.entry_count; i++)
    {
        const DynamicEntry &e = b.table[i];
        memcpy(buf + cursor, &e.fieldKey, 2); cursor += 2;
        memcpy(buf + cursor, &e.flagsAndType, 2); cursor += 2;
        buf[cursor++] = e.size;
        buf[cursor++] = 0;
    }

    char tn[8], vn[8];
    DynamicTableName(idx, tn);
    DynamicValuesName(idx, vn);
    if (!WriteBackupFile(tn, buf, cursor))
        return false;

    if (b.persistent_len)
    {
        if (b.persistent_len > sizeof(buf))
        {
            DeleteDynamicBlockFiles(idx);
            return false;
        }
        memcpy(buf, b.persistent_data, b.persistent_len);
        if (!WriteBackupFile(vn, buf, b.persistent_len))
        {
            DeleteDynamicBlockFiles(idx);
            return false;
        }
    }
    else if (Storage.FileExists(vn) != 0xFFFFFFFF)
    {
        Storage.DeleteFile(vn); // block has no persistent values; drop a stale DV
    }
    return true;
}

// Loads one block from its DT+DV files into `b` (fresh). Returns false when no DT file
// exists (an empty/tombstone slot). The persistent space is restored; the volatile
// space is zero-filled (its values are not persisted).
static bool LoadDynamicBlockFiles(DynamicBlockDescriptor &b, uint16_t idx)
{
    char tn[8], vn[8];
    DynamicTableName(idx, tn);
    DynamicValuesName(idx, vn);
    uint8_t tbuf[MEMORY_BACKUP_CAP], vbuf[MEMORY_BACKUP_CAP];
    uint16_t tlen = ReadBackupFile(tn, tbuf, sizeof(tbuf));
    if (tlen == 0)
        return false;
    uint16_t vlen = ReadBackupFile(vn, vbuf, sizeof(vbuf));

    uint16_t cursor = 0;
    if (cursor + 1 > tlen) return false;
    uint8_t name_len = tbuf[cursor++];
    if (cursor + name_len > tlen) return false;
    if (name_len > BLOCK_NAME_LEN - 1) return false; // malformed table: name would overflow
    if (name_len) memcpy(b.Name, tbuf + cursor, name_len);
    b.Name[name_len] = '\0';
    cursor += name_len;
    if (cursor + 2 > tlen) return false;
    memcpy(&b.type, tbuf + cursor, 2); cursor += 2;
    if (cursor + 2 > tlen) return false;
    uint16_t entry_count; memcpy(&entry_count, tbuf + cursor, 2); cursor += 2;
    if (cursor + (uint16_t)entry_count * 6 > tlen) return false;
    if (entry_count && !b.EnsureTable(entry_count)) return false;

    uint16_t p_needed = 0, v_needed = 0;
    for (uint16_t i = 0; i < entry_count; i++)
    {
        DynamicEntry &e = b.table[i];
        memcpy(&e.fieldKey, tbuf + cursor, 2);
        memcpy(&e.flagsAndType, tbuf + cursor + 2, 2);
        e.size = tbuf[cursor + 4];
        e.pad = tbuf[cursor + 5];
        (e.flagsAndType & FieldFlags::Persistent ? p_needed : v_needed) += e.size;
        cursor += 6;
    }
    b.entry_count = entry_count;
    if (p_needed != vlen)
        return false; // DV length must equal the table's persistent size

    if (v_needed)
    {
        b.volatile_data = (uint8_t *)malloc(v_needed);
        if (!b.volatile_data) return false;
        b.volatile_allocated = b.volatile_len = v_needed;
        memset(b.volatile_data, 0, v_needed);
    }
    if (p_needed)
    {
        b.persistent_data = (uint8_t *)malloc(p_needed);
        if (!b.persistent_data) return false;
        b.persistent_allocated = b.persistent_len = p_needed;
    }
    uint16_t po = 0, vo = 0;
    for (uint16_t i = 0; i < entry_count; i++)
    {
        DynamicEntry &e = b.table[i];
        if (e.flagsAndType & FieldFlags::Persistent)
        {
            if (e.size) memcpy(b.persistent_data + po, vbuf + po, e.size);
            e.memoryOffset = po;
            po += e.size;
        }
        else
        {
            e.memoryOffset = vo;
            vo += e.size;
        }
    }
    return true;
}
#endif
