#pragma once

// The fixed-size variant (DAS, USE_FIXED_STORAGE).
//
// Part of Core/Functions/Storage.h (included from there).

#include "Core/Functions/StorageDefs.h"

// ===========================================================================
// USE_FIXED_STORAGE (DAS): reduced variant (Devices.md "Reduced variant"). Files have a
// FIXED size and FIXED positions; the filetable is NOT a real file but a const array within
// code. Read on the file; write on the file only, always erasing pre-write. No allocation,
// no rename/move, no multi-file support. Standalone on purpose - not aligned with the core.
// ===========================================================================
class StorageSystem {
public:
    uint32_t file_table_offset = 0;  // interface parity only (no flash table)
    uint32_t file_table_size   = 0;

    // Fixed filetable: a const array in code (not a file). Each entry gives a file's fixed
    // name, offset and size. The first record is the filetable itself (".TABLE"), the
    // second is the single settings file occupying the whole region from offset 0
    // ("Memory: 256B, single file from offset 0").
    struct FixedFile {
        const char name[8];
        const uint32_t offset;
        const uint32_t size;
    };
    static constexpr FixedFile FixedFiletable[2] = {
        // Self-describing first record: the fixed filetable itself (2 records x 16 B).
        {{'.','T','A','B','L','E',' ',' '}, 0, (uint32_t)(2 * TABLE_ENTRY_SIZE)},
        // The single settings file (the .SV persistent-space mirror), from the storage base.
        {{'.','S','V',' ',' ',' ',' ',' '}, 0, STORAGE_FLASH_SIZE},
    };

    // Only the .SV-family names address the settings file. The WriteBackupFile's temp name
    // (".SV    ~") belongs to the same family, so the single fixed file is shared.
    static bool IsSettingsName(const char name[8]) {
        return name[0]=='.'&&name[1]=='S'&&name[2]=='V';
    }
    // The fixed filetable is browsable through the virtual ".TABLE  " file: the read path
    // serializes the const array into standard FileEntry records for the app.
    static bool IsFixedTableName(const char name[8]) {
        return memcmp(name, ".TABLE  ", 8) == 0;
    }
    static const FixedFile *SettingsFile(const char name[8]) {
        if (!IsSettingsName(name)) return nullptr;
        // Find the settings entry (the first record is the .TABLE itself).
        for (uint32_t i = 0; i < (sizeof(FixedFiletable) / sizeof(FixedFile)); i++) {
            if (IsSettingsName(FixedFiletable[i].name))
                return &FixedFiletable[i];
        }
        return nullptr;
    }

    void Init() {
        // No table to validate - the layout is fixed in code. A fresh/erased region is all
        // 0xFF, which the .SV reader treats as an empty file (a zero-length / missing file).
        // The flash backend still needs its one-time init: opening the partition on the core
        // and, on the DAS, the code/storage overlap reservation guard in Storage_FlashInit.
        if (!Storage_FlashInit())
            DeviceLog("STORAGE", "Storage flash not available!");
    }

    void Format() {
        Storage_FlashFormat();
    }

    uint32_t FileExists(const char name[8]) {
        const FixedFile *f = SettingsFile(name);
        return f ? f->size : 0xFFFFFFFF;
    }

    bool GetFileInfo(const char name[8], uint32_t *offset, uint32_t *size) {
        if (IsFixedTableName(name)) {
            if (offset) *offset = 0;
            if (size)    *size    = (uint32_t)sizeof(FixedFiletable); // serialized table bytes
            return true;
        }
        const FixedFile *f = SettingsFile(name);
        if (!f) return false;
        if (offset) *offset = f->offset;
        if (size)    *size    = f->size;
        return true;
    }

    // Serializes the fixed filetable into the standard FileEntry wire layout (offset,
    // size, name) starting at `content_off`, up to `len` bytes. The table is tiny, so the
    // normal read path always asks for a single fragment starting at offset 0.
    uint16_t CopyFixedTable(uint8_t *out, uint32_t content_off, uint16_t len) {
        const uint32_t table_size = (uint32_t)sizeof(FixedFiletable);
        uint16_t written = 0;
        if (content_off >= table_size) return 0;
        for (uint32_t i = 0; i < (sizeof(FixedFiletable) / sizeof(FixedFile)); i++) {
            if (written >= len) break;
            FileEntry e;
            e.offset = FixedFiletable[i].offset;
            e.size = FixedFiletable[i].size;
            memcpy(e.name, FixedFiletable[i].name, 8);
            uint32_t rec_start = i * TABLE_ENTRY_SIZE;
            if (rec_start + TABLE_ENTRY_SIZE <= content_off) continue;
            uint32_t src = (content_off > rec_start) ? (content_off - rec_start) : 0;
            uint32_t n = TABLE_ENTRY_SIZE - src;
            const uint32_t remaining = (uint32_t)len - written;
            if (n > remaining) n = remaining;
            memcpy(out + written, ((uint8_t *)&e) + src, n);
            written += (uint16_t)n;
        }
        return written;
    }

    bool CreateFile(const char name[8], uint32_t size) {
        if (!SettingsFile(name)) return false;
        // Always erases pre-write: wipe the whole region so the fixed file starts clean.
        return Storage_FlashErase(0, STORAGE_FLASH_SIZE);
    }

    bool WriteToFile(const char name[8], uint32_t offset, uint32_t length, const char *buffer) {
        uint32_t off, sz;
        if (!GetFileInfo(name, &off, &sz)) return false;
        if (offset >= sz) return false;
        length = ClampFileLength(offset, length, sz);
        return Storage_FlashWrite(off + offset, buffer, length);
    }

    uint32_t ReadFromFile(const char name[8], uint32_t offset, uint32_t length, char *buffer) {
        uint32_t off, sz;
        if (!GetFileInfo(name, &off, &sz)) return 0;
        if (offset >= sz) return 0;
        length = ClampFileLength(offset, length, sz);
        return Storage_FlashRead(off + offset, buffer, length);
    }

    // Fixed single file: delete/rename are dead on this variant (there is only the settings
    // mirror, always present, and CIDs 2/4 are compiled out).
    bool ResizeFile(const char name[8], uint32_t new_size) {
        return SettingsFile(name) != nullptr && new_size <= STORAGE_FLASH_SIZE;
    }

    // Interface parity: the fixed layout has a single file, the settings mirror.
    template <typename F>
    void ForEachFile(F fn)
    {
        for (uint32_t i = 0; i < (sizeof(FixedFiletable) / sizeof(FixedFile)); i++)
            if (IsSettingsName(FixedFiletable[i].name)) fn(FixedFiletable[i].name);
    }

    uint32_t UsedFlashBytes() {
        // The settings file (index 1) occupies the storage; the .TABLE is only a code
        // array with no flash footprint.
        const FixedFile *f = SettingsFile(FixedFiletable[1].name);
        return f ? f->size : 0;
    }
} Storage;
