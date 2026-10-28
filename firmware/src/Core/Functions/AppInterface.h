#pragma once

// App Interface core (Docs/Services/App Interface.md): routing between the attached
// app (USB / BLE links) and the packet dispatcher.
//
// Identity model: the app has NO network address. Its transaction ID is allocated in
// the App TRID range (0xF000-0xFFFF, Packet.h) and carried in SRV SRC / TRID; the app
// matches replies on that full 16-bit value. Frames arriving from a link get id_src
// rewritten to our own short address (the core acts as a proxy), and every response
// echoes the request's TRID, so replies - local or relayed from the bus - carry a CMD
// in the App range and are pushed back to the app's TX stream by the dispatcher.

#ifdef USE_APP_INTERFACE

#include <cstdint>
#include <cstdlib>
#ifdef APP_INTERFACE_SINGLE_THREADED
// No-OS targets (the CH32 Valu app): there is no RTOS, no separate link task, and the USB
// ISR only services TinyUSB - never these rings - so the critical sections are plain no-ops.
// The ESP32 (core) builds keep the FreeRTOS primitives unchanged below.
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m)  ((void)(m))
#else
#include "freertos/FreeRTOS.h"
#endif
#include "Core/Functions/Packet.h"

// Defined in the device AppUSB.h (included after this header); used by the TX
// pump to stop draining responses into a USB port the app has stopped reading.
bool AppUsbSilent();

// Outgoing stream ring: serialized frames waiting to be flushed to the app links.
// A synchronous service reply (e.g. Storage Read File, which streams a whole
// file in one handler) legitimately bursts well past a small ring before
// AppInterface.Pump() drains it. Send() now drains the ring through TxFlush() when a frame does
// not fit, so the ring no longer caps the size of a reply - it only has to absorb the burst
// between two drains, and this keeps that burst cheap.
#define APP_TX_RING_SIZE 8192

// Inbound frame queue depth: frames parsed by the link readers, dispatched by the pump
// in ApplicationTask context so ALL routing happens on one task (no concurrent dispatch).
// Depth of the inbound app packet queue. Must absorb a burst of concurrent
// app transactions (one per outstanding CID) between pump runs; 6 dropped
// frames when an app fired 8 parallel requests (observed on hardware).
#define APP_RX_QUEUE_DEPTH 12

// Which physical link a frame arrived on.
//
// The dispatcher runs every frame on one task and the handlers cannot see the link, so the
// source is carried WITH the frame through the inbound queue and published while the frame is
// being dispatched. A service that has to refuse a write coming over the radio (the `BLE
// Settings` block: a bonded phone must not rotate the passkey or switch the radio off) reads it
// here instead of guessing from which interfaces happen to be up.
//
// None means "not the app link": a frame that the bus delivered (ProcessBus -> DispatchPacket)
// or a locally generated one. Only an explicit USB source satisfies a USB-only rule.
enum class AppLinkSource : uint8_t { None = 0, USB = 1, BLE = 2 };

// The link the frame currently being dispatched arrived on. Written by Pump() for each queued
// app frame and reset to None afterwards; None whenever no app frame is in flight.
AppLinkSource AppFrameSource = AppLinkSource::None;

// True only while the frame being handled was received over the USB link.
inline bool AppFrameCameOverUsb() { return AppFrameSource == AppLinkSource::USB; }

// Link hooks (implemented per device: Devices/Tamu_v2.0A/AppUSB.h + AppBLE.h).
bool AppUSBActive();
bool AppBLEActive();
void AppUSBSend(const uint8_t *Data, uint16_t Len);
void AppUSBTick();
void AppBLETick();
// Defined in Dispatcher.h (single translation unit: forward declaration suffices here).
void DispatchPacket(const PacketFrame &Frame);

// The App Interface service (Docs/Services/App Interface.md): one class, one instance. The
// ring/queue state is public because the device link hooks read and write it directly.
class AppInterfaceService
{
public:
    void Init();
    bool Send(const PacketFrame &Frame);
    bool Enqueue(const PacketFrame &Frame, AppLinkSource From);
    void RouteIn(PacketFrame &Frame);
    void Pump();
    uint16_t TxPeek(uint8_t *Dst, uint16_t Max);
    void TxCommit(uint16_t Count);
    uint16_t TxPop(uint8_t *Dst, uint16_t Max);

    // Drains pending TX bytes to the active link.
    //
    // The RX dispatch loop runs one service handler to completion before Pump() flushes, so a
    // handler that streams a whole file (one call emitting hundreds of fragments) legitimately
    // bursts past the ring. Send() drains through this when a frame does not fit, which is what
    // lets such a reply complete instead of being truncated. Called only from the app task: the
    // sole caller of Send is the dispatcher, and the flush is the same work Pump() does.
    void TxFlush();
    void TxFlushAll();

    bool Connected = false;    // an app is connected on either link (Doc requirement)
    uint8_t *TxRing = nullptr; // malloc'd in Init; the link hooks read it before pumping
    volatile uint16_t TxHead = 0; // write position
    volatile uint16_t TxTail = 0; // read position
    volatile uint32_t TxDropped = 0;
    portMUX_TYPE TxMux = portMUX_INITIALIZER_UNLOCKED;

    PacketFrame RxQueue[APP_RX_QUEUE_DEPTH];
    AppLinkSource RxFrom[APP_RX_QUEUE_DEPTH] = {}; // the link each queued frame arrived on
    volatile uint8_t RxCount = 0;
    volatile uint8_t RxTail = 0; // dequeue position (ring; avoids shifting the queue)
    portMUX_TYPE RxMux = portMUX_INITIALIZER_UNLOCKED;
};

// One instance per binary.
AppInterfaceService AppInterface;

inline void AppInterfaceService::Init()
{
    if (!TxRing)
        TxRing = (uint8_t *)malloc(APP_TX_RING_SIZE);
}

// Serializes `Frame` into the TX ring. Drop-newest policy when full (the oldest data
// belongs to streams already in flight; dropping the head would corrupt them anyway).
inline bool AppInterfaceService::Send(const PacketFrame &Frame)
{
    if (!Connected || !TxRing)
        return false;

    // Copy the wire bytes straight from the packed frame instead of staging a
    // 128-byte stack copy per reply. PacketFrame is packed, so its bytes are the
    // wire layout in order.
    const uint8_t *Wire = reinterpret_cast<const uint8_t *>(&Frame);
    uint16_t Count = PacketWireSize(&Frame);

    bool Ok = false;
    // Try to enqueue; on a full ring, drain and try once more. The retry is what carries a long
    // stream: without it a reply larger than the ring loses its tail fragments and its STOP frame,
    // and the app's reassembler never completes (a whole-file read then returns nothing).
    for (int Attempt = 0; Attempt < 2 && !Ok; Attempt++)
    {
        if (Attempt)
            TxFlush();
        portENTER_CRITICAL(&TxMux);
        uint16_t Used = (uint16_t)(TxHead - TxTail);
        if (Count <= APP_TX_RING_SIZE - 1 - Used)
        {
            for (uint16_t i = 0; i < Count; i++)
            {
                TxRing[TxHead % APP_TX_RING_SIZE] = Wire[i];
                TxHead = (uint16_t)(TxHead + 1);
            }
            Ok = true;
        }
        portEXIT_CRITICAL(&TxMux);
    }
    if (!Ok)
        TxDropped = TxDropped + 1;
    return Ok;
}

// Peek/commit variant: copies up to `Max` pending bytes WITHOUT consuming them; the
// caller confirms consumption with TxCommit(n) once they are actually on the wire
// (used by the BLE link so a failed notify does not lose data).
inline uint16_t AppInterfaceService::TxPeek(uint8_t *Dst, uint16_t Max)
{
    uint16_t Count = 0;
    portENTER_CRITICAL(&TxMux);
    uint16_t Used = (uint16_t)(TxHead - TxTail);
    if (Used > Max)
        Used = Max;
    for (Count = 0; Count < Used; Count++)
        Dst[Count] = TxRing[(TxTail + Count) % APP_TX_RING_SIZE];
    portEXIT_CRITICAL(&TxMux);
    return Count;
}

inline void AppInterfaceService::TxCommit(uint16_t Count)
{
    if (Count == 0)
        return;
    portENTER_CRITICAL(&TxMux);
    TxTail = (uint16_t)(TxTail + Count);
    portEXIT_CRITICAL(&TxMux);
}

// Drains pending TX bytes to the active link.
//
// USB is popped directly (AppUSBSend blocks briefly, so popped bytes are delivered). BLE is
// drained by AppBLETick (TxPeek/TxCommit, one notification per call), so a BLE session cannot
// drain the ring the way USB does: a whole-file reply that bursts past the ring inside one
// handler would find it still full on Send()'s retry and lose its tail fragments. Pump the BLE
// tick here until the ring has room for the next frame, or the link stops making progress (the
// app is not reading - a failed notify leaves its bytes queued, so this then terminates).
// Called only from the app task: the sole caller of Send is the dispatcher, and the flush is the
// same work Pump() does.
inline void AppInterfaceService::TxFlush()
{
    if (AppUSBActive() && TxRing && !(AppBLEActive() && AppUsbSilent()))
    {
        uint8_t Chunk[60];
        uint16_t Count = TxPop(Chunk, sizeof(Chunk));
        while (Count > 0)
        {
            AppUSBSend(Chunk, Count);
            Count = TxPop(Chunk, sizeof(Chunk));
        }
        return;
    }

    // BLE link. Each iteration must drain at least one byte or the loop stops, so it is bounded
    // by the ring size (a stalled/back-pressured app breaks out and lets Send() drop as before).
    if (AppBLEActive() && TxRing)
    {
        for (;;)
        {
            uint16_t Used = (uint16_t)(TxHead - TxTail);
            if (Used <= APP_TX_RING_SIZE - 256)
                break; // room for any single frame
            AppBLETick();
            if ((uint16_t)(TxHead - TxTail) >= Used)
                break; // no progress this tick: the link is not accepting notifications
        }
    }
}

// Drops all pending TX data (called when an app session starts or ends: queued
// responses belong to the previous session's transactions and must not leak).
inline void AppInterfaceService::TxFlushAll()
{
    portENTER_CRITICAL(&TxMux);
    TxTail = TxHead;
    portEXIT_CRITICAL(&TxMux);
}

// Pops up to `Max` stream bytes into `Dst` (peek + commit).
inline uint16_t AppInterfaceService::TxPop(uint8_t *Dst, uint16_t Max)
{
    uint16_t Count = TxPeek(Dst, Max);
    TxCommit(Count);
    return Count;
}

// Called by the link readers (USB console task / BLE RX drain) to hand over a fully
// parsed frame. Returns false when the queue is full (caller should drop the frame).
// `From` is the link it arrived on and is published again while the frame is dispatched.
inline bool AppInterfaceService::Enqueue(const PacketFrame &Frame, AppLinkSource From)
{
    bool Ok = false;
    portENTER_CRITICAL(&RxMux);
    if (RxCount < APP_RX_QUEUE_DEPTH)
    {
        uint8_t Head = (uint8_t)((RxTail + RxCount) % APP_RX_QUEUE_DEPTH);
        RxQueue[Head] = Frame;
        RxFrom[Head] = From;
        RxCount = (uint8_t)(RxCount + 1);
        Ok = true;
    }
    portEXIT_CRITICAL(&RxMux);
    return Ok;
}

// Routes one app-originated frame: rewrites id_src to our address (proxy) and lets the
// dispatcher do all the work - it handles local targets, broadcasts (local + bus) and
// foreign targets (bus forward via its forwarding rule) uniformly.
inline void AppInterfaceService::RouteIn(PacketFrame &Frame)
{
    // The app has no net context: it addresses devices by their device number on the local
    // net (net 0). The dispatcher resolves net 0 to our net at match/forward time.
    Frame.IdSrc = DeviceStatus.ShortAddress;
    DispatchPacket(Frame);
}

// Main loop pump: dispatches queued inbound frames, refreshes the connection status
// and flushes pending TX stream bytes to the USB link (BLE paces itself in AppBLETick,
// which must be called right after this whenever the BLE link may be active).
void AppInterfaceService::Pump()
{
    Connected = AppUSBActive() || AppBLEActive();

    // Physical USB link loss ends an app session (the link task flushes and resets).
    AppUSBTick();

    for (;;)
    {
        PacketFrame Frame;
        AppLinkSource From;
        portENTER_CRITICAL(&RxMux);
        if (RxCount == 0) // read inside the lock so an enqueue can't corrupt the pop
        {
            portEXIT_CRITICAL(&RxMux);
            break;
        }
        Frame = RxQueue[RxTail];
        From = RxFrom[RxTail];
        RxTail = (uint8_t)((RxTail + 1) % APP_RX_QUEUE_DEPTH);
        RxCount = (uint8_t)(RxCount - 1);
        portEXIT_CRITICAL(&RxMux);
        // Publish the link while this frame is handled: a service that must refuse a write from
        // the radio (the BLE Settings block) reads it, and it is None again once the frame is done.
        AppFrameSource = From;
        RouteIn(Frame);
        AppFrameSource = AppLinkSource::None;
    }

    TxFlush();

    AppBLETick();
}

#endif // USE_APP_INTERFACE
