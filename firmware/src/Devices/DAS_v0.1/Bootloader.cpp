// DAS bootloader (Docs/Services/Bootloader.md).
//
// Only the `DAS_bootloader` env defines BOOTLOADER_BUILD, so the app/core envs compile this
// file to nothing (it pulls in ch32v00x.h, which they do not have).
//
// Standalone, linked at 0x0 (2 KB budget); the main app is linked at 0x800. Entered by
// holding the button (PC0, active low) at reset, in which case the white LED (PD0) comes on;
// otherwise the app is launched immediately. It deliberately ignores the standard packet
// format and speaks the raw bootloader frame on the RS485 bus (no 0xAA sync, so the running
// nodes - which wait for 0xAA - ignore it).
//
// Protocol v2 (Core/Functions/Bootloader.h): a 4-bit command field - bits 1-3 the operation, bit 0
// the direction. An identify request returns the app size, the name and the flash page size (64,
// the erase unit); every write (0b0110) is answered by a write response carrying the offset of the
// last chunk that landed; an ERASE request (0b0100) blanks the 64-byte page its offset names; and a
// digest request (0b1000) answers with the CRC32 of the requested range of the app region.
//
// Flash rules: writes are 32-byte aligned (per the doc) and NEVER erase - the erase is its own
// command, so a delta upload can blank exactly the pages it is about to rewrite, and a write the
// node never answers can no longer leave a page blank (E85: the erase used to be a side effect of
// the page-start chunk). A page-start chunk is still checked before it programs: its page's first
// word must read 0xFFFFFFFF, otherwise it is REFUSED with the PAGE named in the reply - that is
// what catches a tool writing into a page it never erased (Bootloader::WriteChunk). The full
// prevention of E85's half-written page is the tool's: erase, then both chunks of the page.
//
// Erases stop at 0x3E00: the storage region at the top of the app region belongs to the app (its
// file table is 512 B of state), so the one command that can be aimed at a whole page per request
// refuses it rather than wiping it. Writes stop there as well (`APP_CODE_SIZE` is the bound the
// write handler passes): a chunk past the code space is refused with its page named, so a write
// cannot reach the storage either. Flash programming only clears bits, so a MID-PAGE chunk in the
// tail used to be programmed and acked as written - and a PAGE-START one there only looked refused
// by accident, because the storage's first word happens not to be blank.
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
#include "Core/Devices/BootloaderCommon.h"  // GpioCfg/GpioOutPP + the identify frame builder

// The app is linked here; the storage region starts at 0x3E00 (512 B flush against the end
// of the chip flash) and is never written. Must match `board_upload.offset_address` in
// platformio.ini (both the DAS_v0_1 app and the DAS_bootloader image share the `[das]` base).
// The upload tool reads that option (scripts/das_app_upload.py); this constant is the
// bootloader's matching copy.
#define APP_BASE 0x800u
// The uploadable area is the whole app region, storage included: [0x800, 0x4000). The storage
// occupies the last 512 B of it at 0x3E00, and the app's binary stops short of that
// (scripts/das_app_upload.py strips the .fixed_data reservation), so a normal update never
// writes the storage - but a full-width image is still accepted.
#define APP_LIMIT 0x4000u
// The uploadable window the app image occupies. Bounds are checked against the OFFSET, not the
// computed address: `APP_BASE + Offset + PAYLOAD_SIZE` wraps near 2^32 and would let a huge
// offset past APP_LIMIT (the same wrap-safe subtraction the Core bootloader uses).
#define APP_SIZE (APP_LIMIT - APP_BASE)
// An erase is refused past this point: the storage region at 0x3E00 is inside the app region,
// but it belongs to the app (its file table is 512 B of state), so the erase command - one page
// per request, 64 B each, but still the only destructive command - stops at the code space's end.
#define APP_CODE_LIMIT 0x3E00u
#define APP_CODE_SIZE (APP_CODE_LIMIT - APP_BASE)


// The flash controller programs/erases through the 0x08000000 alias; the CPU reads/executes
// through 0x00000000 (see Devices/DAS_v0.1/Storage.h).
#define FLASH_CTRL_BASE 0x08000000u

#define CYCLES_PER_US 48u // 48 MHz core clock (SystemInit)
#define BL_BYTE_TIME_US 22u // 460800 baud, matching Docs/RSBus and Packets.md

// Bench-only carrier for the digest: a READ at this offset answers with the CRC32 of the whole
// app region in the first four payload bytes. Only compiled with -DBL_DIGEST_READ_HOOK (the
// DAS_bootloader env does not define it), because the core's bootloader passthrough has no CID
// for the digest command itself.
#define BL_DIGEST_HOOK_OFFSET 0xFFFFFFF0u

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
// The SPL flash helpers are not used: the driver's cost is per call site, not per function, so
// the two sequences with a call site each (the 64-byte page erase, the word program) are plain
// inline functions, and the unlock/lock pairs - two call sites each - are written once and marked
// `noinline`, which keeps a single copy of each and drops the SPL flash driver entirely. Register
// bit values mirror ch32v00x_flash.c.

#define FLASH_KEY1 0x45670123u
#define FLASH_KEY2 0xCDEF89ABu

#define FCR_PG 0x00000001u      // program enable
#define FCR_STRT 0x00000040u    // start
#define FCR_LOCK 0x00000080u    // lock
#define FCR_FLOCK 0x00008000u   // fast-program lock
#define FCR_PAGE_ER 0x00020000u // page erase
#define FSR_BSY 0x00000001u     // busy

// Unlocks both the standard and the fast program/erase controller.
//
// UNLOCKED ONCE, when bootloader mode is entered - not per operation. The controller only acts
// when a command bit is set (FCR_PG / FCR_PAGE_ER), and this image never launches the app after
// unlocking (the launch happens at reset, before this call), so a lock/unlock pair around every
// chunk and every page was pure overhead: measured 32 B of the 2 KB region for the second call
// site plus the lock helper, which is what the erase command needed. The chip's flash controller
// comes back locked on any reset, so nothing inherits this state.
static void FlashUnlock()
{
    FLASH->KEYR = FLASH_KEY1;
    FLASH->KEYR = FLASH_KEY2;
    FLASH->MODEKEYR = FLASH_KEY1;
    FLASH->MODEKEYR = FLASH_KEY2;
}

// Erases the 64-byte page containing `addr` (the 0x08000000 alias).
static void FlashErasePageFast(uint32_t Addr)
{
    FLASH->CTLR |= FCR_PAGE_ER;
    FLASH->ADDR = Addr;
    FLASH->CTLR |= FCR_STRT;
    while (FLASH->STATR & FSR_BSY) {}
    FLASH->CTLR &= ~FCR_PAGE_ER;
}

// Programs one 32-bit word with the standard two-half-word sequence (FCR_PG); the fast
// controller (FCR_PAGE_ER) is used only by the 64-byte page erase.
static void FlashProgramWord(uint32_t Addr, uint32_t Data)
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
    FlashErasePageFast(FLASH_CTRL_BASE + Addr);
    // Page-aligned, so a plain word read: a memcpy here would pull the library routine in.
    return *(volatile uint32_t *)Addr == 0xFFFFFFFFu;
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

    // CTLR1/CTLR2/CTLR3 are at their reset values (frame: 8N1, no flow control) - the bootloader
    // only ever runs straight out of reset, so only the baud rate is written.
    USART1->BRR = (48000000u + 460800u / 2u) / 460800u; // 104 at 48 MHz
    USART1->CTLR1 = USART_Mode_Rx | USART_Mode_Tx | 0x2000u /* UE */;
}

// Reads one raw frame into `Out`; returns its command, or INVALID on timeout/bad frame. `TimeoutUs`
// bounds the whole frame, not each byte, so a stalled frame is abandoned after one window while a
// frame that keeps arriving still completes.
//
// One byte loop over STATR for the whole frame: a per-byte helper call costs 48 B on this target.
static uint8_t ReceiveFrame(uint8_t *Out, uint32_t TimeoutUs)
{
    const uint32_t TimeoutCycles = TimeoutUs * CYCLES_PER_US;
    uint32_t Start = SysTick->CNT;
    size_t Got = 0;
    uint16_t Need = 0;
    for (;;)
    {
        if ((uint32_t)(SysTick->CNT - Start) >= TimeoutCycles) return Bootloader::INVALID;
        if ((USART1->STATR & USART_FLAG_RXNE) == 0) continue;

        uint8_t Byte = (uint8_t)USART1->DATAR;
        if (Got == 0)
        {
            if (Byte != Bootloader::START) continue; // skip anything that is not a frame start
            // Restart the window now the frame has started: the whole-frame budget must cover
            // the body, not the idle search before it. Reusing the search's Start lets a request
            // that arrives late in the window be dropped mid-frame, so the core's own deadline
            // expires - which shows up as intermittent read failures.
            Start = SysTick->CNT;
        }
        Out[Got++] = Byte;
        if (Got == 2)
        {
            // FrameSize() is a table over the 4 command bits whose largest entry is
            // IDENTIFY_RESP_SIZE, i.e. exactly `MAX_FRAME_SIZE`: the size is bounded by
            // construction, so only a command that has no size at all (0) can be rejected here.
            // Keeping the comparison out is 8 B of the 2 KB this image has to live in.
            Need = Bootloader::FrameSize((uint8_t)(Byte & 0x0Fu));
            if (Need == 0) return Bootloader::INVALID;
        }
        else if (Need != 0 && Got == Need)
        {
            break;
        }
    }
    // Decode returns the command (or INVALID), which is what the caller dispatches on.
    return Bootloader::Decode(Out, Need);
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

// The register-level side of a chunk: unlock, erase the page, program one word, read a word back.
// The erase decision and the program-and-verify rule live in Bootloader::WriteChunk (host-tested),
// so there is one copy of each rule and this adapter only touches the flash controller.
struct DasFlash
{
    // The controller is unlocked once, when bootloader mode is entered (FlashUnlock).
    static void Unlock() {}
    static void Lock() {}
    static bool ErasePage(uint32_t Offset) { return FlashErasePage(APP_BASE + Offset); }
    static void ProgramWord(uint32_t Offset, uint32_t Data)
    {
        FlashProgramWord(FLASH_CTRL_BASE + APP_BASE + Offset, Data);
    }
    static uint32_t ReadWord(uint32_t Offset) { return *(volatile uint32_t *)(APP_BASE + Offset); }
};

static void HandleWrite(uint32_t Offset, const uint8_t *Payload)
{
    // Erase positionally, program, and verify - see Bootloader::WriteChunk. `LastWritten` only
    // moves when the bytes read back as sent, so the response's offset is the whole answer: it
    // names the chunk when the write landed and keeps the previous offset when a refusal touched
    // nothing.
    //
    // A page that could not be written is answered with the PAGE, flagged: the erase covers 64
    // bytes, so the tooling has to re-send the page's first chunk as well, and the page address is
    // what says which one. The flag is bit 0 of the offset - every real offset is 32-byte aligned,
    // so bit 0 is never set in an ack - and it is what keeps a failed PAGE-START chunk from being
    // read as a success (its page address IS its own offset). The core relays this offset in its
    // write reply with FLAG_FAIL because it does not equal the offset it asked for.
    const Bootloader::ChunkResult Result =
        Bootloader::WriteChunk<DasFlash>(Offset, Payload, APP_SIZE, APP_CODE_SIZE, LastWritten);

    uint8_t Resp[Bootloader::READ_REQ_SIZE];
    Bootloader::EncodeWriteResponse(Bootloader::WriteAck(Result, Offset, LastWritten), Resp);
    SendRaw(Resp, sizeof(Resp));
}

// CRC32 of the WHOLE app region - there is no offset and no length: the CRC is one number that
// describes the app space, so the tool's expected value is its own CRC of the image padded to the
// app size with 0xFF (the upload blanks the tail so the padding is real flash content). The running
// value is fed through one loop - a table would cost 1 KB the 2 KB image does not have.
__attribute__((always_inline)) static inline uint32_t AppDigest()
{
    const uint8_t *Src = (const uint8_t *)APP_BASE;

    uint32_t Crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < APP_SIZE; i++)
        Crc = Bootloader::Crc32Step(Crc, Src[i]);
    return Crc ^ 0xFFFFFFFFu;
}

static void HandleRead(uint32_t Offset)
{
    // The 32 payload bytes are copied straight from flash into the response frame, so no
    // staging buffer is needed (it also drops the library memcpy/memset from the image).
    static uint8_t Resp[Bootloader::DATA_SIZE];
    const uint8_t *Src = (const uint8_t *)(APP_BASE + Offset);
    bool Valid = Offset <= APP_SIZE && APP_SIZE - Offset >= Bootloader::PAYLOAD_SIZE;
    uint8_t *Dst = Resp + Bootloader::HEADER_SIZE; // the payload slot

#ifdef BL_DIGEST_READ_HOOK
    // Bench hook, compiled only when the env asks for it (never in the production image): the
    // core's bootloader passthrough speaks CIDs 0x20/0x21/0x22 and has none for the digest
    // command, so a bench build hands the CRC out through a READ at an offset no real request
    // uses. The computation is AppDigest, the same one the digest command runs - only the carrier
    // frame differs.
    if (Offset == BL_DIGEST_HOOK_OFFSET)
    {
        uint32_t Crc = AppDigest();
        for (uint8_t i = 0; i < Bootloader::PAYLOAD_SIZE; i++)
            Dst[i] = i < 4 ? (uint8_t)(Crc >> (8u * i)) : 0xFFu;
        Bootloader::EncodeReadFrame(Offset, Resp);
        SendRaw(Resp, sizeof(Resp));
        return;
    }
#endif

    for (uint8_t i = 0; i < Bootloader::PAYLOAD_SIZE; i++)
        Dst[i] = Valid ? Src[i] : 0xFFu;

    Bootloader::EncodeReadFrame(Offset, Resp);
    SendRaw(Resp, sizeof(Resp));
}

constexpr IdentifyFrame Identify =
    MakeIdentify(APP_LIMIT - APP_BASE, Bootloader::PAGE_SIZE, "DAS v0.1");

static void HandleIdentify()
{
    SendRaw(Identify.Bytes, Bootloader::IDENTIFY_RESP_SIZE);
}

static void HandleDigest()
{
    static uint8_t Resp[Bootloader::DIGEST_RESP_SIZE];
    Bootloader::EncodeDigestResponse(AppDigest(), Resp);
    SendRaw(Resp, sizeof(Resp));
}

// Blanks the 64-byte page named by the offset - the erase the write path no longer performs.
// The offset must be PAGE-ALIGNED; a misaligned one is refused with the flag and blanks nothing
// (Bootloader::ErasePage). One page per request: the tool owns the loop. The answer is the page,
// flagged when it is not blank, so an ack always means the page can be programmed.
static void HandleErase(uint32_t Offset)
{
    uint8_t Resp[Bootloader::ERASE_RESP_SIZE];
    Bootloader::EncodeEraseResponse(Bootloader::ErasePage<DasFlash>(Offset, APP_CODE_SIZE), Resp);
    SendRaw(Resp, sizeof(Resp));
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------

static void JumpToApp()
{
    __disable_irq();
    // No USART1 shutdown here: the launch happens at reset, BEFORE SetupUart, so the port is
    // still at its reset value and the app's own startup configures it. (Writing CTLR1 here
    // measured 20 B of the 2 KB region for an instruction pair that cannot run.)
    // The app's own startup sets sp/gp/mtvec; a plain jump to its reset vector is enough.
    ((void (*)(void))APP_BASE)();
    for (;;) {} // unreachable
}

// The frame loop: serve until reset. Split out so the published entry below runs the same loop the
// button path does instead of a second copy of it.
static void Serve()
{
    for (;;)
    {
        uint8_t Frame[Bootloader::MAX_FRAME_SIZE];
        uint8_t Cmd = ReceiveFrame(Frame, 2000000u); // 2 s per frame
        if (Cmd == Bootloader::INVALID) continue;

        uint32_t Offset = Bootloader::Offset(Frame);
        if (Cmd == Bootloader::CMD_IDENTIFY_REQ)
            HandleIdentify();
        else if (Cmd == Bootloader::CMD_WRITE)
            HandleWrite(Offset, Bootloader::Payload(Frame));
        else if (Cmd == Bootloader::CMD_READ_REQ)
            HandleRead(Offset);
        else if (Cmd == Bootloader::CMD_ERASE_REQ)
            HandleErase(Offset);
        else if (Cmd == Bootloader::CMD_JUMP_REQ)
            JumpToApp(); // offset ignored, no response: the app answering is the proof
#ifndef BL_DIGEST_READ_HOOK
        // The bench build reaches the same AppDigest through the read hook below (its carrier
        // frame), so it leaves this dispatch out to stay inside the 2 KB.
        else if (Cmd == Bootloader::CMD_DIGEST_REQ)
            HandleDigest();
#endif
    }
}

// ---------------------------------------------------------------------------
// Published entry (contract)
// ---------------------------------------------------------------------------
// The app jumps here to hand the node back to the bootloader without a reset: the address is
// Bootloader::BL_ENTRY_ADDR (0x7FC), where `.bootentry` holds the jump to this function - the
// stub lives in the last word of the region so the entry does not have to fit in the free tail.
// It skips the button test and the settle delay (the app decided), and does what the C startup
// would have done for the bootloader's own RAM statics: the app has been using that RAM. LastWritten
// is the only one read before it is written (every frame buffer is filled whole before its send),
// and a stale value would name a garbage offset in a refused write's ack.
//
// Interrupts go off first: the app leaves USART1's RXNE interrupt enabled and its own ISR would
// steal the bytes this loop polls for. The port needs nothing beyond SetupUart's CTLR1 write - the
// app's CTLR2/CTLR3 are 0 and its BRR is the same 104, so the frames match. SysTick is already
// running the way SendRaw counts on (the app sets CTLR |= 0x05 too).
extern "C" void BootloaderEntry(void)
{
    __disable_irq();
    LastWritten = 0xFFFFFFFFu;
    SetupUart();
    FlashUnlock();
    GPIOD->BSHR = (1u << 0); // white LED on
    Serve();
}

// The 4-byte contract stub: the app calls Bootloader::BL_ENTRY_ADDR and lands here. Pinned by
// --section-start=.bootentry=0x7FC in platformio.ini; scripts/bootloader_entry_check.py fails the
// build if this is not at that address or the image has grown into it. The linked row is 2 B
// (`jal <BootloaderEntry>`), RVC having a short form for a call this close.
extern "C" void BootloaderEntryStub(void);
__attribute__((section(".bootentry"), used, noinline)) void BootloaderEntryStub(void)
{
    BootloaderEntry();
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
    // Counted on SysTick (48 MHz core clock) like SendRaw's silence window, not as a loop of
    // iterations whose cost depends on what the compiler emits for it.
    {
        uint32_t SettleStart = SysTick->CNT;
        while ((uint32_t)(SysTick->CNT - SettleStart) < 20000u * CYCLES_PER_US) {}
    }
#ifndef BOOTLOADER_FORCE
    // BOOTLOADER_FORCE (test hook) skips the button check so the RSBus update path can be
    // exercised without physically holding PC0.
    if (GPIOC->INDR & (1u << 0))
        JumpToApp();
#endif

    // Bootloader mode: white LED on, unlock the flash controller for the whole session, then
    // serve frames until reset. The same entry the app jumps to, so there is one setup path.
    BootloaderEntry();
}

#endif // BOOTLOADER_BUILD
