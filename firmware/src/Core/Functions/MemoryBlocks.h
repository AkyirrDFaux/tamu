#pragma once

// The runtime block descriptors and the generic registry
//
// Part of Memory.h, split for readability; included from there.

#include "Core/Functions/MemoryBackup.h"
#include <cstdlib>

// Value alignment (Docs/Services/Register.md "Memory with 32-bit alignment"): a value is
// aligned to its own size, capped at 4 - 1 -> 1, 2 -> 2, >= 3 -> 4. Offsets stay byte offsets.
static inline uint16_t AlignValue(uint16_t Off, uint8_t Size) {
    uint16_t AlignBy = (Size >= 3) ? 4 : (Size == 2 ? 2 : 1);
    return (uint16_t)((Off + AlignBy - 1) & ~(uint16_t)(AlignBy - 1));
}

// One entry in a dynamic block's table (Docs/Services/Register.md "Dynamic Block Table"):
// Field&Key(16) + MemoryOffset(16) + ValueInfo(32).
struct DynamicEntry
{
    uint16_t FieldKey;     // (field << 8) | key
    uint16_t MemoryOffset; // offset into the entry's volatile/persistent value space
    ValueInfo Info;        // type + size + flags
};

// A dynamic lookup result: the meta (ValueInfo) + a pointer to the value bytes.
struct KeyResult
{
    ValueInfo Meta = { (uint16_t)DataType::None, 0, 0 };
    void *DataPtr = nullptr;
    uint16_t DataLen = 0;
    bool Exists = false; // entry present (a size-0 entry exists with no value)
};

struct DynamicBlockDescriptor
{
    DynamicEntry *Table = nullptr;
    uint16_t EntryCount = 0;
    uint16_t EntryAllocated = 0;
    uint8_t *VolatileData = nullptr;
    uint16_t VolatileLen = 0, VolatileAllocated = 0;
    uint8_t *PersistentData = nullptr;
    uint16_t PersistentLen = 0, PersistentAllocated = 0;
    char Name[BLOCK_NAME_LEN] = {};
    uint32_t Generation = 0;
    // A live block occupies its slot; a tombstone is an empty slot that keeps the position
    // stable (Docs/Services/Register.md: "Create/write is in specified place"). The block's
    // bank type is derived from its global index (BlockTypeRange::DynamicType), not stored.
    bool Present = false;

    bool IsPersistent(const DynamicEntry &Entry) const { return ValueIsPersistent(Entry.Info); }
    uint8_t *Space(bool Persistent) const { return Persistent ? PersistentData : VolatileData; }
    uint16_t &SpaceLen(bool Persistent) { return Persistent ? PersistentLen : VolatileLen; }
    uint16_t &SpaceAlloc(bool Persistent) { return Persistent ? PersistentAllocated : VolatileAllocated; }
    uint8_t *&SpaceData(bool Persistent) { return Persistent ? PersistentData : VolatileData; }

    // Binary search for `fieldKey`; returns the insert position (first >= fieldKey).
    uint16_t FindEntry(uint16_t FieldKey) const
    {
        uint16_t Lo = 0, Hi = EntryCount;
        while (Lo < Hi)
        {
            uint16_t Mid = (Lo + Hi) >> 1;
            if (Table[Mid].FieldKey < FieldKey) Lo = Mid + 1; else Hi = Mid;
        }
        return Lo;
    }

    bool EnsureTable(uint16_t Count)
    {
        if (Count <= EntryAllocated) return true;
        uint16_t NewCap = Count + 4;
        DynamicEntry *Nt = (DynamicEntry *)realloc(Table, NewCap * sizeof(DynamicEntry));
        if (!Nt) return false;
        Table = Nt;
        EntryAllocated = NewCap;
        return true;
    }

    // Appends `value` to the value space chosen by `persistent`; returns the offset
    // (0xFFFF on allocation failure).
    uint16_t AppendValue(bool Persistent, const void *Value, uint8_t Len)
    {
        uint16_t &Used = SpaceLen(Persistent);
        uint16_t &Alloc = SpaceAlloc(Persistent);
        uint8_t *&Sp = SpaceData(Persistent);
        uint16_t Offset = AlignValue(Used, Len);
        if (Offset + Len > Alloc)
        {
            uint16_t NewCap = Offset + Len + 16;
            uint8_t *Nb = (uint8_t *)realloc(Sp, NewCap);
            if (!Nb) return 0xFFFF;
            Sp = Nb;
            Alloc = NewCap;
        }
        if (Len) memcpy(Sp + Offset, Value, Len);
        Used = Offset + Len;
        return Offset;
    }

    // Rebuilds the value spaces (compaction): copies every entry's value from its
    // current space into fresh compacted buffers and updates memoryOffset. Also moves
    // values between spaces when the Persistent flag changed. Called after any change.
    bool RebuildSpaces()
    {
        uint16_t VNeeded = 0, PNeeded = 0;
        for (uint16_t i = 0; i < EntryCount; i++)
        {
            uint16_t &Needed = IsPersistent(Table[i]) ? PNeeded : VNeeded;
            Needed = AlignValue(Needed, Table[i].Info.Size);
            Needed += Table[i].Info.Size;
        }

        uint8_t *Nv = VNeeded ? (uint8_t *)malloc(VNeeded) : nullptr;
        uint8_t *Np = PNeeded ? (uint8_t *)malloc(PNeeded) : nullptr;
        if ((VNeeded && !Nv) || (PNeeded && !Np)) {
            // Out of memory: skip compaction. The value just written already sits at its
            // entry's memoryOffset in the right space and the other entries are untouched,
            // so the block stays consistent (just less compact) instead of dereferencing
            // a null buffer.
            if (Nv) free(Nv);
            if (Np) free(Np);
            return true;
        }
        uint16_t Vo = 0, Po = 0;
        for (uint16_t i = 0; i < EntryCount; i++)
        {
            DynamicEntry &Entry = Table[i];
            bool Pers = IsPersistent(Entry);
            uint8_t *Src = Pers ? PersistentData : VolatileData;
            uint8_t *Dst = Pers ? Np : Nv;
            uint16_t &Off = Pers ? Po : Vo;
            Off = AlignValue(Off, Entry.Info.Size);
            if (Entry.Info.Size && Src)
                memcpy(Dst + Off, Src + Entry.MemoryOffset, Entry.Info.Size);
            Entry.MemoryOffset = Off;
            Off += Entry.Info.Size;
        }
        if (VolatileData) free(VolatileData);
        if (PersistentData) free(PersistentData);
        VolatileData = Nv;
        PersistentData = Np;
        VolatileLen = Vo;
        PersistentLen = Po;
        VolatileAllocated = VNeeded;
        PersistentAllocated = PNeeded;
        return true;
    }

    // Writes the value at (field, key), creating/updating the sorted entry.
    // Writing type None deletes the entry (docs "Setting the type to None deletes").
    bool SetEntry(uint8_t Field, uint8_t Key, const void *Value, uint8_t Len, const ValueInfo &Desc)
    {
        uint16_t Fk = MakeFieldKey(Field, Key);
        uint16_t i = FindEntry(Fk);
        bool Exists = (i < EntryCount && Table[i].FieldKey == Fk);

        // Read Only (Docs "ValueInfo Flags"): a stored read-only entry is non-writable from
        // outside, so neither its value nor its flags can change (the delete included).
        if (Exists && ValueIsReadOnly(Table[i].Info))
            return false;

        if (ValueInfoType(Desc) == (uint16_t)DataType::None)
            return DeleteEntry(Field, Key);

        ValueInfo Info = Desc;
        Info.Size = Len;              // the stored length is the bytes actually present
        Info.Flags &= ~ValueTrigger;  // the Trigger flag is static-only (Docs: dynamic triggers are gone)
        bool Pers = ValueIsPersistent(Info);

        if (Exists)
        {
            // Fast path: same size and persistence - overwrite the value in place. This is
            // the hot path (e.g. a script rewriting the same render matrix every tick) and
            // avoids the append + full-space compaction.
            if (Table[i].Info.Size == Len && IsPersistent(Table[i]) == Pers)
            {
                uint8_t *Sp = Pers ? PersistentData : VolatileData;
                if (Len && Sp) memcpy(Sp + Table[i].MemoryOffset, Value, Len);
                Table[i].Info = Info;
                Generation++;
                return true;
            }
            // Size/persistence changed: stash the new value at the end of its space first,
            // then RebuildSpaces compacts (reads the new value from the tail).
            uint16_t Off = AppendValue(Pers, Value, Len);
            if (Off == 0xFFFF) return false;
            Table[i].Info = Info;
            Table[i].MemoryOffset = Off;
        }
        else
        {
            uint16_t Off = AppendValue(Pers, Value, Len);
            if (Off == 0xFFFF) return false;
            if (!EnsureTable(EntryCount + 1)) return false;
            for (uint16_t j = EntryCount; j > i; j--)
                Table[j] = Table[j - 1];
            DynamicEntry &Entry = Table[i];
            Entry.FieldKey = Fk;
            Entry.Info = Info;
            Entry.MemoryOffset = Off;
            EntryCount++;
        }
        Generation++;
        return RebuildSpaces();
    }

    // Removes the entry at (field, key) from the table (the record compacts).
    bool DeleteEntry(uint8_t Field, uint8_t Key)
    {
        uint16_t Fk = MakeFieldKey(Field, Key);
        uint16_t i = FindEntry(Fk);
        if (i >= EntryCount || Table[i].FieldKey != Fk) return false;
        // Read Only (Docs "ValueInfo Flags"): a stored read-only entry is non-writable from
        // outside, the delete included.
        if (ValueIsReadOnly(Table[i].Info)) return false;
        for (uint16_t j = i; j + 1 < EntryCount; j++)
            Table[j] = Table[j + 1];
        EntryCount--;
        Generation++;
        return RebuildSpaces();
    }

    // Returns the entry at (field, key) as a KeyResult (meta + value pointer).
    KeyResult GetKey(uint8_t Field, uint8_t Key)
    {
        KeyResult Res;
        uint16_t i = FindEntry(MakeFieldKey(Field, Key));
        if (i >= EntryCount || Table[i].FieldKey != MakeFieldKey(Field, Key))
            return Res;
        const DynamicEntry &Entry = Table[i];
        Res.Meta = Entry.Info;
        Res.DataLen = Entry.Info.Size;
        Res.Exists = true;
        Res.DataPtr = Entry.Info.Size ? (Space(IsPersistent(Entry)) + Entry.MemoryOffset) : nullptr;
        return Res;
    }

    // Reads a keyed value as type `T`, falling back to `defaultValue` when missing or
    // of a different type.
    template <typename Value>
    Value GetKeyValue(uint16_t Field, uint8_t Key, DataType ExpectedType, Value DefaultValue = 0)
    {
        KeyResult Res = GetKey((uint8_t)Field, Key);
        // The allocator aligns each value to its size (capped at 4), so a value of type T is
        // aligned for T - a direct load.
        if (Res.DataPtr && (ValueInfoType(Res.Meta) == (uint16_t)ExpectedType))
            return *reinterpret_cast<const Value *>(Res.DataPtr);
        return DefaultValue;
    }

    // Distinct field indexes present among the entries.
    uint16_t ListFields(uint8_t *Fields, uint16_t Cap) const
    {
        uint16_t Count = 0, Prev = 0xFFFF;
        for (uint16_t i = 0; i < EntryCount; i++)
        {
            uint8_t Field = FieldOf(Table[i].FieldKey);
            if (Field != Prev)
            {
                if (Count < Cap) Fields[Count] = Field;
                Count++;
                Prev = Field;
            }
        }
        return Count;
    }

    // Keys present at `field`.
    uint16_t ListKeys(uint8_t Field, uint8_t *Keys, uint16_t Cap)
    {
        uint16_t Count = 0;
        uint16_t i = FindEntry(MakeFieldKey(Field, 0));
        for (; i < EntryCount && FieldOf(Table[i].FieldKey) == Field; i++)
        {
            if (Count < Cap) Keys[Count] = KeyOf(Table[i].FieldKey);
            Count++;
        }
        return Count;
    }

    // Frees all owned allocations and resets the block to a tombstone-like empty state.
    void Release()
    {
        if (Table) free(Table);
        if (VolatileData) free(VolatileData);
        if (PersistentData) free(PersistentData);
        Table = nullptr;
        VolatileData = nullptr;
        PersistentData = nullptr;
        EntryCount = EntryAllocated = 0;
        VolatileLen = VolatileAllocated = 0;
        PersistentLen = PersistentAllocated = 0;
        memset(Name, 0, sizeof(Name));
        Present = false;
    }
};

// Generic block registry holding `T` (DynamicBlockDescriptor).
template <typename Value>
struct BlockRegistry
{
    Value *Blocks = nullptr;
    uint16_t BlockCount = 0;
    uint16_t BlockAllocated = 0;

    // Appends a new live empty block, growing the descriptor array as needed.
    bool AddBlock()
    {
        if (!Grow()) return false;
        Blocks[BlockCount] = Value();
        Blocks[BlockCount].Present = true;
        BlockCount++;
        return true;
    }

    // Appends a tombstone (an empty slot that reserves a position).
    bool AddTombstone()
    {
        if (!Grow()) return false;
        Blocks[BlockCount] = Value();
        BlockCount++;
        return true;
    }

    // Creates a live block at a specified position (docs "create/write is in specified
    // place, not an append necessarily"): fills a tombstone slot, pads with tombstones
    // beyond, or inserts (shifting later blocks) when the slot is occupied.
    bool AddBlockAt(uint16_t Index)
    {
        if (Index >= BlockCount)
        {
            while (BlockCount < Index)
                if (!AddTombstone()) return false;
            return AddBlock();
        }
        if (!Blocks[Index].Present)
        {
            Blocks[Index] = Value();
            Blocks[Index].Present = true;
            return true;
        }
        // Insert: shift the occupied slot and everything after it right by one.
        if (!AddTombstone()) return false;
        for (uint16_t i = BlockCount - 1; i > Index; i--)
            Blocks[i] = Blocks[i - 1];
        Blocks[Index] = Value();
        Blocks[Index].Present = true;
        return true;
    }

    // Marks the block at `index` as a tombstone (keeps the slot; no compaction).
    void TombstoneBlock(uint16_t Index)
    {
        if (Index >= BlockCount) return;
        Blocks[Index].Release();
        Blocks[Index] = Value();
        Blocks[Index].Present = false;
    }

    // Returns the block descriptor at `index`, or nullptr if out of range.
    Value *GetBlock(uint16_t Index)
    {
        if (Index >= BlockCount)
            return nullptr;
        return &Blocks[Index];
    }

private:
    bool Grow()
    {
        if (BlockCount + 1 <= BlockAllocated) return true;
        uint16_t NewCap = BlockAllocated + 4;
        Value *NewBlocks = (Value *)realloc(Blocks, NewCap * sizeof(Value));
        if (!NewBlocks)
            return false;
        memset((void *)(NewBlocks + BlockAllocated), 0, (NewCap - BlockAllocated) * sizeof(Value));
        Blocks = NewBlocks;
        BlockAllocated = NewCap;
        return true;
    }
};

using DynamicRegistry = BlockRegistry<DynamicBlockDescriptor>;

