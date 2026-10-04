#pragma once

// The dynamic-block registry, naming, cleanup and persistence
//
// Part of Memory.h, split for readability; included from there.

#include "Core/Functions/MemoryBlocks.h"
#include <cstring>
#include <cstdlib>

#ifdef USE_DYNAMIC_BLOCKS
// One registry instance per dynamic memory service (defined in their service files).
DynamicRegistry dynamic_block_registry;

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
static DynamicBlockDescriptor *CreateDynamicBlock(const uint8_t *name, uint16_t name_len, uint16_t index)
{
    if (!dynamic_block_registry.AddBlockAt(index))
        return nullptr;
    DynamicBlockDescriptor &block = *dynamic_block_registry.GetBlock(index);
    SetBlockName(block.Name, (const char *)name, name_len);
    return &block;
}
#pragma GCC diagnostic pop

// Per-block dynamic persistence (Docs/Services/Register.md: .DT_XX / .DV_XX files).
// A block's table lives in ".DT_<hex2>", its persistent value space in ".DV_<hex2>".
// The dynamic memory is four banked block types (0x3F0-0x3F3) of 64 instances each, addressed
// by one global index 0..255 (Docs/Services/Register.md "Block types"). The file name carries
// that global index in hex.
#define MAX_DYNAMIC_BLOCKS 256

// ".D<kind>_<hex2>" space padded to 8 chars (kind = T or V).
static void HexIndexName(char kind, uint16_t idx, char out[8])
{
    const char *hex = "0123456789ABCDEF";
    out[0] = '.';
    out[1] = 'D';
    out[2] = kind;
    out[3] = '_';
    out[4] = hex[(idx >> 4) & 0xF];
    out[5] = hex[idx & 0xF];
    out[6] = out[7] = ' ';
}
static void DynamicTableName(uint16_t idx, char out[8]) { HexIndexName('T', idx, out); }
static void DynamicValuesName(uint16_t idx, char out[8]) { HexIndexName('V', idx, out); }

// True when `name` is one of a dynamic block's .DT_/.DV_ files (or their "~" staging forms),
// writing the global block index to `*idx`.
static bool ParseDynamicFileName(const char name[8], uint16_t *idx)
{
    if (name[0] != '.' || name[1] != 'D' || (name[2] != 'T' && name[2] != 'V') || name[3] != '_')
        return false;
    const char *hex = "0123456789ABCDEF";
    int hi = -1, lo = -1;
    for (int k = 0; k < 16; k++) {
        if (name[4] == hex[k]) hi = k;
        if (name[5] == hex[k]) lo = k;
    }
    if (hi < 0 || lo < 0) return false;
    *idx = (uint16_t)((hi << 4) | lo);
    return true;
}

// Removes a block's DT/DV files (tombstoned slots must not leave files behind), plus any
// staging names a half-finished atomic write left behind. DeleteFile is a no-op on an absent
// name, so no FileExists guard is needed.
static void DeleteDynamicBlockFiles(uint16_t idx)
{
    char tn[8], vn[8], ttn[8], tvn[8];
    DynamicTableName(idx, tn);
    DynamicValuesName(idx, vn);
    BackupTempName(tn, ttn);
    BackupTempName(vn, tvn);
    Storage.DeleteFile(tn);
    Storage.DeleteFile(vn);
    Storage.DeleteFile(ttn);
    Storage.DeleteFile(tvn);
}

// Cleans orphaned DT/DV files for tombstoned or beyond-registry slots. Called before
// every save. File cleanup never changes registry positions. Enumerates only the DT/DV files
// that actually exist instead of probing all 256 slots (their name derives the index).
static void CleanupDynamicFiles()
{
    Storage.ForEachFile([](const char name[8]) {
        uint16_t idx;
        if (!ParseDynamicFileName(name, &idx)) return;
        bool live = (idx < dynamic_block_registry.block_count &&
                     dynamic_block_registry.blocks[idx].present);
        if (!live)
            DeleteDynamicBlockFiles(idx);
    });
}

// Writes the block's DT (table) + DV (persistent value space) files atomically via the
// staging/rename helper. Volatile values are NOT persisted (docs: only the Persistent
// space is saved; the table keeps all entries).
//
// DT layout (Docs/Services/Register.md "Dynamic Block Table"): Name (16 chars, NUL-padded),
// entry count (uint16), 16-bit reserved padding, then the entries, each
// Field&Key (uint16) + MemoryOffset (uint16) + ValueInfo. The block's bank type is not
// stored: it is derived from the file's global index.
static bool SaveDynamicBlockFiles(const DynamicBlockDescriptor &b, uint16_t idx)
{
    char tn[8], vn[8];
    DynamicTableName(idx, tn);
    DynamicValuesName(idx, vn);
    uint8_t buf[MEMORY_BACKUP_CAP];

    uint16_t need = (uint16_t)(BLOCK_NAME_LEN + 2 + 2 + (uint16_t)b.entry_count * (2 + 2 + (int)sizeof(ValueInfo)));
    if (need > sizeof(buf))
        return false;

    // Write the persistent value space FIRST so the table (the block's existence marker) is
    // the commit point: a power cut between the two commits leaves a new DV beside the old DT,
    // and the loader tolerates a DV/table size mismatch rather than tombstoning the block.
    if (b.persistent_len)
    {
        if (b.persistent_len > sizeof(buf))
            return false;
        memcpy(buf, b.persistent_data, b.persistent_len);
        if (!WriteBackupFile(vn, buf, b.persistent_len))
            return false;
    }
    else if (Storage.FileExists(vn) != 0xFFFFFFFF)
    {
        Storage.DeleteFile(vn); // block has no persistent values; drop a stale DV
    }

    // Build the DT (table) and commit it last.
    uint16_t cursor = 0;
    memcpy(buf + cursor, b.Name, BLOCK_NAME_LEN); cursor += BLOCK_NAME_LEN;
    memcpy(buf + cursor, &b.entry_count, 2); cursor += 2;
    uint16_t reserved = 0; memcpy(buf + cursor, &reserved, 2); cursor += 2;
    for (uint16_t i = 0; i < b.entry_count; i++)
    {
        const DynamicEntry &e = b.table[i];
        memcpy(buf + cursor, &e.fieldKey, 2); cursor += 2;
        memcpy(buf + cursor, &e.memoryOffset, 2); cursor += 2;
        memcpy(buf + cursor, &e.info, sizeof(ValueInfo)); cursor += sizeof(ValueInfo);
    }
    return WriteBackupFile(tn, buf, cursor);
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
    if (cursor + BLOCK_NAME_LEN + 4 > tlen) return false;
    memcpy(b.Name, tbuf + cursor, BLOCK_NAME_LEN);
    cursor += BLOCK_NAME_LEN;
    uint16_t entry_count; memcpy(&entry_count, tbuf + cursor, 2); cursor += 2;
    cursor += 2; // 16-bit reserved padding (Docs "Dynamic Block Table")
    if (cursor + (uint16_t)entry_count * (2 + 2 + (int)sizeof(ValueInfo)) > tlen) return false;
    if (entry_count && !b.EnsureTable(entry_count)) return false;

    // The stored MemoryOffset is the file's source of truth (Docs "Dynamic Block Table");
    // each space's size follows from it.
    uint16_t p_needed = 0, v_needed = 0;
    for (uint16_t i = 0; i < entry_count; i++)
    {
        DynamicEntry &e = b.table[i];
        memcpy(&e.fieldKey, tbuf + cursor, 2);
        memcpy(&e.memoryOffset, tbuf + cursor + 2, 2);
        memcpy(&e.info, tbuf + cursor + 4, sizeof(ValueInfo));
        uint16_t &n = ValueIsPersistent(e.info) ? p_needed : v_needed;
        uint16_t end = (uint16_t)(e.memoryOffset + e.info.Size);
        if (end > n) n = end;
        cursor += 2 + 2 + sizeof(ValueInfo);
    }
    b.entry_count = entry_count;

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
        // The DV holds the compacted persistent space. A power cut can leave it from a
        // different generation (its length need not match the table); copy the overlap and
        // keep the block instead of tombstoning it (any tail defaults to zero).
        memset(b.persistent_data, 0, p_needed);
        uint16_t copy = (vlen < p_needed) ? vlen : p_needed;
        if (copy)
            memcpy(b.persistent_data, vbuf, copy);
    }
    b.present = true;
    return true;
}
#endif
