#pragma once

// The single-session latch, and the teardown that has to confirm it.
//
// The Tamu protocol has one receive stream and one session state, so exactly one BLE link may hold
// the radio (AppBLE.h onConnect refuses a second peer). That "one link" is a latch: it is SET where
// a link arrives and CLEARED where a disconnect is reported - and until now the clear was a
// callback nobody checked. Every refusal path called `PServer->disconnect(...)` and dropped the
// answer, so if the teardown did not produce an onDisconnect (a dead ACL, `ble_gap_terminate`
// answering ENOTCONN, a lost event) the latch stayed set for the life of the boot. The device keeps
// advertising - so the app still sees it and still connects - and drops every peer it admits,
// because the connect gate reads the latch as "a link is already live". The only way back was the
// GPIO8 reset.
//
// So the latch is not a variable any more, it is a small state machine with a supervisor:
//
//   - a drop the device asked for is RECORDED (handle + when it may be asked again), so the tick
//     can tell a teardown that happened from one the stack refused and ask again;
//   - the session handle is checked against the stack's own list of live links, so a latch whose
//     link is really gone is cleared even if the callback never came (the self-heal);
//   - a link the stack will not terminate (all retries refused) does NOT keep the latch: the latch
//     is cleared anyway and the event is counted, because a device that advertises must be
//     connectable - that is the invariant this whole file exists for. A zombie ACL may still hold
//     a connection slot until the controller's supervision timeout drops it; the controller allows
//     three, and a slot is a far smaller loss than a radio that answers nobody.
//
// Host-clean (no NimBLE, no logging): the decisions are here, the calls into the stack are in
// AppBLE.h, and test/native/ble_latch_test.cpp drives the table below.

#include <cstdint>

// How long a requested drop may take before the device asks the stack again. `ble_gap_terminate`
// is answered by the controller within a connection event or two, so a second is already generous.
#define BLE_DROP_WAIT_MS 1500
// ... and how long to wait between retries.
#define BLE_DROP_RETRY_MS 1000
// Retries before the link is treated as one the stack will not terminate.
#define BLE_DROP_MAX_ATTEMPTS 3

struct BleLinkLatch
{
    uint16_t Session  = 0xFFFF; // the link that holds the single session (0xFFFF = free)
    uint16_t Drop     = 0xFFFF; // a link the device asked the stack to drop, not yet confirmed gone
    uint16_t Attempts = 0;      // drop requests made for `Drop`
    uint32_t DueMs    = 0;      // when `Drop` may be asked again
    uint32_t Retries  = 0;      // drops asked for more than once (the stack did not take the first)
    uint32_t Heals    = 0;      // latches cleared because the link was gone and onDisconnect never came
    uint32_t Forced   = 0;      // latches cleared while the stack still listed the link
};

enum class BleLatchStep : uint8_t
{
    None = 0,
    Drop = 1, // ask the stack to drop `Handle` again
    Heal = 2, // the session is over: the caller clears the session state it owns
};

struct BleLatchAction
{
    BleLatchStep Step   = BleLatchStep::None;
    uint16_t     Handle = 0xFFFF;
};

// A link arrived. True when it holds the session: either the latch was free (this link takes it) or
// this is the link that already has it.
inline bool BleLatchTake(BleLinkLatch &L, uint16_t Handle)
{
    if (L.Session == 0xFFFF)
    {
        L.Session = Handle;
        return true;
    }
    return L.Session == Handle;
}

// Did a link arrive that cannot hold the session?
inline bool BleLatchHeld(const BleLinkLatch &L)
{
    return L.Session != 0xFFFF;
}

// The device has decided `Handle` must go. Recorded even when that link never held the session (a
// refused peer), because the stack's answer to the drop is what the supervisor checks.
inline void BleLatchDrop(BleLinkLatch &L, uint16_t Handle, uint32_t NowMs)
{
    if (Handle == 0xFFFF)
        return;
    L.Drop     = Handle;
    L.Attempts = 0;
    L.DueMs    = NowMs + BLE_DROP_WAIT_MS;
}

// onDisconnect arrived for `Handle`: the normal, confirmed end of a link.
inline void BleLatchReleased(BleLinkLatch &L, uint16_t Handle)
{
    if (L.Session == Handle)
        L.Session = 0xFFFF;
    if (L.Drop == Handle)
    {
        L.Drop     = 0xFFFF;
        L.Attempts = 0;
        L.DueMs    = 0;
    }
}

// The radio was taken down (the block switched it off, the GPIO8 reset, the stack being rebuilt):
// every handle the old stack knew is meaningless.
inline void BleLatchClearAll(BleLinkLatch &L)
{
    L.Session  = 0xFFFF;
    L.Drop     = 0xFFFF;
    L.Attempts = 0;
    L.DueMs    = 0;
}

// One supervision pass. `DropListed` / `SessionListed` say whether the STACK still has a live
// connection for those handles - the caller reads them from the server, never from its own flags,
// which is the whole point: the latch is checked against the stack instead of against itself.
inline BleLatchAction BleLatchTick(BleLinkLatch &L, bool DropListed, bool SessionListed, uint32_t NowMs)
{
    BleLatchAction A;

    if (L.Drop != 0xFFFF)
    {
        if (!DropListed)
        {
            // Confirmed gone. A normal onDisconnect may already have cleared both; this is the
            // case where it did not.
            //
            // When the dropped link held the session this is a SESSION END, and it has to be
            // reported as one: the caller's session state (`BleConnected`/`BleSecured`, the RX
            // assembler, the MTU, the TX ring) is torn down only for a `Heal`, so clearing the
            // latch here and returning None left the device advertising-under-a-live-session -
            // `BleAdvWanted() && !BleConnected` never restarts the advertisement (the phone
            // cannot find the device) and `AppBLEActive()` still reports a session, so the pump
            // keeps filling a ring nobody drains. That is the stuck state this file exists to
            // remove, one level up. Counted as a heal for the same reason.
            A.Handle = L.Drop;
            A.Step   = BleLatchStep::Heal;
            if (L.Session == L.Drop)
            {
                L.Session = 0xFFFF;
                L.Heals   = L.Heals + 1;
            }
            L.Drop     = 0xFFFF;
            L.Attempts = 0;
            L.DueMs    = 0;
            return A;
        }
        if ((int32_t)(NowMs - L.DueMs) < 0)
            return A; // still inside its grace period

        if (L.Attempts < BLE_DROP_MAX_ATTEMPTS)
        {
            L.Attempts = (uint16_t)(L.Attempts + 1);
            L.DueMs    = NowMs + BLE_DROP_RETRY_MS;
            L.Retries  = L.Retries + 1;
            A.Step     = BleLatchStep::Drop;
            A.Handle   = L.Drop;
            return A;
        }

        // The stack will not terminate it. Clear the latch anyway - advertising must mean
        // connectable - and leave the count behind so the log can say this happened.
        A.Handle = L.Drop;
        A.Step   = BleLatchStep::Heal;
        if (L.Session == L.Drop)
            L.Session = 0xFFFF;
        L.Forced = L.Forced + 1;
        L.Drop     = 0xFFFF;
        L.Attempts = 0;
        L.DueMs    = 0;
        return A;
    }

    if (L.Session != 0xFFFF && !SessionListed)
    {
        // The link is gone and onDisconnect never came: the latch would otherwise be permanent.
        A.Handle  = L.Session;
        A.Step    = BleLatchStep::Heal;
        L.Session = 0xFFFF;
        L.Heals   = L.Heals + 1;
        return A;
    }

    return A;
}
