#pragma once

// Storage geometry, the flash backend hooks and the FileRecord helpers.
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

// Upper bound on allocatable data pages (worst-case flash geometry); sizes the per-call
// usage bitmap in FindSpace (STORAGE_FLASH_SIZE / STORAGE_BLOCK_SIZE bits).
#define STORAGE_MAX_BLOCKS ((STORAGE_FLASH_SIZE / STORAGE_BLOCK_SIZE) + 1)

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
#define STORAGE_TABLE_SIZE (STORAGE_TABLE_PAGES * STORAGE_BLOCK_SIZE)

// --- Flash access (implemented per device, see Devices/<device>/Storage.h) ---
bool StorageFlashInit();                              // find/open the storage partition
uint32_t StorageFlashRead(uint32_t Offset, void *Data, uint32_t Size);   // Reads `size` bytes from flash at `offset`; returns bytes actually read (0 on failure)
bool StorageFlashWrite(uint32_t Offset, const void *Data, uint32_t Size); // Writes `size` bytes to flash at `offset`
bool StorageFlashErase(uint32_t Offset, uint32_t Size);   // Erases `size` bytes of flash starting at `offset`
bool StorageFlashFormat();                            // Wipes the entire storage region (per-device)

// File record format (16 bytes, naturally 4-aligned, Docs/Services/Storage.md):
// Offset is from flash start, Filesize in bytes, Name is 8 plain-text characters.
struct FileRecord
{
    uint32_t Offset;    // 0x00 = invalidated entry, 0xFFFFFFFF = unwritten slot
    uint32_t Size;
    char Name[8];
};

#define TABLE_ENTRY_SIZE sizeof(FileRecord)

// Number of whole pages a byte size occupies, clamped to [1, MAX_DATA_BLOCKS].
// Plain `(size + STORAGE_BLOCK_SIZE - 1) / STORAGE_BLOCK_SIZE` overflows uint32 for sizes near
// 0xFFFFFFFF (the Storage service passes untrusted sizes straight in), silently
// yielding a tiny block count for a huge file.
static inline uint32_t BlocksForSize(uint32_t Size)
{
    if (Size > (0xFFFFFFFFu - STORAGE_BLOCK_SIZE + 1))
        return (0xFFFFFFFFu / STORAGE_BLOCK_SIZE) + 1; // saturate: no overflow
    uint32_t Blocks = (Size + STORAGE_BLOCK_SIZE - 1) / STORAGE_BLOCK_SIZE;
    if (Blocks == 0) Blocks = 1;
    if (Blocks > STORAGE_MAX_BLOCKS) Blocks = STORAGE_MAX_BLOCKS;
    return Blocks;
}

// Clamps a file-relative byte range to the file's size without overflowing: `offset + length`
// wraps for a huge length, which would let an out-of-range access pass a naive check. Returns
// the clamped length; 0 means the offset is at or after the end.
static inline uint32_t ClampFileLength(uint32_t Offset, uint32_t Length, uint32_t Size)
{
    if (Offset >= Size) return 0;
    if (Length > Size - Offset) return Size - Offset;
    return Length;
}

// Number of 32-bit pointer slots in the first (pointer) page
#define PTR_SLOTS (STORAGE_BLOCK_SIZE / 4)

// File-table slot state (Docs/Services/Storage.md): offset 0x00 = invalidated entry,
// 0xFFFFFFFF = unused slot, anything else = a real (page-aligned) flash offset.
static inline bool FileSlotIsFree(uint32_t Offset)
{
    return Offset == 0x00 || Offset == 0xFFFFFFFF;
}
static inline bool FileRecordIsValid(uint32_t Offset)
{
    return !FileSlotIsFree(Offset);
}

// Copies a file name into the fixed 8-byte record form, null-padded to the full width
// (B10: the pad byte is a null, not a space). A NUL terminator ends the name, and trailing
// pad spaces are dropped first, so a name supplied either way (a plain C string, a
// space-padded 8-byte buffer or the NUL-padded wire form) canonicalises to the same record.
static inline void PackName(const char *Plain, char Out[8])
{
    uint8_t Len = 0;
    if (Plain)
    {
        while (Len < 8 && Plain[Len]) Len++;            // up to a NUL terminator or the width
        while (Len > 0 && Plain[Len - 1] == ' ') Len--; // drop trailing pad spaces
    }
    uint8_t i = 0;
    for (; i < Len; i++)
        Out[i] = Plain[i];
    for (; i < 8; i++)
        Out[i] = 0x00;
}

// Compares an on-flash record name against a supplied name, ignoring the pad byte on both
// sides: a null-padded record, a space-padded one (a pre-B10 record) and a plain C string
// all compare equal. A raw `memcmp(record, "SUBREQ", 8)` compared the record's padding
// against the C string's NUL terminator and always differed for names shorter than 8 chars -
// which silently broke FindInFiletable/DeleteFile and left a new SUBREQ/.DT_/.DV_ record
// behind on every save. Pack the plain name first so both sides use the same 8-byte form.
static inline bool NameMatch(const char Record[8], const char *Plain)
{
    char packed[8];
    PackName(Plain, packed);
    uint8_t RecordLen = 8;
    while (RecordLen > 0 && (Record[RecordLen - 1] == ' ' || Record[RecordLen - 1] == 0x00))
        RecordLen--;
    uint8_t PackedLen = 8;
    while (PackedLen > 0 && packed[PackedLen - 1] == 0x00)
        PackedLen--;
    if (RecordLen != PackedLen) return false;
    return memcmp(Record, packed, RecordLen) == 0;
}

