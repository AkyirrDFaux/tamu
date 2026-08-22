#pragma once
#include <cstring>
#include "ch32v00x.h"
#include "ch32v00x_flash.h"

// CH32V003: 16 KB of internal flash, mapped at 0x00000000 (execution) and aliased at
// 0x08000000 (flash controller).
#define CHIP_FLASH_SIZE      0x4000u

// The storage filesystem lives in the top STORAGE_FLASH_SIZE bytes of the chip flash, flush
// against the end (0x4000): STORAGE_CHIP_BASE = 0x4000 - STORAGE_FLASH_SIZE. The build links a
// reservation array into this region via -Wl,--section-start=.fixed_data=0x3800, so code that
// ever grows into it fails to link. STORAGE_FLASH_SIZE/STORAGE_BLOCK_SIZE are defined by the
// DAS build flags (platformio.ini); the derived bases below keep the layout at the very end
// no matter the size.
#define STORAGE_PAGE_SIZE    64u   // CH32V00x fast-mode erase granularity (1page = 64Byte)

// The chip executes code from the 0x00000000 mapping; the flash controller programs/erases
// through the 0x08000000 alias domain (see ch32v00x_flash.c, ValidAddrStart/End).
#define STORAGE_CHIP_BASE    (CHIP_FLASH_SIZE - STORAGE_FLASH_SIZE)
#define STORAGE_FLASH_BASE   (0x08000000u + STORAGE_CHIP_BASE)

static_assert(STORAGE_CHIP_BASE + STORAGE_FLASH_SIZE == CHIP_FLASH_SIZE,
              "Storage must be flush against the end of the chip flash");

#include "Core/Services/Storage.h"

// Pinned at flash 0x3800 by -Wl,--section-start=.fixed_data=0x3800. The region is erased
// (normalised to 0xFF) by Storage.Format() before any use; a code-size overflow fails the
// link instead of corrupting the storage region at runtime.
__attribute__((section(".fixed_data"), used))
const uint8_t storage_flash_reservation[STORAGE_FLASH_SIZE] = {0xFF};

// Opens the flash controller and checks that the code does not overlap the storage region.
bool Storage_FlashInit()
{
    // Reference the reservation so --gc-sections does not discard the pinned section.
    __asm__ volatile("" : : "r"(storage_flash_reservation));

    // The linker must have pinned the section at the derived base (0x4000 - STORAGE_FLASH_SIZE);
    // a stale --section-start flag would put the storage in the wrong place.
    if ((uint32_t)storage_flash_reservation != STORAGE_CHIP_BASE) {
        DeviceLog("STORAGE", "Reservation not pinned at 0x%x!", (unsigned)STORAGE_CHIP_BASE);
        return false;
    }

    extern const uint8_t _etext[];   // end of code+rodata (Link.ld)
    if ((uint32_t)_etext > STORAGE_CHIP_BASE) {
        DeviceLog("STORAGE", "Code overlaps the storage region!");
        return false;
    }
    FLASH_Unlock();
    FLASH_Unlock_Fast();
    FLASH_Lock_Fast();
    FLASH_Lock();
    return true;
}

uint32_t Storage_FlashSize()
{
    return STORAGE_FLASH_SIZE;
}

// Reads `size` bytes from flash at `offset` into `data` (0x00000000 domain).
uint32_t Storage_FlashRead(uint32_t offset, void *data, uint32_t size)
{
    if (offset + size > STORAGE_FLASH_SIZE)
        return 0;
    memcpy(data, (const void *)(STORAGE_CHIP_BASE + offset), size);
    return size;
}

// Writes `size` bytes from `data` to flash at `offset`. The target must be freshly erased:
// flash only programs 1s to 0s, so a word is written as a read-modify-write that keeps the
// surrounding (0xFF) bytes intact.
bool Storage_FlashWrite(uint32_t offset, const void *data, uint32_t size)
{
    if (size == 0)
        return true;
    if (offset + size > STORAGE_FLASH_SIZE)
        return false;

    FLASH_Unlock();
    const uint8_t *src = (const uint8_t *)data;
    const uint8_t *maddr = (const uint8_t *)(STORAGE_CHIP_BASE + offset);
    uint32_t faddr = STORAGE_FLASH_BASE + offset;
    uint32_t start = faddr & ~3u;
    uint32_t end = (faddr + size + 3u) & ~3u;
    for (uint32_t a = start; a < end; a += 4) {
        uint32_t w = 0;
        for (uint32_t k = 0; k < 4; k++) {
            uint32_t byte_addr = a + k;
            if (byte_addr >= faddr && byte_addr < faddr + size)
                w |= ((uint32_t)src[byte_addr - faddr]) << (8 * k);
            else
                w |= ((uint32_t)maddr[byte_addr - faddr]) << (8 * k);
        }
        if (FLASH_ProgramWord(a, w) != FLASH_COMPLETE) {
            FLASH_Lock();
            return false;
        }
    }
    FLASH_Lock();
    return true;
}

// Erases `size` bytes of flash at `offset` (256-byte aligned, one page per storage block).
// Uses the *normal* page erase (FLASH_ErasePage), not the fast erase: fast erase is an
// unbounded `while(BSY)` busy-wait and leaves cells at 0x00 on the CH32V003, and flash can
// only program 1->0, so "programming 0xFFFFFFFF back" cannot restore the 0xFF erase state.
// The normal erase restores 0xFF and is bounded by FLASH_WaitForLastOperation.
bool Storage_FlashErase(uint32_t offset, uint32_t size)
{
    if ((offset & (STORAGE_BLOCK_SIZE - 1)) || (size & (STORAGE_BLOCK_SIZE - 1)))
        return false;
    if (offset + size > STORAGE_FLASH_SIZE)
        return false;

    FLASH_Unlock();
    for (uint32_t o = offset; o < offset + size; o += STORAGE_BLOCK_SIZE) {
        if (FLASH_ErasePage(STORAGE_FLASH_BASE + o) != FLASH_COMPLETE) {
            FLASH_Lock();
            return false;
        }
    }
    FLASH_Lock();
    return true;
}

// Wipes the entire storage region (all pages, including the pointer page).
bool Storage_FlashFormat()
{
    FLASH_Unlock();
    for (uint32_t o = 0; o < STORAGE_FLASH_SIZE; o += STORAGE_BLOCK_SIZE) {
        if (FLASH_ErasePage(STORAGE_FLASH_BASE + o) != FLASH_COMPLETE) {
            FLASH_Lock();
            return false;
        }
    }
    FLASH_Lock();
    return true;
}