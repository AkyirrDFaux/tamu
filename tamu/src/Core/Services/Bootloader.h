#pragma once

// Bootloader service (Docs/Services/Bootloader.md, SRV 0x13).
//
// The core acts as a USB<->RS-Bus bridge: app commands arrive as standard packets
// and are translated to simple control-character messages on the half-duplex RS-Bus.
//
// CID 0 — Bootloader check:  returns bool (true = in bootloader mode)
// CID 1 — Bootloader switch: bool (true = enter, false = leave)
// CID 2 — Device info:       vendor info from node enumeration (16 bytes)
// CID 3 — Bootloader Reader: u32 frag_idx -> u32 frag_idx + 256B (read from node flash)
// CID 4 — Bootloader Writer: u32 frag_idx + 256B -> u32 frag_idx (write to node flash)
//
// Enumeration is non-blocking: CID 1 sets the mode flag and returns immediately.
// The main loop calls BootloaderTick() each iteration, which passively listens for
// "E" + vendor info from the node and responds with "C" to confirm.

#ifdef USE_APP_INTERFACE

#include <cstdint>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "Core/Functions/Packet.h"

// Defined in the device RSBus.h; the bootloader service needs direct UART access
// for raw bridging (no CSMA/CD, no packet framing).
#define BL_RS485_EN_PIN GPIO_NUM_9

// Control characters for the half-duplex UART protocol
#define BL_CTRL_ENUM    'E'
#define BL_CTRL_CONFIRM 'C'
#define BL_CTRL_WRITE   'W'
#define BL_CTRL_DONE    'D'
#define BL_CTRL_VERIFY  'V'
#define BL_CTRL_READ    'R'

#define BL_FRAG_SIZE     256u
#define BL_FRAG_IDX_SIZE 4u
#define BL_ENUM_SIZE     16u  // device_type(2) + serial_number(14)

// Timeout for waiting on node write/read response (ms)
#define BL_RESPONSE_TIMEOUT_MS 2000

// === Global state ===

bool BootloaderMode = false;
bool NodeEnumerated = false;
uint8_t VendorInfo[BL_ENUM_SIZE] = {0};

// === Raw UART helpers (bypass CSMA/CD) ===

static void bl_uart_tx_enable(void)  { gpio_set_level(BL_RS485_EN_PIN, 1); }
static void bl_uart_tx_disable(void) { gpio_set_level(BL_RS485_EN_PIN, 0); }

// Send raw bytes on the RS-Bus (no CSMA/CD, no echo verify — single device on bus).
static void bl_send(const uint8_t *data, uint16_t len)
{
    bl_uart_tx_enable();
    uart_write_bytes(UART_NUM_1, (const char *)data, len);
    uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(100));
    bl_uart_tx_disable();
}

// Receive exactly `len` bytes with a total timeout. Returns true on success.
static bool bl_recv(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    uint16_t got = 0;
    uint32_t start = xTaskGetTickCount() * portTICK_PERIOD_MS;
    while (got < len) {
        uint32_t elapsed = xTaskGetTickCount() * portTICK_PERIOD_MS - start;
        if (elapsed >= timeout_ms) return false;
        int n = uart_read_bytes(UART_NUM_1, buf + got, len - got,
                                pdMS_TO_TICKS(timeout_ms - elapsed));
        if (n <= 0) return false;
        got += (uint16_t)n;
    }
    return true;
}

// Wait for any pending RX data and drain it (ignore leftover bus noise).
static void bl_drain_rx(void)
{
    uint8_t drain[32];
    for (;;) {
        int n = uart_read_bytes(UART_NUM_1, drain, sizeof(drain), pdMS_TO_TICKS(5));
        if (n <= 0) break;
    }
}

// === Enumeration listener (called from main loop) ===

// Non-blocking state for parsing "E" + 16-byte payload.
static int  bl_enum_state = 0;    // 0=idle, 1=reading payload
static uint8_t bl_enum_buf[BL_ENUM_SIZE];
static uint8_t bl_enum_idx = 0;

void BootloaderTick(void)
{
    if (!BootloaderMode || NodeEnumerated) return;

    // Non-blocking: read one byte at a time
    uint8_t b;
    int n = uart_read_bytes(UART_NUM_1, &b, 1, 0);
    if (n <= 0) return;

    switch (bl_enum_state)
    {
    case 0: // Waiting for 'E'
        if (b == BL_CTRL_ENUM)
        {
            bl_enum_idx = 0;
            bl_enum_state = 1;
        }
        break;

    case 1: // Reading 16-byte vendor info
        bl_enum_buf[bl_enum_idx++] = b;
        if (bl_enum_idx >= BL_ENUM_SIZE)
        {
            // Full enumeration received — store and confirm
            memcpy(VendorInfo, bl_enum_buf, BL_ENUM_SIZE);
            NodeEnumerated = true;
            bl_enum_state = 0;

            // Send "C" confirm back to the node
            uint8_t confirm = BL_CTRL_CONFIRM;
            bl_send(&confirm, 1);
        }
        break;
    }
}

// === Bootloader service handlers ===

// CID 0: Bootloader check — returns true if in bootloader mode.
static void HandleBootloaderCheck(const PacketFrame &frame)
{
    uint8_t reply = BootloaderMode ? 1 : 0;
    PacketFrame resp;
    PacketConstruct(&resp, frame.id_src, frame.srv_src, frame.srv_tgt,
                     FLAG_TYPE | FLAG_START | FLAG_STOP, &reply, 1);
    AppInterfaceSend(resp);
}

// CID 1: Bootloader switch — enter or leave bootloader mode.
// Non-blocking: sets the flag and returns immediately. Enumeration happens
// via BootloaderTick() in the main loop.
static void HandleBootloaderSwitch(const PacketFrame &frame)
{
    if (PayloadBytes(frame) < 1) return;
    bool enter = frame.payload[0] != 0;

    if (enter && !BootloaderMode)
    {
        // Enter bootloader mode: stop normal bus processing.
        bl_drain_rx();
        BootloaderMode = true;
        NodeEnumerated = false;
        bl_enum_state = 0;
        memset(VendorInfo, 0, sizeof(VendorInfo));
        // Enumeration will be picked up by BootloaderTick() in the main loop.
    }
    else if (!enter && BootloaderMode)
    {
        // Leave bootloader mode: restore normal bus processing.
        bl_drain_rx();
        BootloaderMode = false;
        NodeEnumerated = false;
        bl_enum_state = 0;
    }

    // Send acknowledgment
    if (frame.flags & FLAG_REQACK)
    {
        PacketFrame resp;
        PacketConstruct(&resp, frame.id_src, frame.srv_src, frame.srv_tgt,
                         FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
        AppInterfaceSend(resp);
    }
}

// CID 2: Device info — return vendor info from node enumeration.
static void HandleBootloaderDeviceInfo(const PacketFrame &frame)
{
    if (!NodeEnumerated)
    {
        PacketFrame resp;
        PacketConstruct(&resp, frame.id_src, frame.srv_src, frame.srv_tgt,
                         FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
        AppInterfaceSend(resp);
        return;
    }

    PacketFrame resp;
    PacketConstruct(&resp, frame.id_src, frame.srv_src, frame.srv_tgt,
                     FLAG_TYPE | FLAG_START | FLAG_STOP,
                     VendorInfo, BL_ENUM_SIZE);
    AppInterfaceSend(resp);
}

// CID 3: Bootloader Reader — send V to node, receive R with data.
static void HandleBootloaderReader(const PacketFrame &frame)
{
    if (PayloadBytes(frame) < BL_FRAG_IDX_SIZE || !BootloaderMode || !NodeEnumerated)
    {
        PacketFrame resp;
        PacketConstruct(&resp, frame.id_src, frame.srv_src, frame.srv_tgt,
                         FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
        AppInterfaceSend(resp);
        return;
    }

    uint8_t frag_idx_bytes[BL_FRAG_IDX_SIZE];
    memcpy(frag_idx_bytes, frame.payload, BL_FRAG_IDX_SIZE);

    // Send: V + u32 LE frag_idx (5 bytes)
    uint8_t cmd[1 + BL_FRAG_IDX_SIZE];
    cmd[0] = BL_CTRL_VERIFY;
    memcpy(cmd + 1, frag_idx_bytes, BL_FRAG_IDX_SIZE);
    bl_send(cmd, sizeof(cmd));

    // Receive: R + u32 LE frag_idx + 256B data (261 bytes)
    uint8_t response[1 + BL_FRAG_IDX_SIZE + BL_FRAG_SIZE];
    if (!bl_recv(response, sizeof(response), BL_RESPONSE_TIMEOUT_MS))
    {
        PacketFrame resp;
        PacketConstruct(&resp, frame.id_src, frame.srv_src, frame.srv_tgt,
                         FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
        AppInterfaceSend(resp);
        return;
    }

    if (response[0] != BL_CTRL_READ)
    {
        PacketFrame resp;
        PacketConstruct(&resp, frame.id_src, frame.srv_src, frame.srv_tgt,
                         FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
        AppInterfaceSend(resp);
        return;
    }

    // Return: u32 frag_idx + 256B data (260 bytes)
    PacketFrame resp;
    PacketConstruct(&resp, frame.id_src, frame.srv_src, frame.srv_tgt,
                     FLAG_TYPE | FLAG_START | FLAG_STOP,
                     response + 1, BL_FRAG_IDX_SIZE + BL_FRAG_SIZE);
    AppInterfaceSend(resp);
}

// CID 4: Bootloader Writer — send W to node, receive D ack.
static void HandleBootloaderWriter(const PacketFrame &frame)
{
    uint16_t plen = PayloadBytes(frame);
    if (plen < BL_FRAG_IDX_SIZE + 1 || !BootloaderMode || !NodeEnumerated)
    {
        PacketFrame resp;
        PacketConstruct(&resp, frame.id_src, frame.srv_src, frame.srv_tgt,
                         FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
        AppInterfaceSend(resp);
        return;
    }

    // Send: W + u32 LE frag_idx + 256B data (261 bytes)
    uint8_t cmd[1 + BL_FRAG_IDX_SIZE + BL_FRAG_SIZE];
    cmd[0] = BL_CTRL_WRITE;
    memcpy(cmd + 1, frame.payload, BL_FRAG_IDX_SIZE + BL_FRAG_SIZE);
    bl_send(cmd, sizeof(cmd));

    // Receive: D + u32 LE frag_idx (5 bytes)
    uint8_t response[1 + BL_FRAG_IDX_SIZE];
    if (!bl_recv(response, sizeof(response), BL_RESPONSE_TIMEOUT_MS))
    {
        PacketFrame resp;
        PacketConstruct(&resp, frame.id_src, frame.srv_src, frame.srv_tgt,
                         FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
        AppInterfaceSend(resp);
        return;
    }

    if (response[0] != BL_CTRL_DONE)
    {
        PacketFrame resp;
        PacketConstruct(&resp, frame.id_src, frame.srv_src, frame.srv_tgt,
                         FLAG_TYPE | FLAG_START | FLAG_STOP, nullptr, 0);
        AppInterfaceSend(resp);
        return;
    }

    // Return: u32 frag_idx (4 bytes)
    PacketFrame resp;
    PacketConstruct(&resp, frame.id_src, frame.srv_src, frame.srv_tgt,
                     FLAG_TYPE | FLAG_START | FLAG_STOP,
                     response + 1, BL_FRAG_IDX_SIZE);
    AppInterfaceSend(resp);
}

// === Main dispatcher ===

static void HandleBootloader(const PacketFrame &frame)
{
    // Ignore responses
    if (frame.flags & FLAG_TYPE) return;

    uint8_t cid = GetServiceCID(frame.srv_tgt);

    switch (cid)
    {
        case 0: HandleBootloaderCheck(frame); break;
        case 1: HandleBootloaderSwitch(frame); break;
        case 2: HandleBootloaderDeviceInfo(frame); break;
        case 3: HandleBootloaderReader(frame); break;
        case 4: HandleBootloaderWriter(frame); break;
        default: break;
    }
}

#endif // USE_APP_INTERFACE
