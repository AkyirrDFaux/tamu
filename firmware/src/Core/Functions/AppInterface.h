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
//
// Size: a whole-file reply must be able to sit in the ring in one burst. The largest file the
// device serves the app is `.TABLE`, measured on the rig as 256 fragments of 64 B content
// (16,384 B of file, 22,528 B on the wire: each fragment carries a 12 B header, 4 B frag info
// and the 8 B name echo). 24 KiB leaves room for that whole reply plus the frames around it.
//
// Cost: the ring is malloc'd once in Init(). Measured free heap on the rig immediately before
// this size was chosen (iteration 26.11.4.5, System field 4): 97,048 B, so the 24 KiB ring
// (- 8 KiB it replaces) leaves ~80.6 kB of heap margin - the increase is not thin.
//
// The ring is NOT what makes a long reply complete: the storage read keeps a cursor and emits a
// few fragments per app-task pass (Core/Services/Storage.h), so a reply of any size survives a
// link that stops accepting notifications. The ring only absorbs the burst between two drains.
#define APP_TX_RING_SIZE 24576

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
// Returns how many stream bytes' link frames the port actually took: a device whose USB stack
// can refuse bytes mid-burst reports the shortfall, and the pump keeps the rest in the ring
// (TxPeek/TxCommit) instead of losing it.
uint16_t AppUSBSend(const uint8_t *Data, uint16_t Len);
void AppUSBTick();
void AppBLETick();
// Defined in Dispatcher.h (single translation unit: forward declaration suffices here).
void DispatchPacket(const PacketFrame &Frame);

// The streamed storage read (Core/Services/Storage.h) is driven from Pump() and abandoned by
// TxFlushAll(), so it is declared here - the service header is parsed after this one and keeps
// only the cursor, not the ring.
inline void StorageStreamTick();
inline void StorageStreamAbort();

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

// Ring state the streamed read asks about (declared inline in Core/Services/Storage.h, which is
// parsed after this header). They live here because the ring's invariants - the mux, the
// free-running head/tail, "empty means equal" - belong to their owner.
inline bool AppTxRoomFor(uint16_t Bytes);
inline bool AppTxIdle();
inline uint32_t AppTxDropped();

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
    // Try to enqueue; on a full ring, drain and try once more. This carries a reply that is
    // larger than the ring ONLY as far as one extra drain reaches - a streamed reply does not
    // rely on it: the storage read (Core/Services/Storage.h) asks AppTxRoomFor() before it emits
    // a fragment and holds the un-emitted remainder in its cursor, so a fragment is never
    // refused. What this protects is the short burst (a few frames) that fills the ring between
    // two drains.
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
// USB: peek, hand the bytes to the port, and commit only what its frames actually took - a port
// that stops accepting mid-burst leaves the rest queued in the ring for the next pass, so no byte
// of a reply is dropped on a live link. BLE is drained by AppBLETick (TxPeek/TxCommit, one
// notification per call). Pump the BLE tick here until the ring has room for the next frame, or
// the link stops making progress (the app is not reading - a failed notify leaves its bytes
// queued, so this then terminates).
// Called only from the app task: the sole caller of Send is the dispatcher, and the flush is the
// same work Pump() does.
inline void AppInterfaceService::TxFlush()
{
    if (AppUSBActive() && TxRing && !(AppBLEActive() && AppUsbSilent()))
    {
        uint8_t Chunk[60];
        for (;;)
        {
            uint16_t Count = TxPeek(Chunk, sizeof(Chunk));
            if (Count == 0)
                break;
            uint16_t Sent = AppUSBSend(Chunk, Count);
            TxCommit(Sent);
            if (Sent < Count)
                break; // the port is not taking bytes: keep the rest for the next pass
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
//
// A dismissed session also ends any streamed file read: its remaining fragments belong to the
// app that asked for them, and a re-request starts a fresh stream at fragment 0.
inline void AppInterfaceService::TxFlushAll()
{
    StorageStreamAbort();
    portENTER_CRITICAL(&TxMux);
    TxTail = TxHead;
    portEXIT_CRITICAL(&TxMux);
}

// Whether the ring can take a frame of `Bytes` on a live link. The streamed read asks this
// before it builds a fragment, so Send() can never refuse one (a refused fragment used to be
// dropped, and the app's reassembler never completed).
inline bool AppTxRoomFor(uint16_t Bytes)
{
    if (!AppInterface.Connected || !AppInterface.TxRing)
        return false;
    portENTER_CRITICAL(&AppInterface.TxMux);
    uint16_t Used = (uint16_t)(AppInterface.TxHead - AppInterface.TxTail);
    portEXIT_CRITICAL(&AppInterface.TxMux);
    return (uint16_t)(APP_TX_RING_SIZE - 1 - Used) >= Bytes;
}

// Whether the ring has fully drained (head == tail: the free-running counters can wrap, so only
// equality means empty). The stream's completion line waits for this instead of writing into a
// ring that still owes the app bytes.
inline bool AppTxIdle()
{
    portENTER_CRITICAL(&AppInterface.TxMux);
    bool Empty = AppInterface.TxHead == AppInterface.TxTail;
    portEXIT_CRITICAL(&AppInterface.TxMux);
    return Empty;
}

// Frames the ring refused since boot. Only a streamed read reports it: a non-zero value on a
// device that has served reads means a fragment was lost.
inline uint32_t AppTxDropped()
{
    return AppInterface.TxDropped;
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

    // The streamed storage read: a few fragments per pass, each one only when the link can take
    // it (Core/Services/Storage.h). Between fragments the pass ends, so the RSBus, the display
    // and the scripts run at their own rate while a whole-file reply is in flight.
    StorageStreamTick();

    AppBLETick();
}

#endif // USE_APP_INTERFACE
