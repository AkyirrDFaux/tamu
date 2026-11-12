#!/bin/bash
# Source guard for the app-forwarded write retry (card t_82c3dc59).
#
# Measured on the rig: a single-shot `Register` CID 3 write to a DAS is answered in 12-15 ms
# (p90 14-15 ms, 143-141 attempts), yet 5 of 143 and 7 of 141 of them were answered by NOTHING for
# 3.02-3.05 s and the field still held its old value afterwards - the request never ran on the node.
# The app cannot retry a write by itself ("a re-sent write is not a safe thing to guess at",
# app/lib/core/register_client.dart), so the relay does it, for the one kind whose execution is a
# set-to-value: `Register` CID 3. A duplicate execution writes the same value again.
#
# The retry lives in the device layer (AppInterface.h) and cannot be linked on the host, so its
# invariants are pinned here. Each check names the defect it prevents:
#
#   (1) the state exists once: armed (this transaction may be retried), due (its window closed
#       unanswered), retrying (the frame on the wire IS the retry), total (what the dump reports);
#   (2) arming happens in ONE place, AppBusForwardStart, the door every forwarded frame goes
#       through - arming anywhere else would miss frames or arm on a reply;
#   (3) arming is gated on an APP frame (not the core's own traffic, which has nobody waiting and
#       its own retry at the transmit layer), on `Register` CID 3 only (the one idempotent kind),
#       and NOT on a frame that is itself the retry (one retry per transaction, never a loop);
#   (4) the retry frame is a COPY of the forwarded frame - the payload is inline in PacketFrame, so
#       the copy stays valid when the app's own buffer is long gone;
#   (5) its TRID is the app's TRID: the app is still waiting on it, so the answer must match that
#       TRID. A fresh TRID would answer a transaction the app never asked about;
#   (6) a reply disarms it - a transaction that was answered is never retried;
#   (7) an expired window converts armed -> due, once, and only while nothing is pending: the
#       medium is then free, which is the point of waiting for the expiry;
#   (8) `retries=` is printed in the BUSGATE totals line, fed by AppBusRetryTotal, so a bench run
#       can tell a clean restore from one that was silently repaired.
#
# Usage: bash test/native/bus_retry_guard.sh <AppInterface.h>
# Exit 0 = the rule holds. Any FAIL line names what broke.
set -u

IFACE="${1:-}"
if [ -z "$IFACE" ] || [ ! -f "$IFACE" ]; then
    echo "FAIL: missing source file (usage: bash $0 <AppInterface.h>)" >&2
    exit 1
fi

fail=0
note() { echo "FAIL $1: $2"; fail=1; }
count() { grep -cF "$1" "$IFACE"; }

# --- (1) the state exists, once ------------------------------------------------------------------
for def in 'PacketFrame AppBusRetryFrame;' 'bool AppBusRetryArmed = false;' \
           'bool AppBusRetryDue = false;' 'bool AppBusRetrying = false;' \
           'uint8_t AppBusRetryLeft = 0;' 'uint32_t AppBusRetryTotal = 0;'; do
    if [ "$(count "$def")" != 1 ]; then
        note "$IFACE" "the retry state must be defined exactly once ($def)"
    fi
done
if [ "$(count '#define APP_BUS_WRITE_RETRIES 2')" != 1 ]; then
    note "$IFACE" "the retry budget must be one constant (APP_BUS_WRITE_RETRIES = 2: measured loss up to 13 % of value-changing writes, one re-send would leave ~1.7 % unrepaired)"
fi

# --- (2) armed at the forward door, re-armed only by the pump after a retry ----------------------
arm_sites=$(grep -c 'AppBusRetryArmed = true;' "$IFACE")
if [ "$arm_sites" != 2 ]; then
    note "$IFACE" "the retry must be armed at exactly two sites - the forward door and the pump's re-arm (found $arm_sites)"
fi
start_body=$(sed -n '/^inline void AppBusForwardStart/,/^}/p' "$IFACE")
if [ -z "$start_body" ]; then
    note "$IFACE" "AppBusForwardStart not found - the retry has no forward door to arm from"
elif ! printf '%s\n' "$start_body" | grep -q 'AppBusRetryArmed = true;'; then
    note "$IFACE" "arming must happen in AppBusForwardStart, the door every forwarded frame goes through"
elif ! printf '%s\n' "$start_body" | grep -q 'AppBusRetryLeft = APP_BUS_WRITE_RETRIES;'; then
    note "$IFACE" "a newly forwarded write must reset the retry budget (AppBusRetryLeft = APP_BUS_WRITE_RETRIES)"
fi

# --- (3) gated on an app-forwarded `Register` CID 3 write that is not itself the retry ------------
ARM_GATE='if (!CoreOrigin && !AppBusRetrying &&'
if ! grep -qF "$ARM_GATE" "$IFACE"; then
    note "$IFACE" "arming must be gated on '$ARM_GATE' (the core's own traffic, and the retry itself, must not arm)"
fi
if ! grep -q 'GetServiceType(Frame.Cmd) == ServiceType::Register && GetServiceCID(Frame.Cmd) == 3' "$IFACE"; then
    note "$IFACE" "only a Register CID 3 write may be retried - it is the one kind whose execution is a set-to-value"
fi

# --- (4) the retry frame is a copy ----------------------------------------------------------------
if [ "$(count 'AppBusRetryFrame = Frame;')" != 1 ]; then
    note "$IFACE" "the retry must keep a COPY of the frame (its payload is inline, so the copy outlives the app's buffer)"
fi

# --- (5) the app's TRID, never a fresh one --------------------------------------------------------
if [ "$(grep -cE 'AppBusRetryFrame\.(Trid|Cmd|IdTgt)[[:space:]]*=' "$IFACE")" != 0 ]; then
    note "$IFACE" "the retry must not edit the stored frame (a new TRID would answer a transaction the app never asked about)"
fi

# --- (6) a reply disarms it ------------------------------------------------------------------------
if ! grep -q 'AppBusRetryArmed = false; // answered: nothing to retry' "$IFACE"; then
    note "$IFACE" "an answer must disarm the retry (AppBusRetryArmed = false beside AppBusGateClose(ABG_REPLY))"
fi

# --- (7) one retry, from the expiry, only when nothing is pending ----------------------------------
pending_body=$(sed -n '/^inline bool AppBusForwardPending/,/^}/p' "$IFACE")
if [ -z "$pending_body" ]; then
    note "$IFACE" "AppBusForwardPending not found - the expiry has no place to arm the retry from"
else
    if ! printf '%s\n' "$pending_body" | grep -q 'AppBusRetryDue = true;'; then
        note "$IFACE" "an expired window must convert an armed write into a due retry"
    fi
    if ! printf '%s\n' "$pending_body" | grep -q 'AppBusRetryArmed = false;'; then
        note "$IFACE" "the expiry must clear the armed flag as it becomes due - otherwise one loss arms twice"
    fi
fi
if ! grep -q 'if (AppBusRetryDue && !AppBusForwardPending())' "$IFACE"; then
    note "$IFACE" "the retry must wait for the medium ('AppBusRetryDue && !AppBusForwardPending()')"
fi
if ! grep -q 'AppBusRetrying = true;' "$IFACE" || ! grep -q 'AppBusRetrying = false;' "$IFACE"; then
    note "$IFACE" "the retry must mark itself on the wire (AppBusRetrying) so the frame's own forward cannot re-arm a fresh budget"
fi
if [ "$(count 'AppBusRetryLeft--;')" != 1 ]; then
    note "$IFACE" "every retry must spend the budget once (AppBusRetryLeft--), or the repair becomes an unbounded loop"
fi
if ! grep -q 'if (AppBusRetryLeft > 0)' "$IFACE"; then
    note "$IFACE" "the budget must gate the re-arm (if (AppBusRetryLeft > 0)) - the app's own timeout is the only report after it"
fi

# --- (8) reported ---------------------------------------------------------------------------------
if ! grep -q 'retries=%lu strays=%lu' "$IFACE"; then
    note "$IFACE" "the BUSGATE totals line must print retries= (a repaired restore must be visible)"
fi
drain_body=$(sed -n '/^inline void AppBusGateDrain/,/^}/p' "$IFACE")
if ! printf '%s\n' "$drain_body" | grep -q 'AppBusRetryTotal'; then
    note "$IFACE" "retries= must be fed by AppBusRetryTotal"
fi

if [ "$fail" = 0 ]; then
    echo "PASS: a forwarded Register CID 3 write whose answer never came is re-sent within APP_BUS_WRITE_RETRIES sends, with the app's own TRID, and retries= reports it"
fi
exit "$fail"
