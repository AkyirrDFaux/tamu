#!/bin/bash
# Source guard for the core-origin lost-request counter (card t_a20f0442).
#
# A core-origin request is one the CORE starts for itself: a subscription install or renewal, a
# script's foreign register access, a confirm-required value update. None of those has an app link to
# report back on, and before 52c07aa the dispatcher called `SendAndVerifyPacket(Frame);` and threw
# the result away - so a request that exhausted its three retries simply never existed, while its
# caller went on believing it had been sent. That is the silent loss this counter names.
#
# The counter cannot be linked on the host (Dispatcher.h pulls every service, the RTOS rings and the
# device layer), so the WIRING is asserted here and the counter's arithmetic is the plain increment
# it looks like. Each check names the defect it pins:
#
#   (1) the counter exists, once, and sits beside AppBusSendFails - the counter it is the companion
#       of (that one says a transmit failed; this one says whose it was and that nobody was told);
#   (2) it is incremented in ONE place only: a second site would double-count one loss, and a site
#       outside the forward rule would count a frame that was never meant for the bus;
#   (3) the increment is gated on the send having FAILED and on the frame being core-origin. A gate
#       that counts by caller ("is the pump running?") would charge a lost APP forward to the core's
#       counter, and the dispatcher is called from both;
#   (4) the transmit's return value is KEPT (that is the whole defect: `SendAndVerifyPacket(Frame);`
#       as a statement cannot report anything);
#   (5) a frame the transaction gate HELD is not a loss - SendAndVerifyPacket must report true for it,
#       because AppBusDeferredDrain puts it on the wire when the answer it waits behind lands. If it
#       reported false, every queued frame would be counted as lost;
#   (6) it is REPORTED, in the same BUSGATE totals line as sendfails, from the drain that a bench run
#       reads back (Core/Functions/AppInterface.h AppBusGateDrain);
#   (7) the companion count still exists where retries are exhausted, so "sendfails=N corelost=M"
#       stays readable as a pair (M <= N, and M is the part nobody was told about).
#
# Usage: bash test/native/bus_core_lost_guard.sh <AppInterface.h> <Dispatcher.h> <Tamu RSBus.h>
# Exit 0 = the rule holds. Any FAIL line names what broke.
set -u

IFACE="${1:-}"
DISPATCH="${2:-}"
RSBUS="${3:-}"
SRC_DIR="$(dirname "$IFACE")"
for f in "$IFACE" "$DISPATCH" "$RSBUS"; do
    if [ -z "$f" ] || [ ! -f "$f" ]; then
        echo "FAIL: missing source file (usage: bash $0 <AppInterface.h> <Dispatcher.h> <RSBus.h>)" >&2
        exit 1
    fi
done

fail=0
note() { echo "FAIL $1: $2"; fail=1; }

DEF='uint32_t AppBusCoreRequestLost = 0;'
INC='AppBusCoreRequestLost = AppBusCoreRequestLost + 1;'
GATE='if (!Sent && AppFrameSource == AppLinkSource::None)'

# --- (1) one definition, beside AppBusSendFails ------------------------------------------------
def_line=$(grep -nF "$DEF" "$IFACE" | head -1 | cut -d: -f1)
if [ -z "$def_line" ]; then
    note "$IFACE" "the core-origin lost-request counter is gone ($DEF) - a lost core request is silent again"
elif [ "$(grep -cF "$DEF" "$IFACE")" != 1 ]; then
    note "$IFACE" "the counter must be defined exactly once"
fi
sendfails_line=$(grep -n 'uint32_t AppBusSendFails' "$IFACE" | head -1 | cut -d: -f1)
if [ -n "$def_line" ] && [ -n "$sendfails_line" ] && [ "$def_line" -le "$sendfails_line" ]; then
    note "$IFACE" "the counter must sit beside AppBusSendFails (it is that counter's companion), not above it"
fi
# Nothing may zero it: it is a run-long total, and a reset would hide the loss behind a later dump.
if [ "$(grep -c 'AppBusCoreRequestLost = 0;' "$IFACE")" != 1 ]; then
    note "$IFACE" "the counter's only assignment must be its definition - it is a run-long total"
fi

# --- (2) exactly one increment, in the dispatcher ----------------------------------------------
inc_total=$(grep -rnF "$INC" "$SRC_DIR" --include=*.h 2>/dev/null | wc -l | tr -d ' ')
if [ "$inc_total" != 1 ]; then
    note "$SRC_DIR" "the counter must have exactly one increment site (found $inc_total) - two would double-count one loss"
fi
if [ "$(grep -cF "$INC" "$DISPATCH")" != 1 ]; then
    note "$DISPATCH" "the increment must be in the dispatcher's forward rule, where the send's outcome is known"
fi

# --- (3) gated on a FAILED, CORE-ORIGIN frame ---------------------------------------------------
inc_line=$(grep -nF "$INC" "$DISPATCH" | head -1 | cut -d: -f1)
if [ -n "$inc_line" ] && [ "$inc_line" -gt 1 ]; then
    prev=$(sed -n "$((inc_line - 1))p" "$DISPATCH" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    if [ "$prev" != "$GATE" ]; then
        note "$DISPATCH" "the increment must be guarded by '$GATE' (a sent frame is not lost, and an app forward has its own reporting)"
    fi
elif [ -z "$inc_line" ]; then
    note "$DISPATCH" "no increment site found"
fi

# --- (4) the send's return value is kept --------------------------------------------------------
if [ "$(grep -cF 'const bool Sent = SendAndVerifyPacket(Frame);' "$DISPATCH")" != 1 ]; then
    note "$DISPATCH" "the forward rule must KEEP the transmit's return value (const bool Sent = ...) - ignoring it is the defect"
fi
if [ "$(grep -cE '^[[:space:]]+SendAndVerifyPacket\(Frame\);' "$DISPATCH")" != 0 ]; then
    note "$DISPATCH" "a bare SendAndVerifyPacket(Frame); statement is back - its result is discarded, so a loss is invisible"
fi
sent_line=$(grep -nF 'const bool Sent = SendAndVerifyPacket(Frame);' "$DISPATCH" | head -1 | cut -d: -f1)
if [ -n "$inc_line" ] && [ -n "$sent_line" ] && [ "$sent_line" -ge "$inc_line" ]; then
    note "$DISPATCH" "the counter must be decided AFTER the send, from its result"
fi

# --- (5) a held frame is not a loss -------------------------------------------------------------
offer_line=$(grep -n 'if (!AppBusGateOffer(Data))' "$RSBUS" | head -1 | cut -d: -f1)
if [ -z "$offer_line" ]; then
    note "$RSBUS" "the gate is no longer asked at the transmit primitive - a held frame could be reported as lost"
else
    if ! sed -n "$((offer_line + 1)),$((offer_line + 2))p" "$RSBUS" | grep -q 'return true;'; then
        note "$RSBUS" "a gate-held frame must report TRUE (AppBusDeferredDrain sends it) - otherwise every queued frame counts as lost"
    fi
fi

# --- (6) reported in the BUSGATE totals line, from the drain a bench run reads -------------------
drain_body=$(sed -n '/^inline void AppBusGateDrain()/,/^}/p' "$IFACE")
if [ -z "$drain_body" ]; then
    note "$IFACE" "AppBusGateDrain not found - the counter would have no reporting path"
else
    bus_line=$(printf '%s\n' "$drain_body" | grep -n 'corelost=%lu' | head -1 | cut -d: -f1)
    if [ -z "$bus_line" ]; then
        note "$IFACE" "the BUSGATE totals line no longer prints corelost= (%lu) - the rig cannot read the counter"
    else
        # The format string and its arguments span several lines (a %lu per counter), so read the
        # whole DeviceLog call: from the format line to its closing ');'.
        bus_call=$(printf '%s\n' "$drain_body" |
            awk '/corelost=%lu/ { on = 1 } on { print } on && /\);/ { exit }')
        if ! printf '%s\n' "$bus_call" | grep -q 'AppBusCoreRequestLost'; then
            note "$IFACE" "corelost= must be fed by AppBusCoreRequestLost, not another counter"
        fi
        # The pair has to be one line: it is what makes the loss attributable.
        if ! printf '%s\n' "$bus_call" | head -1 | grep -q 'bus sendfails=%lu.*corelost=%lu'; then
            note "$IFACE" "sendfails= and corelost= must share one totals line (the pair says which loss was the core's)"
        fi
    fi
fi

# --- (7) the companion count at the retry-exhaustion site ---------------------------------------
if [ "$(grep -c 'AppBusSendFails = AppBusSendFails + 1;' "$RSBUS")" != 1 ]; then
    note "$RSBUS" "a transmit that exhausts its retries must still count AppBusSendFails (once) - corelost= is read beside it"
fi

if [ "$fail" = 0 ]; then
    echo "PASS: a core-origin frame that exhausts its retries is counted once, beside AppBusSendFails, and printed as corelost= in the BUSGATE totals line"
fi
exit "$fail"
