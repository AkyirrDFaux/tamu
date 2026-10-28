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
// AppInterface.Pump() (the main loop) calls AppUSBTick() every iteration, which pumps TinyUSB and
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
static const tusb_desc_device_t DescDevice = {
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

uint8_t const *tud_descriptor_device_cb(void) { return (uint8_t const *)&DescDevice; }

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_TOTAL };

#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t DescConfiguration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t Index)
{
    (void)Index;
    return DescConfiguration;
}

static char const *const StringDescArr[] = {
    (const char[]){0x09, 0x04}, // 0: supported language = English (0x0409)
    "Tamu",                     // 1: Manufacturer
    "Valu v2.0",                // 2: Product
    "0001",                     // 3: Serial (static; a per-unit serial is a later change)
    "Valu v2.0 CDC",            // 4: CDC interface
};

static uint16_t DescStr[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t Index, uint16_t Langid)
{
    (void)Langid;
    size_t ChrCount;

    if (Index == 0)
    {
        memcpy(&DescStr[1], StringDescArr[0], 2);
        ChrCount = 1;
    }
    else
    {
        if (Index >= sizeof(StringDescArr) / sizeof(StringDescArr[0]))
            return NULL;

        const char *Str = StringDescArr[Index];
        ChrCount = strlen(Str);
        if (ChrCount > 32)
            ChrCount = 32;

        for (size_t i = 0; i < ChrCount; i++)
            DescStr[1 + i] = (uint16_t)Str[i];
    }

    DescStr[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * ChrCount + 2));
    return DescStr;
}

} // extern "C"

// ---------------------------------------------------------------------------
// Link framing (both directions): 0xFA | CRC8 | Length | Payload | 0xBF
// ---------------------------------------------------------------------------

struct UsbFramer
{
    bool InFrame = false;
    uint16_t Len = 0;
    uint8_t Buf[2 + USB_FRAME_MAX_PAYLOAD + 1]; // crc + len + payload + stop

    void Reset()
    {
        InFrame = false;
        Len = 0;
    }

    // Feeds one byte. Returns: 0 = incomplete, >0 = valid frame (payload length), -1 = invalid.
    int Feed(uint8_t Byte, uint8_t *Payload)
    {
        if (!InFrame)
        {
            if (Byte == USB_FRAME_START)
            {
                InFrame = true;
                Len = 0;
            }
            return 0;
        }

        Buf[Len++] = Byte;

        if (Len < 2)
            return 0; // CRC pending

        uint8_t PayloadLen = Buf[1];
        if (PayloadLen > USB_FRAME_MAX_PAYLOAD)
        {
            Reset();
            return -1; // impossible length: resync
        }

        if (Len < (uint8_t)(2 + PayloadLen + 1))
            return 0; // payload/stop pending

        InFrame = false;
        if (Buf[2 + PayloadLen] != USB_FRAME_STOP)
            return -1;

        if (Crc8(&Buf[1], (uint16_t)(1 + PayloadLen)) != Buf[0]) // CRC8 over Length + Payload
            return -1;

        memcpy(Payload, &Buf[2], PayloadLen);
        return PayloadLen;
    }
};

static UsbFramer AppFramer;

// Turns the continuous crc8-first packet stream into frames (same parser as the core).
struct WireStreamParser
{
    PacketFrame Frame __attribute__((aligned(4)));
    uint16_t Got = 0;
    bool Full = false;

    void Reset()
    {
        Got = 0;
        Full = false;
    }

    // Returns true when a complete CRC-valid PacketFrame was assembled into `frame`.
    bool Feed(uint8_t Byte)
    {
        if (Full)
            return false;

        if (Got >= sizeof(PacketFrame))
        {
            Reset();
            return false;
        }
        ((uint8_t *)&Frame)[Got++] = Byte;

        if (Got < 4)
            return false; // payload_len not known yet

        uint16_t Need = PacketWireSize(&Frame);
        if (Got < Need)
            return false;

        Full = true;
        bool Ok = Crc8(&Frame.Flags, (uint16_t)(11 + PayloadBytes(Frame))) == Frame.Crc8;
        return Ok; // invalid CRC: caller should Reset() and resync
    }
};

static WireStreamParser WireParser;

// Handles one validated link-frame payload: parses the contained packet stream and queues
// complete packets for dispatch by AppInterface.Pump().
static void AppRxStream(const uint8_t *Data, uint16_t Len)
{
    for (uint16_t i = 0; i < Len; i++)
    {
        if (WireParser.Feed(Data[i]))
        {
            AppInterface.Enqueue(WireParser.Frame, AppLinkSource::USB);
            WireParser.Reset();
        }
        else if (WireParser.Full || WireParser.Got > sizeof(PacketFrame))
        {
            WireParser.Reset(); // CRC failure / overflow: drop and resync
        }
    }
}

// ---------------------------------------------------------------------------
// App Interface hooks (called from AppInterface.Pump() in the main loop)
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
void AppUSBSend(const uint8_t *Data, uint16_t Len)
{
    uint16_t Off = 0;
    while (Off < Len)
    {
        uint8_t Chunk = (uint8_t)((Len - Off > USB_FRAME_MAX_PAYLOAD) ? USB_FRAME_MAX_PAYLOAD : (Len - Off));

        uint8_t Frame[4 + USB_FRAME_MAX_PAYLOAD];
        Frame[0] = USB_FRAME_START;
        Frame[2] = Chunk;
        memcpy(&Frame[3], Data + Off, Chunk);
        Frame[3 + Chunk] = USB_FRAME_STOP;
        Frame[1] = Crc8(&Frame[2], (uint16_t)(1 + Chunk)); // CRC8 over Length + Payload

        uint32_t Timeout = 1000; // bounded wait for TX buffer space, pumping the stack
        while (tud_cdc_write_available() < (uint32_t)(4 + Chunk) && Timeout > 0)
        {
            tud_task();
            Timeout--;
        }
        if (Timeout == 0)
            break;

        tud_cdc_write(Frame, (uint32_t)(4 + Chunk));
        tud_cdc_write_flush();

        Off += Chunk;
    }
}

// No BLE link on this board (Docs/Devices.md lists USB only as the App interface). The core's
// AppInterface.Pump() calls these unconditionally, so they are no-ops.
bool AppBLEActive() { return false; }
void AppBLETick() {}

// Called from AppInterface.Pump() every main-loop iteration: pumps TinyUSB and drains the CDC
// receive buffer into the shared RX queue. This is the no-OS replacement for the core's
// AppLinkTask.
void AppUSBTick()
{
    tud_task();

    while (tud_cdc_available())
    {
        uint8_t Buf[64];
        uint32_t ReadLen = tud_cdc_read(Buf, sizeof(Buf));
        if (ReadLen == 0)
            break;
        for (uint32_t i = 0; i < ReadLen; i++)
        {
            uint8_t Payload[USB_FRAME_MAX_PAYLOAD];
            int FeedResult = AppFramer.Feed(Buf[i], Payload);
            if (FeedResult > 0)
                AppRxStream(Payload, (uint16_t)FeedResult);
            else if (FeedResult < 0)
                WireParser.Reset(); // broken link frame: resync the stream too
        }
    }
}
