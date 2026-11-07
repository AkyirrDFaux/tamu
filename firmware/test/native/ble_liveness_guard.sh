#!/bin/bash
# Source guard for the latch's liveness read (card t_ab6c7bdc, item 4) and for the session teardown
# its heal has to reach (item 1).
#
# AppBLE.h cannot be linked on the host (NimBLE, the host task, NVS), so the DECISION is host-tested
# (test/native/ble_latch_test.cpp drives Blocks/BleLinkLatch.h) and what is asserted here is the
# wiring the decision depends on. The two defects this pins, both from the second-pass review of
# df3680d:
#
#   (4) Liveness was read through `NimBLEServer::getPeerDevices()`, which builds a fresh std::vector
#       from the server's fixed array on every call
#       (managed_components/h2zero__esp-nimble-cpp/src/NimBLEServer.cpp:388). That is a heap
#       allocation per check, asked from the app task every BLE_LATCH_CHECK_MS and from onConnect in
#       the host task, and an allocation that failed read as "not listed" - i.e. a false Heal: a
#       healthy encrypted session torn down, advertising restarted, a second phone admitted into the
#       freed latch, two peers into one RX stream. The read must be the host's own connection table
#       (ble_gap_conn_find, one descriptor copy, "not found" is an answer) and never a snapshot.
#
#   (1) The supervisor's heal returns AFTER BleLatchTick has cleared the latch, so the session
#       teardown only runs if the session-ness captured before the tick is passed to it. Deriving it
#       from the latch inside BleSessionEnded answered "no" on every heal and the teardown did
#       nothing: the session flags stayed set, advertising never restarted and the pump kept filling
#       a ring nobody drains - the stuck state the latch exists to remove, one level up.
#
# Usage: bash test/native/ble_liveness_guard.sh src/Devices/Tamu_v2.0A/AppBLE.h
# Exit 0 = the rule holds. Any FAIL line names what broke.
set -u

SRC="${1:-}"
if [ -z "$SRC" ] || [ ! -f "$SRC" ]; then
    echo "FAIL: no source file given (usage: bash $0 <path to AppBLE.h>)" >&2
    exit 1
fi

fail=0
note() { echo "FAIL $SRC: $1"; fail=1; }

supervise=$(sed -n '/^static void BleLatchSupervise()/,/^}/p' "$SRC")
ended=$(sed -n '/^static void BleSessionEnded(uint16_t Handle, bool WasSession)/,/^}/p' "$SRC")
live=$(sed -n '/^static bool BleHandleLive(uint16_t Handle)/,/^}/p' "$SRC")
[ -n "$supervise" ] || note "BleLatchSupervise() not found - is this AppBLE.h?"
[ -n "$ended" ]     || note "BleSessionEnded(Handle, WasSession) not found"
[ -n "$live" ]      || note "BleHandleLive() not found - the liveness read has no single owner"

# --- (4) the liveness read: no snapshot, no allocation ---------------------------------------------
# Anchored to lines that do not start a comment: the header may (and does) name the accessor it
# replaced, with the reason.
if grep -qE '^[^/]*getPeerDevices\(' "$SRC"; then
    note "getPeerDevices() is back - it allocates a std::vector per check, and a failed allocation reads as a gone link (a false Heal)"
fi
if grep -qE 'std::vector *< *uint16_t *>' "$SRC"; then
    note "a std::vector of handles is back - the liveness check must not carry a heap snapshot"
fi
if [ "$(grep -c 'ble_gap_conn_find(' "$SRC")" != 1 ]; then
    note "the liveness read must be ble_gap_conn_find() in exactly one place (the host's connection table), found $(grep -c 'ble_gap_conn_find(' "$SRC")"
fi
if ! printf '%s\n' "$live" | grep -q 'return ble_gap_conn_find(Handle, &Desc) == 0;'; then
    note "BleHandleLive must answer with the stack's own table: ble_gap_conn_find(Handle, &Desc) == 0"
fi
if ! printf '%s\n' "$live" | grep -q 'if (Handle == 0xFFFF'; then
    note "BleHandleLive must treat 0xFFFF (no handle) as not live without asking the stack"
fi
# Both questions the supervisor asks come from that one read - not from a snapshot passed around.
if [ "$(printf '%s\n' "$supervise" | grep -c 'BleHandleLive(')" -lt 2 ]; then
    note "BleLatchSupervise must ask BleHandleLive() for both handles (it no longer takes a snapshot)"
fi
if ! grep -q 'BleLatchHeld(StaticBleLatch) && !BleHandleLive(StaticBleLatch.Session)' "$SRC"; then
    note "the onConnect stale-latch check must use BleHandleLive() too (one liveness read, one meaning)"
fi

# --- (1) the heal must reach the session teardown with the session-ness the tick already cleared ----
if ! printf '%s\n' "$supervise" | grep -q 'const bool WasSession = (A.Handle == SessionBefore);'; then
    note "BleLatchSupervise must capture the session-ness BEFORE the tick (the tick clears the latch)"
fi
if ! printf '%s\n' "$supervise" | grep -q 'BleSessionEnded(A.Handle, WasSession);'; then
    note "the supervisor's heal must pass WasSession to BleSessionEnded - re-deriving it after the tick always answers 'no' and the session flags stay set"
fi
if [ "$(grep -c 'const bool WasSession = (StaticBleLatch.Session == Handle);' "$SRC")" != 0 ]; then
    note "BleSessionEnded re-derives WasSession from the latch again - that is the no-op teardown"
fi
if ! printf '%s\n' "$ended" | grep -q 'if (!WasSession)'; then
    note "BleSessionEnded must still skip the session state for a link that never held it"
fi
if ! printf '%s\n' "$ended" | grep -q 'BleConnected = false;'; then
    note "BleSessionEnded must clear BleConnected (advertising restarts on it)"
fi

if [ "$fail" = 0 ]; then
    echo "PASS: liveness is one allocation-free stack read, and the heal reaches the session teardown with the session-ness captured before the tick"
fi
exit "$fail"
