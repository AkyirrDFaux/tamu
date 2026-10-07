// Valu v2.0 (CH32V203G8R6) bootloader (Docs/Services/Bootloader.md).
//
// Only the `Valu_bootloader` env defines BOOTLOADER_BUILD, so the app/core envs compile
// this file to nothing (it pulls in ch32v20x.h / tusb.h, which they do not have).
//
// Standalone, linked at 0x0 (12 KB budget); the main app is linked at 0x3000. Entered by
// holding the LED-button (PA2, active high per Docs/Devices.md) at reset, in which case
// the red LED comes on; otherwise the app is launched immediately. It deliberately ignores
// the standard packet format and speaks the raw bootloader frame on USB (CDC-ACM). Writes
// give no acknowledgement: the app paces the flash and verifies with read requests.
//
// Flash rules: writes are 32-byte aligned (per the doc); the bootloader erases the
// containing 4 KB sector on the first write into it and programs the 32 bytes. It never
// writes outside [APP_BASE, APP_LIMIT). The erase/program mode choice (standard 4 KB erase,
// not the 256-byte fast page) and its manual citations are in the flash section below.
//
// Flash driver: the WCH SPL standard-mode helpers (FLASH_ErasePage + FLASH_ProgramWord),
// i.e. vendor code, rather than a hand-rolled direct-register sequence - the bootloader
// cannot be exercised without hardware, so vendor code correct by construction beats a
// guessed register sequence.
//
// Clock: the bootloader runs at the framework SystemInit clock (144 MHz, hsi+pll). Per
// CH32VRM.pdf RM 32.1 note 2 flash operations are recommended at <=120 MHz (FLASH access
// clock <=60 MHz); the framework default CTLR.SCKMOD=0 gives SYSCLK/2 = 72 MHz, above that
// ceiling. The manual's prescribed mitigation (halve HCLK around the flash op, or run the
// whole part slower) is NOT implemented - see the report's open items. This is the one part
// of the design the manual does not settle cleanly (its CTLR.SCKMOD bit is described as
// SYSCLK-derived, while the note speaks of halving HCLK).
//
// This file is built ONLY into the `Valu_bootloader` env (its build_src_filter names it),
// and carries the USB bring-up, descriptors and ISR too, so the env's source set is a
// single file (mirroring Devices/DAS_v0.1/Bootloader.cpp).

#ifdef BOOTLOADER_BUILD

#include <cstdint>
#include <cstring>

#include "ch32v20x.h"
#include "ch32v20x_flash.h"
#include "tusb.h"

#include "Core/Functions/Bootloader.h"

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

// The app is linked here; must match `board_upload.offset_address` for the [valu] app base.
// 0x3000, not 0x2000: the bootloader region is 12 KB (see [env:Valu_bootloader]) because the
// FSDEV/USBD USB stack TinyUSB needs overflows an 8 KB region by ~2 KB.
#define APP_BASE 0x3000u

// The app must not be allowed to eat the storage region. Docs/Devices.md gives the Valu an
// 8 kB storage region; it lives in the top 8 kB of the 64 kB flash (0xE000..0x10000), so the
// writable app window is [0x2000, 0xE000) = 48 kB. This mirrors the DAS reserving 0x3E00
// upward (its storage is 512 B flush against the end).
//
// OWNER DECISION TO CONFIRM: 0xE000 (top 8 kB reserved) is inferred from the "Storage: 8kB"
// line in Docs/Devices.md, not from an explicit address in the spec. If the storage region
// is placed elsewhere, this constant (and only this constant) moves.
#define APP_LIMIT 0xE000u

// The flash controller programs/erases through the 0x08000000 alias; the CPU reads and
// executes through 0x00000000 (same aliasing as the DAS, Devices/DAS_v0.1/Storage.h).
#define FLASH_CTRL_BASE 0x08000000u

// The CH32V20x has two USB device controllers, and the one TinyUSB drives here is the
// FSDEV/USBD controller on roothub port 0. (Port 1 is the USBFS/OTG controller, which needs its
// own 48 MHz source via RCC_CFGR2.USBFSSRC and is not what this board is wired for.) The
// project's previous working Valu v2 release used port 0 as well.
#define USBD_RHPORT 0

// ---------------------------------------------------------------------------
// GPIO (direct register: CFGLR is 4 bits per pin; PA2 < 8)
// ---------------------------------------------------------------------------

static void GpioCfg(GPIO_TypeDef *port, uint32_t pin, uint32_t cfg)
{
    uint32_t shift = pin * 4u;
    port->CFGLR = (port->CFGLR & ~(0xFu << shift)) | (cfg << shift);
}

static void GpioOutPP(GPIO_TypeDef *port, uint32_t pin) { GpioCfg(port, pin, 0x3u); } // out push-pull


// ===========================================================================
// USB CDC-ACM (TinyUSB) - identity, descriptors, hardware bring-up, ISR
// ===========================================================================
//
// The bring-up below follows the project's previous working Valu v2 release step for step
// (datasheet/tamu-beta_1.zip -> src/Hardware/USB.h, the BOARD_Valu_v2_0 branch): USBPRE divider,
// the APB1 USBD clock gate, a USBD reset pulse, tusb_init(), then the NVIC entry and an explicit
// D+ pull-up. The earlier version of this file - USBFS on port 1, no reset pulse, no pull-up -
// never put anything on the bus.

// USB identity decision. NOT the WCH-Link probe's 0x1A86:0x8010: that is the board's
// declared hwids entry AND the identity of the probe on this machine's bus, so a CDC device
// pretending to be it is indistinguishable in a device list. 0x1A86 is kept as the silicon
// vendor (the CH32V203 has no factory USB identity of its own); the PID is a placeholder.
//
// OWNER DECISION TO CONFIRM: 0x1A86 / 0x6001. The host app opens the port by name
// (flutter_libserialport), never by VID/PID, so this is identity hygiene, not function.
#define USB_VID 0x1A86
#define USB_PID 0x6001
#define USB_BCD 0x0200

// --- Clock, gate and reset. Runs BEFORE tusb_init(). ---
static void UsbHwInit(void)
{
    // USBPRE[1:0] (RCC_CFGR0 bits 23:22) = 10b -> PLL/3 = 48 MHz. The manual routes exactly this
    // clock to USBD: "By configuring the USBPRE[1:0] bits in the RCC_CFGR0 register, the 48MHz
    // clock is output to USBD" (CH32VRM.pdf 3.3.5.6). The working release selects the same
    // divider via RCC_USBCLKConfig(RCC_USBCLKSource_PLLCLK_Div3).
    RCC->CFGR0 = (RCC->CFGR0 & ~(3u << 22)) | ((uint32_t)RCC_USBCLKSource_PLLCLK_Div3 << 22);

    // APB1PCENR bit 23 ("USBD"; SPL name RCC_APB1Periph_USB) gates the device controller's clock.
    RCC->APB1PCENR |= RCC_APB1Periph_USB;

    // Reset the USBD peripheral before use, as the working release does: the controller may be in
    // an unknown state left by the ROM bootloader's USB session, and would then stay silent.
    RCC->APB1PRSTR |= RCC_APB1Periph_USB;
    for (volatile uint32_t i = 0; i < 200000u; i++) {} // ~1 ms at 144 MHz
    RCC->APB1PRSTR &= ~RCC_APB1Periph_USB;
}

// --- NVIC entry and the D+ pull-up. Runs AFTER tusb_init(). ---
static void UsbConnect(void)
{
    NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 1);
    NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);
    EXTEN->EXTEN_CTR |= EXTEN_USBD_PU_EN; // pull up D+ - without this nothing appears on the bus
}

// --- TinyUSB device interrupt ---
// The FSDEV/USBD controller raises the ST-style low-priority vector. This handler name is the one
// the working release uses, and startup_ch32v20x_D6.S already carries it as a weak vector entry.
// WCH-Interrupt-fast gives the hardware prologue/epilogue their toolchain examples rely on.
extern "C" void USB_LP_CAN1_RX0_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
extern "C" void USB_LP_CAN1_RX0_IRQHandler(void)
{
    tud_int_handler(USBD_RHPORT);
}

extern "C" {

//--------------------------------------------------------------------
// Device descriptor
//--------------------------------------------------------------------
static const tusb_desc_device_t s_desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) { return (uint8_t const *)&s_desc_device; }

//--------------------------------------------------------------------
// Configuration descriptor: one interface, notification + two data EPs
//--------------------------------------------------------------------
enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_TOTAL };

#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t s_desc_configuration[] = {
    // config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    // interface number, string index, EP notification address and size, EP data address
    // (out, in) and size. The host sees this as a CDC-ACM serial port.
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_desc_configuration;
}

//--------------------------------------------------------------------
// String descriptors
//--------------------------------------------------------------------
static char const *const s_string_desc_arr[] = {
    (const char[]){0x09, 0x04}, // 0: supported language = English (0x0409)
    "Tamu",                     // 1: Manufacturer
    "Valu v2.0 Bootloader",     // 2: Product
    "0001",                     // 3: Serial (static; a per-unit serial is a later change)
    "Valu v2.0 CDC",            // 4: CDC interface
};

static uint16_t s_desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t chr_count;

    if (index == 0)
    {
        memcpy(&s_desc_str[1], s_string_desc_arr[0], 2);
        chr_count = 1;
    }
    else
    {
        if (index >= sizeof(s_string_desc_arr) / sizeof(s_string_desc_arr[0]))
            return NULL;

        const char *str = s_string_desc_arr[index];
        chr_count = strlen(str);
        if (chr_count > 32)
            chr_count = 32;

        for (size_t i = 0; i < chr_count; i++)
            s_desc_str[1 + i] = (uint16_t)str[i];
    }

    s_desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return s_desc_str;
}

} // extern "C"

// ===========================================================================
// Flash (WCH SPL, standard programming mode)
// ===========================================================================
//
// Sequence per CH32VRM.pdf chapter 32:
//   - Standard programming (RM 32.2.1, "the default programming method"): erase is done in
//     4 KB units (RM 32.5.4, CTLR.PER) and programming in 2-byte units (RM 32.5.3, CTLR.PG).
//     The framework's FLASH_ErasePage issues the 4 KB erase and FLASH_ProgramWord the two
//     16-bit writes, so the halfword rule (finding #4) is satisfied by vendor code.
//   - The 256-byte page in Docs/Devices.md is the FAST path (CTLR.FTER / FTPG, RM 32.5.6-7):
//     it needs its own unlock (FLASH_MODEKEYR, RM 32.5.5) and programs a whole 256-byte page
//     at once. This bootloader does NOT use it: the standard SPL pair is a single coherent
//     mode, needs no page buffer and no read-modify-write.
//
// So the erase unit is 4 KB, not the fast path's 256 B. The 32-byte protocol writes map onto
// it by erasing the containing 4 KB sector on the first write into it (offset % 4096 == 0),
// the direct analog of the DAS erasing its 64-byte page on its first 32-byte half.
//
// OWNER DECISION TO CONFIRM: standard 4 KB erase (this) vs the 256-byte fast-mode path. This
// is a deviation from the 256-byte page Docs/Devices.md quotes, chosen for a single coherent
// vendor-SPL mode. A consequence: a chunk cannot be corrected in place - a bad write inside
// an already-programmed sector needs the whole image rewritten from offset 0 (the model the
// core ESP32 bootloader uses, where offset 0 resets its erased-sector bitmap).

#define FLASH_ERASE_SECTOR 4096u

// RM 32.3: enhanced read mode must be off before any erase/program ("otherwise the erasing
// and programming operations will fail"). It is 0 after reset (RM 32.3 note 3), so this is a
// defensive clear; CTLR.EHMOD is bit 24 (RM 32.4.3).
#define FLASH_CTLR_EHMOD (1u << 24)

// --- Flash-path diagnostics ---------------------------------------------------------------
// A read-request at this pseudo-offset answers with this state instead of flash content, so the
// erase path can be inspected from the host. Real app offsets are < APP_LIMIT, so it cannot
// collide with one.
#define DIAG_OFFSET 0xFFFFFFE0u
static uint32_t g_diag[8]; // [0] calls, [1] STATR after the flag clear, [2] STATR before erase,
                           // [3] FLASH_ErasePage status, [4] STATR after, [5] verify word,
                           // [6] CTLR after, [7] CFGR0 while slowed

// RM 32.2 note 2: FLASH operations need the FLASH access clock <= 60 MHz. That clock is
// SYSCLK or SYSCLK/2, selected by FLASH_CTLR.SCKMOD (bit 25; default /2) - NOT HCLK. This board
// runs at 144 MHz, so even /2 gives 72 MHz, over the ceiling, and erase/program silently no-op
// (STATR.EOP still sets). The earlier code divided HCLK, which does not change the flash access
// clock at all - hence the ineffective erase.
//
// Fix: drop SYSCLK to HSI (8 MHz) for the operation -> flash access = 4 MHz. The PLL keeps
// running, so USBPRE = PLL/3 (48 MHz to USBD) is unaffected; restore the PLL source afterwards.
#define CFGR0_SW_MASK (0x3u << 0)  // SYSCLK source select (00 HSI, 10 PLL)
#define CFGR0_SWS_MASK (0x3u << 2) // SYSCLK source status

static uint32_t FlashSlowDown(void)
{
    uint32_t old = RCC->CFGR0 & CFGR0_SW_MASK;
    RCC->CFGR0 = (RCC->CFGR0 & ~CFGR0_SW_MASK) | 0x0u; // SW = HSI (8 MHz)
    while ((RCC->CFGR0 & CFGR0_SWS_MASK) != 0x0u) {}    // wait until SWS reports HSI
    return old;
}

static void FlashRestore(uint32_t old)
{
    RCC->CFGR0 = (RCC->CFGR0 & ~CFGR0_SW_MASK) | (old & CFGR0_SW_MASK); // back to PLL
    while ((RCC->CFGR0 & CFGR0_SWS_MASK) != (old & CFGR0_SWS_MASK)) {}
}

static bool FlashEraseSector(uint32_t addr /* 0x00000000 domain, 4 KB aligned */)
{
    g_diag[0]++;
    // Clear the status flags first: STATR is write-1-to-clear, and a pending WRPRTERR would make
    // the SPL's entry check report an error (that path is gone now, but the flags still must not
    // be left set for the next operation).
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_WRPRTERR);
    g_diag[1] = FLASH->STATR;

    uint32_t hpre = FlashSlowDown(); // RM 32.1 note 2
    g_diag[7] = RCC->CFGR0;

    FLASH_Unlock(); // RM 32.5.2: KEYR = 0x45670123 then 0xCDEF89AB (releases LOCK)
    FLASH->CTLR &= ~FLASH_CTLR_EHMOD;
    g_diag[2] = FLASH->STATR;

    // Hand-rolled standard erase (RM 32.5.4): PER, page address, STRT, then poll BSY.
    // Deliberately NOT the SPL's FLASH_ErasePage any more - measured on hardware it reported
    // FLASH_TIMEOUT while the hardware set EOP, so its exit status and its internal waiting
    // cannot be trusted on this part. Polling BSY directly has no arbitrary timeout to expire
    // early, so when this returns the erase has genuinely finished.
    FLASH->CTLR |= (1u << 1);         // PER
    FLASH->ADDR = FLASH_CTRL_BASE + addr;
    FLASH->CTLR |= (1u << 6);         // STRT
    while (FLASH->STATR & 1u) {}      // BSY
    FLASH->CTLR &= ~(1u << 1);        // clear PER

    g_diag[3] = FLASH->STATR;
    g_diag[6] = FLASH->CTLR;

    // CH32V203 flash quirk (the previous working Valu v2 release hit it too: its
    // Hardware/Memory.h programs 0xFFFFFFFF after every erase, commented "Prevent incorrect
    // reading"): a just-erased word does NOT read back as 0xFFFFFFFF until it has been through a
    // program cycle - it reads a bogus pattern (measured 0xE339E339 here). Program 0xFFFFFFFF into
    // every word of the sector so reads become correct; writing all-1s does not change an erased
    // cell. Without this the verify below "fails" and HandleWrite skips the sector's FIRST chunk -
    // which is exactly the 7/881 read-back mismatches (one per sector).
    for (uint32_t o = 0; o < FLASH_ERASE_SECTOR; o += 4)
    {
        uint32_t w;
        memcpy(&w, (const void *)(addr + o), sizeof(w));
        if (w != 0xFFFFFFFFu)
            FLASH_ProgramWord(FLASH_CTRL_BASE + addr + o, 0xFFFFFFFFu);
    }

    // Verify: both reads are now expected to be 0xFFFFFFFF. The first is taken immediately, the
    // second after a delay, in case the first was served while an operation was still settling.
    uint32_t r1 = 0, r2 = 0;
    memcpy(&r1, (const void *)addr, sizeof(r1));
    for (volatile uint32_t i = 0; i < 200000u; i++) {} // ~1 ms
    memcpy(&r2, (const void *)addr, sizeof(r2));
    g_diag[4] = r1;
    g_diag[5] = r2;

    FLASH_Lock();
    FlashRestore(hpre);
    return r2 == 0xFFFFFFFFu;
}

static void FlashWrite(uint32_t addr /* 0x00000000 domain */, const uint8_t *src, uint32_t len)
{
    uint32_t clk = FlashSlowDown(); // flash access clock <= 60 MHz, same rule as the erase
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_WRPRTERR);
    FLASH_Unlock();
    FLASH->CTLR &= ~FLASH_CTLR_EHMOD;
    for (uint32_t off = 0; off < len; off += 4)
    {
        uint32_t w;
        memcpy(&w, src + off, sizeof(w));
        FLASH_ProgramWord(FLASH_CTRL_BASE + addr + off, w); // two 16-bit writes (RM 32.5.3)
    }
    FLASH_Lock();
    FlashRestore(clk);
}

// ===========================================================================
// Frame server
// ===========================================================================
//
// Raw bootloader frames arrive as a CDC byte stream; a USB read hands over an arbitrary
// number of bytes, so the bytes are fed through a small state machine that scans for the
// 0xCA start marker and only then trusts the control byte's length (the DAS's ReceiveFrame
// does the same scan, byte-at-a-time, on the UART).

static uint8_t s_frame[Bootloader::MAX_FRAME_SIZE];
static uint16_t s_have = 0; // bytes currently in s_frame
static uint16_t s_need = 0; // total bytes the frame needs once known

static void HandleWrite(uint32_t offset, const uint8_t *payload);

static void SendReadResponse(uint32_t offset)
{
    uint8_t data[Bootloader::PAYLOAD_SIZE];
    if (offset == DIAG_OFFSET)
    {
        // Diagnostics pseudo-offset (see g_diag): report the flash path's own state.
        memcpy(data, g_diag, sizeof(data));
    }
    else
    {
        uint32_t addr = APP_BASE + offset;
        if (addr < APP_BASE || addr + Bootloader::PAYLOAD_SIZE > APP_LIMIT)
            memset(data, 0xFF, sizeof(data));
        else
            memcpy(data, (const void *)addr, sizeof(data));
    }

    uint8_t resp[Bootloader::DATA_SIZE];
    Bootloader::EncodeReadResponse(offset, data, resp);
    tud_cdc_write(resp, sizeof(resp));
    tud_cdc_write_flush();
}

static void DispatchFrame(const uint8_t *frame)
{
    uint8_t cmd = (uint8_t)(frame[1] & 0x03u);
    uint32_t offset = Bootloader::Offset(frame);
    if (cmd == Bootloader::CMD_WRITE)
        HandleWrite(offset, Bootloader::Payload(frame));
    else if (cmd == Bootloader::CMD_READ_REQ)
        SendReadResponse(offset);
}

static void FeedByte(uint8_t b)
{
    if (s_have == 0)
    {
        if (b == Bootloader::START)
        {
            s_frame[0] = b;
            s_have = 1;
            s_need = 2; // need the control byte to learn the length
        }
        return;
    }

    s_frame[s_have++] = b;

    if (s_have == 2)
    {
        uint16_t need = Bootloader::FrameSize((uint8_t)(b & 0x03u));
        if (need == 0) // unknown command: drop and resync
        {
            s_have = 0;
            s_need = 0;
            return;
        }
        s_need = need;
    }

    if (s_have >= s_need)
    {
        if (Bootloader::Decode(s_frame, s_need))
            DispatchFrame(s_frame);
        s_have = 0;
        s_need = 0;
    }
}

static void HandleWrite(uint32_t offset, const uint8_t *payload)
{
    if ((offset & 31u) != 0)
        return; // 32-byte aligned per the doc
    uint32_t addr = APP_BASE + offset;
    if (addr < APP_BASE || addr + Bootloader::PAYLOAD_SIZE > APP_LIMIT)
        return;

    // Erase the containing 4 KB sector on the first write into it, then the 128 32-byte
    // writes of the sector program into the freshly erased sector.
    if ((addr & (FLASH_ERASE_SECTOR - 1u)) == 0)
    {
        if (!FlashEraseSector(addr))
            return;
    }
    FlashWrite(addr, payload, Bootloader::PAYLOAD_SIZE);
}

// ===========================================================================
// Entry
// ===========================================================================

static void JumpToApp(void)
{
    __disable_irq();
    // The app's own startup sets sp/gp/mtvec; a plain jump to its reset vector is enough (exactly
    // what the DAS bootloader does). The clock is deliberately LEFT RUNNING on the 144 MHz PLL -
    // the app's SystemInit wrapper (Devices/Valu_v2.0/System.h) detects that and skips the
    // framework's SystemInit, which would otherwise hang (see System.h). Attempting to put SYSCLK
    // back on HSI here is NOT safe: the bootloader then waits on SWS, which never settles.
    ((void (*)(void))APP_BASE)();
    for (;;) {} // unreachable
}

int main(void)
{
    // LED-button PA2: active high, so it idles low on a pull-down and reads high when held.
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOA;
    GPIOA->BCR = (1u << 2); // pull-down
    GpioCfg(GPIOA, 2, 0x8u);

    // Let the pull-down settle, then sample: released (low) launches the app.
    for (volatile uint32_t i = 0; i < 200000u; i++) {}
#ifndef BOOTLOADER_FORCE
    // BOOTLOADER_FORCE (test hook) skips the button check so the USB update path can be
    // exercised without physically holding PA2.
    if ((GPIOA->INDR & (1u << 2)) == 0)
        JumpToApp();
#endif

    // Bootloader mode: light the red LED (same pin, now an output driven high), then bring
    // up USB and serve frames until reset.
    GpioOutPP(GPIOA, 2);
    GPIOA->BSHR = (1u << 2);

    UsbHwInit();
    tusb_init();
    // NVIC entry and the explicit D+ pull-up. The device only appears on the bus after these, and
    // the working release does them in this order, straight after tusb_init().
    UsbConnect();
    // Global interrupts are off at reset and nothing else here enables them, but the USB interrupt
    // must reach tud_int_handler. Enabled last, so no USB event is serviced before the stack and
    // the pull-up are both in place.
    __enable_irq();

    for (;;)
    {
        tud_task();

        while (tud_cdc_available())
        {
            uint8_t buf[64];
            uint32_t n = tud_cdc_read(buf, sizeof(buf));
            if (n == 0)
                break;
            for (uint32_t i = 0; i < n; i++)
                FeedByte(buf[i]);
        }
    }
}

#endif // BOOTLOADER_BUILD
