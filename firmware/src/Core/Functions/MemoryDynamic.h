#pragma once

// The dynamic-block registry, naming, cleanup and persistence
//
// Part of Memory.h, split for readability; included from there.

#include "Core/Functions/MemoryBlocks.h"
#include <cstring>
#include <cstdlib>

#ifdef USE_DYNAMIC_BLOCKS
// One registry instance per dynamic memory service (defined in their service files).
extern DynamicRegistry dynamic_block_registry;
DynamicRegistry dynamic_block_registry;

// Creates a dynamic block at position `index` (see AddBlockAt). Returns the block or nullptr.
//
// GCC's -Warray-bounds (only enabled at -O2+) cannot follow the realloc'd registry storage and
// reports the memcpy destination as a zero-length object. It is a false positive: the
// descriptor's Name is BLOCK_NAME_LEN bytes, `len` is clamped to BLOCK_NAME_LEN-1 here, and the
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
    uint16_t len = name_len;
    if (len > BLOCK_NAME_LEN - 1) len = BLOCK_NAME_LEN - 1;
    if (len && name) memcpy(block.Name, name, len);
    block.Name[len] = '\0';
    return &block;
}
#pragma GCC diagnostic pop

// Per-block dynamic persistence (Docs/Services/Register.md: DT_XXX / DV_XXX files).
// A block's table lives in "DT_<hex2>", its persistent value space in "DV_<hex2>".
// The dynamic memory is four banked block types (0x3F0-0x3F3) of 64 instances each, addressed
// by one global index 0..255 (Docs/Services/Register.md "Block types"). The file name carries
// that global index in hex.
#define MAX_DYNAMIC_BLOCKS 256

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
                     dynamic_block_registry.blocks[i].present);
        if (!live)
            DeleteDynamicBlockFiles(i);
    }
}

// Writes the block's DT (table) + DV (persistent value space) files atomically via the
// staging/rename helper. Volatile values are NOT persisted (docs: only the Persistent
// space is saved; the table keeps all entries).
//
// DT layout (Docs/Services/Register.md "Dynamic Block Table"): Name (16 chars, NUL-padded),
// entry count (uint16), 16-bit reserved padding, then the entries. The block's bank type is
// not stored: it is derived from the file's global index.
static bool SaveDynamicBlockFiles(const DynamicBlockDescriptor &b, uint16_t idx)
{
    uint8_t buf[MEMORY_BACKUP_CAP];
    uint16_t cursor = 0;
    uint16_t need = (uint16_t)(BLOCK_NAME_LEN + 2 + 2 + (uint16_t)b.entry_count * (2 + (int)sizeof(ValueInfo)));
    if (need > sizeof(buf))
        return false;

    memcpy(buf + cursor, b.Name, BLOCK_NAME_LEN); cursor += BLOCK_NAME_LEN;
    memcpy(buf + cursor, &b.entry_count, 2); cursor += 2;
    uint16_t reserved = 0; memcpy(buf + cursor, &reserved, 2); cursor += 2;
    for (uint16_t i = 0; i < b.entry_count; i++)
    {
        const DynamicEntry &e = b.table[i];
        memcpy(buf + cursor, &e.fieldKey, 2); cursor += 2;
        memcpy(buf + cursor, &e.info, sizeof(ValueInfo)); cursor += sizeof(ValueInfo);
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
    if (cursor + BLOCK_NAME_LEN + 4 > tlen) return false;
    memcpy(b.Name, tbuf + cursor, BLOCK_NAME_LEN);
    b.Name[BLOCK_NAME_LEN - 1] = '\0';
    cursor += BLOCK_NAME_LEN;
    uint16_t entry_count; memcpy(&entry_count, tbuf + cursor, 2); cursor += 2;
    cursor += 2; // 16-bit reserved padding (Docs "Dynamic Block Table")
    if (cursor + (uint16_t)entry_count * (2 + (int)sizeof(ValueInfo)) > tlen) return false;
    if (entry_count && !b.EnsureTable(entry_count)) return false;

    uint16_t p_needed = 0, v_needed = 0;
    for (uint16_t i = 0; i < entry_count; i++)
    {
        DynamicEntry &e = b.table[i];
        memcpy(&e.fieldKey, tbuf + cursor, 2);
        memcpy(&e.info, tbuf + cursor + 2, sizeof(ValueInfo));
        {
            uint16_t &n = ValueIsPersistent(e.info) ? p_needed : v_needed;
            n = AlignValue(n, e.info.Size);
            n += e.info.Size;
        }
        cursor += 2 + sizeof(ValueInfo);
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
        if (ValueIsPersistent(e.info))
        {
            po = AlignValue(po, e.info.Size);
            if (e.info.Size) memcpy(b.persistent_data + po, vbuf + po, e.info.Size);
            e.memoryOffset = po;
            po += e.info.Size;
        }
        else
        {
            vo = AlignValue(vo, e.info.Size);
            e.memoryOffset = vo;
            vo += e.info.Size;
        }
    }
    b.present = true;
    return true;
}
#endif
