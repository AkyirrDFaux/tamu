#pragma once

// The dynamic-block registry, naming, cleanup and persistence
//
// Part of Memory.h, split for readability; included from there.

#include "Core/Functions/MemoryBlocks.h"
#include <cstring>
#include <cstdlib>

#ifdef USE_DYNAMIC_BLOCKS
// One registry instance per dynamic memory service (defined in their service files).
DynamicRegistry DynamicBlockRegistry;

// Creates a dynamic block at position `index` (see AddBlockAt). Returns the block or nullptr.
//
// GCC's -Warray-bounds (only enabled at -O2+) cannot follow the realloc'd registry storage and
// reports the name copy's destination as a zero-length object. It is a false positive: the
// descriptor's Name is BLOCK_NAME_LEN bytes, `SetBlockName` clamps `name_len` to it, and the
// caller's source is bounded by the payload length. Suppress it at this site rather than
// disarming the warning project-wide.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
static DynamicBlockDescriptor *CreateDynamicBlock(const uint8_t *Name, uint16_t NameLen, uint16_t Index)
{
    if (!DynamicBlockRegistry.AddBlockAt(Index))
        return nullptr;
    DynamicBlockDescriptor &Block = *DynamicBlockRegistry.GetBlock(Index);
    SetBlockName(Block.Name, (const char *)Name, NameLen);
    return &Block;
}
#pragma GCC diagnostic pop

// Per-block dynamic persistence (Docs/Services/Register.md: .DT_XX / .DV_XX files).
// A block's table lives in ".DT_<hex2>", its persistent value space in ".DV_<hex2>".
// The dynamic memory is four banked block types (0x3F0-0x3F3) of 64 instances each, addressed
// by one global index 0..255 (Docs/Services/Register.md "Block types"). The file name carries
// that global index in hex.
#define MAX_DYNAMIC_BLOCKS 256

// ".D<kind>_<hex2>" space padded to 8 chars (kind = T or V).
static void HexIndexName(char Kind, uint16_t Idx, char Out[8])
{
    const char *Hex = "0123456789ABCDEF";
    Out[0] = '.';
    Out[1] = 'D';
    Out[2] = Kind;
    Out[3] = '_';
    Out[4] = Hex[(Idx >> 4) & 0xF];
    Out[5] = Hex[Idx & 0xF];
    Out[6] = Out[7] = ' ';
}
static void DynamicTableName(uint16_t Idx, char Out[8]) { HexIndexName('T', Idx, Out); }
static void DynamicValuesName(uint16_t Idx, char Out[8]) { HexIndexName('V', Idx, Out); }

// True when `name` is one of a dynamic block's .DT_/.DV_ files (or their "~" staging forms),
// writing the global block index to `*idx`.
static bool ParseDynamicFileName(const char Name[8], uint16_t *Idx)
{
    if (Name[0] != '.' || Name[1] != 'D' || (Name[2] != 'T' && Name[2] != 'V') || Name[3] != '_')
        return false;
    const char *Hex = "0123456789ABCDEF";
    int Hi = -1, Lo = -1;
    for (int k = 0; k < 16; k++) {
        if (Name[4] == Hex[k]) Hi = k;
        if (Name[5] == Hex[k]) Lo = k;
    }
    if (Hi < 0 || Lo < 0) return false;
    *Idx = (uint16_t)((Hi << 4) | Lo);
    return true;
}

// Removes a block's DT/DV files (tombstoned slots must not leave files behind), plus any
// staging names a half-finished atomic write left behind. DeleteFile is a no-op on an absent
// name, so no FileExists guard is needed.
static void DeleteDynamicBlockFiles(uint16_t Idx)
{
    char Tn[8], Vn[8], Ttn[8], Tvn[8];
    DynamicTableName(Idx, Tn);
    DynamicValuesName(Idx, Vn);
    BackupTempName(Tn, Ttn);
    BackupTempName(Vn, Tvn);
    Storage.DeleteFile(Tn);
    Storage.DeleteFile(Vn);
    Storage.DeleteFile(Ttn);
    Storage.DeleteFile(Tvn);
}

// Cleans orphaned DT/DV files for tombstoned or beyond-registry slots. Called before
// every save. File cleanup never changes registry positions. Enumerates only the DT/DV files
// that actually exist instead of probing all 256 slots (their name derives the index).
//
// The stale slots are collected first and deleted only once the walk has finished. ForEachFile
// reads the table lazily by index and every create/delete rewrites or moves that table, so
// deleting from inside the callback leaves the walk reading records that have shifted underneath
// it: it then both misses files and leaves the table in a state the save that follows records
// into, which loses the DT/DV files it has just written.
static void CleanupDynamicFiles()
{
    uint16_t Stale[MAX_DYNAMIC_BLOCKS];
    uint16_t StaleCount = 0;
    Storage.ForEachFile([&](const char Name[8]) {
        uint16_t Idx;
        if (!ParseDynamicFileName(Name, &Idx)) return;
        bool Live = (Idx < DynamicBlockRegistry.BlockCount &&
                     DynamicBlockRegistry.Blocks[Idx].Present);
        if (Live) return;
        for (uint16_t k = 0; k < StaleCount; k++)
            if (Stale[k] == Idx) return; // .DT_ and .DV_ name the same slot
        if (StaleCount < MAX_DYNAMIC_BLOCKS)
            Stale[StaleCount++] = Idx;
    });
    for (uint16_t k = 0; k < StaleCount; k++)
        DeleteDynamicBlockFiles(Stale[k]);
}

// Writes the block's DT (table) + DV (persistent value space) files atomically via the
// staging/rename helper. Volatile values are NOT persisted (docs: only the Persistent
// space is saved; the table keeps all entries).
//
// DT layout (Docs/Services/Register.md "Dynamic Block Table"): Name (16 chars, NUL-padded),
// entry count (uint16), 16-bit reserved padding, then the entries, each
// Field&Key (uint16) + MemoryOffset (uint16) + ValueInfo. The block's bank type is not
// stored: it is derived from the file's global index.
static bool SaveDynamicBlockFiles(const DynamicBlockDescriptor &Block, uint16_t Idx)
{
    char Tn[8], Vn[8];
    DynamicTableName(Idx, Tn);
    DynamicValuesName(Idx, Vn);
    uint8_t Buf[MEMORY_BACKUP_CAP];

    uint16_t Need = (uint16_t)(BLOCK_NAME_LEN + 2 + 2 + (uint16_t)Block.EntryCount * (2 + 2 + (int)sizeof(ValueInfo)));
    if (Need > sizeof(Buf))
        return false;

    // Write the persistent value space FIRST so the table (the block's existence marker) is
    // the commit point: a power cut between the two commits leaves a new DV beside the old DT,
    // and the loader tolerates a DV/table size mismatch rather than tombstoning the block.
    if (Block.PersistentLen)
    {
        if (Block.PersistentLen > sizeof(Buf))
            return false;
        memcpy(Buf, Block.PersistentData, Block.PersistentLen);
        if (!WriteBackupFile(Vn, Buf, Block.PersistentLen))
            return false;
    }
    else if (Storage.FileExists(Vn) != 0xFFFFFFFF)
    {
        Storage.DeleteFile(Vn); // block has no persistent values; drop a stale DV
    }

    // Build the DT (table) and commit it last.
    uint16_t Cursor = 0;
    memcpy(Buf + Cursor, Block.Name, BLOCK_NAME_LEN); Cursor += BLOCK_NAME_LEN;
    memcpy(Buf + Cursor, &Block.EntryCount, 2); Cursor += 2;
    uint16_t Reserved = 0; memcpy(Buf + Cursor, &Reserved, 2); Cursor += 2;
    for (uint16_t i = 0; i < Block.EntryCount; i++)
    {
        const DynamicEntry &Entry = Block.Table[i];
        memcpy(Buf + Cursor, &Entry.FieldKey, 2); Cursor += 2;
        memcpy(Buf + Cursor, &Entry.MemoryOffset, 2); Cursor += 2;
        memcpy(Buf + Cursor, &Entry.Info, sizeof(ValueInfo)); Cursor += sizeof(ValueInfo);
    }
    return WriteBackupFile(Tn, Buf, Cursor);
}

// Loads one block from its DT+DV files into `b` (fresh). Returns false when no DT file
// exists (an empty/tombstone slot). The persistent space is restored; the volatile
// space is zero-filled (its values are not persisted).
static bool LoadDynamicBlockFiles(DynamicBlockDescriptor &Block, uint16_t Idx)
{
    char Tn[8], Vn[8];
    DynamicTableName(Idx, Tn);
    DynamicValuesName(Idx, Vn);
    uint8_t Tbuf[MEMORY_BACKUP_CAP], Vbuf[MEMORY_BACKUP_CAP];
    uint16_t Tlen = ReadBackupFile(Tn, Tbuf, sizeof(Tbuf));
    if (Tlen == 0)
        return false;
    uint16_t Vlen = ReadBackupFile(Vn, Vbuf, sizeof(Vbuf));

    uint16_t Cursor = 0;
    if (Cursor + BLOCK_NAME_LEN + 4 > Tlen) return false;
    memcpy(Block.Name, Tbuf + Cursor, BLOCK_NAME_LEN);
    Cursor += BLOCK_NAME_LEN;
    uint16_t EntryCount; memcpy(&EntryCount, Tbuf + Cursor, 2); Cursor += 2;
    Cursor += 2; // 16-bit reserved padding (Docs "Dynamic Block Table")
    if (Cursor + (uint16_t)EntryCount * (2 + 2 + (int)sizeof(ValueInfo)) > Tlen) return false;
    if (EntryCount && !Block.EnsureTable(EntryCount)) return false;

    // The stored MemoryOffset is the file's source of truth (Docs "Dynamic Block Table");
    // each space's size follows from it.
    uint16_t PNeeded = 0, VNeeded = 0;
    for (uint16_t i = 0; i < EntryCount; i++)
    {
        DynamicEntry &Entry = Block.Table[i];
        memcpy(&Entry.FieldKey, Tbuf + Cursor, 2);
        memcpy(&Entry.MemoryOffset, Tbuf + Cursor + 2, 2);
        memcpy(&Entry.Info, Tbuf + Cursor + 4, sizeof(ValueInfo));
        uint16_t &Needed = ValueIsPersistent(Entry.Info) ? PNeeded : VNeeded;
        uint16_t End = (uint16_t)(Entry.MemoryOffset + Entry.Info.Size);
        if (End > Needed) Needed = End;
        Cursor += 2 + 2 + sizeof(ValueInfo);
    }
    Block.EntryCount = EntryCount;

    if (VNeeded)
    {
        Block.VolatileData = (uint8_t *)malloc(VNeeded);
        if (!Block.VolatileData) return false;
        Block.VolatileAllocated = Block.VolatileLen = VNeeded;
        memset(Block.VolatileData, 0, VNeeded);
    }
    if (PNeeded)
    {
        Block.PersistentData = (uint8_t *)malloc(PNeeded);
        if (!Block.PersistentData) return false;
        Block.PersistentAllocated = Block.PersistentLen = PNeeded;
        // The DV holds the compacted persistent space. A power cut can leave it from a
        // different generation (its length need not match the table); copy the overlap and
        // keep the block instead of tombstoning it (any tail defaults to zero).
        memset(Block.PersistentData, 0, PNeeded);
        uint16_t Copy = (Vlen < PNeeded) ? Vlen : PNeeded;
        if (Copy)
            memcpy(Block.PersistentData, Vbuf, Copy);
    }
    Block.Present = true;
    return true;
}
#endif
