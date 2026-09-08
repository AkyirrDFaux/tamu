#pragma once

// App Interface core (Docs/Services/App Interface.md): routing between the attached
// app (USB / BLE links) and the packet dispatcher.
//
// Identity model: the app has NO network address. Its identity is the App service
// type (ServiceType::App) carried in SRV SRC, with the CID byte used as an app-managed
// transaction ID. Frames arriving from a link get id_src rewritten to our own short
// address (the core acts as a proxy), so every response - local or relayed from the
// bus - comes back addressed to us with SRV TGT type == App and is pushed to the app's
// TX stream.

#ifdef USE_APP_INTERFACE

#include <cstdint>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "Core/Functions/Packet.h"

// Defined in the device AppUSB.h (included after this header); used by the TX
// pump to stop draining responses into a USB port the app has stopped reading.
bool AppUsbSilent();

// Doc requirement: global status bool reporting whether an app is actually connected
// (either link active).
bool AppConnected = false;

// Outgoing stream ring: serialized frames waiting to be flushed to the app links.
// A synchronous service reply (e.g. Storage Read File, which streams a whole
// file in one handler) can legitimately burst well past a small ring before
// AppInterfacePump drains it. Sized to hold a full 4 KiB file read (~4.5 KB of
// wire frames); larger reads would still truncate, so the app reads files in
// 4 KiB slices.
#define APP_TX_RING_SIZE 8192

// Inbound frame queue depth: frames parsed by the link readers, dispatched by the pump
// in ApplicationTask context so ALL routing happens on one task (no concurrent dispatch).
// Depth of the inbound app packet queue. Must absorb a burst of concurrent
// app transactions (one per outstanding CID) between pump runs; 6 dropped
// frames when an app fired 8 parallel requests (observed on hardware).
#define APP_RX_QUEUE_DEPTH 12

uint8_t *AppTxRing = nullptr;
volatile uint16_t AppTxHead = 0; // write position
volatile uint16_t AppTxTail = 0; // read position
volatile uint32_t AppTxDropped = 0;
portMUX_TYPE AppTxMux = portMUX_INITIALIZER_UNLOCKED;

PacketFrame AppRxQueue[APP_RX_QUEUE_DEPTH];
volatile uint8_t AppRxCount = 0;
portMUX_TYPE AppRxMux = portMUX_INITIALIZER_UNLOCKED;

uint32_t AppStrayFrames = 0; // App-targeted frames received while no app is connected

// Link hooks (implemented per device: Devices/Tamu_v2.0A/AppUSB.h + AppBLE.h).
bool AppUSBActive();
bool AppBLEActive();
void AppUSBSend(const uint8_t *data, uint16_t len);
void AppUSBTick();
void AppBLETick();
// Defined in Dispatcher.h (single translation unit: forward declaration suffices here).
void DispatchPacket(const PacketFrame &frame);

inline void AppInterfaceInit()
{
    if (!AppTxRing)
        AppTxRing = (uint8_t *)malloc(APP_TX_RING_SIZE);
}

// Serializes `frame` into the TX ring. Drop-newest policy when full (the oldest data
// belongs to streams already in flight; dropping the head would corrupt them anyway).
inline bool AppInterfaceSend(const PacketFrame &frame)
{
    if (!AppConnected || !AppTxRing)
        return false;

    uint8_t wire[12 + MAX_PAYLOAD_SIZE];
    uint16_t n = PacketWireSize(&frame);
    PacketToWire(&frame, wire);

    bool ok = false;
    portENTER_CRITICAL(&AppTxMux);
    uint16_t used = (uint16_t)(AppTxHead - AppTxTail);
    if (n <= APP_TX_RING_SIZE - 1 - used)
    {
        for (uint16_t i = 0; i < n; i++)
        {
            AppTxRing[AppTxHead % APP_TX_RING_SIZE] = wire[i];
            AppTxHead = (uint16_t)(AppTxHead + 1);
        }
        ok = true;
    }
    portEXIT_CRITICAL(&AppTxMux);
    if (!ok)
        AppTxDropped = AppTxDropped + 1;
    return ok;
}

// Peek/commit variant: copies up to `max` pending bytes WITHOUT consuming them; the
// caller confirms consumption with AppTxCommit(n) once they are actually on the wire
// (used by the BLE link so a failed notify does not lose data).
inline uint16_t AppTxPeek(uint8_t *dst, uint16_t max)
{
    uint16_t n = 0;
    portENTER_CRITICAL(&AppTxMux);
    uint16_t used = (uint16_t)(AppTxHead - AppTxTail);
    if (used > max)
        used = max;
    for (n = 0; n < used; n++)
        dst[n] = AppTxRing[(AppTxTail + n) % APP_TX_RING_SIZE];
    portEXIT_CRITICAL(&AppTxMux);
    return n;
}

inline void AppTxCommit(uint16_t n)
{
    if (n == 0)
        return;
    portENTER_CRITICAL(&AppTxMux);
    AppTxTail = (uint16_t)(AppTxTail + n);
    portEXIT_CRITICAL(&AppTxMux);
}

// Drops all pending TX data (called when an app session starts or ends: queued
// responses belong to the previous session's transactions and must not leak).
inline void AppTxFlushAll()
{
    portENTER_CRITICAL(&AppTxMux);
    AppTxTail = AppTxHead;
    portEXIT_CRITICAL(&AppTxMux);
}

// Pops up to `max` stream bytes into `dst` (peek + commit).
inline uint16_t AppTxPop(uint8_t *dst, uint16_t max)
{
    uint16_t n = AppTxPeek(dst, max);
    AppTxCommit(n);
    return n;
}

// Called by the link readers (USB console task / BLE RX drain) to hand over a fully
// parsed frame. Returns false when the queue is full (caller should drop the frame).
inline bool AppInterfaceEnqueue(const PacketFrame &frame)
{
    bool ok = false;
    portENTER_CRITICAL(&AppRxMux);
    if (AppRxCount < APP_RX_QUEUE_DEPTH)
    {
        AppRxQueue[AppRxCount] = frame;
        AppRxCount = (uint8_t)(AppRxCount + 1);
        ok = true;
    }
    portEXIT_CRITICAL(&AppRxMux);
    return ok;
}

// Routes one app-originated frame: rewrites id_src to our address (proxy) and lets the
// dispatcher do all the work - it handles local targets, broadcasts (local + bus) and
// foreign targets (bus forward via its forwarding rule) uniformly.
inline void AppInterfaceRouteIn(PacketFrame &frame)
{
    frame.id_src = DeviceStatus.ShortAddress;
    DispatchPacket(frame);
}

// Main loop pump: dispatches queued inbound frames, refreshes the connection status
// and flushes pending TX stream bytes to the USB link (BLE paces itself in AppBLETick,
// which must be called right after this whenever the BLE link may be active).
void AppInterfacePump()
{
    AppConnected = AppUSBActive() || AppBLEActive();

    // Physical USB link loss ends an app session (revert to CLI).
    AppUSBTick();

    while (AppRxCount > 0)
    {
        portENTER_CRITICAL(&AppRxMux);
        PacketFrame f = AppRxQueue[0];
        for (int i = 1; i < AppRxCount; i++)
            AppRxQueue[i - 1] = AppRxQueue[i];
        AppRxCount = (uint8_t)(AppRxCount - 1);
        portEXIT_CRITICAL(&AppRxMux);
        AppInterfaceRouteIn(f);
    }

    // Flush as many 60-byte USB link frames as possible. AppUSBSend blocks briefly
    // (bounded) until the bytes are in the driver's TX buffer, so everything popped
    // here is considered delivered. When a BLE session is up and USB has been
    // silent, the app has switched links (closed its USB port, cable still
    // plugged): skip USB so responses route to BLE.
    if (AppUSBActive() && AppTxRing && !(AppBLEActive() && AppUsbSilent()))
    {
        uint8_t chunk[60];
        uint16_t n = AppTxPop(chunk, sizeof(chunk));
        while (n > 0)
        {
            AppUSBSend(chunk, n);
            n = AppTxPop(chunk, sizeof(chunk));
        }
    }

    AppBLETick();
}

#endif // USE_APP_INTERFACE