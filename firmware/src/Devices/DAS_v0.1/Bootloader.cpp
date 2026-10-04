// DAS bootloader (Docs/Services/Bootloader.md).
//
// Only the `DAS_bootloader` env defines BOOTLOADER_BUILD, so the app/core envs compile this
// file to nothing (it pulls in ch32v00x.h, which they do not have).
//
// Standalone, linked at 0x0 (2 KB budget); the main app is linked at 0x800. Entered by
// holding the button (PC0, active low) at reset, in which case the white LED (PD0) comes on;
// otherwise the app is launched immediately. It deliberately ignores the standard packet
// format and speaks the raw bootloader frame on the RS485 bus (no 0xAA sync, so the running
// nodes - which wait for 0xAA - ignore it). Writes give no acknowledgement: the app paces
// the flash and verifies with read requests.
//
// Flash rules: writes are 32-byte aligned (per the doc); the bootloader erases the 64-byte
// page on the first half (offset % 64 == 0) and programs both halves. It never touches the
// storage region at 0x3E00 (the app's `.SV`/table live there).
//
// It is written with direct register access (no SPL GPIO/USART/RCC/FLASH) to fit the 2 KB
// region alongside the framework startup/SystemInit. The clock is already 48 MHz: the
// framework's `_start` calls SystemInit before main.
//
// This file is built ONLY into the `DAS_bootloader` env (the app env filters it out).

#ifdef BOOTLOADER_BUILD

#include <cstring>

#include "ch32v00x.h"
#include "Core/Functions/Bootloader.h"

// The app is linked here; the storage region starts at 0x3E00 (512 B flush against the end
// of the chip flash) and is never written. Must match `board_upload.offset_address` in
// platformio.ini (both the DAS_v0_1 app and the DAS_bootloader image share the `[das]` base).
// The upload tool reads that option (scripts/das_app_upload.py); this constant is the
// bootloader's matching copy.
#define APP_BASE 0x800u
#define APP_LIMIT 0x3E00u

// The flash controller programs/erases through the 0x08000000 alias; the CPU reads/executes
// through 0x00000000 (see Devices/DAS_v0.1/Storage.h).
#define FLASH_CTRL_BASE 0x08000000u

#define CYCLES_PER_US 48u // 48 MHz core clock (SystemInit)
#define BL_BYTE_TIME_US 22u // 460800 baud, matching Docs/RSBus and Packets.md

// Reply guard: how long the half-duplex line must be quiet before the bootloader answers.
// The running nodes only need the standard CSMA window (8 byte times) because their reply
// follows a full service-handler pass; the bootloader answers immediately, so the window is
// widened to cover the core's TX-enable release. Counted from the tick counter (bus-state
// based), not a fixed delay. The wait is capped so a continuously noisy/stuck bus cannot
// block the reply forever.
#define BL_SILENCE_BYTES 32u
#define BL_SILENCE_MAX_US 10000u

// ---------------------------------------------------------------------------
// GPIO (direct register: CFGLR is 4 bits per pin; all DAS pins are < 8)
// ---------------------------------------------------------------------------

static void GpioCfg(GPIO_TypeDef *port, uint32_t pin, uint32_t cfg)
{
    uint32_t shift = pin * 4u;
    port->CFGLR = (port->CFGLR & ~(0xFu << shift)) | (cfg << shift);
}

static void GpioOutPP(GPIO_TypeDef *port, uint32_t pin) { GpioCfg(port, pin, 0x3u); }  // out push-pull 50 MHz
static void GpioAfPP(GPIO_TypeDef *port, uint32_t pin) { GpioCfg(port, pin, 0xBu); }   // AF push-pull 50 MHz
static void GpioInPU(GPIO_TypeDef *port, uint32_t pin)
{
    port->BSHR = (1u << pin); // pull-up
    GpioCfg(port, pin, 0x8u); // input, pull-up/down
}

// ---------------------------------------------------------------------------
// Flash (direct registers)
// ---------------------------------------------------------------------------
//
// The SPL flash helpers are not used: under LTO their unlock/lock sequences get inlined into
// both the erase and the program paths (duplicated - the driver's cost is per call site, not
// per function). Writing the sequences here once, `noinline`, keeps a single copy of each and
// drops the SPL flash driver entirely. Register bit values mirror ch32v00x_flash.c.

#define FLASH_KEY1 0x45670123u
#define FLASH_KEY2 0xCDEF89ABu

#define FCR_PG 0x00000001u      // program enable
#define FCR_STRT 0x00000040u    // start
#define FCR_LOCK 0x00000080u    // lock
#define FCR_FLOCK 0x00008000u   // fast-program lock
#define FCR_PAGE_ER 0x00020000u // page erase
#define FSR_BSY 0x00000001u     // busy

// Unlocks both the standard and the fast program/erase controller.
__attribute__((noinline)) static void FlashUnlock()
{
    FLASH->KEYR = FLASH_KEY1;
    FLASH->KEYR = FLASH_KEY2;
    FLASH->MODEKEYR = FLASH_KEY1;
    FLASH->MODEKEYR = FLASH_KEY2;
}

__attribute__((noinline)) static void FlashLock()
{
    FLASH->CTLR |= FCR_LOCK | FCR_FLOCK;
}

// Erases the 64-byte page containing `addr` (the 0x08000000 alias).
__attribute__((noinline)) static void FlashErasePageFast(uint32_t addr)
{
    FLASH->CTLR |= FCR_PAGE_ER;
    FLASH->ADDR = addr;
    FLASH->CTLR |= FCR_STRT;
    while (FLASH->STATR & FSR_BSY) {}
    FLASH->CTLR &= ~FCR_PAGE_ER;
}

// Programs one 32-bit word with the standard two-half-word sequence (FCR_PG); the fast
// controller (FCR_PAGE_ER) is used only by the 64-byte page erase.
__attribute__((noinline)) static void FlashProgramWord(uint32_t addr, uint32_t data)
{
    FLASH->CTLR |= FCR_PG;
    *(volatile uint16_t *)addr = (uint16_t)data;
    while (FLASH->STATR & FSR_BSY) {}
    *(volatile uint16_t *)(addr + 2) = (uint16_t)(data >> 16);
    while (FLASH->STATR & FSR_BSY) {}
    FLASH->CTLR &= ~FCR_PG;
}

static bool FlashErasePage(uint32_t addr)
{
    FlashUnlock();
    FlashErasePageFast(FLASH_CTRL_BASE + addr);
    uint32_t check = 0;
    memcpy(&check, (const void *)addr, sizeof(check));
    FlashLock();
    return check == 0xFFFFFFFFu;
}

static void FlashWrite(uint32_t addr, const uint8_t *src, uint32_t len)
{
    FlashUnlock();
    for (uint32_t off = 0; off < len; off += 4)
    {
        uint32_t w;
        memcpy(&w, src + off, sizeof(w));
        FlashProgramWord(FLASH_CTRL_BASE + addr + off, w);
    }
    FlashLock();
}

// ---------------------------------------------------------------------------
// USART1 (RS485)
// ---------------------------------------------------------------------------

static void SetupUart()
{
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOD | RCC_APB2Periph_USART1;

    GpioAfPP(GPIOD, 5);  // TX PD5
    GpioInPU(GPIOD, 6);  // RX PD6
    GpioOutPP(GPIOD, 4); // TXEN PD4
    GPIOD->BCR = (1u << 4);

    RCC->APB2PRSTR |= RCC_APB2Periph_USART1;
    RCC->APB2PRSTR &= ~RCC_APB2Periph_USART1;

    USART1->CTLR1 = USART_Mode_Rx | USART_Mode_Tx;
    USART1->CTLR2 = 0;
    USART1->CTLR3 = 0;
    USART1->BRR = (48000000u + 460800u / 2u) / 460800u; // 104 at 48 MHz
    USART1->CTLR1 |= 0x2000u /* UE */;
}

static bool WaitByte(uint8_t *b, uint32_t start, uint32_t timeout_cycles)
{
    while ((SysTick->CNT - start) < timeout_cycles)
    {
        if (USART1->STATR & USART_FLAG_RXNE)
        {
            *b = (uint8_t)USART1->DATAR;
            return true;
        }
    }
    return false;
}

// Reads one raw frame into `out`; returns its length or 0 on timeout/invalid. `timeout_us`
// bounds the whole frame, not each byte, so a stalled frame is abandoned after one window
// while a frame that keeps arriving still completes.
static int ReceiveFrame(uint8_t *out, size_t cap, uint32_t timeout_us)
{
    uint32_t start = SysTick->CNT;
    uint32_t timeout_cycles = timeout_us * CYCLES_PER_US;
    uint8_t b;
    for (;;)
    {
        if (!WaitByte(&b, start, timeout_cycles)) return 0;
        if (b == Bootloader::START) break; // skip anything that is not a frame start
    }

    size_t got = 0;
    out[got++] = b;
    if (!WaitByte(&b, start, timeout_cycles)) return 0;
    out[got++] = b;

    uint16_t need = Bootloader::FrameSize((uint8_t)(b & 0x03));
    if (need == 0 || need > cap) return 0;
    while (got < need)
    {
        if (!WaitByte(&b, start, timeout_cycles)) return 0;
        out[got++] = b;
    }
    return Bootloader::Decode(out, need) ? (int)need : 0;
}

static void SendRaw(const uint8_t *data, size_t len)
{
    // Wait for the line to be quiet (BL_SILENCE_BYTES) before driving the half-duplex line:
    // a request's last byte reaches us before the core drops its TX enable, so replying
    // immediately collides. Like the running nodes we count bus activity by draining RX and
    // time the quiet span with the tick counter, not a fixed delay.
    const uint32_t silence_cycles = BL_SILENCE_BYTES * BL_BYTE_TIME_US * CYCLES_PER_US;
    const uint32_t silence_max_cycles = BL_SILENCE_MAX_US * CYCLES_PER_US;
    uint32_t wait_start = SysTick->CNT;
    uint32_t idle_since = wait_start;
    for (;;)
    {
        if (USART1->STATR & USART_FLAG_RXNE)
        {
            (void)USART1->DATAR;
            idle_since = SysTick->CNT;
        }
        else if ((uint32_t)(SysTick->CNT - idle_since) >= silence_cycles)
        {
            break;
        }
        // Bound the wait: a bus that never goes quiet must not block the bootloader.
        if ((uint32_t)(SysTick->CNT - wait_start) >= silence_max_cycles)
        {
            break;
        }
    }

    GPIOD->BSHR = (1u << 4); // drive the line
    for (size_t i = 0; i < len; i++)
    {
        while ((USART1->STATR & USART_FLAG_TXE) == 0) {}
        USART1->DATAR = data[i];
    }
    while ((USART1->STATR & USART_FLAG_TC) == 0) {}
    GPIOD->BCR = (1u << 4);
    // Drain our own half-duplex echo so it cannot be mistaken for the start of the next
    // frame. The app retries a read if the reply was lost, so no extra delay is needed here.
    while (USART1->STATR & USART_FLAG_RXNE)
        (void)USART1->DATAR;
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static void HandleWrite(uint32_t offset, const uint8_t *payload)
{
    if ((offset & 31u) != 0) return; // 32-byte aligned per the doc
    uint32_t addr = APP_BASE + offset;
    if (addr < APP_BASE || addr + Bootloader::PAYLOAD_SIZE > APP_LIMIT) return;

    // Erase the 64-byte page on its first half; the second half programs the erased page.
    if ((addr & 63u) == 0)
    {
        if (!FlashErasePage(addr)) return;
    }
    FlashWrite(addr, payload, Bootloader::PAYLOAD_SIZE);
}

static void HandleRead(uint32_t offset)
{
    uint8_t data[Bootloader::PAYLOAD_SIZE];
    uint32_t addr = APP_BASE + offset;
    if (addr < APP_BASE || addr + Bootloader::PAYLOAD_SIZE > APP_LIMIT)
        memset(data, 0xFF, sizeof(data));
    else
        memcpy(data, (const void *)addr, sizeof(data));

    uint8_t resp[Bootloader::DATA_SIZE];
    Bootloader::EncodeReadResponse(offset, data, resp);
    SendRaw(resp, sizeof(resp));
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------

static void JumpToApp()
{
    __disable_irq();
    USART1->CTLR1 &= ~0x2000u /* UE */;
    // The app's own startup sets sp/gp/mtvec; a plain jump to its reset vector is enough.
    ((void (*)(void))APP_BASE)();
    for (;;) {} // unreachable
}

int main(void)
{
    SysTick->CTLR |= 0x05; // 48 MHz core clock; the framework startup already ran SystemInit

    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD;

    // Button PC0 (pull-up, active low) and white LED PD0 (active high).
    GpioInPU(GPIOC, 0);
    GpioOutPP(GPIOD, 0);
    GPIOD->BCR = (1u << 0);

    // Let the pull-up settle, then sample the button: released (high) launches the app.
    for (volatile uint32_t i = 0; i < 200000u; i++) {}
#ifndef BOOTLOADER_FORCE
    // BOOTLOADER_FORCE (test hook) skips the button check so the RSBus update path can be
    // exercised without physically holding PC0.
    if (GPIOC->INDR & (1u << 0))
        JumpToApp();
#endif

    // Bootloader mode: white LED on, then serve frames until reset.
    GPIOD->BSHR = (1u << 0);
    SetupUart();

    for (;;)
    {
        uint8_t frame[Bootloader::MAX_FRAME_SIZE];
        int n = ReceiveFrame(frame, sizeof(frame), 2000000u); // 2 s per byte
        if (n <= 0) continue;

        uint8_t cmd = (uint8_t)(frame[1] & 0x03);
        uint32_t offset = Bootloader::Offset(frame);
        if (cmd == Bootloader::CMD_WRITE)
            HandleWrite(offset, Bootloader::Payload(frame));
        else if (cmd == Bootloader::CMD_READ_REQ)
            HandleRead(offset);
    }
}

#endif // BOOTLOADER_BUILD
