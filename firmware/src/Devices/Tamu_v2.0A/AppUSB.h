#pragma once

// USB link for the App Interface (Docs/Services/App Interface.md).
//
// The App Interface owns the USB Serial/JTAG port: the host (the app, or a test harness such
// as test/tamu_proto.py) writes link frames and the device answers over the same port.
//
// There is no console/REPL: the CLI was removed, and with it the two-mode state machine it
// shared the port with - which also removed the hazard where a half-fed link left the port
// unreachable until a replug.
//
// Link framing (both directions):
//   0xFA | CRC8 | Length | Payload (max 60 bytes of packet stream) | 0xBF
// The CRC8 covers Length + Payload. The payload is a serialized PacketFrame stream
// (crc8-first wire layout, see PacketToWire).

#include <cstdint>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "Core/Functions/AppInterface.h"

#define USB_FRAME_START 0xFA
#define USB_FRAME_STOP 0xBF
#define USB_FRAME_MAX_PAYLOAD 60

// TimeFromBoot() of the last validated app frame received over USB. Used to detect an app that
// CLOSED its USB port but left the cable plugged in: the SOF-based
// usb_serial_jtag_is_connected() stays true, so without this the TX pump would keep draining
// responses into the dead port.
//
// It is also the "is this link busy" test the console copy in Log.h gates on (Issues.md E67): a
// port nobody has spoken on is idle, which is what lets a rig session read runtime lines while a
// host merely holds the port.
static uint32_t UsbLastRx = 0;
#define USB_TX_SILENCE_MS 500

// True when no app frame has arrived over USB for a while (the port is open but the app is gone),
// or when none has arrived at all since boot - nothing received means nothing to be in the middle
// of. Guards the TX pump against a dead port, and the console copy in Log.h against a live one.
bool AppUsbSilent()
{
    if (UsbLastRx == 0)
        return true;
    return (uint32_t)(TimeFromBoot() - UsbLastRx) > USB_TX_SILENCE_MS;
}

// ---------------------------------------------------------------------------
// Link framing
// ---------------------------------------------------------------------------

struct UsbFramer
{
    bool FrameOpen = false;
    uint16_t Len = 0;
    uint8_t Buf[2 + USB_FRAME_MAX_PAYLOAD + 1]; // crc + len + payload + stop

    void Reset()
    {
        FrameOpen = false;
        Len = 0;
    }

    // Feeds one byte. Returns: 0 = incomplete, >0 = valid frame, payload length written
    // to `payload`, -1 = invalid sequence (resync automatically).
    int Feed(uint8_t Byte, uint8_t *Payload)
    {
        if (!FrameOpen)
        {
            if (Byte == USB_FRAME_START)
            {
                FrameOpen = true;
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

        FrameOpen = false;
        if (Buf[2 + PayloadLen] != USB_FRAME_STOP)
            return -1;

        // CRC8 over Length + Payload
        if (Crc8(&Buf[1], (uint16_t)(1 + PayloadLen)) != Buf[0])
            return -1;

        memcpy(Payload, &Buf[2], PayloadLen);
        return PayloadLen;
    }
};

static UsbFramer AppFramer;

// Requests a framer resync on host disconnect. AppUSBTick (ApplicationTask) only sets it;
// AppLinkTask, which feeds both framers, consumes it and performs the Reset() itself, so
// every framer is fed and reset on a single task.
static volatile bool UsbResync = false;

// Installs the USJ driver and routes the VFS stdio to it so Crc8/log output has a home.
void AppUSBInit()
{
    if (!usb_serial_jtag_is_driver_installed())
    {
        usb_serial_jtag_driver_config_t Cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        Cfg.tx_buffer_size = 2048;
        Cfg.rx_buffer_size = 1024;
        usb_serial_jtag_driver_install(&Cfg);
    }
    usb_serial_jtag_vfs_use_driver();
}

// ---------------------------------------------------------------------------
// Packet stream parser: turns the continuous crc8-first packet stream into frames
// ---------------------------------------------------------------------------

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

    // Feeds one stream byte. Returns true when a complete CRC-valid PacketFrame was
    // assembled into `frame` (ready for AppInterfaceEnqueue).
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

// ---------------------------------------------------------------------------
// RX: link frames -> queued packets
// ---------------------------------------------------------------------------

// Handles one validated app link-frame payload: parses the contained packet-stream
// bytes and queues complete packets for dispatch.
static void AppRxStream(const uint8_t *Data, uint16_t Len)
{
    UsbLastRx = TimeFromBoot();
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

// Link task: reads the USJ port and feeds the app framer. Nothing else owns the port, so a
// stray byte outside a frame is simply ignored by the framer.
static void AppLinkTask(void *)
{
    uint8_t Buf[128];

    for (;;)
    {
        if (UsbResync)
        {
            UsbResync = false;
            AppFramer.Reset();
            WireParser.Reset();
        }

        int ReadLen = usb_serial_jtag_read_bytes(Buf, sizeof(Buf), pdMS_TO_TICKS(20));
        if (ReadLen <= 0)
            continue;

        for (int i = 0; i < ReadLen; i++)
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

// Creates the link task (called from boot, after AppUSBInit).
void AppUSBStartTask()
{
    xTaskCreate(AppLinkTask, "usblink", 4096, NULL, 5, NULL);
}

// ---------------------------------------------------------------------------
// App Interface hooks (called from ApplicationTask context via the pump)
// ---------------------------------------------------------------------------

bool AppUSBActive()
{
    // Only counts while the USB host is actually present; writing into an unplugged port
    // would silently buffer (and stall the pump) otherwise.
    return usb_serial_jtag_is_connected();
}

// Sends up to `len` stream bytes as one or more link frames; returns the number of stream bytes
// whose frames the port took whole.
//
// The driver's TX ring is 2 KB and `usb_serial_jtag_write_bytes` returns how much it accepted -
// it can be LESS than asked when the host is not draining. Ignoring that return (which this did)
// truncates a link frame: the host's framer then resyncs inside the packet stream and the packet
// that straddled the truncation is lost. A frame is therefore retried until the driver has taken
// all of it, and a frame that cannot be completed ends the call - the pump keeps the remaining
// bytes in the TX ring (TxPeek/TxCommit in AppInterface.h) and tries again on its next pass.
uint16_t AppUSBSend(const uint8_t *Data, uint16_t Len)
{
    uint16_t Off = 0;
    while (Off < Len)
    {
        uint8_t Chunk = (uint8_t)((Len - Off > USB_FRAME_MAX_PAYLOAD) ? USB_FRAME_MAX_PAYLOAD : (Len - Off));

        // Wire layout (Docs/Services/App Interface.md): START | CRC8 | Length |
        // Payload | STOP. The CRC covers Length + Payload.
        uint8_t Frame[4 + USB_FRAME_MAX_PAYLOAD];
        Frame[0] = USB_FRAME_START;
        Frame[2] = Chunk;
        memcpy(&Frame[3], Data + Off, Chunk);
        Frame[3 + Chunk] = USB_FRAME_STOP;
        Frame[1] = Crc8(&Frame[2], (uint16_t)(1 + Chunk)); // CRC8 over Length + Payload

        size_t Total = (size_t)(4 + Chunk);
        size_t Sent = 0;
        // Bounded: four 50 ms writes is 200 ms for 64 bytes, far beyond a host that is reading -
        // a longer wait means the port is dead and holding the app task would stall the device.
        for (int Attempt = 0; Attempt < 4 && Sent < Total; Attempt++)
        {
            int Wrote = usb_serial_jtag_write_bytes(Frame + Sent, Total - Sent, pdMS_TO_TICKS(50));
            if (Wrote <= 0)
                break;
            Sent += (size_t)Wrote;
        }
        if (Sent < Total)
            break; // the link is not taking bytes: the caller keeps the rest queued
        Off += Chunk;
    }
    return Off;
}

// Called from the pump (ApplicationTask). Watches the physical USB link: when the host
// disconnects, flush anything pending and reset the framing state so a re-attaching host
// starts from a clean stream.
void AppUSBTick()
{
    static bool WasConnected = false;
    bool Connected = usb_serial_jtag_is_connected();
    if (WasConnected && !Connected)
    {
        AppInterface.TxFlushAll();
        // AppLinkTask owns both framers; a Reset() here would race the task's Feed().
        UsbResync = true;
    }
    WasConnected = Connected;
}
