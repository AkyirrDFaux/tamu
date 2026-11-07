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
#include "Core/Functions/BusGate.h" // the gate's decisions (host-tested)
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
    // The link-only half of Pump(): moves queued reply bytes and ticks the radios, and dispatches
    // nothing. The bus transmit calls it while a send blocks the main task (Devices/Tamu_v2.0A/
    // RSBus.h RS485AppLinkTick) - a dispatch there could start a second transmit on the wire the
    // running one owns.
    void PumpLink();
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

    // The one bus transaction the medium owes an answer to (see AppBusForwardPending). Not
    // app-only any more: a core-origin request that carries REQACK holds the same slot, because
    // it is the same wire (BusGateNeedsAnswer in Core/Functions/BusGate.h decides which frames
    // are; AppBusGateOffer below opens and releases the slot).
    volatile bool BusPending = false;
    volatile uint16_t BusPendingTrid = 0;
    uint32_t BusPendingSince = 0;
    uint16_t BusPendingWindow = BUS_GATE_WINDOW_MS; // fitted to the request kind that opened it
};

// One instance per binary.
AppInterfaceService AppInterface;

// One bus transaction at a time, whoever started it.
//
// The RSBus is one half-duplex medium, and SendAndVerifyPacket only proves that OUR bytes went
// out intact - it verifies the transceiver echo and does not wait for the target's answer. So
// two answered unicasts sent back to back put the second request on the wire while the first
// target's answer is still on its way, and the two collide. Measured on the rig 2026-11-05
// (firmware/.pio/probe/burst_probe.py, image 26.11.5.1): a 5-frame identity burst written in one
// go - the shape `app/lib/core/device_identity.dart` uses, five concurrent reads - answered
// 4/20 over four rounds on every node (1/5 each), the same five reads 10 ms apart answered
// 20/20, and the same burst addressed at the CORE's own System block answered 20/20.
//
// The gate is asked at the ONE place a frame actually goes onto the wire
// (Devices/Tamu_v2.0A/RSBus.h SendAndVerifyPacket -> AppBusGateOffer below), so it now covers
// every answered unicast and not only the app's forwards: a core-origin request (a subscription
// install/renewal, a script's foreign register access, a confirm-required value update) used to
// leave SendAndVerifyPacket ungated, so two of them could sit on the wire 3-10 ms apart with the
// first answer still owed. The window per request kind - and why each number is what it is -
// lives in Core/Functions/BusGate.h (BUS_GATE_WINDOW_MS / _CORE_MS / _STORE_MS). 600 ms was
// fitted to a forwarded app read/write: measured 13-14 ms per request on the rig 2026-11-06
// (`.pio/probe/gate_latency.py`, USB, one request at a time) and 171 ms the longest ANSWERED
// forward of a whole restore (`restore_measure4.gate.log`).
//
// A reply-less forward is NOT a case to gate: every app request carries REQACK
// (`app/lib/core/protocol.dart`, PacketFrame.single - "Requests carry REQACK"), and a frame that
// expects no answer leaves the gate alone (BusGateNeedsAnswer).

// ---- Forward-gate diagnostics (card t_a76974ef) ---------------------------------------------
//
// Every answered unicast that leaves the core holds the gate for as long as its node takes to
// answer it, and the per-kind window (BusGate.h) decides whether that answer is still waited
// for. The app link cannot show it: `DeviceLog` is silent whenever the app is TALKING
// (AppConsole.h, `AppConsoleBusy` - a line written into that stream splits a frame), which is the
// whole of a restore. So every gated transaction is recorded in a ring and the ring is re-emitted
// to the console once the link goes idle, where a bench capture
// (`firmware/.pio/probe/gate_dump.py`) reads it back per frame.
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
// from one whose answer went missing. Written by the USB/BLE readers (AppUSB.h/AppBLE.h), per
// transport: the BLE reader used to discard the result of AppInterface.Enqueue outright, so a
// frame the radio delivered and the queue refused was invisible - the app saw only its timeout.
uint32_t AppLinkFrameRejectsUsb = 0; // a USB link frame failed its CRC and was re-synced
uint32_t AppLinkEnqueueDropsUsb = 0; // the inbound queue was full and the USB frame was dropped
uint32_t AppLinkFrameRejectsBle = 0; // a BLE link frame failed its CRC and was re-synced
uint32_t AppLinkEnqueueDropsBle = 0; // the inbound queue was full and the BLE frame was dropped
uint32_t AppBusSendFails = 0;     // a bus transmit exhausted its retries (core-origin included)
// (card t_763b4cde) The counter above cannot tell WHICH frame was lost. A core-origin request
// that never got onto the wire used to be invisible twice over: the dispatcher ignored the
// transmit's return and no gate slot tracks a frame the core started for itself, so a subscription
// install or a script's foreign access simply never happened while its caller believed it had been
// sent. Counted here, printed in the same BUSGATE line as sendfails.
uint32_t AppBusCoreRequestLost = 0;
// A retransmission answered from the repeat record instead of being executed a second time
// (Core/Functions/BusRepeat.h). Zero on a quiet bus; non-zero proves the guard is doing its job.
uint32_t AppBusRepeatsAnswered = 0;
extern uint32_t BusRxCrcRejects;  // defined in Core/Functions/Bus.h (the RX assembler's owner)
extern uint32_t BusRxDestroyed;   // defined in Core/Functions/Bus.h (bytes the RX path had to drop)
extern uint32_t RS485TxStintWorstMs; // defined in Devices/Tamu_v2.0A/RSBus.h (longest send, ms)

// Frames that are due for the bus while another transaction is still owed an answer. They are
// held here and dispatched the moment the gate closes: one half-duplex medium means OUR request
// and the node's answer must not share the wire. Since the gate is asked at SendAndVerifyPacket,
// this now also holds the core's own answered requests - the ones that used to slip out between
// an app forward and its answer.
//
// Depth 8 rather than 4: a core request now takes the slot for itself, so the frames a pass
// produces while it waits (a provider's updates for several requesters, a script's accesses, the
// next subscription poll) queue up behind it instead of going out. A frame the ring cannot hold
// is not lost - it goes out and the open transaction records a clash, so the totals line's
// `clash=` and `deferred=<total>/<dropped>` together say whether the depth is enough.
#define APP_BUS_DEFER_DEPTH 8
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
                  "deferred=%lu/%lu worst=%ums",
                  (unsigned long)AppBusGateForwards, (unsigned long)AppBusGateReplies,
                  (unsigned long)AppBusGateExpiries, (unsigned long)AppBusGateLates,
                  (unsigned long)AppBusGateClashes, (unsigned long)AppBusGateLosses,
                  (unsigned long)AppBusDeferredTotal, (unsigned long)AppBusDeferredDrops,
                  (unsigned)AppBusGateWorst);
        // The window in force per request kind (Core/Functions/BusGate.h): an ordinary forwarded
        // app read/write, a core-origin request, and a store save.
        DeviceLog("BUSGATE", "window app/core/store=%u/%u/%ums held=%ums",
                  (unsigned)BUS_GATE_WINDOW_MS, (unsigned)BUS_GATE_WINDOW_CORE_MS,
                  (unsigned)BUS_GATE_WINDOW_STORE_MS, (unsigned)AppInterface.BusPendingWindow);
        // Both link loss paths, per transport: a request the queue refused never became a
        // transaction, so it must not read as a node that timed out (they are different defects).
        DeviceLog("BUSGATE", "link usb rejects=%lu drops=%lu | ble rejects=%lu drops=%lu",
                  (unsigned long)AppLinkFrameRejectsUsb, (unsigned long)AppLinkEnqueueDropsUsb,
                  (unsigned long)AppLinkFrameRejectsBle, (unsigned long)AppLinkEnqueueDropsBle);
        DeviceLog("BUSGATE", "bus sendfails=%lu rxrejects=%lu rxdestroyed=%lu txstint=%lums corelost=%lu repeats=%lu",
                  (unsigned long)AppBusSendFails, (unsigned long)BusRxCrcRejects,
                  (unsigned long)BusRxDestroyed, (unsigned long)RS485TxStintWorstMs,
                  (unsigned long)AppBusCoreRequestLost, (unsigned long)AppBusRepeatsAnswered);
    }
}
#endif

// Opens the transaction a frame that expects an answer creates on the medium. `WindowMs` comes
// from BusGateWindowMs for the request kind, so the slot is held exactly as long as that kind's
// legitimate answer can take (Core/Functions/BusGate.h).
inline void AppBusForwardStart(const PacketFrame &Frame, uint16_t WindowMs)
{
    AppInterface.BusPendingTrid = Frame.Trid;
    AppInterface.BusPendingSince = DeviceStatus.UptimeMs;
    AppInterface.BusPendingWindow = WindowMs;
    AppInterface.BusPending = true;
#ifdef TYPE_CORE
    AppBusGateForwards++;
    AppBusGateOpenTgt = Frame.IdTgt;
    AppBusGateOpenAt = AppInterface.BusPendingSince;
    AppBusGateOpen = (uint8_t)(AppBusGateWrites % APP_BUS_GATE_RING);
    AppBusGateIsOpen = true;
    AppBusGateLastFrame = DeviceStatus.UptimeMs;
    AppBusGateRecord(Frame.IdTgt, Frame.Trid, 0, ABG_REPLY); // filled in when it is released
#endif
}

// The frame is on the wire: the answer is owed from NOW, not from before the send. A transmit that
// first waited out a busy line (RS485WaitForSilence) or retried with backoff can spend a large
// part of the window before the frame has even left, which would make a short core-origin window
// expire on a healthy node.
inline void AppBusForwardSent()
{
    if (!AppInterface.BusPending)
        return;
    AppInterface.BusPendingSince = DeviceStatus.UptimeMs;
#ifdef TYPE_CORE
    AppBusGateOpenAt = DeviceStatus.UptimeMs;
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

// True while the medium still owes an answer to a transaction - an app forward or a core request
// alike. It expires, so a lost reply (or a node that is gone) costs the queue one window and the
// app's own retry takes over instead of the queue wedging. The window is the one the request kind
// was fitted with (BusGateWindowMs), so a Save All is not cut off by a read's figure.
inline bool AppBusForwardPending()
{
    if (!AppInterface.BusPending)
        return false;
    if (!BusGateHeld(DeviceStatus.UptimeMs, AppInterface.BusPendingSince,
                     AppInterface.BusPendingWindow))
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

// The gate, asked by the ONE function that puts a frame on the wire
// (Devices/Tamu_v2.0A/RSBus.h SendAndVerifyPacket). Returns false when the frame was HELD: the
// caller must not transmit it - AppBusDeferredDrain puts it on the wire once the answer the
// medium is waiting for has landed.
//
// This is where the gate's scope widened from "the app's forwards" to "every answered unicast":
// the core's own requests (a subscription install/renewal, a script's foreign register access, a
// confirm-required value update) leave through the same door, so they take the slot instead of
// sliding onto the wire 3-10 ms after another request whose answer is still owed. The frame that
// opens the slot is the frame that expects an answer (BusGateNeedsAnswer), and the slot is held
// for that request kind's window (BusGateWindowMs).
inline bool AppBusGateOffer(const PacketFrame &Frame)
{
    switch (BusGateVerdictFor(AppBusForwardPending(), AppBusDeferredCount < APP_BUS_DEFER_DEPTH))
    {
    case BusGateVerdict::Defer:
        if (AppBusDefer(Frame))
            return false;
        AppBusGateNoteClash(); // the ring filled after the check: it goes out, and that is counted
        break;
    case BusGateVerdict::SendAndClash:
        AppBusGateNoteClash();
        break;
    case BusGateVerdict::SendNow:
    default:
        break;
    }
    if (BusGateNeedsAnswer(Frame.IdTgt, Frame.Flags))
        AppBusForwardStart(Frame,
                           BusGateWindowMs(Frame.Cmd, AppFrameSource == AppLinkSource::None));
    return true;
}
#endif

// Whether a frame has to leave this core (a unicast to another address). A broadcast needs no
// slot: several devices may answer one, so there is no single reply to wait for.
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
        // Does the medium still owe an answer? Asked outside the lock: it can expire the gate, and
        // it reads the identity of the transaction the bus is holding.
        const bool GateOpen = AppBusForwardPending();
        int Pick;
        portENTER_CRITICAL(&RxMux);
        if (RxCount == 0) // read inside the lock so an enqueue can't corrupt the pop
        {
            portEXIT_CRITICAL(&RxMux);
            break;
        }
        // Which queued frame may go now (Core/Functions/BusGate.h BusGateQueuePick): the head,
        // unless it has to leave the core while the medium still owes an answer - then the first
        // frame behind it that needs no bus is taken instead. In-order dispatch is kept for the
        // frames that share the wire; only a core-local frame overtakes a blocked forward, and
        // those two are separate transactions to different devices (the app keeps up to 8 in
        // flight), so the order between them was never part of the contract.
        bool NeedsBus[APP_RX_QUEUE_DEPTH];
        for (uint8_t i = 0; i < RxCount; i++)
            NeedsBus[i] = AppLinkFrameNeedsBus(RxQueue[(RxTail + i) % APP_RX_QUEUE_DEPTH]);
        Pick = BusGateQueuePick(NeedsBus, RxCount, GateOpen);
        if (Pick < 0)
        {
            portEXIT_CRITICAL(&RxMux);
            break; // every queued frame needs the wire and the wire is owed an answer: not now
        }
        Frame = RxQueue[(RxTail + Pick) % APP_RX_QUEUE_DEPTH];
        From = RxFrom[(RxTail + Pick) % APP_RX_QUEUE_DEPTH];
        // Take it out of the ring: at the head the tail simply advances, and past the head the
        // frames before it move up one slot, which leaves the remaining order untouched.
        for (uint8_t i = (uint8_t)Pick; i > 0; i--)
        {
            RxQueue[(RxTail + i) % APP_RX_QUEUE_DEPTH] = RxQueue[(RxTail + i - 1) % APP_RX_QUEUE_DEPTH];
            RxFrom[(RxTail + i) % APP_RX_QUEUE_DEPTH] = RxFrom[(RxTail + i - 1) % APP_RX_QUEUE_DEPTH];
        }
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

// The link-only half of Pump(), for the one caller that must not dispatch: the bus transmit
// (RS485AppLinkTick). While a send blocks the main task - a silence window, an echo window, a
// retry backoff - the app is mid-transaction and its reply bytes still have to leave, so this does
// the link work and none of the dispatch work. Dispatch is deliberately absent: RouteIn() can
// start a bus transmit of its own, and the send that is running holds TX-enable and the wire.
//
// No AppBusGateDrain/AppBusDeferredDrain either: both exist to put more frames on that wire.
void AppInterfaceService::PumpLink()
{
    Connected = AppUSBActive() || AppBLEActive();
    AppUSBTick();
    TxFlush();
    AppBLETick();
}

#endif // USE_APP_INTERFACE
