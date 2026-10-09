#pragma once

// Storage geometry, the flash backend hooks and the FileEntry helpers.
//
// Part of Core/Functions/Storage.h (included from there).

#include <cstring>
#include <cstdint>
#include "Core/Functions/Log.h"


// Total storage flash available to the file layer. Constant at compile time: the device
// build flags define it (see platformio.ini), so the directory/data layout never depends on
// runtime values and no RAM is spent caching flash geometry.
#ifndef STORAGE_FLASH_SIZE
#define STORAGE_FLASH_SIZE 0x10000
#endif
// Filesystem block/page size: allocation granularity and erase unit. Defined per device
// (4096 on ESP32-based devices; 64 on the CH32V003 DAS node, matching its hardware erase
// page).
#ifndef STORAGE_BLOCK_SIZE
#define STORAGE_BLOCK_SIZE 4096
#endif
#define PAGE_SIZE STORAGE_BLOCK_SIZE

// Upper bound on allocatable data pages (worst-case flash geometry); sizes the per-call
// usage bitmap in FindSpace (STORAGE_FLASH_SIZE / PAGE_SIZE bits).
#define STORAGE_MAX_BLOCKS ((STORAGE_FLASH_SIZE / PAGE_SIZE) + 1)

// Fixed file-table size, in pages, per device (Docs/Devices.md): 4 pages on Tamu v2.0A, 1 on
// the DAS, 2 on Valu v2.0. The table never grows or shrinks; it is moved only when full
// (Storage.md "MoveFiletable"). Declared here rather than in Devices/<device>/Storage.h
// because Main.cpp pulls Core headers in first, so the device header is seen too late to
// size the table; it is guarded on the device the same way every device-specific
// implementation is (Docs/Devices.md), with a -D override still winning.
#ifndef STORAGE_TABLE_PAGES
#if defined(BOARD_Tamu_v2_0A)
#define STORAGE_TABLE_PAGES 4
#elif defined(BOARD_Valu_v2_0)
#define STORAGE_TABLE_PAGES 2
#elif defined(BOARD_DAS_v0_1)
#define STORAGE_TABLE_PAGES 1
#else
#define STORAGE_TABLE_PAGES 1
#endif
#endif
#define STORAGE_TABLE_SIZE (STORAGE_TABLE_PAGES * PAGE_SIZE)

// --- Flash access (implemented per device, see Devices/<device>/Storage.h) ---
bool Storage_FlashInit();                              // find/open the storage partition
uint32_t Storage_FlashRead(uint32_t offset, void *data, uint32_t size);   // Reads `size` bytes from flash at `offset`; returns bytes actually read (0 on failure)
bool Storage_FlashWrite(uint32_t offset, const void *data, uint32_t size); // Writes `size` bytes to flash at `offset`
bool Storage_FlashErase(uint32_t offset, uint32_t size);   // Erases `size` bytes of flash starting at `offset`
bool Storage_FlashFormat();                            // Wipes the entire storage region (per-device)

// File record format (16 bytes, naturally 4-aligned, Docs/Services/Storage.md):
// Offset is from flash start, Filesize in bytes, Name is 8 plain-text characters.
struct FileEntry
{
    uint32_t offset;    // 0x00 = invalidated entry, 0xFFFFFFFF = unwritten slot
    uint32_t size;
    char name[8];
};

#define TABLE_ENTRY_SIZE sizeof(FileEntry)

// Number of whole pages a byte size occupies, clamped to [1, MAX_DATA_BLOCKS].
// Plain `(size + PAGE_SIZE - 1) / PAGE_SIZE` overflows uint32 for sizes near
// 0xFFFFFFFF (the Storage service passes untrusted sizes straight in), silently
// yielding a tiny block count for a huge file.
static inline uint32_t BlocksForSize(uint32_t size)
{
    if (size > (0xFFFFFFFFu - PAGE_SIZE + 1))
        return (0xFFFFFFFFu / PAGE_SIZE) + 1; // saturate: no overflow
    uint32_t blocks = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (blocks == 0) blocks = 1;
    if (blocks > STORAGE_MAX_BLOCKS) blocks = STORAGE_MAX_BLOCKS;
    return blocks;
}

// Clamps a file-relative byte range to the file's size without overflowing: `offset + length`
// wraps for a huge length, which would let an out-of-range access pass a naive check. Returns
// the clamped length; 0 means the offset is at or after the end.
static inline uint32_t ClampFileLength(uint32_t offset, uint32_t length, uint32_t size)
{
    if (offset >= size) return 0;
    if (length > size - offset) return size - offset;
    return length;
}

// Number of 32-bit pointer slots in the first (pointer) page
#define PTR_SLOTS (PAGE_SIZE / 4)

// File-table slot state (Docs/Services/Storage.md): offset 0x00 = invalidated entry,
// 0xFFFFFFFF = unused slot, anything else = a real (page-aligned) flash offset.
static inline bool FileSlotIsFree(uint32_t offset)
{
    return offset == 0x00 || offset == 0xFFFFFFFF;
}
static inline bool FileEntryIsValid(uint32_t offset)
{
    return !FileSlotIsFree(offset);
}

// Copies a file name into the fixed 8-byte record form, null-padded to the full width
// (B10: the pad byte is a null, not a space). A NUL terminator ends the name, and trailing
// pad spaces are dropped first, so a name supplied either way (a plain C string, a
// space-padded 8-byte buffer or the NUL-padded wire form) canonicalises to the same record.
static inline void PackName(const char *plain, char out[8])
{
    uint8_t len = 0;
    if (plain)
    {
        while (len < 8 && plain[len]) len++;            // up to a NUL terminator or the width
        while (len > 0 && plain[len - 1] == ' ') len--; // drop trailing pad spaces
    }
    uint8_t i = 0;
    for (; i < len; i++)
        out[i] = plain[i];
    for (; i < 8; i++)
        out[i] = 0x00;
}

// Compares an on-flash record name against a supplied name, ignoring the pad byte on both
// sides: a null-padded record, a space-padded one (a pre-B10 record) and a plain C string
// all compare equal. A raw `memcmp(record, "SUBREQ", 8)` compared the record's padding
// against the C string's NUL terminator and always differed for names shorter than 8 chars -
// which silently broke FindInFiletable/DeleteFile and left a new SUBREQ/.DT_/.DV_ record
// behind on every save. Pack the plain name first so both sides use the same 8-byte form.
static inline bool NameMatch(const char record[8], const char *plain)
{
    char packed[8];
    PackName(plain, packed);
    uint8_t record_len = 8;
    while (record_len > 0 && (record[record_len - 1] == ' ' || record[record_len - 1] == 0x00))
        record_len--;
    uint8_t packed_len = 8;
    while (packed_len > 0 && packed[packed_len - 1] == 0x00)
        packed_len--;
    if (record_len != packed_len) return false;
    return memcmp(record, packed, record_len) == 0;
}

