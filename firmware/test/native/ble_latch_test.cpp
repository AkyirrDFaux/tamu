// Host-side test for the single-session latch (Blocks/BleLinkLatch.h).
//
// The defect this pins: the latch was SET where a link arrived and CLEARED only in onDisconnect, and
// every refusal's `disconnect()` answer was thrown away. A teardown that did not produce the callback
// (a dead ACL, ENOTCONN, a lost event) left the device advertising and dropping every peer for the
// life of the boot, with the GPIO8 reset as the only way back.
//
// What is checked here, all on the state machine (the calls into the stack are in AppBLE.h):
//  1. one session at a time, and a second peer does not disturb the first;
//  2. a normal disconnect releases the latch (no heal, no retry);
//  3. a latch whose link is really gone is cleared WITHOUT onDisconnect - and the next connect is
//     admitted, which is the "no reset needed" property;
//  4. a drop the stack refuses is retried, and if it never happens the latch is cleared anyway
//     (advertising must mean connectable) with the event counted;
//  5. a drop of a link that never held the session does not touch the session.
//
// No device dependency: built like align/stride (see run.sh).

#include <cstdio>

#include "Blocks/BleLinkLatch.h"

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                            \
    } while (0)

// One session, and a second peer that is refused at connect time.
static void TestOneSessionAtATime()
{
    BleLinkLatch L;
    CHECK(!BleLatchHeld(L));
    CHECK(BleLatchTake(L, 5));       // first link takes the session
    CHECK(BleLatchHeld(L));
    CHECK(L.Session == 5);
    CHECK(BleLatchTake(L, 5));       // the same link asking again is not a second session
    CHECK(!BleLatchTake(L, 6));      // a second peer is refused
    CHECK(L.Session == 5);           // ... and the live session is untouched
}

// The normal path: onDisconnect arrives, the latch is released, and nothing is counted as a heal or
// a retry (a quiet device must not look damaged).
static void TestNormalDisconnect()
{
    BleLinkLatch L;
    BleLatchTake(L, 5);
    BleLatchReleased(L, 5);
    CHECK(!BleLatchHeld(L));
    CHECK(L.Heals == 0);
    CHECK(L.Retries == 0);
    CHECK(L.Forced == 0);

    // An onDisconnect for some other link does not release this session.
    BleLatchTake(L, 7);
    BleLatchReleased(L, 9);
    CHECK(BleLatchHeld(L));
}

// PROOF (i), the self-heal: the session's link is gone (the stack lists nothing) and onDisconnect
// never came. The tick must clear the latch, and the NEXT connect must be admitted - no GPIO8, no
// reboot. This is the exact shape the rig shows: advertising up, every connect dropped.
static void TestLostDisconnectHealsAndNextConnectWorks()
{
    BleLinkLatch L;
    BleLatchTake(L, 5);

    // The stack's list is empty: the link is really gone.
    const BleLatchAction A = BleLatchTick(L, false, false, 1000);
    CHECK(A.Step == BleLatchStep::Heal);
    CHECK(A.Handle == 5);
    CHECK(!BleLatchHeld(L));
    CHECK(L.Heals == 1);
    CHECK(L.Forced == 0);

    // The recovery the owner actually needs: the next peer is admitted, not refused.
    CHECK(BleLatchTake(L, 6));
    CHECK(L.Session == 6);

    // A second tick with nothing latched does nothing.
    const BleLatchAction Idle = BleLatchTick(L, false, true, 2000);
    CHECK(Idle.Step == BleLatchStep::None);
    CHECK(L.Heals == 1);
}

// A latch whose link the stack still lists is a live session, not a stale one: the supervisor must
// leave it alone.
static void TestLiveSessionIsNotHealed()
{
    BleLinkLatch L;
    BleLatchTake(L, 5);
    for (uint32_t Now = 0; Now < 10000; Now += 500)
    {
        const BleLatchAction A = BleLatchTick(L, false, true, Now);
        CHECK(A.Step == BleLatchStep::None);
    }
    CHECK(BleLatchHeld(L));
    CHECK(L.Heals == 0);
}

// A refused link that will not tear down: the drop is retried BLE_DROP_MAX_ATTEMPTS times, and then
// the latch is cleared anyway - counted as forced - because a device that advertises must be
// connectable. A zombie ACL may hold a connection slot; a dead radio is worse.
static void TestRefusedTeardownIsRetriedThenForced()
{
    BleLinkLatch L;
    BleLatchTake(L, 5);              // the link took the latch and was then refused (not bonded)
    BleLatchDrop(L, 5, 1000);        // the device asked the stack to drop it

    // Inside the grace period nothing is asked again.
    CHECK(BleLatchTick(L, true, true, 1000).Step == BleLatchStep::None);
    CHECK(BleLatchTick(L, true, true, 1000 + BLE_DROP_WAIT_MS - 1).Step == BleLatchStep::None);

    // The grace expires: ask again. Three attempts.
    uint32_t Now = 1000 + BLE_DROP_WAIT_MS;
    for (uint8_t Attempt = 1; Attempt <= BLE_DROP_MAX_ATTEMPTS; Attempt++)
    {
        const BleLatchAction A = BleLatchTick(L, true, true, Now);
        CHECK(A.Step == BleLatchStep::Drop);
        CHECK(A.Handle == 5);
        CHECK(L.Attempts == Attempt);
        CHECK(L.Retries == Attempt);
        Now += BLE_DROP_RETRY_MS;
    }
    CHECK(BleLatchHeld(L)); // still held while it is being asked

    // The fourth expiration is the give-up: the latch is freed, forced, and a peer can connect.
    const BleLatchAction A = BleLatchTick(L, true, true, Now);
    CHECK(A.Step == BleLatchStep::Heal);
    CHECK(A.Handle == 5);
    CHECK(L.Forced == 1);
    CHECK(!BleLatchHeld(L));
    CHECK(BleLatchTake(L, 6));
}

// The good case of a refused drop: the stack does let it go, and the latch is simply released - no
// forced heal, no repeated requests.
static void TestRefusedThenGone()
{
    BleLinkLatch L;
    BleLatchTake(L, 5);
    BleLatchDrop(L, 5, 1000);
    CHECK(L.Drop == 5);

    // Still up at the first check, then gone.
    CHECK(BleLatchTick(L, true, true, 1000).Step == BleLatchStep::None);
    const BleLatchAction A = BleLatchTick(L, false, false, 1000 + BLE_DROP_WAIT_MS);
    CHECK(A.Step == BleLatchStep::None);
    CHECK(!BleLatchHeld(L));
    CHECK(L.Drop == 0xFFFF);
    CHECK(L.Forced == 0);
    CHECK(L.Heals == 0);
}

// onDisconnect for the dropped link also clears the supervisor's record (the two paths must agree).
static void TestReleaseClearsDrop()
{
    BleLinkLatch L;
    BleLatchTake(L, 5);
    BleLatchDrop(L, 5, 1000);
    BleLatchReleased(L, 5);
    CHECK(L.Drop == 0xFFFF);
    CHECK(!BleLatchHeld(L));
    // Nothing left to supervise.
    CHECK(BleLatchTick(L, false, false, 99999).Step == BleLatchStep::None);
}

// A second peer refused while a session is live: dropping IT must never release the session.
static void TestSecondPeerDropLeavesSession()
{
    BleLinkLatch L;
    BleLatchTake(L, 5);
    BleLatchDrop(L, 6, 1000); // the refused second peer's handle

    uint32_t Now = 1000 + BLE_DROP_WAIT_MS;
    for (uint8_t Attempt = 0; Attempt < BLE_DROP_MAX_ATTEMPTS; Attempt++)
    {
        const BleLatchAction A = BleLatchTick(L, true, true, Now);
        CHECK(A.Step == BleLatchStep::Drop);
        CHECK(A.Handle == 6);
        Now += BLE_DROP_RETRY_MS;
    }
    // The refused peer will not die: its record is dropped, the session is NOT.
    const BleLatchAction A = BleLatchTick(L, true, true, Now);
    CHECK(A.Step == BleLatchStep::Heal);
    CHECK(BleLatchHeld(L));
    CHECK(L.Session == 5);
    CHECK(L.Forced == 1);
}

// The radio going down (block disabled, GPIO8 reset) forgets every handle, and the counters stay as
// history.
static void TestClearAll()
{
    BleLinkLatch L;
    BleLatchTake(L, 5);
    BleLatchDrop(L, 6, 1000);
    L.Heals = 2;
    BleLatchClearAll(L);
    CHECK(!BleLatchHeld(L));
    CHECK(L.Drop == 0xFFFF);
    CHECK(L.Attempts == 0);
    CHECK(L.Heals == 2);
}

int main()
{
    TestOneSessionAtATime();
    TestNormalDisconnect();
    TestLostDisconnectHealsAndNextConnectWorks();
    TestLiveSessionIsNotHealed();
    TestRefusedTeardownIsRetriedThenForced();
    TestRefusedThenGone();
    TestReleaseClearsDrop();
    TestSecondPeerDropLeavesSession();
    TestClearAll();

    printf("ble session latch: %d checks passed", checks - failures);
    if (failures != 0)
        printf(", %d FAILED", failures);
    printf("\n");
    return failures == 0 ? 0 : 1;
}
