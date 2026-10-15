#pragma once
#include <cstring>
#include "ch32v00x.h"
#include "ch32v00x_flash.h"

// CH32V003: 16 KB of internal flash, mapped at 0x00000000 (execution) and aliased at
// 0x08000000 (flash controller).
#define CHIP_FLASH_SIZE      0x4000u

// The storage filesystem lives in the top STORAGE_FLASH_SIZE bytes of the chip flash, flush
// against the end. The build links a reservation array into this region via
// -Wl,--section-start=.fixed_data, so code that ever grows into it fails to link.
// STORAGE_FLASH_SIZE/STORAGE_BLOCK_SIZE are defined by the DAS build flags (platformio.ini).

// The chip executes code from the 0x00000000 mapping; the flash controller programs/erases
// through the 0x08000000 alias domain (see ch32v00x_flash.c, ValidAddrStart/End).
#define STORAGE_CHIP_BASE    (CHIP_FLASH_SIZE - STORAGE_FLASH_SIZE)
#define STORAGE_FLASH_BASE   (0x08000000u + STORAGE_CHIP_BASE)

// Erase operations on the CH32V003 (see ch32v00x_flash.c, ROM_ERASE):
//   CR_PAGE_ER (FLASH_ErasePage_Fast) -> 64 BYTE page erase  <- used for all file operations
//   CR_PER (FLASH_ErasePage)          -> 1 KB sector erase   <- used only by Format()
// They are different ops, not synonyms: using FLASH_ErasePage for small blocks wipes a whole
// kilobyte around them - that is exactly what corrupted the pointer page + file table on
// RealHW 2026-08-23 (every create/save left only the newest file). Per the CH32V003
// application note, the 64-byte page erase is the intended mechanism for regular operation.
// The filesystem allocation unit MUST equal the 64-byte PAGE_ER page so each block erases as
// one unit and no two structures share an erase unit.
#define FLASH_ERASE_PAGE_SIZE 64u

static_assert(STORAGE_BLOCK_SIZE == FLASH_ERASE_PAGE_SIZE,
              "STORAGE_BLOCK_SIZE must equal the 64-byte PAGE_ER erase page");

static_assert(STORAGE_CHIP_BASE + STORAGE_FLASH_SIZE == CHIP_FLASH_SIZE,
              "Storage must be flush against the end of the chip flash");

#include "Core/Services/Storage.h"

// Pinned at flash 0x3E00 by -Wl,--section-start=.fixed_data=0x3E00. The region is erased
// (normalised to 0xFF) by Storage.Format() before any use; a code-size overflow fails the
// link instead of corrupting the storage region at runtime.
__attribute__((section(".fixed_data"), used))
const uint8_t StorageFlashReservation[STORAGE_FLASH_SIZE] = {0xFF};

// Opens the flash controller and checks that the code does not overlap the storage region.
bool StorageFlashDevice::Init()
{
    // Reference the reservation so --gc-sections does not discard the pinned section.
    __asm__ volatile("" : : "r"(StorageFlashReservation));

    // The linker must have pinned the section at the derived base (0x4000 - STORAGE_FLASH_SIZE);
    // a stale --section-start flag would put the storage in the wrong place.
    if ((uint32_t)StorageFlashReservation != STORAGE_CHIP_BASE) {
        DeviceLog("STORAGE", "Reservation not pinned at 0x%x!", (unsigned)STORAGE_CHIP_BASE);
        return false;
    }

    extern const uint8_t _etext[];   // end of code+rodata (Link.ld)
    if ((uint32_t)_etext > STORAGE_CHIP_BASE) {
        DeviceLog("STORAGE", "Code overlaps the storage region!");
        return false;
    }
    // NOTE: no global unlock here - the write/erase paths below lock/unlock the
    // controller themselves, and a redundant Unlock/Lock dance only costs flash.
    return true;
}

// Reads `size` bytes from flash at `offset` into `data` (0x00000000 domain).
uint32_t StorageFlashDevice::Read(uint32_t Offset, void *Data, uint32_t Size)
{
    // Overflow-safe bound: `offset + size` could wrap for a huge size.
    if (Offset > STORAGE_FLASH_SIZE || Size > STORAGE_FLASH_SIZE - Offset)
        return 0;
    memcpy(Data, (const void *)(STORAGE_CHIP_BASE + Offset), Size);
    return Size;
}

// Writes `size` bytes from `data` to flash at `offset`. The target must be freshly erased:
// flash only programs 1s to 0s, so a word is written as a read-modify-write that keeps the
// surrounding (0xFF) bytes intact.
bool StorageFlashDevice::Write(uint32_t Offset, const void *Data, uint32_t Size)
{
    if (Size == 0)
        return true;
    if (Offset > STORAGE_FLASH_SIZE || Size > STORAGE_FLASH_SIZE - Offset)
        return false;
    // The read-modify-write below indexes `maddr[byte_addr - faddr]`; for the leading
    // partial word `byte_addr < faddr`, so require a word-aligned target to keep that
    // index non-negative (all storage structures are 4-byte aligned).
    if (Offset & 3u)
        return false;

    FLASH_Unlock();
    const uint8_t *Src = (const uint8_t *)Data;
    const uint8_t *Maddr = (const uint8_t *)(STORAGE_CHIP_BASE + Offset);
    uint32_t Faddr = STORAGE_FLASH_BASE + Offset;
    uint32_t Start = Faddr & ~3u;
    uint32_t End = (Faddr + Size + 3u) & ~3u;
    for (uint32_t Addr = Start; Addr < End; Addr += 4) {
        uint32_t Word = 0;
        for (uint32_t ByteIndex = 0; ByteIndex < 4; ByteIndex++) {
            uint32_t ByteAddr = Addr + ByteIndex;
            if (ByteAddr >= Faddr && ByteAddr < Faddr + Size)
                Word |= ((uint32_t)Src[ByteAddr - Faddr]) << (8 * ByteIndex);
            else
                Word |= ((uint32_t)Maddr[ByteAddr - Faddr]) << (8 * ByteIndex);
        }
        if (FLASH_ProgramWord(Addr, Word) != FLASH_COMPLETE) {
            FLASH_Lock();
            return false;
        }
    }
    FLASH_Lock();
    return true;
}

// Erases `size` bytes of flash at `offset` (STORAGE_BLOCK_SIZE aligned, one PAGE_ER page per
// allocation block). Uses the 64-byte CR_PAGE_ER operation (FLASH_ErasePage_Fast), NOT
// FLASH_ErasePage: that is the 1 KB CR_PER sector erase and wipes every other structure
// sharing its kilobyte (the RealHW 2026-08-23 table-loss bug). FLASH_ErasePage_Fast returns
// no status, so each erased page is verified by reading back its first word - a failed erase
// must not be reported as success into table/file commits. The BSY wait mirrors the SDK's
// ROM_ERASE; the controller requires both standard and fast-mode unlocks.
bool StorageFlashDevice::Erase(uint32_t Offset, uint32_t Size)
{
    if ((Offset & (FLASH_ERASE_PAGE_SIZE - 1)) || (Size & (FLASH_ERASE_PAGE_SIZE - 1)))
        return false;
    // Overflow-safe bound: `offset + size` could wrap for a huge size.
    if (Offset > STORAGE_FLASH_SIZE || Size > STORAGE_FLASH_SIZE - Offset)
        return false;

    FLASH_Unlock();
    FLASH_Unlock_Fast();
    for (uint32_t PageOffset = Offset; PageOffset < Offset + Size; PageOffset += FLASH_ERASE_PAGE_SIZE) {
        FLASH_ErasePage_Fast(STORAGE_FLASH_BASE + PageOffset);
        uint32_t Check = 0;
        memcpy(&Check, (const void *)(STORAGE_CHIP_BASE + PageOffset), sizeof(Check));
        if (Check != 0xFFFFFFFFu) {
            FLASH_Lock_Fast();
            FLASH_Lock();
            return false;
        }
    }
    FLASH_Lock_Fast();
    FLASH_Lock();
    return true;
}

// Wipes the entire storage region. The region is small (512 B on the DAS), so it is erased
// with the 64-byte CR_PAGE_ER pages (like every other storage op) - a coarse 1 KB CR_PER
// sector erase would spill past the region on the 512 B layout.
bool StorageFlashDevice::Format()
{
    return Erase(0, STORAGE_FLASH_SIZE);
}