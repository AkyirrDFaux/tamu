#pragma once

// USB link for the App Interface (Docs/Services/App Interface.md) + CLI console,
// sharing the single USB Serial/JTAG peripheral EXCLUSIVELY: the app always has
// priority. The port runs a two-mode state machine:
//
//   USB_MODE_CLI --(valid app frame received)--> USB_MODE_APP --(host unplugged)--> USB_MODE_CLI
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
#include "esp_console.h"
#include "Core/Functions/AppInterface.h"

#define USB_FRAME_START 0xFA
#define USB_FRAME_STOP 0xBF
#define USB_FRAME_MAX_PAYLOAD 60

enum UsbMode : uint8_t { USB_MODE_CLI = 0, USB_MODE_APP = 1 };

static uint8_t s_usb_mode = USB_MODE_CLI;
static volatile bool s_usb_revert_req = false; // set by AppUSBTick when the host disappears
// TimeFromBoot() of the last validated app frame received over USB. Used to
// detect an app that CLOSED its USB port but left the cable plugged in: the
// SOF-based usb_serial_jtag_is_connected() stays true, so without this the
// TX pump would keep draining responses into the dead port after a USB->BLE
// link switch.
static uint32_t s_usb_last_rx = 0;
#define USB_TX_SILENCE_MS 500

// True when no app frame has arrived over USB for a while. Only meaningful as a
// "the app moved to BLE" signal when a BLE session is also up.
bool AppUsbSilent()
{
    return (uint32_t)(TimeFromBoot() - s_usb_last_rx) > USB_TX_SILENCE_MS;
}

// ---------------------------------------------------------------------------
// Link frame parser (shared by both modes)
// ---------------------------------------------------------------------------

struct UsbFramer
{
    uint8_t buf[2 + USB_FRAME_MAX_PAYLOAD + 1]; // crc + len + payload + stop
    uint8_t len = 0;
    bool in_frame = false;

    bool InFrame() const { return in_frame; }

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

static UsbFramer s_app_framer; // APP mode RX

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

// Installs the USJ driver (the CLI no longer uses the stock REPL) and routes the
// VFS stdio to it so printf output reaches the USB host.
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
// Console task (CLI mode line editor + APP mode forwarding)
// ---------------------------------------------------------------------------

#define CONSOLE_LINE_MAX 256

static char s_line[CONSOLE_LINE_MAX];
static uint16_t s_line_len = 0;

static UsbFramer s_cli_framer;      // shadow sniffer while in CLI mode
static bool s_cli_candidate = false; // bytes of a possible frame are being captured
static uint16_t s_cli_frame_at = 0;  // line-buffer position where the candidate started

static void PrintPrompt()
{
    printf("tamu> ");
}

// Enters APP mode silently (any output would corrupt the app's byte stream).
static void EnterAppMode()
{
    s_usb_mode = USB_MODE_APP;
    s_line_len = 0;
    s_app_framer.Reset();
    s_wire_parser.Reset();
    AppTxFlushAll(); // pending responses belong to previous sessions
}

static void EnterCliMode()
{
    s_usb_mode = USB_MODE_CLI;
    s_cli_framer.Reset();
    s_cli_candidate = false;
    s_line_len = 0;
    AppTxFlushAll();
    PrintPrompt();
}

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

// CLI-mode line editor: append/echo/backspace/run. Returns when the byte is consumed.
static void CliLineByte(uint8_t b)
{
    if (b == '\r' || b == '\n')
    {
        if (s_line_len > 0)
        {
            s_line[s_line_len] = '\0';
            printf("\n");
            int ret;
            esp_console_run(s_line, &ret);
            if (ret != 0)
                printf("Command returned %d\n", ret);
            s_line_len = 0;
        }
        PrintPrompt();
        return;
    }

    if (b == '\b' || b == 0x7F)
    {
        if (s_line_len > 0)
        {
            s_line_len--;
            printf("\b \b");
        }
        return;
    }

    if (b >= 32 && b < 127 && s_line_len < CONSOLE_LINE_MAX - 1)
    {
        s_line[s_line_len++] = (char)b;
        putchar(b);
    }
}

// Console task body: reads the USJ port and routes bytes according to the mode.
void ConsoleTask(void *)
{
    uint8_t buf[128];

    PrintPrompt();

    for (;;)
    {
        // Deferred mode revert (requested by AppUSBTick after physical unplug).
        if (s_usb_mode == USB_MODE_APP && s_usb_revert_req)
        {
            s_usb_revert_req = false;
            printf("\n[App detached]\n");
            EnterCliMode();
        }

        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (n <= 0)
            continue;


        // Per-byte mode dispatch: a mode switch (CLI -> APP) can happen mid-batch,
        // and the remaining bytes of the batch already belong to the app stream.
        for (int i = 0; i < n; i++)
        {
            uint8_t b = buf[i];

            if (s_usb_mode == USB_MODE_APP)
            {
                // A CR/LF OUTSIDE any app frame cannot be app traffic (packet
                // payloads only exist inside FA..BF frames) - it is a human
                // typing at a terminal. A software-only host close does not
                // drop the USJ connection state, so without this the console
                // would stay unreachable until the cable is replugged.
                if ((b == '\r' || b == '\n') && !s_app_framer.InFrame())
                {
                    EnterCliMode();
                    continue; // the triggering byte starts a fresh line
                }

                uint8_t payload[USB_FRAME_MAX_PAYLOAD];
                int r = s_app_framer.Feed(b, payload);
                if (r > 0)
                    AppRxStream(payload, (uint16_t)r);
                else if (r < 0)
                    s_wire_parser.Reset(); // broken link frame: resync the stream too
                continue;
            }

            // CLI mode: line editor + shadow sniffer for incoming app frames.
            // A typed 0xFA never happens, but an attaching app can start mid-line.
            // While a candidate frame is being captured its bytes are withheld from
            // the line editor (a candidate could contain \n or \r, which would
            // execute a garbage command).
            if (!s_cli_candidate && b == USB_FRAME_START)
            {
                s_cli_candidate = true;
                s_cli_frame_at = s_line_len;
            }

            if (s_cli_candidate)
            {
                uint8_t payload[USB_FRAME_MAX_PAYLOAD];
                int r = s_cli_framer.Feed(b, payload);

                if (r > 0)
                {
                    // Valid app frame: drop the captured bytes from the line and
                    // hand the port to the app (app priority). The loop continues
                    // in APP mode with any remaining bytes of this batch.
                    s_line_len = s_cli_frame_at;
                    EnterAppMode();
                    AppRxStream(payload, (uint16_t)r);
                    continue;
                }
                if (r < 0)
                    s_cli_candidate = false; // not a frame; byte is consumed
                continue;
            }

            CliLineByte(b);
        }

    }
}

// ---------------------------------------------------------------------------
// App Interface hooks (called from ApplicationTask context via the pump)
// ---------------------------------------------------------------------------

bool AppUSBActive()
{
    // APP mode only counts while the USB host is actually present; writing into an
    // unplugged port would silently buffer (and stall the pump) otherwise.
    return s_usb_mode == USB_MODE_APP && usb_serial_jtag_is_connected();
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
// disconnects during an app session, request reverting to CLI mode (the console task
// prints the banner once the port is back in CLI hands).
void AppUSBTick()
{
    if (s_usb_mode == USB_MODE_APP && !usb_serial_jtag_is_connected())
        s_usb_revert_req = true;
}
