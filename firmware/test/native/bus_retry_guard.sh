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

# --- (9) WHY it was lost: E79's four candidates, counted apart (card t_e707d153) -------------------
#
# The retry above repairs the loss, which is exactly why the loss went unaccounted: `retries=8` said
# eight writes were repaired and nothing said what had lost them. The account below names one cause
# per lost write - the node's refusal (a FLAG_FAIL answer), the answer never arriving (silent), the
# keep ring refusing bytes (keepdrop, E82), the gate's window closing before the answer (late), the
# frame never leaving (notsent) - and prints it per write. The checks pin (a) one counter per cause,
# (b) one increment site each, (c) the classification order that makes "silent" mean "the node never
# acted" rather than "we do not know", and (d) the print, so a rig capture can be read without a
# wire capture.
for def in 'AppBusWriteRefused = 0' 'AppBusWriteNotSent = 0' 'AppBusWriteKeepDrop = 0' \
           'AppBusWriteLate = 0' 'AppBusWriteSilent = 0' 'AppBusWriteStray = 0' \
           'AppBusWriteTotal = 0' 'AppBusWriteLost = 0' 'AppBusWriteRepaired = 0'; do
    if [ "$(count "$def")" != 1 ]; then
        note "$IFACE" "the write account must define $def exactly once"
    fi
done
count_body=$(sed -n '/^inline void AppBusWriteCount/,/^}/p' "$IFACE")
if [ -z "$count_body" ]; then
    note "$IFACE" "AppBusWriteCount not found - the five causes have no single counting site"
else
    for cause in Refused NotSent KeepDrop Late Silent Stray; do
        if [ "$(grep -cF "AppBusWrite$cause = AppBusWrite$cause + 1;" "$IFACE")" != 1 ]; then
            note "$IFACE" "cause $cause must be incremented in exactly one place (AppBusWriteCount)"
        elif ! printf '%s\n' "$count_body" | grep -qF "AppBusWrite$cause = AppBusWrite$cause + 1;"; then
            note "$IFACE" "cause $cause is incremented outside AppBusWriteCount - the dump's figures would drift from the records"
        fi
    done
fi
because_body=$(sed -n '/^inline void AppBusWriteRecause/,/^}/p' "$IFACE")
if [ -z "$because_body" ] || ! printf '%s\n' "$because_body" | grep -qF 'AppBusWriteLate = AppBusWriteLate - 1;'; then
    note "$IFACE" "an upgraded cause must MOVE its count (AppBusWriteRecause decrements the old cause), or late+silent exceed the lost total"
fi

lost_body=$(sed -n '/^inline void AppBusWriteNoteLost/,/^}/p' "$IFACE")
if [ -z "$lost_body" ]; then
    note "$IFACE" "AppBusWriteNoteLost not found - a lost write has no cause"
else
    # keepdrop first: a refused byte means an answer existed and could not be held (E82), whatever
    # else the window carried. Then the send itself, then the clean line (the node never acted),
    # and only then 'stray'.
    if ! printf '%s\n' "$lost_body" | grep -q 'if (Record.Drops != 0)'; then
        note "$IFACE" "the classification must test the keep ring's refusals first (E82) - a kept answer makes the node innocent"
    fi
    for cause in ABW_KEEPDROP ABW_NOTSENT ABW_SILENT ABW_STRAY; do
        if ! printf '%s\n' "$lost_body" | grep -qE "Cause = $cause;"; then
            note "$IFACE" "AppBusWriteNoteLost must be able to name $cause"
        fi
    done
    order=$(printf '%s\n' "$lost_body" | grep -oE 'Cause = ABW_(KEEPDROP|NOTSENT|SILENT|STRAY);' |
                sed 's/Cause = ABW_//; s/;//' | tr '\n' ' ')
    if [ "$order" != "KEEPDROP NOTSENT SILENT STRAY " ]; then
        note "$IFACE" "the cause is decided in the order keepdrop -> notsent -> silent -> stray (found: $order)"
    fi
    if ! printf '%s\n' "$lost_body" | grep -q 'if (AppBusWriteRecorded)'; then
        note "$IFACE" "a retry that expires again must not count the write twice (if (AppBusWriteRecorded) return)"
    fi
    if [ "$(grep -cF 'AppBusWriteLost = AppBusWriteLost + 1;' "$IFACE")" != 1 ]; then
        note "$IFACE" "one increment per lost write (AppBusWriteLost), and only in AppBusWriteNoteLost"
    fi
fi

# The refusal is the ANSWER's flag, not a window: a node that says no is not a lost write.
answer_body=$(sed -n '/^inline void AppBusWriteAnswer/,/^}/p' "$IFACE")
if [ -z "$answer_body" ] || ! printf '%s\n' "$answer_body" | grep -q 'if (Flags & FLAG_FAIL)'; then
    note "$IFACE" "a FLAG_FAIL answer must be counted as a REFUSAL (AppBusWriteAnswer tests Flags & FLAG_FAIL)"
fi
if [ "$(grep -cF 'AppBusWriteRepaired = AppBusWriteRepaired + 1;' "$IFACE")" != 1 ] ||
   ! printf '%s\n' "$answer_body" | grep -q 'AppBusWriteRepaired = AppBusWriteRepaired + 1;'; then
    note "$IFACE" "a repaired write must be counted where its answer lands (AppBusWriteRepaired in AppBusWriteAnswer)"
fi

# 'late' is only knowable when the answer turns up after the window, so it is reached from that one
# branch and nowhere else.
if [ "$(grep -cF 'AppBusWriteNoteLate(Trid, Lateness);' "$IFACE")" != 1 ]; then
    note "$IFACE" "the gate's late-answer branch is the only place a write may be re-caused to late (AppBusWriteNoteLate)"
fi

# ... printed, with the figures a capture is reconciled against.
if ! grep -q 'appwrite writes=%lu answered=%lu refused=%lu lost=%lu repaired=%lu' "$IFACE"; then
    note "$IFACE" "the dump must print the write totals (writes/answered/refused/lost/repaired)"
fi
if ! grep -q 'appwrite cause silent=%lu late=%lu keepdrop=%lu notsent=%lu stray=%lu' "$IFACE"; then
    note "$IFACE" "the dump must print the five causes apart"
fi
if ! printf '%s\n' "$drain_body" | grep -q 'cause=%s sends=%u'; then
    note "$IFACE" "the dump must name each lost write's cause and how many sends it took"
fi

if [ "$fail" = 0 ]; then
    echo "PASS: a forwarded Register CID 3 write whose answer never came is re-sent within APP_BUS_WRITE_RETRIES sends, with the app's own TRID, and retries= reports it"
    echo "PASS: every lost write is named - refused (a FLAG_FAIL answer), notsent, keepdrop, late or silent/stray - with the numbers it was read from, and writes = answered + refused + lost"
fi
exit "$fail"

