#pragma once

// The bus transaction gate's DECISIONS, in one place that can be compiled and driven on the host
// (firmware/test/native/bus_gate_test.cpp). The device half - the transaction slot itself, the
// deferral ring and the diagnostics ring - lives in Core/Functions/AppInterface.h.
//
// The RSBus is one half-duplex medium: while a unicast that expects an answer is on the wire,
// nothing else may be. SendAndVerifyPacket only proves that OUR bytes went out intact (it
// verifies the transceiver echo); it never waits for the target's answer. So the gate is what
// keeps the next frame off the wire until the answer to the previous one has arrived - and it is
// asked at that one place (Devices/Tamu_v2.0A/RSBus.h), which is what extends it to every
// answered unicast: the app's forwards AND the core's own requests (a subscription
// install/renewal, a script's foreign register access, a confirm-required value update).
//
// Two of the decisions below are not obvious and are each the fix for a measured defect:
//   - BusGateNeedsAnswer takes the Cmd as well as the flags: a `Subscriptions` Change (040x/1) is
//     answered although it carries no REQACK (BUS_GATE_SUB_CHANGE_CID);
//   - BusGateIsAnswer matches the Cmd as well as the TRID: a provider's value update shares the
//     subscription TRID with the Change's answer, so the TRID alone would close the slot early.

#include <cstdint>
#include "Core/Functions/Packet.h"

// --- windows, per request kind ---------------------------------------------------------------
//
// The window bounds a LOST reply, not a healthy one: the gate is released by the answer itself
// (AppInterface.h AppBusForwardSawReply) and the expiry is the exceptional path. Each kind is
// fitted to the slowest legitimate answer measured for THAT kind, with margin.
//
//   BUS_GATE_WINDOW_MS (600) - an ordinary forwarded app read/write. Measured 13-14 ms per
//     request on the rig (.pio/probe/gate_latency.py, 2026-11-06), and 171 ms was the longest
//     ANSWERED forward in a whole restore (restore_measure4.gate.log), so 600 ms is 3.5x that.
//   BUS_GATE_WINDOW_CORE_MS (250) - a core-origin request (a subscription install/renewal, a
//     script's foreign register access). Same 13-14 ms answer, but no app is waiting on it: a
//     poll to a node that is gone must not hold the app's queue for the app frame's window. A
//     core answer that arrives outside this shows up in the totals line as LATE, which is the
//     measurement that would widen it.
//   BUS_GATE_WINDOW_STORE_MS (3000) - `Register` CID 5 (Save All), the one request kind that
//     legitimately runs long: 125-184 ms for a healthy node, and the reclaim path
//     (Core/Functions/MemoryBackup.h WriteBackupFile: a file-table compaction and re-stage, or
//     the in-place rewrite) has no measurement of its own. The app itself budgets 25 s for one
//     Save All (app/lib/core/backup_restore.dart restoreSavePolicy), so 3 s still leaves the node
//     the app's whole room before the app gives up, while a lost reply costs the queue 3 s
//     instead of 25.
#define BUS_GATE_WINDOW_MS 600
#define BUS_GATE_WINDOW_CORE_MS 250
#define BUS_GATE_WINDOW_STORE_MS 3000

// `Register` service CID 5 is Save All (Core/Services/RegisterDefs.h RegisterCid::SaveAll). The
// number is repeated here on purpose: this header stays host-clean, so it cannot include the
// service headers (they pull the storage layer in).
#define BUS_GATE_SAVE_ALL_CID 5

// `Subscriptions` service CID 1 is Change subscription (Core/Services/SubscriptionsDefs.h
// SubCid::Change) - the requester's install, renewal and cancel (040x/1). Repeated here for the
// same reason as the CID above.
//
// It is the ONE request in the tree that is answered without carrying FLAG_REQACK, so a flag-only
// transaction test misses it - the card's evidence item 1. Every construction of it is
// `FLAG_START | FLAG_STOP`:
//   Core/Services/SubscriptionsPersist.h  RegisterRequesterProvider (install and renewal)
//   Core/Services/SubscriptionsControl.h  SubscriptionsCancelProvider  (cancel)
//   Core/Services/SubscriptionsRequester.h HandleRequesterValueUpdate (orphan cancel)
// and every one of them is answered: the provider's Change handler answers with the source's
// current value, or with FLAG_SUCCESS for a cancel (SubscriptionsControl.h SubCid::Change, Docs
// Services/Subscriptions.md "Inter-device Commands": request = subscription table, response =
// current value; "Renewal": "extends its timeout and answers success as usual"). Docs/RSBus and
// Packets.md defines REQACK as "request a response", so the frame is a transaction whether or not
// the sender set the bit. Classified by CID here rather than by adding the flag to the wire: the
// gate is the only reader of it, and a node's Change handler does not look at flags at all.
#define BUS_GATE_SUB_CHANGE_CID 1

// Whether this frame puts a transaction on the wire, i.e. whether an answer is owed to it.
//
// Every app request carries REQACK (app/lib/core/protocol.dart PacketFrame.single), and so
// does every core-origin request that wants a reply: a script's foreign register access, a
// subscription value update that requests a confirmation, a time sync. Two shapes are NOT
// transactions:
//   - a status reply (SUCCESS/FAIL) is an answer, never a request, even if REQACK was echoed
//     into it - nothing will come back for it;
//   - a broadcast can be answered by several devices, so there is no single answer to wait for,
//     and one device's answer closing a gate another device is still owed would be a false
//     release that starts the next frame on top of that answer.
// And one shape IS a transaction without the bit: a unicast `Subscriptions` Change (040x/1), see
// BUS_GATE_SUB_CHANGE_CID. `Cmd` is what decides that, so it is part of the test.
inline bool BusGateNeedsAnswer(uint16_t IdTgt, uint8_t Flags, uint16_t Cmd)
{
    if (Flags & (FLAG_SUCCESS | FLAG_FAIL)) return false;
    if (IdTgt == ADDR_BROADCAST) return false;
    if (GetServiceType(Cmd) == ServiceType::Subscriptions &&
        GetServiceCID(Cmd) == BUS_GATE_SUB_CHANGE_CID)
        return true;
    return (Flags & FLAG_REQACK) != 0;
}

// Is a frame that arrived from another device the ANSWER to the open transaction?
//
// An answer echoes the request's TRID *and its CMD* (Core/Functions/Packet.h FinalizeReply -
// "the request's CMD names the service"; Dispatcher.h routes by it; every reply builder in the
// tree echoes both: SendResponse, SubReply, FinalizeReply, the Device service, the repeat replay).
//
// The TRID alone is not enough. A subscription TRID is shared by every frame about that
// subscription, and the one that matters here is the provider's VALUE UPDATE (CID 0): it is a
// FLAG_TYPE frame, it carries the subscription's TRID, and it can arrive between a Change and the
// Change's answer - closing on it would let the next frame go out while the Change is still owed
// its answer, which is exactly the collision the gate exists to prevent. The Change's answer comes
// back with Cmd = Change (SubReply echoes the request's Cmd), so the CMD is what separates them;
// the value update's confirmation comes back with Cmd = ValueUpdate, the Cmd of the update it
// answers, so the confirmation still closes its own slot.
inline bool BusGateIsAnswer(uint16_t OpenTrid, uint16_t OpenCmd, uint16_t Trid, uint16_t Cmd)
{
    return Trid == OpenTrid && Cmd == OpenCmd;
}

// The window a request kind keeps the medium (see the table above). `CoreOrigin` is a frame the
// core generated itself, not one it is relaying for the app.
inline uint16_t BusGateWindowMs(uint16_t Cmd, bool CoreOrigin)
{
    if (GetServiceType(Cmd) == ServiceType::Register && GetServiceCID(Cmd) == BUS_GATE_SAVE_ALL_CID)
        return BUS_GATE_WINDOW_STORE_MS;
    return CoreOrigin ? BUS_GATE_WINDOW_CORE_MS : BUS_GATE_WINDOW_MS;
}

// Is the transaction that started at `OpenedAt` still owed an answer at `Now`? The uptime counter
// is free-running and wraps, so this is a subtraction, never a comparison of the two values.
inline bool BusGateHeld(uint32_t Now, uint32_t OpenedAt, uint16_t Window)
{
    return (uint32_t)(Now - OpenedAt) < (uint32_t)Window;
}

// What a frame that is about to go on the wire does while another transaction is owed an answer.
enum class BusGateVerdict : uint8_t
{
    SendNow = 0,     // the medium is free
    Defer = 1,       // held until the answer arrives (AppBusDeferredDrain sends it then)
    SendAndClash = 2 // no room to hold it: it goes out, and the open gate records the collision
};

// ---------------------------------------------------------------- one rule, whatever the frame is
//
// While an answer is owed, EVERY frame is held - a request, a value update, a log record, a
// broadcast. The gate cannot tell when the node's answer starts, so there is no idle moment in the
// window to transmit into: the frame that "expects no answer" would be transmitted on top of one.
// Being answer-less is therefore not a reason to go now; it only decides whether the frame OPENS a
// slot of its own (BusGateNeedsAnswer) and whether the drain may emit several of them in a row.
//
// The cost is a delay: an answer-less frame can wait a Save All's 3000 ms window. That delay is
// counted where it is paid (AppBusDeferredNoAnswer, `noreply=` in the BUSGATE totals line) so it is
// visible rather than silent - see Dispatcher.h AppBusDeferredDrain for the other half of the rule.
inline BusGateVerdict BusGateVerdictFor(bool GateOpen, bool DeferRoom)
{
    if (!GateOpen) return BusGateVerdict::SendNow;
    return DeferRoom ? BusGateVerdict::Defer : BusGateVerdict::SendAndClash;
}

// Which queued inbound frame the app pump dispatches next (its offset from the queue's head), or
// -1 when none of them may go now.
//
// In order, except for one case: the head is a frame that has to leave the core while a bus
// transaction is still owed an answer, so the head waits - and the first frame behind it that
// needs no bus (`NeedsBus[i] == false`, i.e. one addressed to the core itself) is dispatched
// instead. Before this, the pump left the whole loop on a blocked head, so a core-local frame
// queued behind a forward waited out the full window and the app read that as a stall.
//
// Ordering, since in-order dispatch is why the old code gave up instead of scanning:
//   - bus frames keep their order among themselves: the scan only ever jumps past the head to
//     frames that need NO bus, and the head stays first among the frames that do;
//   - a core-local frame overtaking an earlier foreign one is not an ordering the app can
//     observe: the two go to different devices, they are separate transactions with separate
//     TRIDs, and the app already keeps up to 8 of them in flight at once
//     (app/lib/core/connection.dart), so it never relied on the core serialising them.
inline int BusGateQueuePick(const bool *NeedsBus, uint8_t Count, bool GateOpen)
{
    if (Count == 0) return -1;
    if (!GateOpen || !NeedsBus[0]) return 0;
    for (uint8_t i = 1; i < Count; i++)
        if (!NeedsBus[i]) return (int)i;
    return -1; // every queued frame waits for the wire: the gate's answer decides
}
