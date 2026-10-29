#!/bin/bash
# Source guard for Issues.md E69 - the automatic pairing window follows an EVENT, never a reading
# of the bond store.
#
# AppBLE.h cannot be linked on the host (NimBLE, the host task, NVS), so the DECISION is host-tested
# (test/native/ble_window_test.cpp drives Blocks/BleWindowPolicy.h) and what is asserted here is that
# the radio code goes through it, at the right places and nowhere else. The defect this pins:
#
#   AppBLETick used to open BLE_AUTO_WINDOW_MS as soon as `AppBLEBondCount() == 0`. The vendored
#   stack deletes a bond by itself when a peer it already has a bond for asks to pair again
#   (managed_components/h2zero__esp-nimble-cpp/src/NimBLEServer.cpp BLE_GAP_EVENT_REPEAT_PAIRING ->
#   ble_store_util_delete_peer + RETRY, before any application callback), so a LINK could empty the
#   store and make a locked device hand out a 5-minute window in which any peer that pairs is
#   admitted. The window is now owed to two owner events only - the radio coming up with an empty
#   store (headless first use) and an explicit forget that emptied the store (zeroing `Bonds`, a passkey
#   change, the epoch revoke, all through AppBLEForgetAllBonds) - and the tick consumes the event
#   exactly once, so nothing can open a window "because the store reads empty".
#
# Usage: bash test/native/ble_window_guard.sh src/Devices/Tamu_v2.0A/AppBLE.h
# Exit 0 = the rule holds. Any FAIL line names what broke.
set -u

SRC="${1:-}"
if [ -z "$SRC" ] || [ ! -f "$SRC" ]; then
    echo "FAIL: no source file given (usage: bash $0 <path to AppBLE.h>)" >&2
    exit 1
fi

fail=0
note() { echo "FAIL $SRC: $1"; fail=1; }

# The three functions the rule lives in, and the tick.
tick=$(sed -n '/^void AppBLETick()/,/^}/p' "$SRC")
forget=$(sed -n '/^bool AppBLEForgetAllBonds()/,/^}/p' "$SRC")
radio=$(sed -n '/^static bool AppBLERadioStart()/,/^}/p' "$SRC")
[ -n "$tick" ] || note "AppBLETick() not found - is this AppBLE.h?"
[ -n "$forget" ] || note "AppBLEForgetAllBonds() not found - is this AppBLE.h?"
[ -n "$radio" ] || note "AppBLERadioStart() not found - is this AppBLE.h?"

# The rule header has to be part of the radio code, or the decision below cannot be the tested one.
if ! grep -q '#include "Blocks/BleWindowPolicy.h"' "$SRC"; then
    note "the radio code no longer includes Blocks/BleWindowPolicy.h - the window's rule is not the tested one"
fi

# 1. The tick must not derive a window from the store's count. This is E69 itself.
count=$(grep -c 'if (AppBLEBondCount() == 0)' "$SRC")
if [ "$count" != 0 ]; then
    note "a window (or anything else) still triggers on 'AppBLEBondCount() == 0' - a link-driven repeat-pairing deletion empties the store and would hand out a window (E69)"
fi

# 2. The one automatic-window call in the file is in the tick, behind the tested predicate.
calls=$(grep -c 'AppBLEAutoWindow();' "$SRC")
if [ "$calls" != 1 ]; then
    note "AppBLEAutoWindow() must have exactly one caller (the tick's gated block), found $calls"
fi
gate=$(printf '%s\n' "$tick" | grep -c 'if (BleAutoWindowGranted(BleAutoWindowCause, AppBLEBondCount()))')
if [ "$gate" != 1 ]; then
    note "the tick's automatic window must be gated on BleAutoWindowGranted(BleAutoWindowCause, AppBLEBondCount()) exactly once, found $gate"
fi
gate_line=$(printf '%s\n' "$tick" | grep -n 'BleAutoWindowGranted(BleAutoWindowCause' | head -1 | cut -d: -f1)
call_line=$(printf '%s\n' "$tick" | grep -n 'AppBLEAutoWindow();' | head -1 | cut -d: -f1)
if [ -z "$gate_line" ] || [ -z "$call_line" ] || [ "$gate_line" -ge "$call_line" ]; then
    note "the automatic window must be opened inside the grant, not before it"
fi

# 3. The event is consumed once per pass, in the tick, so a window is opened per event and never per
#    loop pass. Two None assignments in the file: the declaration and this clear.
clears=$(grep -c 'BleAutoWindowCause = BleWindowCauseNone;' "$SRC")
if [ "$clears" != 2 ]; then
    note "the event must be cleared in exactly one place besides its declaration (the tick), found $clears None assignments"
fi
if ! printf '%s\n' "$tick" | grep -q 'BleAutoWindowCause = BleWindowCauseNone;'; then
    note "the tick must consume the event - otherwise it opens a window on every pass"
fi

# 4. Exactly two grant sites, one per owner event, each in the function that owns the event.
if [ "$(grep -c 'BleAutoWindowCause = BleWindowCauseForgotten;' "$SRC")" != 1 ] ||
   ! printf '%s\n' "$forget" | grep -q 'BleAutoWindowCause = BleWindowCauseForgotten;'; then
    note "the explicit forget (AppBLEForgetAllBonds) must be the one and only forgotten-store grant site"
fi
if [ "$(grep -c 'BleAutoWindowCause = BleWindowCauseRadioStart;' "$SRC")" != 1 ] ||
   ! printf '%s\n' "$radio" | grep -q 'BleAutoWindowCause = BleWindowCauseRadioStart;'; then
    note "the radio start must be the one and only first-use grant site"
fi
#    ... and nothing else may set it: declaration + 2 grants + 1 clear = 4 assignments in the file.
sets=$(grep -c 'BleAutoWindowCause = BleWindowCause' "$SRC")
if [ "$sets" != 4 ]; then
    note "the automatic window's cause may only be set at the two owner events and cleared in the tick (found $sets assignments)"
fi

# 5. Both sites decide with the host-tested predicate, and the forget passes the count read back
#    AFTER the delete - a store that still holds an entry has nobody to invite back.
if ! printf '%s\n' "$forget" | grep -q 'BleAutoWindowGranted(BleWindowCauseForgotten, After)'; then
    note "the forget must decide with BleAutoWindowGranted(BleWindowCauseForgotten, After) - the post-delete store"
fi
if ! printf '%s\n' "$radio" | grep -q 'BleAutoWindowGranted(BleWindowCauseRadioStart, Bonds)'; then
    note "the radio start must decide with BleAutoWindowGranted(BleWindowCauseRadioStart, Bonds)"
fi
grant_line=$(printf '%s\n' "$forget" | grep -n 'BleAutoWindowCause = BleWindowCauseForgotten;' | head -1 | cut -d: -f1)
del_line=$(printf '%s\n' "$forget" | grep -n 'NimBLEDevice::deleteAllBonds()' | head -1 | cut -d: -f1)
if [ -z "$grant_line" ] || [ -z "$del_line" ] || [ "$grant_line" -le "$del_line" ]; then
    note "the forgotten-store grant must follow the delete inside AppBLEForgetAllBonds"
fi

# 6. The count-driven episode flag the defect was built on is gone for good.
if [ "$(grep -c 'BleAutoWindowDone' "$SRC")" != 0 ]; then
    note "BleAutoWindowDone is back - the window is following the store's count again"
fi

if [ "$fail" = 0 ]; then
    echo "PASS: the automatic window follows two owner events only, is consumed once by the tick, and nothing triggers on the bond count"
fi
exit "$fail"
