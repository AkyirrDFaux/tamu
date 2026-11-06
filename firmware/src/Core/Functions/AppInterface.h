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
#include "Core/Functions/AppConsole.h" // AppConsoleBusy: the gate's ring is re-emitted when idle

// Defined in the device AppUSB.h (included after this header); used by the TX
// pump to stop draining responses into a USB port the app has stopped reading.
bool AppUsbSilent();

// Outgoing stream ring: serialized frames waiting to be flushed to the app links.
//
// Size, cost and the 16-bit-counter indexing rule live with the ring's geometry in
// Core/Functions/TxRing.h (E75's root cause was a position taken from a wider sum: a read then
// handed out ring bytes 16384 positions from where they were written).
#include "Core/Functions/TxRing.h"

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

// True only while the frame being handled was received over the BLE link. A service that allows a
// write from the radio (the `BLE Settings` block since the GPIO8 reset exists) still has to ask
// whether THAT link is a secured session - the source says which transport, not how trusted it is.
inline bool AppFrameCameOverBle() { return AppFrameSource == AppLinkSource::BLE; }

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

    // The one forwarded app transaction the bus owes an answer to (see AppBusForwardPending).
    volatile bool BusPending = false;
    volatile uint16_t BusPendingTrid = 0;
    uint32_t BusPendingSince = 0;
};

// One instance per binary.
AppInterfaceService AppInterface;

// One forwarded app transaction at a time.
//
// The RSBus is one half-duplex medium, and SendAndVerifyPacket only proves that OUR bytes went
// out intact - it verifies the transceiver echo and does not wait for the target's answer. So
// two app frames dispatched back to back put the second request on the wire while the first
// target's answer is still on its way, and the two collide. Measured on the rig 2026-11-05
// (firmware/.pio/probe/burst_probe.py, image 26.11.5.1): a 5-frame identity burst written in one
// go - the shape `app/lib/core/device_identity.dart` uses, five concurrent reads - answered
// 4/20 over four rounds on every node (1/5 each), the same five reads 10 ms apart answered
// 20/20, and the same burst addressed at the CORE's own System block answered 20/20. So an
// app-origin unicast forward holds the rest of the queue until its reply lands.
// How long a forwarded app transaction keeps the bus. It must exceed the SLOWEST LEGITIMATE
// ANSWER, not the average one. Measured on the rig 2026-11-06 (`.pio/probe/gate_latency.py`, USB,
// one request at a time): an ordinary forwarded read or write is answered in 13-14 ms, a node's
// Save All in 125-184 ms. Per frame, in one restore (`restore_measure4.gate.log`): of 280
// forwards 73 (26 %) had their 50 ms window release the queue before the answer was seen, the
// longest answer that DID make it inside the window was 171 ms (3.4x the window), and 7 of the 73
// had a core-origin frame go onto the bus while the answer was still owed. 50 ms was E76's
// number, picked only to bound a lost reply, and it was never the answer time.
//
// 600 ms covers the measured worst case (184 ms) with ~3x margin, and it still bounds what a lost
// reply costs: the app's own request timeout is 2 s, so the queue is released long before it.
// A reply-less forward is NOT the case here: every app request carries REQACK
// (`app/lib/core/protocol.dart`, PacketFrame.single - "Requests carry REQACK"), so no forward of a
// restore is a write that expects no answer, and the other rule (do not gate a no-reply write)
// would have changed nothing.
#define APP_BUS_REPLY_TIMEOUT_MS 600

// ---- Forward-gate diagnostics (card t_a76974ef) ---------------------------------------------
//
// Every app frame that leaves the core holds the gate for as long as its node takes to answer it,
// and the window above is the number that decides whether that answer is still waited for. The
// app link cannot show it: `DeviceLog` is silent whenever the app is TALKING (AppConsole.h,
// `AppConsoleBusy` - a line written into that stream splits a frame), which is the whole of a
// restore. So every gated transaction is recorded in a ring and the ring is re-emitted to the
// console once the link goes idle, where a bench capture (`firmware/.pio/probe/gate_dump.py`)
// reads it back per frame.
//
// Measured on the rig 2026-11-06 (`.pio/probe/gate_latency.py`, USB, one request at a time):
// an ordinary forwarded read or write is answered in 13-14 ms, a node's Save All in 125-184 ms.
// The 50 ms window this replaces was shorter than the slowest legitimate answer by an order of
// magnitude, which is what this ring exists to prove per frame.
//
// Core only: a node's RAM has no room for a diagnostic it could never show (DAS_v0_1 links at
// 2048/2048 B), and every value here is read and written from the app task alone (Pump and the
// dispatcher), so no lock is needed.
// One restore records ~300 gated forwards (every read and write that leaves the core for a
// node: the plan, the writes themselves and the read-back), so the ring holds a whole run's
// worth and the totals line still counts everything. 512 * 6 B = 3 kB of the core's 320 kB.
#ifdef TYPE_CORE
#define APP_BUS_GATE_RING 512

// What the ring costs the console. 0 (the shipped build) re-emits only the transactions that did
// NOT go cleanly - expired, late, clash, lost - plus the totals line, so a healthy restore costs
// one line instead of one per forward. 1 re-emits every record, which is what a per-frame bench
// measurement reads (`-DAPP_BUS_GATE_DUMP_ALL=1`).
#ifndef APP_BUS_GATE_DUMP_ALL
#define APP_BUS_GATE_DUMP_ALL 0
#endif

// What became of one gated transaction.
enum : uint8_t
{
    ABG_REPLY = 0x00,   // released by the matching reply
    ABG_EXPIRED = 0x01, // no reply inside the window: the timeout released the queue
    ABG_LATE = 0x02,    // the reply turned up AFTER the window had already released the gate
    ABG_CLASH = 0x04,   // a core-origin frame went onto the bus while this gate was owed an answer
    ABG_LOST = 0x08,    // the forward never reached the wire (SendAndVerifyPacket exhausted)
};

struct AppBusGateEntry
{
    uint16_t Tgt;
    uint16_t Trid;
    uint8_t Hold;  // ms from send to release, saturated at 255
    uint8_t Flags; // ABG_* above
};

AppBusGateEntry AppBusGateRing[APP_BUS_GATE_RING];
uint32_t AppBusGateWrites = 0; // records written since boot; the ring keeps the newest
uint32_t AppBusGateDumped = 0; // records already re-emitted to the console
bool AppBusGateDumpStarted = false;
// The ring is re-emitted only after the app link has been QUIET for a long time: a restore holds
// the port for minutes and its own stream is where a mid-run dump would land (the running tool
// discards text, and those records would be gone). The port's "session" cannot be used for this -
// closing /dev/ttyACM0 does not un-configure the USB-Serial-JTAG interface, so AppUSBActive()
// stays true after the host lets go. 10 s is far longer than any gap a run leaves between frames
// (the app waits for one reply at a time; its longest single wait - a Save All - is 355 ms).
bool AppBusGateHostHeld = false; // a host has held the port at least once
uint32_t AppBusGateLastFrame = 0;
#define APP_BUS_GATE_DUMP_QUIET_MS 10000
uint32_t AppBusGateForwards = 0, AppBusGateReplies = 0, AppBusGateExpiries = 0,
         AppBusGateLates = 0, AppBusGateClashes = 0, AppBusGateLosses = 0;
uint16_t AppBusGateWorst = 0;   // longest hold that WAS answered (ms)
uint8_t AppBusGateOpen = 0;     // ring slot of the transaction the gate is waiting on
bool AppBusGateIsOpen = false;
uint16_t AppBusGateOpenTgt = 0;
uint32_t AppBusGateOpenAt = 0;
// The last few windows that closed unanswered, so a reply that arrives after one of them can be
// named as late. That is what tells "the node answered too slowly for the window" from "the node
// never answered at all" - and one slot could not: a restore has dozens of expiries in a row, and
// a single watch only ever sees the newest.
#define APP_BUS_GATE_LATE_SLOTS 8
struct AppBusGateLateSlot
{
    uint16_t Tgt;
    uint16_t Trid;
    uint32_t At; // send time of the window that closed unanswered
    bool Used;
};
AppBusGateLateSlot AppBusGateLate[APP_BUS_GATE_LATE_SLOTS];
uint8_t AppBusGateLateNext = 0;

// The app link's own loss paths, so a request that never became a transaction can be told apart
// from one whose answer went missing. Written by the USB/BLE readers (AppUSB.h/AppBLE.h).
uint32_t AppLinkFrameRejects = 0; // a link frame failed its CRC and was re-synced
uint32_t AppLinkEnqueueDrops = 0; // the inbound queue was full and the frame was dropped
uint32_t AppBusSendFails = 0;     // a bus transmit exhausted its retries (core-origin included)
extern uint32_t BusRxCrcRejects;  // defined in Core/Functions/Bus.h (the RX assembler's owner)

// Frames the core itself wants to put on the bus while an app transaction is still owed an
// answer. They are held here and dispatched the moment the gate closes: one half-duplex medium
// means OUR request and the node's answer must not share the wire either, and the E76 gate only
// ever held APP frames - so the core's own subscription/script/log traffic still went out
// mid-transaction (measured 7 times in one restore, before this queue existed). Depth 4 with a
// 600 ms window: a deferral ring that is full drops nothing important - the frame goes out and is
// recorded as a clash instead (the counter in the totals line is the witness).
#define APP_BUS_DEFER_DEPTH 4
PacketFrame AppBusDeferred[APP_BUS_DEFER_DEPTH];
uint8_t AppBusDeferredCount = 0;
uint32_t AppBusDeferredTotal = 0, AppBusDeferredDrops = 0;

inline bool AppBusDefer(const PacketFrame &Frame)
{
    if (AppBusDeferredCount >= APP_BUS_DEFER_DEPTH)
    {
        AppBusDeferredDrops++;
        return false;
    }
    AppBusDeferred[AppBusDeferredCount++] = Frame;
    AppBusDeferredTotal++;
    return true;
}

inline void AppBusDeferredDrain(); // Core/Functions/Dispatcher.h (it owns the dispatch order)

inline void DeviceLog(const char *Tag, const char *Fmt, ...); // Core/Functions/Log.h

// Records one transaction. The open transaction's slot is filled in when it is released, so a
// CLASH can be marked on it while it is still owed an answer.
inline void AppBusGateRecord(uint16_t Tgt, uint16_t Trid, uint32_t HoldMs, uint8_t Flags)
{
    AppBusGateEntry &Entry = AppBusGateRing[AppBusGateWrites % APP_BUS_GATE_RING];
    Entry.Tgt = Tgt;
    Entry.Trid = Trid;
    Entry.Hold = (uint8_t)(HoldMs > 255 ? 255 : HoldMs);
    Entry.Flags = Flags;
    AppBusGateWrites++;
}

// Re-emits the ring to the console a few records per pass, and only while the app link is idle
// (a line written into a talking link would split a frame). Ends with the run's totals, which is
// the line a capture is checked against.
inline void AppBusGateDrain()
{
    if (AppBusGateDumped == AppBusGateWrites)
        return;
    if (!AppUSBActive())
        return; // never write into a port nobody holds
    if (AppConsoleBusy(AppUSBActive(), AppUsbSilent(), AppInterface.TxHead, AppInterface.TxTail))
        return;
    if ((uint32_t)(DeviceStatus.UptimeMs - AppBusGateLastFrame) < APP_BUS_GATE_DUMP_QUIET_MS)
        return; // a run may still be in progress: keep the records until the link is quiet
    uint32_t Oldest = AppBusGateWrites > APP_BUS_GATE_RING ? AppBusGateWrites - APP_BUS_GATE_RING : 0;
    if (AppBusGateDumped < Oldest)
        AppBusGateDumped = Oldest; // the ring overwrote them while the link was talking
    if (!AppBusGateDumpStarted)
    {
        AppBusGateDumpStarted = true;
        DeviceLog("BUSGATE", "records %lu..%lu of %lu%s",
                  (unsigned long)(AppBusGateDumped + 1), (unsigned long)AppBusGateWrites,
                  (unsigned long)AppBusGateWrites,
                  APP_BUS_GATE_DUMP_ALL ? "" : " (anomalies only)");
    }
    for (int n = 0; n < 4 && AppBusGateDumped < AppBusGateWrites; n++, AppBusGateDumped++)
    {
        const AppBusGateEntry &Entry = AppBusGateRing[AppBusGateDumped % APP_BUS_GATE_RING];
        if (!APP_BUS_GATE_DUMP_ALL && Entry.Flags == ABG_REPLY)
            continue; // a clean transaction: the totals line already counts it
        DeviceLog("BUSGATE", "tgt=%u trid=0x%04X hold=%ums%s%s%s",
                  (unsigned)Entry.Tgt, (unsigned)Entry.Trid, (unsigned)Entry.Hold,
                  (Entry.Flags & ABG_EXPIRED) ? " EXPIRED" : "",
                  (Entry.Flags & ABG_LATE) ? " LATE" : "",
                  (Entry.Flags & ABG_CLASH) ? " CLASH" : "");
    }
    if (AppBusGateDumped == AppBusGateWrites)
    {
        AppBusGateDumpStarted = false;
        DeviceLog("BUSGATE",
                  "total forwards=%lu replies=%lu expired=%lu late=%lu clash=%lu lost=%lu "
                  "deferred=%lu/%lu worst=%ums window=%ums",
                  (unsigned long)AppBusGateForwards, (unsigned long)AppBusGateReplies,
                  (unsigned long)AppBusGateExpiries, (unsigned long)AppBusGateLates,
                  (unsigned long)AppBusGateClashes, (unsigned long)AppBusGateLosses,
                  (unsigned long)AppBusDeferredTotal, (unsigned long)AppBusDeferredDrops,
                  (unsigned)AppBusGateWorst, (unsigned)APP_BUS_REPLY_TIMEOUT_MS);
        DeviceLog("BUSGATE", "link rejects=%lu drops=%lu | bus sendfails=%lu rxrejects=%lu",
                  (unsigned long)AppLinkFrameRejects, (unsigned long)AppLinkEnqueueDrops,
                  (unsigned long)AppBusSendFails, (unsigned long)BusRxCrcRejects);
    }
}
#endif

inline void AppBusForwardStart(uint16_t Tgt, uint16_t Trid)
{
    AppInterface.BusPendingTrid = Trid;
    AppInterface.BusPendingSince = DeviceStatus.UptimeMs;
    AppInterface.BusPending = true;
#ifdef TYPE_CORE
    AppBusGateForwards++;
    AppBusGateOpenTgt = Tgt;
    AppBusGateOpenAt = AppInterface.BusPendingSince;
    AppBusGateOpen = (uint8_t)(AppBusGateWrites % APP_BUS_GATE_RING);
    AppBusGateIsOpen = true;
    AppBusGateLastFrame = DeviceStatus.UptimeMs;
    AppBusGateRecord(Tgt, Trid, 0, ABG_REPLY); // replaced when the transaction is released
#endif
}

// Closes the open transaction: fills in how long it was held and what released it.
#ifdef TYPE_CORE
inline void AppBusGateClose(uint8_t Flags)
{
    if (!AppBusGateIsOpen)
        return;
    AppBusGateIsOpen = false;
    uint32_t Hold = (uint32_t)(DeviceStatus.UptimeMs - AppBusGateOpenAt);
    AppBusGateEntry &Entry = AppBusGateRing[AppBusGateOpen % APP_BUS_GATE_RING];
    Entry.Hold = (uint8_t)(Hold > 255 ? 255 : Hold);
    Entry.Flags = (uint8_t)(Flags | (Entry.Flags & ABG_CLASH)); // keep a clash noted while open
    if (!(Flags & ABG_EXPIRED))
    {
        AppBusGateReplies++;
        if (Entry.Hold > AppBusGateWorst)
            AppBusGateWorst = Entry.Hold;
    }
    if (Flags & ABG_EXPIRED)
        AppBusGateExpiries++;
    if (Flags & ABG_LOST)
        AppBusGateLosses++;
}
#endif

// Ends the transaction without a reply: the frame never reached the wire.
inline void AppBusForwardFinish()
{
    AppInterface.BusPending = false;
#ifdef TYPE_CORE
    AppBusGateClose(ABG_LOST);
#endif
}

// True while a forwarded request still owes the bus a reply. It expires, so a lost reply (or a
// node that is gone) costs the queue one timeout and the app's own retry takes over instead of
// the queue wedging.
inline bool AppBusForwardPending()
{
    if (!AppInterface.BusPending)
        return false;
    if ((uint32_t)(DeviceStatus.UptimeMs - AppInterface.BusPendingSince) >= APP_BUS_REPLY_TIMEOUT_MS)
    {
        AppInterface.BusPending = false;
#ifdef TYPE_CORE
        AppBusGateClose(ABG_EXPIRED);
        // Remember the window that closed unanswered: a reply that turns up afterwards is what
        // separates "the node answered too late for the window" from "the node never answered".
        AppBusGateLate[AppBusGateLateNext].Tgt = AppBusGateOpenTgt;
        AppBusGateLate[AppBusGateLateNext].Trid = AppInterface.BusPendingTrid;
        AppBusGateLate[AppBusGateLateNext].At = AppInterface.BusPendingSince;
        AppBusGateLate[AppBusGateLateNext].Used = false;
        AppBusGateLateNext = (uint8_t)((AppBusGateLateNext + 1) % APP_BUS_GATE_LATE_SLOTS);
#endif
        return false;
    }
    return true;
}

// The answer to a forwarded request came back (a response from another device).
inline void AppBusForwardSawReply(uint16_t Trid)
{
    if (AppInterface.BusPending && AppInterface.BusPendingTrid == Trid)
    {
        AppInterface.BusPending = false;
#ifdef TYPE_CORE
        AppBusGateClose(ABG_REPLY);
        AppBusGateLastFrame = DeviceStatus.UptimeMs;
#endif
        return;
    }
#ifdef TYPE_CORE
    // A reply for a window that has already closed: record how late it was. That is the number
    // the window has to cover, and it is invisible from the app (the app saw a timeout).
    for (uint8_t i = 0; i < APP_BUS_GATE_LATE_SLOTS; i++)
    {
        AppBusGateLateSlot &Slot = AppBusGateLate[i];
        if (!Slot.Used && Slot.Trid == Trid)
        {
            Slot.Used = true;
            AppBusGateRecord(Slot.Tgt, Trid, (uint32_t)(DeviceStatus.UptimeMs - Slot.At), ABG_LATE);
            break;
        }
    }
#endif
}

#ifdef TYPE_CORE
// A frame that is not the app's went onto the bus while the gate was owed an answer: two
// transmitters on a half-duplex medium, which is the collision the gate exists to prevent.
inline void AppBusGateNoteClash()
{
    if (!AppBusGateIsOpen)
        return;
    AppBusGateRing[AppBusGateOpen % APP_BUS_GATE_RING].Flags |= ABG_CLASH;
    AppBusGateClashes++;
}
#endif

// Whether an app frame has to leave this core (a unicast to another address). A broadcast is not
// held: several devices may answer one, so there is no single reply to wait for.
inline bool AppLinkFrameNeedsBus(const PacketFrame &Frame)
{
    return Frame.IdTgt != ADDR_BROADCAST &&
           NetQualifyLocal(Frame.IdTgt, DeviceStatus.NetId) != DeviceStatus.ShortAddress;
}

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
                TxRing[AppTxRingPos(TxHead)] = Wire[i];
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
//
// The position comes from AppTxRingPos() with the counter narrowed to uint16_t first: the sum
// `TxTail + Count` is an int, and 65536 is not a multiple of APP_TX_RING_SIZE, so a peek that
// straddles the counter wrap would otherwise read 16384 bytes away from the writer (E75).
inline uint16_t AppInterfaceService::TxPeek(uint8_t *Dst, uint16_t Max)
{
    uint16_t Count = 0;
    portENTER_CRITICAL(&TxMux);
    uint16_t Used = (uint16_t)(TxHead - TxTail);
    if (Used > Max)
        Used = Max;
    for (Count = 0; Count < Used; Count++)
        Dst[Count] = TxRing[AppTxRingPos((uint16_t)(TxTail + Count))];
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
        portEXIT_CRITICAL(&RxMux);
        // One bus transaction at a time (AppBusForwardPending): a frame that has to leave the
        // core waits in the queue while an earlier forward still owes the bus a reply, so the
        // node's answer and our next request never share the wire.
        if (AppBusForwardPending() && AppLinkFrameNeedsBus(Frame))
            break;
        portENTER_CRITICAL(&RxMux);
        RxTail = (uint8_t)((RxTail + 1) % APP_RX_QUEUE_DEPTH);
        RxCount = (uint8_t)(RxCount - 1);
        portEXIT_CRITICAL(&RxMux);
        // Publish the link while this frame is handled: a service that must refuse a write from
        // the radio (the BLE Settings block) reads it, and it is None again once the frame is done.
        AppFrameSource = From;
#ifdef TYPE_CORE
        AppBusGateLastFrame = DeviceStatus.UptimeMs; // this link is talking: no dump yet
#endif
        RouteIn(Frame);
        AppFrameSource = AppLinkSource::None;
    }

    TxFlush();

#ifdef TYPE_CORE
    // Re-emit the forward gate's per-frame records once the link is idle (AppBusGateDrain).
    AppBusGateDrain();
    // Frames the core held back while an app transaction was owed an answer go out now that the
    // gate has closed (AppBusDeferredDrain).
    AppBusDeferredDrain();
#endif

    // The streamed storage read: a few fragments per pass, each one only when the link can take
    // it (Core/Services/Storage.h). Between fragments the pass ends, so the RSBus, the display
    // and the scripts run at their own rate while a whole-file reply is in flight.
    StorageStreamTick();

    AppBLETick();
}

#endif // USE_APP_INTERFACE
