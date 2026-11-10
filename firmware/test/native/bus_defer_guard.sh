#!/bin/bash
# Source guard for the bus deferral ring (card t_ab6c7bdc, items 2 and 3).
#
# AppInterface.h and Dispatcher.h cannot be linked on the host (the RTOS rings, every service), so
# the DECISIONS are host-tested where they are pure (test/native/bus_gate_test.cpp drives
# Core/Functions/BusGate.h) and what is asserted here is the wiring the review found broken:
#
#   (2) A deferred frame was re-dispatched by AppBusDeferredDrain with `AppFrameSource == None`, and
#       the gate classifies a transaction by that flag (AppInterface.h AppBusGateOffer ->
#       BusGateWindowMs). So an app `Storage/Read` forwarded directly got 600 ms and the SAME frame
#       re-sent by the drain got 250 ms: a node answering at 300 ms - inside the window that was
#       fitted for it (BusGate.h) - expired, the next frame went out on top of its answer, and the
#       app reported a timeout for a node that answered. The same line counted a failed app forward
#       as AppBusCoreRequestLost, the counter that exists to name core-origin losses. The origin must
#       travel WITH the frame and be restored around the drain's dispatch.
#
#   (3) Answer-less frames are deferred like every other frame although Dispatcher.h claimed "only
#       frames that expect NO answer can leave back to back". Two things are pinned here: the gate's
#       verdict still takes ONLY (gate open, room) - being answer-less is not a reason to transmit
#       into a wire owed an answer - and the holds are COUNTED where they are paid
#       (AppBusDeferredNoAnswer, `noreply=` in the BUSGATE totals line), so the delay is visible.
#
# Usage: bash test/native/bus_defer_guard.sh <AppInterface.h> <Dispatcher.h> <BusGate.h>
# Exit 0 = the rule holds. Any FAIL line names what broke.
set -u

IFACE="${1:-}"
DISPATCH="${2:-}"
GATE="${3:-}"
for f in "$IFACE" "$DISPATCH" "$GATE"; do
    if [ -z "$f" ] || [ ! -f "$f" ]; then
        echo "FAIL: missing source file (usage: bash $0 <AppInterface.h> <Dispatcher.h> <BusGate.h>)" >&2
        exit 1
    fi
done

fail=0
note() { echo "FAIL $1: $2"; fail=1; }

drain=$(sed -n '/^inline void AppBusDeferredDrain()/,/^}/p' "$DISPATCH")
[ -n "$drain" ] || note "$DISPATCH" "AppBusDeferredDrain() not found"

# --- (2) the origin travels with the held frame ----------------------------------------------------
if ! grep -q 'AppLinkSource AppBusDeferredFrom\[APP_BUS_DEFER_DEPTH\]' "$IFACE"; then
    note "$IFACE" "AppBusDeferredFrom[] is gone - a held frame no longer carries the link it arrived on"
fi
if ! grep -q 'AppBusDeferredFrom\[AppBusDeferredCount\] = AppFrameSource;' "$IFACE"; then
    note "$IFACE" "AppBusDefer must record the frame's own origin (AppFrameSource) beside it"
fi
if ! printf '%s\n' "$drain" | grep -q 'const AppLinkSource From = AppBusDeferredFrom\[0\];'; then
    note "$DISPATCH" "the drain must take the origin it recorded with the frame (AppBusDeferredFrom[0])"
fi
if ! printf '%s\n' "$drain" | grep -q 'AppBusDeferredFrom\[i - 1\] = AppBusDeferredFrom\[i\];'; then
    note "$DISPATCH" "the drain must shift the origin array with the frames, or the two indexes drift"
fi

# ... and it must be RESTORED around the dispatch: set before it, put back after it.
from_line=$(printf '%s\n' "$drain" | grep -n 'AppFrameSource = From;' | head -1 | cut -d: -f1)
call_line=$(printf '%s\n' "$drain" | grep -n 'DispatchPacket(Frame);' | head -1 | cut -d: -f1)
save_line=$(printf '%s\n' "$drain" | grep -n 'AppFrameSource = Saved;' | head -1 | cut -d: -f1)
if [ -z "$from_line" ] || [ -z "$call_line" ] || [ -z "$save_line" ]; then
    note "$DISPATCH" "the drain must set AppFrameSource from the recorded origin, dispatch, then restore it (found set=$from_line dispatch=$call_line restore=$save_line)"
elif [ "$from_line" -ge "$call_line" ] || [ "$call_line" -ge "$save_line" ]; then
    note "$DISPATCH" "the origin must be restored AROUND the dispatch (lines set=$from_line dispatch=$call_line restore=$save_line)"
fi
if [ "$(printf '%s\n' "$drain" | grep -c 'DispatchPacket(Frame);')" != 1 ]; then
    note "$DISPATCH" "the drain must dispatch exactly once per frame"
fi
# A frame put on the wire with no origin is what the review measured: the drain must not clear it.
if printf '%s\n' "$drain" | grep -qE 'AppFrameSource = AppLinkSource::None'; then
    note "$DISPATCH" "the drain clears the origin again - a queued app forward would get the core's window"
fi

# --- (3) one rule for every frame, and the answer-less holds are counted ----------------------------
if ! grep -q '^inline BusGateVerdict BusGateVerdictFor(bool GateOpen, bool DeferRoom)$' "$GATE"; then
    note "$GATE" "BusGateVerdictFor changed shape - the gate holds EVERY frame while an answer is owed; 'expects no answer' is not a reason to transmit"
fi
# --- (4) the answered unicast the flag-only test missed, and the answer that closes it ---------------
#
# A `Subscriptions` Change (040x/1) is answered - the provider returns the source's current value, or
# FLAG_SUCCESS for a cancel - but every construction of it is FLAG_START|FLAG_STOP, with no REQACK
# (SubscriptionsPersist.h RegisterRequesterProvider, SubscriptionsControl.h
# SubscriptionsCancelProvider, SubscriptionsRequester.h orphan cancel). A gate keyed only on REQACK
# therefore never held a slot for it, and the next frame went out with the Change still owed its
# answer - the card's evidence item 1. The gate must classify it by CID, and the release must match
# the CMD as well as the TRID, because the provider's value UPDATES carry the same subscription TRID.
if ! grep -q '#define BUS_GATE_SUB_CHANGE_CID' "$GATE"; then
    note "$GATE" "BUS_GATE_SUB_CHANGE_CID is gone - the 040x/1 Change is answered but carries no REQACK"
fi
if ! grep -q 'GetServiceType(Cmd) == ServiceType::Subscriptions' "$GATE" ||
   ! grep -q 'GetServiceCID(Cmd) == BUS_GATE_SUB_CHANGE_CID' "$GATE"; then
    note "$GATE" "BusGateNeedsAnswer must classify the 040x/1 Change as a transaction (by service and CID)"
fi
if ! grep -q '^inline bool BusGateIsAnswer(' "$GATE"; then
    note "$GATE" "BusGateIsAnswer is gone - the slot would close on any frame echoing the TRID"
fi
if ! grep -q 'AppInterface.BusPendingCmd = Frame.Cmd;' "$IFACE"; then
    note "$IFACE" "the open slot must remember the request's CMD, or a value update closes it early"
fi
if ! grep -q 'if (!BusGateNeedsAnswer(Frame.IdTgt, Frame.Flags, Frame.Cmd))' "$IFACE"; then
    note "$IFACE" "AppBusDefer must classify held frames with the host-tested BusGateNeedsAnswer()"
fi
if ! grep -qE 'AppBusDeferredNoAnswer *(\+\+|=[^;]*\+ *1)' "$IFACE"; then
    note "$IFACE" "an answer-less frame that is held must be counted (AppBusDeferredNoAnswer) - the delay is otherwise invisible"
fi
if ! grep -q 'noreply=%lu' "$IFACE"; then
    note "$IFACE" "the BUSGATE totals line must print the answer-less deferrals (noreply=)"
fi
# The two halves of the rule name each other, so the prose cannot drift from the code again.
if ! grep -q 'AppBusDeferredNoAnswer' "$DISPATCH"; then
    note "$DISPATCH" "the drain's comment must name the counter that makes the answer-less hold visible (it is where the delay is paid)"
fi

if [ "$fail" = 0 ]; then
    echo "PASS: a held frame carries and gets back its origin, the gate holds every frame by one rule, an answer-less hold is counted, and the answered 040x/1 Change takes the slot while a value update cannot close it"
fi
exit "$fail"
