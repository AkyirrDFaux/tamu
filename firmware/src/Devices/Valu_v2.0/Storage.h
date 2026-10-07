#pragma once

#include <cstring>
#include "ch32v20x.h"
#include "ch32v20x_flash.h"

// CH32V203G8R6: 64 KB of internal flash, mapped at 0x00000000 (execution) and aliased at
// 0x08000000 (flash controller). Same aliasing model as Devices/DAS_v0.1/Storage.h.
#define CHIP_FLASH_SIZE      0x10000u

// The storage filesystem lives in the top STORAGE_FLASH_SIZE bytes of the chip flash, flush
// against the end (Docs/Devices.md "Valu v2.0": 8 kB). With STORAGE_FLASH_SIZE = 8192 that
// base is 0xE000 - exactly the top of the [env:Valu_v2_0] app window, so the app image and
// the storage region never overlap: the linker region is [0x3000, 0xE000) and a build that
// grows past it fails to link instead of programming over storage.
#define STORAGE_CHIP_BASE    (CHIP_FLASH_SIZE - STORAGE_FLASH_SIZE)
#define STORAGE_FLASH_BASE   (0x08000000u + STORAGE_CHIP_BASE)

// Erase unit. The Valu's documented page is 256 bytes, Fast mode (Docs/Devices.md), which is
// exactly the CH32V20x FLASH_ErasePage_Fast page (ch32v20x_flash.c: "1page = 256Byte", CR_PAGE_ER).
// The filesystem allocation unit MUST equal this page so each block erases as one unit and no
// two structures share an erase unit.
#define FLASH_ERASE_PAGE_SIZE 256u

static_assert(STORAGE_BLOCK_SIZE == FLASH_ERASE_PAGE_SIZE,
              "STORAGE_BLOCK_SIZE must equal the 256-byte fast-erase page");

static_assert(STORAGE_CHIP_BASE + STORAGE_FLASH_SIZE == CHIP_FLASH_SIZE,
              "Storage must be flush against the end of the chip flash");

#include "Core/Services/Storage.h"

// The FLASH access clock must be <= 60 MHz (RM 32.2 note 2) and is SYSCLK or SYSCLK/2 per
// FLASH_CTLR.SCKMOD (bit 25; default /2) - NOT HCLK. This board runs at 144 MHz, so even /2 is
// 72 MHz, over the ceiling. Drop SYSCLK to HSI (8 MHz) around flash operations; the PLL keeps
// running so USBPRE = PLL/3 (48 MHz to USBD) is unaffected. Same approach as the bootloader's
// FlashSlowDown/FlashRestore (Devices/Valu_v2.0/Bootloader.cpp).
#define VALU_CFGR0_SW   0x3u
#define VALU_CFGR0_SWS  0xCu
static uint32_t StorageFlashSlowDown(void)
{
    uint32_t old = RCC->CFGR0 & VALU_CFGR0_SW;
    RCC->CFGR0 = (RCC->CFGR0 & ~VALU_CFGR0_SW) | 0x0u; // SW = HSI (8 MHz)
    while ((RCC->CFGR0 & VALU_CFGR0_SWS) != 0x0u) {}
    return old;
}
static void StorageFlashRestore(uint32_t old)
{
    RCC->CFGR0 = (RCC->CFGR0 & ~VALU_CFGR0_SW) | (old & VALU_CFGR0_SW);
    while ((RCC->CFGR0 & VALU_CFGR0_SWS) != (old & VALU_CFGR0_SWS)) {}
}

// Opens the flash controller and checks that the code does not overlap the storage region.
bool Storage_FlashInit()
{
    // The linker region is the app window [0x3000, 0xE000) (board_upload.maximum_size), so a
    // build that grows into storage fails to link. This runtime check is the belt-and-braces
    // confirmation that the loaded image indeed ends before storage (the base is derived from
    // STORAGE_FLASH_SIZE, not a literal, so a geometry change cannot silently misplace it).
    extern const uint8_t _etext[]; // end of code+rodata (Link.tpl)
    if ((uint32_t)_etext > STORAGE_CHIP_BASE) {
        DeviceLog("STORAGE", "Code overlaps the storage region!");
        return false;
    }
    // NOTE: no global unlock here - the write/erase paths below lock/unlock the controller
    // themselves, and a redundant Unlock/Lock dance only costs flash.
    return true;
}

// Reads `size` bytes from flash at `offset` into `data` (0x00000000 domain).
uint32_t Storage_FlashRead(uint32_t offset, void *data, uint32_t size)
{
    // Overflow-safe bound: `offset + size` could wrap for a huge size.
    if (offset > STORAGE_FLASH_SIZE || size > STORAGE_FLASH_SIZE - offset)
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
    if (offset > STORAGE_FLASH_SIZE || size > STORAGE_FLASH_SIZE - offset)
        return false;
    // The read-modify-write below indexes `maddr[byte_addr - faddr]`; for the leading
    // partial word `byte_addr < faddr`, so require a word-aligned target to keep that
    // index non-negative (all storage structures are 4-byte aligned).
    if (offset & 3u)
        return false;

    uint32_t clk = StorageFlashSlowDown();
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
            StorageFlashRestore(clk);
            return false;
        }
    }
    FLASH_Lock();
    StorageFlashRestore(clk);
    return true;
}

// Erases `size` bytes of flash at `offset` (STORAGE_BLOCK_SIZE aligned, one fast page per
// allocation block). Uses the 256-byte CR_PAGE_ER operation (FLASH_ErasePage_Fast), matching
// the documented "256 Bytes (Fast mode)" page. FLASH_ErasePage_Fast returns no status, so each
// erased page is verified by reading back its first word - a failed erase must not be reported
// as success into table/file commits. The fast erase needs its own unlock (FLASH_Unlock_Fast,
// CR_PAGE_ER is the fast-mode page erase keyed by MODEKEYR on the CH32V20x).
bool Storage_FlashErase(uint32_t offset, uint32_t size)
{
    if ((offset & (FLASH_ERASE_PAGE_SIZE - 1)) || (size & (FLASH_ERASE_PAGE_SIZE - 1)))
        return false;
    // Overflow-safe bound: `offset + size` could wrap for a huge size.
    if (offset > STORAGE_FLASH_SIZE || size > STORAGE_FLASH_SIZE - offset)
        return false;

    uint32_t clk = StorageFlashSlowDown();
    FLASH_Unlock();
    FLASH_Unlock_Fast();
    for (uint32_t o = offset; o < offset + size; o += FLASH_ERASE_PAGE_SIZE) {
        FLASH_ErasePage_Fast(STORAGE_FLASH_BASE + o);
        // CH32V203 flash quirk (same one the Valu bootloader works around - see its
        // FlashEraseSector, and the previous release's Hardware/Memory.h "Prevent incorrect
        // reading"): a just-erased word does NOT read back as 0xFFFFFFFF until it has been through
        // a program cycle; it reads a bogus pattern. Program 0xFFFFFFFF over each word so reads
        // (including the verify below) are correct; writing all-1s does not change an erased cell.
        for (uint32_t w = 0; w < FLASH_ERASE_PAGE_SIZE; w += 4) {
            uint32_t v;
            memcpy(&v, (const void *)(STORAGE_CHIP_BASE + o + w), sizeof(v));
            if (v != 0xFFFFFFFFu)
                FLASH_ProgramWord(STORAGE_FLASH_BASE + o + w, 0xFFFFFFFFu);
        }
        uint32_t check = 0;
        memcpy(&check, (const void *)(STORAGE_CHIP_BASE + o), sizeof(check));
        if (check != 0xFFFFFFFFu) {
            FLASH_Lock_Fast();
            FLASH_Lock();
            StorageFlashRestore(clk);
            return false;
        }
    }
    FLASH_Lock_Fast();
    FLASH_Lock();
    StorageFlashRestore(clk);
    return true;
}

// Wipes the entire storage region, one fast page at a time (the whole region is page-aligned).
bool Storage_FlashFormat()
{
    return Storage_FlashErase(0, STORAGE_FLASH_SIZE);
}
