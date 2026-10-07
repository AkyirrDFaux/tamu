#pragma once

// USB link for the App Interface (Docs/Services/App Interface.md) on the Valu v2.0.
//
// The Valu's only byte stream is this USB CDC-ACM port: the host (the app, or a test harness
// such as test/tamu_proto.py) writes link frames and the device answers over the same port.
// The wire protocol is exactly the core's (Core/Functions/AppInterface.h routes the packets;
// Devices/Tamu_v2.0A/AppUSB.h speaks the same link framing):
//
//   0xFA | CRC8 | Length | Payload (max 60 bytes of packet stream) | 0xBF
// The CRC8 covers Length + Payload. The payload is a serialized PacketFrame stream
// (crc8-first wire layout, see PacketToWire).
//
// Unlike the ESP32 core (AppUSBTick/AppUSBStartTask on FreeRTOS), there is no RTOS here:
// AppInterfacePump (the main loop) calls AppUSBTick() every iteration, which pumps TinyUSB and
// drains the CDC receive buffer into the shared RX queue. The USB interrupt only services
// TinyUSB (tud_int_handler), never these framers, so no locking is needed
// (APP_INTERFACE_SINGLE_THREADED).

#include <cstdint>
#include <cstring>
#include "ch32v20x.h"
#include "tusb.h"
#include "Core/Functions/AppInterface.h"

#define USB_FRAME_START 0xFA
#define USB_FRAME_STOP 0xBF
#define USB_FRAME_MAX_PAYLOAD 60

// ===========================================================================
// USB hardware bring-up - the recipe proven on this board (see
// Devices/Valu_v2.0/Bootloader.cpp, which documents it in full). FSDEV/USBD is roothub
// port 0; USBPRE = /3 for the 144 MHz clock; a USBD reset pulse; the NVIC entry; and the
// explicit D+ pull-up without which nothing appears on the bus.
// ===========================================================================

#define USB_VID 0x1A86
#define USB_PID 0x6001
#define USB_BCD 0x0200

// Runs BEFORE tusb_init().
static void UsbHwInit(void)
{
    // USBPRE[1:0] (RCC_CFGR0 bits 23:22) = 10b -> PLL/3 = 48 MHz to USBD.
    RCC->CFGR0 = (RCC->CFGR0 & ~(3u << 22)) | ((uint32_t)RCC_USBCLKSource_PLLCLK_Div3 << 22);
    // APB1PCENR bit 23 gates the device controller's clock.
    RCC->APB1PCENR |= RCC_APB1Periph_USB;
    // Reset the USBD peripheral before use (the ROM bootloader may have left it in an
    // unknown state).
    RCC->APB1PRSTR |= RCC_APB1Periph_USB;
    for (volatile uint32_t i = 0; i < 200000u; i++) {} // ~1 ms at 144 MHz
    RCC->APB1PRSTR &= ~RCC_APB1Periph_USB;
}

// Runs AFTER tusb_init().
static void UsbConnect(void)
{
    NVIC_SetPriority(USB_LP_CAN1_RX0_IRQn, 1);
    NVIC_EnableIRQ(USB_LP_CAN1_RX0_IRQn);
    EXTEN->EXTEN_CTR |= EXTEN_USBD_PU_EN; // pull up D+ - without this nothing appears on the bus
}

// TinyUSB device interrupt (FSDEV/USBD raises the ST-style low-priority vector; the startup
// file carries it as a weak entry).
extern "C" void USB_LP_CAN1_RX0_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
extern "C" void USB_LP_CAN1_RX0_IRQHandler(void)
{
    tud_int_handler(0);
}

extern "C" {

//--------------------------------------------------------------------
// Descriptors: one CDC-ACM function
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

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_TOTAL };

#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t s_desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_desc_configuration;
}

static char const *const s_string_desc_arr[] = {
    (const char[]){0x09, 0x04}, // 0: supported language = English (0x0409)
    "Tamu",                     // 1: Manufacturer
    "Valu v2.0",                // 2: Product
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

// ---------------------------------------------------------------------------
// Link framing (both directions): 0xFA | CRC8 | Length | Payload | 0xBF
// ---------------------------------------------------------------------------

struct UsbFramer
{
    bool in_frame = false;
    uint16_t len = 0;
    uint8_t buf[2 + USB_FRAME_MAX_PAYLOAD + 1]; // crc + len + payload + stop

    void Reset()
    {
        in_frame = false;
        len = 0;
    }

    // Feeds one byte. Returns: 0 = incomplete, >0 = valid frame (payload length), -1 = invalid.
    int Feed(uint8_t b, uint8_t *payload)
    {
        if (!in_frame)
        {
            if (b == USB_FRAME_START)
            {
                in_frame = true;
                len = 0;
            }
            return 0;
        }

        buf[len++] = b;

        if (len < 2)
            return 0; // CRC pending

        uint8_t n = buf[1];
        if (n > USB_FRAME_MAX_PAYLOAD)
        {
            Reset();
            return -1; // impossible length: resync
        }

        if (len < (uint8_t)(2 + n + 1))
            return 0; // payload/stop pending

        in_frame = false;
        if (buf[2 + n] != USB_FRAME_STOP)
            return -1;

        if (Crc8(&buf[1], (uint16_t)(1 + n)) != buf[0]) // CRC8 over Length + Payload
            return -1;

        memcpy(payload, &buf[2], n);
        return n;
    }
};

static UsbFramer s_app_framer;

// Turns the continuous crc8-first packet stream into frames (same parser as the core).
struct WireStreamParser
{
    PacketFrame frame __attribute__((aligned(4)));
    uint16_t got = 0;
    bool full = false;

    void Reset()
    {
        got = 0;
        full = false;
    }

    // Returns true when a complete CRC-valid PacketFrame was assembled into `frame`.
    bool Feed(uint8_t b)
    {
        if (full)
            return false;

        if (got >= sizeof(PacketFrame))
        {
            Reset();
            return false;
        }
        ((uint8_t *)&frame)[got++] = b;

        if (got < 4)
            return false; // payload_len not known yet

        uint16_t need = PacketWireSize(&frame);
        if (got < need)
            return false;

        full = true;
        bool ok = Crc8(&frame.flags, (uint16_t)(11 + PayloadBytes(frame))) == frame.crc8;
        return ok; // invalid CRC: caller should Reset() and resync
    }
};

static WireStreamParser s_wire_parser;

// Handles one validated link-frame payload: parses the contained packet stream and queues
// complete packets for dispatch by AppInterfacePump.
static void AppRxStream(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
    {
        if (s_wire_parser.Feed(data[i]))
        {
            AppInterfaceEnqueue(s_wire_parser.frame);
            s_wire_parser.Reset();
        }
        else if (s_wire_parser.full || s_wire_parser.got > sizeof(PacketFrame))
        {
            s_wire_parser.Reset(); // CRC failure / overflow: drop and resync
        }
    }
}

// ---------------------------------------------------------------------------
// App Interface hooks (called from AppInterfacePump in the main loop)
// ---------------------------------------------------------------------------

// Brings the controller up. Called once from the device main().
void AppUSBInit()
{
    UsbHwInit();
    tusb_init();
    UsbConnect();
    __enable_irq(); // USB events must reach tud_int_handler
}

bool AppUSBActive()
{
    // Counts only while the host has actually opened the port.
    return tud_cdc_connected();
}

// Sends up to `len` stream bytes as one or more link frames.
void AppUSBSend(const uint8_t *data, uint16_t len)
{
    uint16_t off = 0;
    while (off < len)
    {
        uint8_t chunk = (uint8_t)((len - off > USB_FRAME_MAX_PAYLOAD) ? USB_FRAME_MAX_PAYLOAD : (len - off));

        uint8_t frame[4 + USB_FRAME_MAX_PAYLOAD];
        frame[0] = USB_FRAME_START;
        frame[2] = chunk;
        memcpy(&frame[3], data + off, chunk);
        frame[3 + chunk] = USB_FRAME_STOP;
        frame[1] = Crc8(&frame[2], (uint16_t)(1 + chunk)); // CRC8 over Length + Payload

        uint32_t timeout = 1000; // bounded wait for TX buffer space, pumping the stack
        while (tud_cdc_write_available() < (uint32_t)(4 + chunk) && timeout > 0)
        {
            tud_task();
            timeout--;
        }
        if (timeout == 0)
            break;

        tud_cdc_write(frame, (uint32_t)(4 + chunk));
        tud_cdc_write_flush();

        off += chunk;
    }
}

// No BLE link on this board (Docs/Devices.md lists USB only as the App interface). The core's
// AppInterfacePump calls these unconditionally, so they are no-ops.
bool AppBLEActive() { return false; }
void AppBLETick() {}

// Called from AppInterfacePump every main-loop iteration: pumps TinyUSB and drains the CDC
// receive buffer into the shared RX queue. This is the no-OS replacement for the core's
// AppLinkTask.
void AppUSBTick()
{
    tud_task();

    while (tud_cdc_available())
    {
        uint8_t buf[64];
        uint32_t n = tud_cdc_read(buf, sizeof(buf));
        if (n == 0)
            break;
        for (uint32_t i = 0; i < n; i++)
        {
            uint8_t payload[USB_FRAME_MAX_PAYLOAD];
            int r = s_app_framer.Feed(buf[i], payload);
            if (r > 0)
                AppRxStream(payload, (uint16_t)r);
            else if (r < 0)
                s_wire_parser.Reset(); // broken link frame: resync the stream too
        }
    }
}
