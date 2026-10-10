// DAS bootloader (Docs/Services/Bootloader.md).
//
// Only the `DAS_bootloader` env defines BOOTLOADER_BUILD, so the app/core envs compile this
// file to nothing (it pulls in ch32v00x.h, which they do not have).
//
// Standalone, linked at 0x0 (2 KB budget); the main app is linked at 0x800. Entered by
// holding the button (PC0, active low) at reset, in which case the white LED (PD0) comes on;
// otherwise the app is launched immediately. It deliberately ignores the standard packet
// format and speaks the raw bootloader frame on the RS485 bus (no 0xAA sync, so the running
// nodes - which wait for 0xAA - ignore it). Every write is answered by a write response carrying
// the last successfully written offset, and an identify request returns the app size and name.
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

static void GpioCfg(GPIO_TypeDef *Port, uint32_t Pin, uint32_t Cfg)
{
    uint32_t Shift = Pin * 4u;
    Port->CFGLR = (Port->CFGLR & ~(0xFu << Shift)) | (Cfg << Shift);
}

static void GpioOutPP(GPIO_TypeDef *Port, uint32_t Pin) { GpioCfg(Port, Pin, 0x3u); }  // out push-pull 50 MHz
static void GpioAfPP(GPIO_TypeDef *Port, uint32_t Pin) { GpioCfg(Port, Pin, 0xBu); }   // AF push-pull 50 MHz
static void GpioInPU(GPIO_TypeDef *Port, uint32_t Pin)
{
    Port->BSHR = (1u << Pin); // pull-up
    GpioCfg(Port, Pin, 0x8u); // input, pull-up/down
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
__attribute__((noinline)) static void FlashErasePageFast(uint32_t Addr)
{
    FLASH->CTLR |= FCR_PAGE_ER;
    FLASH->ADDR = Addr;
    FLASH->CTLR |= FCR_STRT;
    while (FLASH->STATR & FSR_BSY) {}
    FLASH->CTLR &= ~FCR_PAGE_ER;
}

// Programs one 32-bit word with the standard two-half-word sequence (FCR_PG); the fast
// controller (FCR_PAGE_ER) is used only by the 64-byte page erase.
__attribute__((noinline)) static void FlashProgramWord(uint32_t Addr, uint32_t Data)
{
    FLASH->CTLR |= FCR_PG;
    *(volatile uint16_t *)Addr = (uint16_t)Data;
    while (FLASH->STATR & FSR_BSY) {}
    *(volatile uint16_t *)(Addr + 2) = (uint16_t)(Data >> 16);
    while (FLASH->STATR & FSR_BSY) {}
    FLASH->CTLR &= ~FCR_PG;
}

static bool FlashErasePage(uint32_t Addr)
{
    FlashUnlock();
    FlashErasePageFast(FLASH_CTRL_BASE + Addr);
    uint32_t Check = 0;
    memcpy(&Check, (const void *)Addr, sizeof(Check));
    FlashLock();
    return Check == 0xFFFFFFFFu;
}

static void FlashWrite(uint32_t Addr, const uint8_t *Src, uint32_t Len)
{
    FlashUnlock();
    for (uint32_t Off = 0; Off < Len; Off += 4)
    {
        uint32_t Word;
        memcpy(&Word, Src + Off, sizeof(Word));
        FlashProgramWord(FLASH_CTRL_BASE + Addr + Off, Word);
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

static bool WaitByte(uint8_t *Byte, uint32_t Start, uint32_t TimeoutCycles)
{
    while ((SysTick->CNT - Start) < TimeoutCycles)
    {
        if (USART1->STATR & USART_FLAG_RXNE)
        {
            *Byte = (uint8_t)USART1->DATAR;
            return true;
        }
    }
    return false;
}

// Reads one raw frame into `out`; returns its length or 0 on timeout/invalid. `timeout_us`
// bounds the whole frame, not each byte, so a stalled frame is abandoned after one window
// while a frame that keeps arriving still completes.
static int ReceiveFrame(uint8_t *Out, size_t Cap, uint32_t TimeoutUs)
{
    uint32_t Start = SysTick->CNT;
    uint32_t TimeoutCycles = TimeoutUs * CYCLES_PER_US;
    uint8_t Byte;
    for (;;)
    {
        if (!WaitByte(&Byte, Start, TimeoutCycles)) return 0;
        if (Byte == Bootloader::START) break; // skip anything that is not a frame start
    }

    size_t Got = 0;
    Out[Got++] = Byte;
    if (!WaitByte(&Byte, Start, TimeoutCycles)) return 0;
    Out[Got++] = Byte;

    uint16_t Need = Bootloader::FrameSize((uint8_t)(Byte & 0x07));
    if (Need == 0 || Need > Cap) return 0;
    while (Got < Need)
    {
        if (!WaitByte(&Byte, Start, TimeoutCycles)) return 0;
        Out[Got++] = Byte;
    }
    return Bootloader::Decode(Out, Need) != Bootloader::INVALID ? (int)Need : 0;
}

static void SendRaw(const uint8_t *Data, size_t Len)
{
    // Wait for the line to be quiet (BL_SILENCE_BYTES) before driving the half-duplex line:
    // a request's last byte reaches us before the core drops its TX enable, so replying
    // immediately collides. Like the running nodes we count bus activity by draining RX and
    // time the quiet span with the tick counter, not a fixed delay.
    const uint32_t SilenceCycles = BL_SILENCE_BYTES * BL_BYTE_TIME_US * CYCLES_PER_US;
    const uint32_t SilenceMaxCycles = BL_SILENCE_MAX_US * CYCLES_PER_US;
    uint32_t WaitStart = SysTick->CNT;
    uint32_t IdleSince = WaitStart;
    for (;;)
    {
        if (USART1->STATR & USART_FLAG_RXNE)
        {
            (void)USART1->DATAR;
            IdleSince = SysTick->CNT;
        }
        else if ((uint32_t)(SysTick->CNT - IdleSince) >= SilenceCycles)
        {
            break;
        }
        // Bound the wait: a bus that never goes quiet must not block the bootloader.
        if ((uint32_t)(SysTick->CNT - WaitStart) >= SilenceMaxCycles)
        {
            break;
        }
    }

    GPIOD->BSHR = (1u << 4); // drive the line
    for (size_t i = 0; i < Len; i++)
    {
        while ((USART1->STATR & USART_FLAG_TXE) == 0) {}
        USART1->DATAR = Data[i];
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

// Offset of the last successfully programmed chunk (0xFFFFFFFF = nothing yet). Reported back
// on every write so the app resynchronises after a rejected chunk instead of guessing.
static uint32_t LastWritten = 0xFFFFFFFFu;

static void HandleWrite(uint32_t Offset, const uint8_t *Payload)
{
    uint32_t Addr = APP_BASE + Offset;
    bool Ok = (Offset & 31u) == 0; // 32-byte aligned per the doc
    if (Ok) Ok = Addr >= APP_BASE && Addr + Bootloader::PAYLOAD_SIZE <= APP_LIMIT;

    // Erase the 64-byte page on its first half; the second half programs the erased page.
    if (Ok && (Addr & 63u) == 0)
    {
        Ok = FlashErasePage(Addr);
    }
    if (Ok)
    {
        FlashWrite(Addr, Payload, Bootloader::PAYLOAD_SIZE);
        LastWritten = Offset;
    }

    uint8_t Resp[Bootloader::READ_REQ_SIZE];
    Bootloader::EncodeWriteResponse(LastWritten, Resp);
    SendRaw(Resp, sizeof(Resp));
}

static void HandleRead(uint32_t Offset)
{
    uint8_t Data[Bootloader::PAYLOAD_SIZE];
    uint32_t Addr = APP_BASE + Offset;
    if (Addr < APP_BASE || Addr + Bootloader::PAYLOAD_SIZE > APP_LIMIT)
        memset(Data, 0xFF, sizeof(Data));
    else
        memcpy(Data, (const void *)Addr, sizeof(Data));

    uint8_t Resp[Bootloader::DATA_SIZE];
    Bootloader::EncodeReadResponse(Offset, Data, Resp);
    SendRaw(Resp, sizeof(Resp));
}

// The identify response is entirely build-time constant (app size + name), so the whole frame
// - control/parity included - is precomputed and lives in flash. Nothing is built at runtime.
struct IdentifyFrame { uint8_t Bytes[Bootloader::DATA_SIZE]; };

constexpr IdentifyFrame MakeIdentify(uint32_t Space, const char *Name)
{
    IdentifyFrame Frame{};
    Frame.Bytes[0] = Bootloader::START;
    Frame.Bytes[1] = Bootloader::CMD_IDENTIFY_RESP;
    Frame.Bytes[2] = (uint8_t)(Space & 0xFFu);
    Frame.Bytes[3] = (uint8_t)((Space >> 8) & 0xFFu);
    Frame.Bytes[4] = (uint8_t)((Space >> 16) & 0xFFu);
    Frame.Bytes[5] = (uint8_t)((Space >> 24) & 0xFFu);
    for (uint8_t i = 0; i < Bootloader::NAME_SIZE; i++)
    {
        if (Name[i] == '\0') break;
        Frame.Bytes[6 + i] = (uint8_t)Name[i];
    }
    Frame.Bytes[Bootloader::DATA_SIZE - 1] = Bootloader::END;
    Frame.Bytes[1] |= (uint8_t)(Bootloader::Parity(Frame.Bytes, Bootloader::DATA_SIZE) << 3);
    return Frame;
}

constexpr IdentifyFrame Identify = MakeIdentify(APP_LIMIT - APP_BASE, "DAS v0.1");

static void HandleIdentify()
{
    SendRaw(Identify.Bytes, Bootloader::DATA_SIZE);
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
        uint8_t Frame[Bootloader::MAX_FRAME_SIZE];
        int Length = ReceiveFrame(Frame, sizeof(Frame), 2000000u); // 2 s per byte
        if (Length <= 0) continue;

        uint8_t Cmd = (uint8_t)(Frame[1] & 0x07);
        uint32_t Offset = Bootloader::Offset(Frame);
        if (Cmd == Bootloader::CMD_IDENTIFY_REQ)
            HandleIdentify();
        else if (Cmd == Bootloader::CMD_WRITE)
            HandleWrite(Offset, Bootloader::Payload(Frame));
        else if (Cmd == Bootloader::CMD_READ_REQ)
            HandleRead(Offset);
    }
}

#endif // BOOTLOADER_BUILD
