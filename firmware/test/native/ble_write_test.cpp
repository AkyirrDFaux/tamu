// Host-side test for who may write the `BLE Settings` block (Blocks/BleWritePolicy.h).
//
// The block was USB-only because a phone could lock the owner out of the radio it was reaching the
// device through. The GPIO8 reset is that way back, so the rule became the LINK rather than the cable
// (owner ruling, 2026-11-05):
//
//   USB frame                     -> writable (the cable IS the owner's access)
//   BLE frame on a secured link   -> writable (encrypted, bonded, authenticated when a passkey is set)
//   BLE frame on an unsecured one -> refused  (an unpaired peer still cannot rewrite passkey/window/name)
//   bus frame / local write       -> refused  (not from either link)
//
// The three inputs are the frame's own transport (Core/Functions/AppInterface.h) and the session flag
// (AppBLELinkSecured), so the whole table is a decision the device can be driven through.

#include <cstdio>

#include "Blocks/BleWritePolicy.h"

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                            \
    } while (0)

int main()
{
    printf("### BLE Settings write rule: the cable, or a secured BLE session\n");

    // USB: always allowed, whatever the radio is doing (the radio may be off altogether).
    CHECK(BleWriteAllowed(/*OverUsb=*/true, /*OverBle=*/false, /*Secured=*/false));
    CHECK(BleWriteAllowed(true, false, true));

    // BLE: only on the secured session the radio admitted.
    CHECK(BleWriteAllowed(false, true, true));
    CHECK(!BleWriteAllowed(false, true, false));

    // Neither link (a bus frame, a script's own write): refused, and a stray secured flag cannot
    // make a non-BLE frame writable.
    CHECK(!BleWriteAllowed(false, false, false));
    CHECK(!BleWriteAllowed(false, false, true));

    // The predicate is not "not USB": a BLE frame whose link is not secured is refused even though
    // it is not the cable - the case that used to be the whole rule.
    CHECK(!BleWriteAllowed(false, true, false));

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
