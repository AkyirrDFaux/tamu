#pragma once

// The runtime block descriptors and the generic registry
//
// Part of Memory.h, split for readability; included from there.

#include "Core/Functions/MemoryBackup.h"
#include <cstdlib>

// Value alignment (Docs/Services/Register.md "Memory with 32-bit alignment"): a value is
// aligned to its own size, capped at 4 - 1 -> 1, 2 -> 2, >= 3 -> 4. Offsets stay byte offsets.
static inline uint16_t AlignValue(uint16_t off, uint8_t size) {
    uint16_t a = (size >= 3) ? 4 : (size == 2 ? 2 : 1);
    return (uint16_t)((off + a - 1) & ~(uint16_t)(a - 1));
}

// One entry in a dynamic block's table (Docs/Services/Register.md "Dynamic Block Table"):
// Field&Key(16) + MemoryOffset(16) + ValueInfo(32).
struct DynamicEntry
{
    uint16_t fieldKey;     // (field << 8) | key
    uint16_t memoryOffset; // offset into the entry's volatile/persistent value space
    ValueInfo info;        // type + size + flags
};

// A dynamic lookup result: the meta (ValueInfo) + a pointer to the value bytes.
struct KeyResult
{
    ValueInfo meta = { (uint16_t)DataType::Unknown, 0, 0 };
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
    char Name[BLOCK_NAME_LEN] = {};
    uint32_t generation = 0;
    // A live block occupies its slot; a tombstone is an empty slot that keeps the position
    // stable (Docs/Services/Register.md: "Create/write is in specified place"). The block's
    // bank type is derived from its global index (BlockTypeRange::DynamicType), not stored.
    bool present = false;

    bool IsPersistent(const DynamicEntry &e) const { return ValueIsPersistent(e.info); }
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
        uint16_t offset = AlignValue(used, len);
        if (offset + len > alloc)
        {
            uint16_t new_cap = offset + len + 16;
            uint8_t *nb = (uint8_t *)realloc(sp, new_cap);
            if (!nb) return 0xFFFF;
            sp = nb;
            alloc = new_cap;
        }
        if (len) memcpy(sp + offset, value, len);
        used = offset + len;
        return offset;
    }

    // Rebuilds the value spaces (compaction): copies every entry's value from its
    // current space into fresh compacted buffers and updates memoryOffset. Also moves
    // values between spaces when the Persistent flag changed. Called after any change.
    bool RebuildSpaces()
    {
        uint16_t v_needed = 0, p_needed = 0;
        for (uint16_t i = 0; i < entry_count; i++)
        {
            uint16_t &n = IsPersistent(table[i]) ? p_needed : v_needed;
            n = AlignValue(n, table[i].info.Size);
            n += table[i].info.Size;
        }

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
            off = AlignValue(off, e.info.Size);
            if (e.info.Size && src)
                memcpy(dst + off, src + e.memoryOffset, e.info.Size);
            e.memoryOffset = off;
            off += e.info.Size;
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
    bool SetEntry(uint8_t field, uint8_t key, const void *value, uint8_t len, const ValueInfo &desc)
    {
        uint16_t fk = MakeFieldKey(field, key);
        uint16_t i = FindEntry(fk);
        bool exists = (i < entry_count && table[i].fieldKey == fk);

        // Read Only (Docs "ValueInfo Flags"): a stored read-only entry is non-writable from
        // outside, so neither its value nor its flags can change (the delete included).
        if (exists && ValueIsReadOnly(table[i].info))
            return false;

        if (ValueInfoType(desc) == (uint16_t)DataType::None)
            return DeleteEntry(field, key);

        ValueInfo info = desc;
        info.Size = len;              // the stored length is the bytes actually present
        info.Flags &= ~ValueTrigger;  // the Trigger flag is static-only (Docs: dynamic triggers are gone)
        bool pers = ValueIsPersistent(info);

        if (exists)
        {
            // Fast path: same size and persistence - overwrite the value in place. This is
            // the hot path (e.g. a script rewriting the same render matrix every tick) and
            // avoids the append + full-space compaction.
            if (table[i].info.Size == len && IsPersistent(table[i]) == pers)
            {
                uint8_t *sp = pers ? persistent_data : volatile_data;
                if (len && sp) memcpy(sp + table[i].memoryOffset, value, len);
                table[i].info = info;
                generation++;
                return true;
            }
            // Size/persistence changed: stash the new value at the end of its space first,
            // then RebuildSpaces compacts (reads the new value from the tail).
            uint16_t off = AppendValue(pers, value, len);
            if (off == 0xFFFF) return false;
            table[i].info = info;
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
            e.info = info;
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
        // Read Only (Docs "ValueInfo Flags"): a stored read-only entry is non-writable from
        // outside, the delete included.
        if (ValueIsReadOnly(table[i].info)) return false;
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
        res.meta = e.info;
        res.data_len = e.info.Size;
        res.exists = true;
        res.data_ptr = e.info.Size ? (Space(IsPersistent(e)) + e.memoryOffset) : nullptr;
        return res;
    }

    // Reads a keyed value as type `T`, falling back to `defaultValue` when missing or
    // of a different type.
    template <typename T>
    T GetKeyValue(uint16_t field, uint8_t key, DataType expectedType, T defaultValue = 0)
    {
        KeyResult res = GetKey((uint8_t)field, key);
        // The allocator aligns each value to its size (capped at 4), so a value of type T is
        // aligned for T - a direct load.
        if (res.data_ptr && (ValueInfoType(res.meta) == (uint16_t)expectedType))
            return *reinterpret_cast<const T *>(res.data_ptr);
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
        memset(Name, 0, sizeof(Name));
        present = false;
    }
};

// Generic block registry holding `T` (DynamicBlockDescriptor).
template <typename T>
struct BlockRegistry
{
    T *blocks = nullptr;
    uint16_t block_count = 0;
    uint16_t block_allocated = 0;

    // Appends a new live empty block, growing the descriptor array as needed.
    bool AddBlock()
    {
        if (!Grow()) return false;
        blocks[block_count] = T();
        blocks[block_count].present = true;
        block_count++;
        return true;
    }

    // Appends a tombstone (an empty slot that reserves a position).
    bool AddTombstone()
    {
        if (!Grow()) return false;
        blocks[block_count] = T();
        block_count++;
        return true;
    }

    // Creates a live block at a specified position (docs "create/write is in specified
    // place, not an append necessarily"): fills a tombstone slot, pads with tombstones
    // beyond, or inserts (shifting later blocks) when the slot is occupied.
    bool AddBlockAt(uint16_t index)
    {
        if (index >= block_count)
        {
            while (block_count < index)
                if (!AddTombstone()) return false;
            return AddBlock();
        }
        if (!blocks[index].present)
        {
            blocks[index] = T();
            blocks[index].present = true;
            return true;
        }
        // Insert: shift the occupied slot and everything after it right by one.
        if (!AddTombstone()) return false;
        for (uint16_t i = block_count - 1; i > index; i--)
            blocks[i] = blocks[i - 1];
        blocks[index] = T();
        blocks[index].present = true;
        return true;
    }

    // Marks the block at `index` as a tombstone (keeps the slot; no compaction).
    void TombstoneBlock(uint16_t index)
    {
        if (index >= block_count) return;
        blocks[index].Release();
        blocks[index] = T();
        blocks[index].present = false;
    }

    // Returns the block descriptor at `index`, or nullptr if out of range.
    T *GetBlock(uint16_t index)
    {
        if (index >= block_count)
            return nullptr;
        return &blocks[index];
    }

private:
    bool Grow()
    {
        if (block_count + 1 <= block_allocated) return true;
        uint16_t new_cap = block_allocated + 4;
        T *new_blocks = (T *)realloc(blocks, new_cap * sizeof(T));
        if (!new_blocks)
            return false;
        memset((void *)(new_blocks + block_allocated), 0, (new_cap - block_allocated) * sizeof(T));
        blocks = new_blocks;
        block_allocated = new_cap;
        return true;
    }
};

using DynamicRegistry = BlockRegistry<DynamicBlockDescriptor>;

