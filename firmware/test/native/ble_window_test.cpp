// Host-side test for the automatic pairing window's provenance (Blocks/BleWindowPolicy.h).
//
// Issues.md E69 is the reason this file exists. The device used to open its 5-minute automatic
// window whenever the bond store READ empty, and the vendored stack empties that store by itself
// when a peer it already has a bond for asks to pair again (`BLE_GAP_EVENT_REPEAT_PAIRING` ->
// `ble_store_util_delete_peer` + `RETRY`, before any application callback). A peer that could
// present a bonded identity could therefore make a locked device offer itself to the room.
//
// The rule the window now follows is an EVENT, not a reading:
//   - the radio came up with an empty store (headless first use) -> window;
//   - an explicit forget emptied the store (`Action = 1`, a passkey change, the epoch revoke)
//     -> window, because every paired phone has to pair again;
//   - no event, whatever the store reads -> nothing. That is the E69 case: a link-driven deletion
//     leaves the store empty and must still open no window.
// A store that already holds a bond grants nothing in either event - there is nobody to invite
// back, and a window over a stored bond would be a pairing state nobody asked for.
//
// AppBLE.h cannot be linked on the host (NimBLE, the host task, NVS), so the decision lives in its
// own header and is driven here; AppBLE.h is asserted to go through it by the source guard in
// run.sh (test/native/ble_window_guard.sh).

#include <cstdio>

#include "Blocks/BleWindowPolicy.h"

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                            \
    } while (0)

// The two events, spelled out as the owner's actions rather than as magic numbers.
static const int EmptyStore = 0;
static const int OneBond = 1;
static const int ThreeBonds = 3;

static void TestExplicitEventsGrant()
{
    // Headless first use: a radio that comes up with nothing stored has to be pairable.
    CHECK(BleAutoWindowGranted(BleWindowCauseRadioStart, EmptyStore));
    // An explicit forget empties the store, so the forgotten phones have a way back in.
    CHECK(BleAutoWindowGranted(BleWindowCauseForgotten, EmptyStore));
}

static void TestReadingEmptyIsNotAnEvent()
{
    // E69, the regression this header exists for: the store reads empty and nothing happened.
    // A repeat-pairing deletion, a stranger's bond being forgotten by the session gate, or simply
    // boot state - none of them may open the window.
    CHECK(!BleAutoWindowGranted(BleWindowCauseNone, EmptyStore));
    // ... and no loop pass can turn the reading into an event: None is the resting value.
    CHECK(BleWindowCauseNone == 0);
}

static void TestStoredBondGrantsNothing()
{
    // Nothing to recover, whoever asked.
    CHECK(!BleAutoWindowGranted(BleWindowCauseRadioStart, OneBond));
    CHECK(!BleAutoWindowGranted(BleWindowCauseForgotten, OneBond));
    CHECK(!BleAutoWindowGranted(BleWindowCauseNone, OneBond));
    // A full store is the same answer.
    CHECK(!BleAutoWindowGranted(BleWindowCauseForgotten, ThreeBonds));
    CHECK(!BleAutoWindowGranted(BleWindowCauseRadioStart, ThreeBonds));
}

static void TestEveryGrantNeedsAnEmptyStore()
{
    // The whole table, so adding a cause cannot silently grant over a stored bond: for each of the
    // causes the enum defines, a grant is possible only with an empty store.
    const BleWindowCause Causes[] = {BleWindowCauseNone, BleWindowCauseRadioStart, BleWindowCauseForgotten};
    for (BleWindowCause Cause : Causes)
    {
        if (BleAutoWindowGranted(Cause, OneBond))
        {
            printf("FAIL cause %u granted over a stored bond\n", (unsigned)Cause);
            failures++;
        }
        checks++;
    }
    // Exactly two of the three do grant over an empty store - the third is the reading.
    int Granting = 0;
    for (BleWindowCause Cause : Causes)
        Granting += BleAutoWindowGranted(Cause, EmptyStore) ? 1 : 0;
    CHECK(Granting == 2);
}

int main()
{
    TestExplicitEventsGrant();
    TestReadingEmptyIsNotAnEvent();
    TestStoredBondGrantsNothing();
    TestEveryGrantNeedsAnEmptyStore();

    printf("ble window: %d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
