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
#include <cstdio>
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
static uint32_t s_usb_last_rx = 0;
#define USB_TX_SILENCE_MS 500

// True when no app frame has arrived over USB for a while (the port is open but the app is
// gone). Only meaningful as a TX guard.
bool AppUsbSilent()
{
    return (uint32_t)(TimeFromBoot() - s_usb_last_rx) > USB_TX_SILENCE_MS;
}

// ---------------------------------------------------------------------------
// Link framing
// ---------------------------------------------------------------------------

struct UsbFramer
{
    bool in_frame = false;
    uint16_t len = 0;
    uint8_t buf[2 + USB_FRAME_MAX_PAYLOAD + 1]; // crc + len + payload + stop

    bool InFrame() const
    {
        return in_frame;
    }

    void Reset()
    {
        in_frame = false;
        len = 0;
    }

    // Feeds one byte. Returns: 0 = incomplete, >0 = valid frame, payload length written
    // to `payload`, -1 = invalid sequence (resync automatically).
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

        // CRC8 over Length + Payload
        if (Crc8(&buf[1], (uint16_t)(1 + n)) != buf[0])
            return -1;

        memcpy(payload, &buf[2], n);
        return n;
    }
};

static UsbFramer s_app_framer;

// Installs the USJ driver and routes the VFS stdio to it so Crc8/log output has a home.
void AppUSBInit()
{
    if (!usb_serial_jtag_is_driver_installed())
    {
        usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        cfg.tx_buffer_size = 2048;
        cfg.rx_buffer_size = 1024;
        usb_serial_jtag_driver_install(&cfg);
    }
    usb_serial_jtag_vfs_use_driver();
}

// ---------------------------------------------------------------------------
// Packet stream parser: turns the continuous crc8-first packet stream into frames
// ---------------------------------------------------------------------------

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

    // Feeds one stream byte. Returns true when a complete CRC-valid PacketFrame was
    // assembled into `frame` (ready for AppInterfaceEnqueue).
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

// ---------------------------------------------------------------------------
// RX: link frames -> queued packets
// ---------------------------------------------------------------------------

// Handles one validated app link-frame payload: parses the contained packet-stream
// bytes and queues complete packets for dispatch.
static void AppRxStream(const uint8_t *data, uint16_t len)
{
    s_usb_last_rx = TimeFromBoot();
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

// Link task: reads the USJ port and feeds the app framer. Nothing else owns the port, so a
// stray byte outside a frame is simply ignored by the framer.
static void AppLinkTask(void *)
{
    uint8_t buf[128];

    for (;;)
    {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (n <= 0)
            continue;

        for (int i = 0; i < n; i++)
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

// Sends up to `len` stream bytes as one or more link frames. Blocks briefly until the
// driver accepted the bytes (bounded by the write timeout).
void AppUSBSend(const uint8_t *data, uint16_t len)
{
    uint16_t off = 0;
    while (off < len)
    {
        uint8_t chunk = (uint8_t)((len - off > USB_FRAME_MAX_PAYLOAD) ? USB_FRAME_MAX_PAYLOAD : (len - off));

        // Wire layout (Docs/Services/App Interface.md): START | CRC8 | Length |
        // Payload | STOP. The CRC covers Length + Payload.
        uint8_t frame[4 + USB_FRAME_MAX_PAYLOAD];
        frame[0] = USB_FRAME_START;
        frame[2] = chunk;
        memcpy(&frame[3], data + off, chunk);
        frame[3 + chunk] = USB_FRAME_STOP;
        frame[1] = Crc8(&frame[2], (uint16_t)(1 + chunk)); // CRC8 over Length + Payload

        usb_serial_jtag_write_bytes(frame, (size_t)(4 + chunk), pdMS_TO_TICKS(50));
        off += chunk;
    }
}

// Called from the pump (ApplicationTask). Watches the physical USB link: when the host
// disconnects, flush anything pending and reset the framing state so a re-attaching host
// starts from a clean stream.
void AppUSBTick()
{
    static bool was_connected = false;
    bool connected = usb_serial_jtag_is_connected();
    if (was_connected && !connected)
    {
        AppTxFlushAll();
        s_app_framer.Reset();
        s_wire_parser.Reset();
    }
    was_connected = connected;
}
