#!/bin/bash
# Builds and runs the host-side firmware tests. Everything here is device-free, so it runs
# without hardware - which is how the numeric core, the offset tables and the OPTIMIZE_SPEED
# variants are kept honest.
#
#   numeric_test.cpp  Number.h + Vector/Matrix/Colour, in the three shapes the targets select
#   stride_test.cpp   the script VM's prefix-sum symbol offsets (Core/Functions/StrideOffsets.h)
#   script_test.cpp   the script file header, Service opcode set (Core/Services/ScriptDefs.h),
#                     the custom log's 32-bit code and Power's domain/fast paths
#                     (Core/Services/ScriptPow.h)
#   crc_test.cpp      Crc8 (Core/Functions/Packet.h), both OPTIMIZE_SPEED shapes
#   value_update_test.cpp  a 0-length value write is refused at the dynamic block
#                     (Core/Functions/MemoryBlocks.h), so the target keeps its value
#   register_read_test.cpp  a row with no bytes is unreadable - a read of it fails the access
#                     (REGISTER), it is not an empty value whose type then complains (the read-side
#                     ValueReadable, Core/Functions/MemoryTypes.h)
#   lease_test.cpp    the requester row's lifetime and install/keepalive cadence (Core/Services/
#                     SubscriptionsLease.h): rows are never dropped on time, an unanswered install
#                     retries at SUB_RETRY_MS, capped at 5, then the keepalive, and an answer
#                     resets the counter; the once-per-episode give-up line and its exact text -
#                     plus a source guard that the only removal of a row is the explicit-cancel
#                     branch of RequesterSet
#   meas_transform_test.cpp  the Raw Resistance unit transform over the whole usable ADC window
#                     (Core/Devices/MeasuringResistance.h), in both Number shapes
#
# The numeric core is built in three shapes: the core (64-bit) and the DAS (32-bit, no
# Vector/Matrix) are the real targets; the third - 32-bit *with* Vector/Matrix - is a
# supported combination that neither target selects, so without it the isqrt32/FixedMul32
# path through sqrt() is never even compiled.
#
# OPTIMIZE_SPEED (TODO A9) selects the speed variants where a site has two shapes; the size
# variant is the default and the core defines it. The flag must not change any result, so the
# tests are built and run both ways and each must pass against the same assertions.
set -e

cd "$(dirname "$0")/../.."   # firmware/
ROOT="$PWD"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

CXX="${CXX:-g++}"
# TAMU_INT32_IS_INT: on the host int32_t is int, so Number's extra `int` constructor would be
# a redefinition. The embedded toolchains (where int32_t is long) do not define it.
FLAGS=(-std=c++17 -O1 -Wall -Wextra -Werror -DTAMU_INT32_IS_INT -I"$ROOT/src")

fail=0

# $1 name, $2 source, then the config flags. Built once per OPTIMIZE_SPEED state.
build_run() {
    local name="$1" src="$2"; shift 2
    if "$CXX" "${FLAGS[@]}" "$@" "$ROOT/test/native/$src" -o "$OUT/$name"; then
        "$OUT/$name" || fail=1
    else
        fail=1
    fi
}

for SPEED in "-DOPTIMIZE_SPEED" ""; do
    label="size variant"
    [ -n "$SPEED" ] && label="speed variant (-DOPTIMIZE_SPEED)"

    echo "### native numeric tests: core build (64-bit, Vector/Matrix), $label"
    build_run "numeric_core_${SPEED:+speed}" "numeric_test.cpp" $SPEED
    echo

    echo "### native numeric tests: 32-bit build (NUMBER_ONLY_32BIT, Vector/Matrix), $label"
    build_run "numeric_32_${SPEED:+speed}" "numeric_test.cpp" -DNUMBER_ONLY_32BIT $SPEED
    echo

    echo "### native numeric tests: DAS build (NUMBER_ONLY_32BIT + SCALAR_ONLY), $label"
    build_run "numeric_das_${SPEED:+speed}" "numeric_test.cpp" -DNUMBER_ONLY_32BIT -DSCALAR_ONLY $SPEED
    echo

    echo "### native crc8 tests (wire checksum), $label"
    build_run "crc_${SPEED:+speed}" "crc_test.cpp" $SPEED
    echo

    echo "### native geometry tests (LED display mask shapes), $label"
    build_run "geometry_${SPEED:+speed}" "geometry_test.cpp" $SPEED
    echo
done

echo "### native alignment tests (PacketFrame alignment + unaligned buffer access)"
build_run "align" "align_test.cpp"
echo

echo "### native stride-offset tests (script VM symbol resolution)"
build_run "stride" "stride_test.cpp"
echo

echo "### native script tests (file header + Properties + Service opcode set + Power's domain rule)"
build_run "script" "script_test.cpp" -DUSE_SCRIPTS -DSCRIPT_HOST_TEST
echo

echo "### native bootloader packet tests (codec layout + even parity)"
build_run "bootloader" "bootloader_test.cpp"
echo

echo "### native TRID tests (System/Log counter range + increment/wrap)"
build_run "trid" "trid_test.cpp"
echo

echo "### native log layout tests (LogMessage source/code/timestamp)"
build_run "log" "log_test.cpp"
echo

echo "### native value-update tests (a 0-length value is refused; the dynamic target keeps its value)"
build_run "valueupdate" "value_update_test.cpp"
echo

echo "### native register-read tests (a row with no bytes is unreadable, not a type mismatch)"
build_run "registerread" "register_read_test.cpp"
echo

# t_e57a9667, structural half: RegisterGetByBlockInfo and the script read instruction both pull the
# packet/register/device layer, so the host cannot link them. The facts the three cases depend on
# are asserted on the source instead:
#   - the copy accessor refuses a row with no bytes BEFORE it copies anything (asking
#     ValueReadable, the shared read-side predicate the native test drives), so a size-0 row is a
#     failed access - never a 0-length value handed to the reader's assignment;
#   - the read instruction carries SCRIPT_ERR_REGISTER for that failed access, from the one
#     RegisterGetByBlockInfo call in its block;
#   - it still returns the assignment's own error, so a genuine type mismatch ("short Number rows"
#     and any other decode failure) stays SCRIPT_ERR_TYPE rather than being folded into REGISTER.
echo "### source guard: an empty row fails the access, a bad type still fails the assignment"
DISPATCH_SRC="src/Core/Services/RegisterDispatch.h"
EXEC_SRC="src/Core/Services/ScriptExec.h"
read_block=$(sed -n '/case SCRIPT_OP_SERVICE_REG_READ: {/,/case SCRIPT_OP_SERVICE_REG_WRITE: {/p' "$EXEC_SRC")
regread_fail=0
if [ "$(grep -c 'if (!ValueReadable(Meta)) return false;' "$DISPATCH_SRC")" != 1 ]; then
    echo "FAIL $DISPATCH_SRC: the copy accessor must refuse a size-0 row exactly once (ValueReadable)"
    regread_fail=1
fi
guard_line=$(grep -n 'if (!ValueReadable(Meta)) return false;' "$DISPATCH_SRC" | cut -d: -f1)
copy_line=$(grep -n 'memcpy(Vbuf, Fr.Data, Meta.Size);' "$DISPATCH_SRC" | cut -d: -f1)
if [ -z "$guard_line" ] || [ -z "$copy_line" ] || [ "$guard_line" -ge "$copy_line" ]; then
    echo "FAIL $DISPATCH_SRC: the ValueReadable guard must precede the copy, not follow it"
    regread_fail=1
fi
if [ "$(printf '%s\n' "$read_block" | grep -c 'if (!RegisterGetByBlockInfo')" != 1 ]; then
    echo "FAIL $EXEC_SRC: the read block must resolve the register exactly once"
    regread_fail=1
fi
if [ "$(printf '%s\n' "$read_block" | grep -c 'SCRIPT_ERR_REGISTER')" != 1 ]; then
    echo "FAIL $EXEC_SRC: a failed read must carry SCRIPT_ERR_REGISTER (once, and not another code)"
    regread_fail=1
fi
if [ "$(printf '%s\n' "$read_block" | grep -c 'if (Err) return Err;')" != 1 ]; then
    echo "FAIL $EXEC_SRC: the read must still return the assignment's own error (a real type mismatch is TYPE)"
    regread_fail=1
fi
[ $regread_fail = 0 ] && echo "PASS: empty row => REGISTER (guarded before the copy), bad type => the assignment's error"
fail=$((fail + regread_fail))
echo

echo "### native subscription-keepalive tests (rows never expire; unanswered installs retry fast, capped)"
build_run "lease" "lease_test.cpp"
echo

echo "### native edge-counter tests (the counter IS the payload, parity is the counter's level, the pulse)"
build_run "edgecounter" "edge_counter_test.cpp" -DUSE_SUB_REQUEST
echo

echo "### native edge-ack tests (an edge update is confirmed; an unacked counter is re-sent, not dropped)"
build_run "edgeack" "edge_ack_test.cpp" -DUSE_SUB_REQUEST
echo

echo "### native renewal-contract tests (a renewal extends the timeouts and nothing else)"
build_run "renewal" "renewal_test.cpp" -DUSE_SUB_REQUEST
echo

echo "### native button-latch tests (a tap shorter than the loop is still one press)"
build_run "buttonlatch" "button_latch_test.cpp"
echo

# t_25f04000, structural half: the device side of the button press capture cannot be linked on
# the host (it is a GPIO/EXTI register), so the facts the sampling decision depends on are
# asserted on the source:
#   - the DAS button reports through ButtonSample, the decision the native test drives - not a
#     second copy of `captured || pin low`;
#   - the hardware capture is consumed on the pass that reported it (INTFR written back), so one
#     caught edge is one press and a held press cannot be counted again;
#   - only the falling edge is armed (a press is the active-low edge), and the capture is armed
#     after the arming itself is discarded - otherwise the first pass can report a phantom press;
#   - it stays a polled capture: no ISR, no PFIC line, no interrupt enable - which is what keeps
#     it inside the DAS's remaining code space;
#   - the main loop still samples the block once per pass.
echo "### source guard: the DAS button reports the capture through ButtonSample and consumes it"
BTN_SRC="src/Devices/DAS_v0.1/Button.h"
MAIN_SRC="src/Devices/DAS_v0.1/Main.h"
btn_fail=0
if [ "$(grep -c 'ButtonSample(' "$BTN_SRC")" != 1 ]; then
    echo "FAIL $BTN_SRC: the block must report through ButtonSample exactly once"
    btn_fail=1
fi
if [ "$(grep -c 'EXTI->INTFR = DAS_BUTTON_EXTI' "$BTN_SRC")" != 2 ]; then
    echo "FAIL $BTN_SRC: the capture must be cleared on the pass that reported it and after arming"
    btn_fail=1
fi
consume_line=$(grep -n 'EXTI->INTFR = DAS_BUTTON_EXTI;' "$BTN_SRC" | tail -1 | cut -d: -f1)
init_line=$(grep -n 'void DasButtonInit()' "$BTN_SRC" | cut -d: -f1)
update_line=$(grep -n 'void DasButtonUpdate()' "$BTN_SRC" | cut -d: -f1)
arm_clear_line=$(grep -n 'EXTI->INTFR = DAS_BUTTON_EXTI;' "$BTN_SRC" | head -1 | cut -d: -f1)
if [ -z "$consume_line" ] || [ -z "$arm_clear_line" ] || [ -z "$init_line" ] || [ -z "$update_line" ]; then
    echo "FAIL $BTN_SRC: both clears (arm and consume) must be present, in DasButtonInit/DasButtonUpdate"
    btn_fail=1
elif [ "$arm_clear_line" -le "$init_line" ] || [ "$arm_clear_line" -ge "$update_line" ]; then
    echo "FAIL $BTN_SRC: the arming must clear the capture it latched, inside DasButtonInit"
    btn_fail=1
elif [ "$consume_line" -le "$update_line" ]; then
    echo "FAIL $BTN_SRC: the capture must be cleared on the pass that reported it (DasButtonUpdate)"
    btn_fail=1
fi
if [ "$(grep -c 'EXTI->FTENR |= DAS_BUTTON_EXTI' "$BTN_SRC")" != 1 ] ||
   [ "$(grep -c 'EXTI->RTENR' "$BTN_SRC")" != 0 ]; then
    echo "FAIL $BTN_SRC: only the falling edge may be armed (a press is the active-low edge)"
    btn_fail=1
fi
if [ "$(grep -cE 'EXTI->INTENR|NVIC_EnableIRQ|__enable_irq|EXTI7_0_IRQHandler' "$BTN_SRC")" != 0 ]; then
    echo "FAIL $BTN_SRC: the capture must stay polled - no ISR, no PFIC line, no interrupt enable"
    btn_fail=1
fi
if [ "$(grep -c 'DasButtonUpdate();' "$MAIN_SRC")" != 1 ]; then
    echo "FAIL $MAIN_SRC: the main loop must sample the button once per pass"
    btn_fail=1
fi
[ $btn_fail = 0 ] && echo "PASS: the button reports the latched edge through ButtonSample and consumes it"
fail=$((fail + btn_fail))
echo

# t_378cc55c, structural half: the two device sites that put the counter on the wire and read it back
# off again cannot be linked on the host (they pull the packet/register/device layer), so the facts
# the accounting depends on are asserted on the source itself:
#   - the provider lays out an edge payload ONLY through ProviderEdgePayload (one owner of the
#     layout), that payload is the counter alone (nothing is copied behind it), only for an edge
#     trigger, and the provider steps the counter through SubEdgeClassify (every level change counts,
#     so parity is the level);
#   - the requester reads the counter with SubEdgeCounterLoad and nothing else - there is no value
#     behind it (no `Payload + SUB_EDGE_COUNTER_SIZE`), and the counter is never written to the
#     target: the one write is the pulse value the state machine hands out (SubEdgeState::NextPass),
#     one byte, once per loop pass;
#   - every target write goes through the single WriteRequesterTarget (so a pulse cannot take a
#     different path than a live update);
#   - the two payload shapes are split on the length, and the value-only answer re-baselines;
#   - no level is derived for the target any more (no SubEdgeLevelByte anywhere): parity describes
#     the counter, and the value the requester writes is the pulse.
echo "### source guard: the counter is the whole payload, and the target takes a pulse, on both ends"
REQ_SRC="src/Core/Services/SubscriptionsRequester.h"
PROV_SRC="src/Core/Services/SubscriptionsProvider.h"
CTL_SRC="src/Core/Services/SubscriptionsControl.h"
edge_guard_fail=0
if [ "$(grep -c 'SubEdgeCounterStore' "$PROV_SRC")" != 1 ]; then
    echo "FAIL $PROV_SRC: the provider must lay out the counter in exactly one place (ProviderEdgePayload)"
    edge_guard_fail=1
fi
if [ "$(grep -c 'memcpy(TxFrame.Payload' "$PROV_SRC")" != 0 ]; then
    echo "FAIL $PROV_SRC: nothing may be appended behind the counter - it is the whole payload"
    edge_guard_fail=1
fi
if [ "$(grep -c 'return SUB_EDGE_COUNTER_SIZE;' "$PROV_SRC")" != 1 ]; then
    echo "FAIL $PROV_SRC: the edge payload length must be the counter size and nothing more"
    edge_guard_fail=1
fi
if [ "$(grep -c 'SubEdgeClassify' "$PROV_SRC")" != 1 ]; then
    echo "FAIL $PROV_SRC: the provider must count level changes through SubEdgeClassify (parity is the level)"
    edge_guard_fail=1
fi
if [ "$(grep -c 'SubEdgeCounterStore' "$CTL_SRC")" != 0 ]; then
    echo "FAIL $CTL_SRC: the 0401 answer carries the value alone; it must not write a counter"
    edge_guard_fail=1
fi
if [ "$(grep -c 'SubEdgeCounterLoad' "$REQ_SRC")" != 1 ]; then
    echo "FAIL $REQ_SRC: the counter must be read in exactly one place (the value-update path)"
    edge_guard_fail=1
fi
if [ "$(grep -c 'Frame.Payload + SUB_EDGE_COUNTER_SIZE' "$REQ_SRC")" != 0 ]; then
    echo "FAIL $REQ_SRC: nothing may sit behind the counter - the update has no value bytes"
    edge_guard_fail=1
fi
if [ "$(grep -rc 'SubEdgeLevelByte' src --include=*.h | grep -vc ':0$')" != 0 ]; then
    echo "FAIL: nothing may derive a level for the target any more - its value is the pulse"
    edge_guard_fail=1
fi
if [ "$(grep -c 'WriteRequesterTarget(Entry, &Value, 1)' "$REQ_SRC")" != 1 ]; then
    echo "FAIL $REQ_SRC: there must be exactly one target write, and it must be the pulse value"
    edge_guard_fail=1
fi
if [ "$(grep -c 'Edge.NextPass()' "$REQ_SRC")" != 1 ]; then
    echo "FAIL $REQ_SRC: the pulse must come from SubEdgeState::NextPass (one call per pass)"
    edge_guard_fail=1
fi
# One target writer: a replay and a live update must not be able to disagree about what may be written.
if [ "$(grep -c 'RegisterSetByBlockInfo' "$REQ_SRC")" != 1 ]; then
    echo "FAIL $REQ_SRC: every target write must go through WriteRequesterTarget"
    edge_guard_fail=1
fi
if ! grep -q 'Len >= SUB_EDGE_COUNTER_SIZE' "$REQ_SRC"; then
    echo "FAIL $REQ_SRC: the push/answer split must be on the payload length (a push is the counter, no value byte)"
    edge_guard_fail=1
fi
if [ "$(grep -c 'Edge.ResetBaseline()' "$REQ_SRC")" != 1 ]; then
    echo "FAIL $REQ_SRC: the value-only answer must re-baseline the counter (once)"
    edge_guard_fail=1
fi
if [ "$(grep -rc 'SUB_EDGE_REPLAY_VALUE_MAX' src --include=*.h | grep -vc ':0$')" != 0 ]; then
    echo "FAIL: no value bytes are retained for a replay any more - the level is derived from the counter"
    edge_guard_fail=1
fi
# The pulse has to be driven from the main loop, one pass at a time.
if ! grep -q 'SubscriptionsPulseTargets();' "$CTL_SRC"; then
    echo "FAIL $CTL_SRC: Tick must run the edge pulse"
    edge_guard_fail=1
fi
# t_d43718b5, structural half: an edge update is ACKNOWLEDGED (owner ruling, 2026-10-27), and the
# sites that send it, decide to re-send it and confirm it are device-side, so the facts are asserted
# on the source:
#   - the send is tagged FLAG_REQACK for an edge trigger as well as for a confirm trigger;
#   - the provider's edge row counts through SubEdgeAdvance and decides a send through
#     SubEdgeUnacked (the packed word: counter low, last confirmed counter high) - exactly once each,
#     so nothing can count or gate behind the helpers' back;
#   - the old EdgeDue flag is gone everywhere: it was cleared AT the send, which is what dropped a
#     lost update;
#   - the confirmation of an edge row goes through SubEdgeAnswers (only the counter it answers is
#     confirmed) instead of overwriting the packed word with a hash;
#   - the requester confirms an edge PUSH with the counter it received (SubscriptionsConfirmEdge),
#     gated on FLAG_REQACK, and does it for a duplicate too - a duplicate is a re-send whose earlier
#     ack was lost.
if [ "$(grep -c 'if (Entry->Sub.Trigger == TriggerType::OnChangeConfirm || Edge) Flags |= FLAG_REQACK;' "$PROV_SRC")" != 1 ]; then
    echo "FAIL $PROV_SRC: an edge update must ask for a confirmation (FLAG_REQACK)"
    edge_guard_fail=1
fi
if [ "$(grep -c 'SubEdgeAdvance' "$PROV_SRC")" != 1 ]; then
    echo "FAIL $PROV_SRC: the edge counter must advance through SubEdgeAdvance (once)"
    edge_guard_fail=1
fi
if [ "$(grep -c 'SubEdgeUnacked' "$PROV_SRC")" != 1 ]; then
    echo "FAIL $PROV_SRC: the send must be gated on SubEdgeUnacked (once)"
    edge_guard_fail=1
fi
if [ "$(grep -rc '\.EdgeDue\|bool EdgeDue' src --include=*.h | grep -vc ':0$')" != 0 ]; then
    echo "FAIL: EdgeDue is gone - a send state cleared at the send is what dropped a lost edge update"
    edge_guard_fail=1
fi
if [ "$(grep -c 'SubEdgeAnswers(Entry->Hash, Answer)' "$PROV_SRC")" != 1 ]; then
    echo "FAIL $PROV_SRC: an edge confirmation must be matched with SubEdgeAnswers (once)"
    edge_guard_fail=1
fi
if [ "$(grep -c 'SubEdgeConfirmWord' "$PROV_SRC")" != 1 ]; then
    echo "FAIL $PROV_SRC: the same-device send must be confirmed locally (SubEdgeConfirmWord, once)"
    edge_guard_fail=1
fi
if [ "$(grep -c 'FLAG_REQACK) SubscriptionsConfirmEdge(Entry, Counter)' "$REQ_SRC")" != 1 ]; then
    echo "FAIL $REQ_SRC: an edge push must be acknowledged when the provider asks for it"
    edge_guard_fail=1
fi
if [ "$(grep -c 'SubscriptionsConfirmEdge(RequesterEntry' "$REQ_SRC")" != 1 ]; then
    echo "FAIL $REQ_SRC: the edge acknowledgement must exist in exactly one place"
    edge_guard_fail=1
fi
if [ "$(grep -c 'SubEdgeConfirmWord(Counter)' "$REQ_SRC")" != 1 ]; then
    echo "FAIL $REQ_SRC: the acknowledgement must carry the counter it answers"
    edge_guard_fail=1
fi
[ $edge_guard_fail = 0 ] && echo "PASS: counter-only payload, one target writer, the pulse driven from Tick"
fail=$((fail + edge_guard_fail))
echo

# t_5da34b15, structural half: the requester row now carries the documented Hash/Hashlike
# (Docs/Services/Subscriptions.md "Requester Table Entry": uint32, after the timeout), and by owner
# ruling (2026-10-27) it is a WIRE field only - never in `.SUBREQ`. The sites that keep it are
# device-side and do not link on the host, so the facts are asserted on the source and on the
# layout:
#   - an edge row mirrors its counter from the state machine AFTER the classification
#     (Entry->Hash = Entry->Edge.Last), so a duplicate cannot move it and the wire cannot disagree
#     with the accounting;
#   - a value row keeps the last value it applied - the value itself little-endian when it fits the
#     32-bit field (Bool/Index/Uint32/Number, the types the docs name), its FNV-1a fingerprint
#     otherwise - gated on NOT being an edge row, so a value can never overwrite an edge counter;
#   - a Set and a recall start it empty, so nothing the device missed is replayed as pulses;
#   - the field is never written into a target and never decides a send;
#   - the wire entry is 32 B with the hash at offset 28, while the `.SUBREQ` record stays 24 B and
#     ends before the timeout/hash. SubscriptionsDefs.h pins all three with static_asserts at build
#     time; the defines are re-checked here so an edit cannot slip past a stale binary.
echo "### source guard: the requester Hash/Hashlike is a wire field of the row, never persisted"
HASH_DEFS_SRC="src/Core/Services/SubscriptionsDefs.h"
HASH_REQ_SRC="src/Core/Services/SubscriptionsRequester.h"
HASH_CTL_SRC="src/Core/Services/SubscriptionsControl.h"
HASH_PERSIST_SRC="src/Core/Services/SubscriptionsPersist.h"
hash_fail=0
if [ "$(grep -c '#define REQUESTER_ENTRY_WIRE_SIZE 32' "$HASH_DEFS_SRC")" != 1 ]; then
    echo "FAIL $HASH_DEFS_SRC: the requester wire entry is 32 B (the hash is the 6th field)"
    hash_fail=1
fi
if [ "$(grep -c '#define REQUESTER_FILE_ENTRY_SIZE 24' "$HASH_DEFS_SRC")" != 1 ]; then
    echo "FAIL $HASH_DEFS_SRC: the .SUBREQ record stays 24 B (the hash is not persisted)"
    hash_fail=1
fi
if [ "$(grep -c 'offsetof(RequesterEntry, Hash) == 28' "$HASH_DEFS_SRC")" != 1 ]; then
    echo "FAIL $HASH_DEFS_SRC: the hash must sit at wire offset 28, right after the timeout"
    hash_fail=1
fi
if [ "$(grep -c 'REQUESTER_FILE_ENTRY_SIZE == offsetof(RequesterEntry, Timeout)' "$HASH_DEFS_SRC")" != 1 ]; then
    echo "FAIL $HASH_DEFS_SRC: the .SUBREQ record must end before the timeout/hash (pinned by an assert)"
    hash_fail=1
fi
if [ "$(grep -c 'Entry->Hash = Entry->Edge.Last;' "$HASH_REQ_SRC")" != 1 ]; then
    echo "FAIL $HASH_REQ_SRC: an edge row's hash must mirror the state machine's counter (once)"
    hash_fail=1
fi
if [ "$(grep -c 'Entry->Hash = Fnv1a(Val, Vlen);' "$HASH_REQ_SRC")" != 1 ]; then
    echo "FAIL $HASH_REQ_SRC: a value too wide for the field must be kept as its FNV-1a fingerprint (once)"
    hash_fail=1
fi
if [ "$(grep -c 'Entry->Hash = Seen;' "$HASH_REQ_SRC")" != 1 ]; then
    echo "FAIL $HASH_REQ_SRC: a value that fits the field must be kept as the value itself (once)"
    hash_fail=1
fi
if [ "$(grep -c 'if (!SubTriggerIsEdge(Entry->Sub.Trigger)) {' "$HASH_REQ_SRC")" != 1 ]; then
    echo "FAIL $HASH_REQ_SRC: the value hash must not overwrite an edge row's counter"
    hash_fail=1
fi
if [ "$(grep -c 'Entry->Hash = 0;' "$HASH_CTL_SRC")" != 1 ]; then
    echo "FAIL $HASH_CTL_SRC: a Set must start the row's hash empty (once)"
    hash_fail=1
fi
if [ "$(grep -c 'Entry->Hash = 0;' "$HASH_PERSIST_SRC")" != 1 ]; then
    echo "FAIL $HASH_PERSIST_SRC: a recall must start the row's hash empty (once)"
    hash_fail=1
fi
# The file record copies the wire prefix up to the timeout, and nothing more.
if [ "$(grep -c 'memcpy(Buf + Off, Entry, REQUESTER_FILE_ENTRY_SIZE)' "$HASH_PERSIST_SRC")" != 2 ]; then
    echo "FAIL $HASH_PERSIST_SRC: the save must copy exactly REQUESTER_FILE_ENTRY_SIZE bytes (2 sites)"
    hash_fail=1
fi
if [ "$(grep -c 'memcpy((void \*)Entry, Buf + Off, REQUESTER_FILE_ENTRY_SIZE)' "$HASH_PERSIST_SRC")" != 1 ]; then
    echo "FAIL $HASH_PERSIST_SRC: the recall must read exactly REQUESTER_FILE_ENTRY_SIZE bytes"
    hash_fail=1
fi
if [ "$(grep -c 'REQUESTER_ENTRY_WIRE_SIZE' "$HASH_PERSIST_SRC")" != 0 ]; then
    echo "FAIL $HASH_PERSIST_SRC: the wire entry size must not reach the .SUBREQ codec"
    hash_fail=1
fi
# Bookkeeping only: never handed to a target write, never a send gate.
if [ "$(grep -rc '&Entry->Hash' src --include=*.h | grep -vc ':0$')" != 0 ]; then
    echo "FAIL: the hash must never be written into a target (no &Entry->Hash anywhere)"
    hash_fail=1
fi
if [ "$(grep -rc 'if (Entry->Hash' src --include=*.h | grep -vc ':0$')" != 0 ]; then
    echo "FAIL: the requester's hash must never decide a send (it is bookkeeping)"
    hash_fail=1
fi
[ $hash_fail = 0 ] && echo "PASS: hash at wire offset 28 (32 B entry), .SUBREQ still 24 B and hash-free"
fail=$((fail + hash_fail))
echo

# t_eb35360c, structural half: a host test cannot compile RequesterSet's cancel branch or Tick (both
# pull the packet/register/storage layer), so the two facts the owner's ruling depends on are
# asserted on the source itself - Tick has no time-based requester drop, and the ONLY removal of a
# requester row is the explicit-cancel branch of RequesterSet, which also cancels the provider side.
# A failure here is either the expiry coming back or the cancel path being lost with it.
# t_e7e40f2e, structural half: a renewal is a plain 0401 with the same entry (owner ruling,
# 2026-10-25), so an EXISTING provider entry may only have its TIMEOUT extended - no hash/counter,
# value, last-sent or trigger reset - and a renewal's answer may neither re-baseline the requester's
# edge counter nor write its target. The reset decision itself is host-clean code
# (SubscriptionsProviderReset.h) and is driven by renewal_test.cpp; what is asserted here is that the
# device goes THROUGH it: the 0401 call site passes Reset = false, the explicit 0421 Set keeps
# Reset = true, and ProviderApply gates its wipe on SubProviderReinstallResets. The requester's edge
# branch asks SubEdgeAnswer for the decision, keeps its single ResetBaseline in the Install branch,
# and never treats an unanswered renewal as an install.
echo "### source guard: a renewal extends the timeouts and resets no state"
RENEW_REQ_SRC="src/Core/Services/SubscriptionsRequester.h"
RENEW_CTL_SRC="src/Core/Services/SubscriptionsControl.h"
RENEW_PROV_SRC="src/Core/Services/SubscriptionsProvider.h"
renew_fail=0
if [ "$(grep -c 'ProviderApply(Entry, Frame.Trid, Frame.IdSrc, Table, false);' "$RENEW_CTL_SRC")" != 1 ]; then
    echo "FAIL $RENEW_CTL_SRC: the 0401 (renewal) path must extend the entry, not reset it"
    renew_fail=1
fi
if [ "$(grep -c 'ProviderApply(Entry, Frame.Trid, Frame.IdSrc, Table, true);' "$RENEW_CTL_SRC")" != 0 ]; then
    echo "FAIL $RENEW_CTL_SRC: no 0401 may reset the provider entry"
    renew_fail=1
fi
if [ "$(grep -c 'ProviderInstall(Frame.Trid, RequesterAddr, Table, true)' "$RENEW_CTL_SRC")" != 1 ]; then
    echo "FAIL $RENEW_CTL_SRC: the explicit provider Set (0421) must still re-install with a reset"
    renew_fail=1
fi
if [ "$(grep -c 'SubProviderReinstallResets(Reset, Entry->RequesterAddr == 0,' "$RENEW_PROV_SRC")" != 1 ]; then
    echo "FAIL $RENEW_PROV_SRC: ProviderApply's reset decision must be the host-tested SubProviderReinstallResets"
    renew_fail=1
fi
if [ "$(grep -c 'if (ResetState)' "$RENEW_PROV_SRC")" != 1 ]; then
    echo "FAIL $RENEW_PROV_SRC: the trigger-state wipe must be gated on the reset decision"
    renew_fail=1
fi
if [ "$(grep -c 'Reset || TargetChanged' "$RENEW_PROV_SRC")" != 0 ]; then
    echo "FAIL $RENEW_PROV_SRC: the old reset rule (source-only) is back; a changed trigger must reset too"
    renew_fail=1
fi
if [ "$(grep -c 'SubEdgeAnswer(Entry->Lease.Served())' "$RENEW_REQ_SRC")" != 1 ]; then
    echo "FAIL $RENEW_REQ_SRC: the install/renewal decision must be SubEdgeAnswer(Served())"
    renew_fail=1
fi
if [ "$(grep -c 'SubEdgeAnswerKind::Install' "$RENEW_REQ_SRC")" != 1 ]; then
    echo "FAIL $RENEW_REQ_SRC: only the install answer may re-baseline the counter"
    renew_fail=1
fi
if [ "$(grep -c 'Edge.ResetBaseline()' "$RENEW_REQ_SRC")" != 1 ]; then
    echo "FAIL $RENEW_REQ_SRC: the counter baseline may be reset in exactly one place (the install)"
    renew_fail=1
fi
if [ "$(grep -c 'Unanswered != 0' "$RENEW_REQ_SRC")" != 0 ]; then
    echo "FAIL $RENEW_REQ_SRC: an unanswered RENEWAL must not turn a press into a new baseline"
    renew_fail=1
fi
# The renewal branch itself: it must extend the lease AND the deadline, and touch nothing else.
renew_branch=$(sed -n '/A renewal: a plain 0401/,/return;/p' "$RENEW_REQ_SRC")
case "$renew_branch" in
    *'Entry->Lease.OnAnswer('*'Entry->Timeout = SubTimeoutFrom('*) ;;
    *)
        echo "FAIL $RENEW_REQ_SRC: the renewal answer must extend the lease and the row's timeout"
        renew_fail=1
        ;;
esac
case "$renew_branch" in
    *'Edge.'*|*'ApplyRequesterValue'*)
        echo "FAIL $RENEW_REQ_SRC: the renewal answer must not touch the edge accounting or the target"
        renew_fail=1
        ;;
esac
[ $renew_fail = 0 ] && echo "PASS: 0401 extends only, 0421 resets, the install is the only re-baseline"
fail=$((fail + renew_fail))
echo

echo "### source guard: requester rows are dropped only by an explicit cancel"
SUBS_SRC="src/Core/Services/SubscriptionsControl.h"
tick_body=$(sed -n '/inline void SubscriptionsService::Tick/,$p' "$SUBS_SRC")
cancel_body=$(sed -n '/Set subscription (requester)/,/Recall all subscriptions (requester)/p' "$SUBS_SRC")
guard_fail=0
if printf '%s\n' "$tick_body" | grep -n 'RequesterRemove\|SubscriptionsCancelProvider\|entr(ies) expired'; then
    echo "FAIL $SUBS_SRC: Tick still drops requester rows (see the lines above)"
    guard_fail=1
fi
if [ "$(printf '%s\n' "$cancel_body" | grep -c 'RequesterRemove')" != 1 ]; then
    echo "FAIL $SUBS_SRC: RequesterSet's cancel branch must remove the row exactly once"
    guard_fail=1
fi
if [ "$(printf '%s\n' "$cancel_body" | grep -c 'SubscriptionsCancelProvider')" != 1 ]; then
    echo "FAIL $SUBS_SRC: the explicit cancel must also cancel the provider side"
    guard_fail=1
fi
if [ "$(grep -c 'RequesterRemove' "$SUBS_SRC")" != 1 ]; then
    echo "FAIL $SUBS_SRC: the only requester-removal call site must be the cancel branch"
    guard_fail=1
fi
[ $guard_fail = 0 ] && echo "PASS: no time-based requester drop, 1 removal (explicit cancel) and no other"
fail=$((fail + guard_fail))
echo

# t_62e8f3d3, structural half: the give-up line is emitted from RegisterRequesterProvider, which
# pulls the packet/register/storage layer and cannot be compiled on the host. Assert the emit site
# still exists and is still gated on the once-per-episode flag - a bare DeviceLog inside the retry
# path would print on every send after the cap, which is exactly what the flag exists to prevent.
echo "### source guard: the give-up line is emitted once per episode from the requester's send"
PERSIST_SRC="src/Core/Services/SubscriptionsPersist.h"
gaveup_block=$(sed -n '/if (Entry->Lease.ShouldReportGaveUp())/,/^    }/p' "$PERSIST_SRC")
gaveup_fail=0
if printf '%s\n' "$gaveup_block" | grep -q 'DeviceLog(SUB_GAVE_UP_TAG'; then
    echo "PASS: gated on ShouldReportGaveUp and logged with the SUB tag"
else
    echo "FAIL $PERSIST_SRC: no DeviceLog(SUB_GAVE_UP_TAG, ...) inside the ShouldReportGaveUp() block"
    gaveup_fail=1
fi
if [ "$(grep -c 'OnGaveUpReported' "$PERSIST_SRC")" != 1 ]; then
    echo "FAIL $PERSIST_SRC: the flag must be set exactly once, at the emit site"
    gaveup_fail=1
fi
# The message is formatted, never inlined per call site: one owner of the argument order, and the
# text itself is pinned by lease_test.cpp through the same helper.
if [ "$(grep -c 'SubFormatGaveUp' "$PERSIST_SRC")" != 1 ]; then
    echo "FAIL $PERSIST_SRC: the line must be formatted through SubFormatGaveUp (SUB_GAVE_UP_FMT)"
    gaveup_fail=1
fi
fail=$((fail + gaveup_fail))
echo

echo "### native reset-cause tests (the DAS boot report's flag byte)"
build_run "resetcause" "reset_test.cpp"
echo

# t_1381f097: the rail flags are computed in each device's sample loop (which pulls the chip
# header and the block instances, so the host cannot link it). The predicate and the block layout
# are covered by meas_rail_test.cpp; what remains is the WIRING, asserted on the source itself:
# the DAS judges both flags from (the active range, the raw input sample of this pass) in the pass
# that publishes Measured Value / Current Range, and never from the range the auto-range picks later
# - nor from the EMA output, which would hide a rail for as long as its time constant (t_26e15feb);
# no other code writes the two fields; and the Valu (one fixed reference, no range chain) keeps
# both false.
echo "### source guard: the rail flags are computed per sample in the sample loops"
DAS_RUN="src/Devices/DAS_v0.1/MeasuringRun.h"
VALU_RUN="src/Devices/Valu_v2.0/MeasuringRun.h"
MEAS_SRC="src/Core/Devices/Measuring.h"
rail_fail=0
if [ "$(grep -c 'StaticVol.Meas\[Index\].OpenCircuit = MeasOpenCircuit(Range, Number(Raw));' "$DAS_RUN")" != 1 ]; then
    echo "FAIL $DAS_RUN: OpenCircuit must be set once, from MeasOpenCircuit(Range, Number(Raw)) - the raw input, not FilteredRaw"
    rail_fail=1
fi
if [ "$(grep -c 'StaticVol.Meas\[Index\].ShortCircuit = MeasShortCircuit(Range, Number(Raw));' "$DAS_RUN")" != 1 ]; then
    echo "FAIL $DAS_RUN: ShortCircuit must be set once, from MeasShortCircuit(Range, Number(Raw)) - the raw input, not FilteredRaw"
    rail_fail=1
fi
# The filtered sample must not reach either predicate: the flags are the INPUT at the limits, and
# the auto-range beside them already judges Raw.
if [ "$(grep -c 'MeasOpenCircuit(Range, FilteredRaw)\|MeasShortCircuit(Range, FilteredRaw)' "$DAS_RUN")" != 0 ]; then
    echo "FAIL $DAS_RUN: the rail flags must not be judged from the EMA output (FilteredRaw)"
    rail_fail=1
fi
# Same pass as the published value: the flags are set after Measured Value, and before the
# auto-range block that may change the range for the NEXT sample.
meas_line=$(grep -n 'StaticVol.Meas\[Index\].MeasuredValue = In;' "$DAS_RUN" | cut -d: -f1)
open_line=$(grep -n 'StaticVol.Meas\[Index\].OpenCircuit = MeasOpenCircuit' "$DAS_RUN" | cut -d: -f1)
short_line=$(grep -n 'StaticVol.Meas\[Index\].ShortCircuit = MeasShortCircuit' "$DAS_RUN" | cut -d: -f1)
range_line=$(grep -n 'uint8_t NewRange = Range;' "$DAS_RUN" | cut -d: -f1)
if [ -z "$meas_line" ] || [ -z "$open_line" ] || [ -z "$short_line" ] || [ -z "$range_line" ] \
   || [ "$open_line" -le "$meas_line" ] || [ "$short_line" -le "$meas_line" ] || [ "$range_line" -le "$short_line" ]; then
    echo "FAIL $DAS_RUN: the flags must be set after Measured Value and before the auto-range step"
    rail_fail=1
fi
# No other writer of the two fields anywhere in the firmware: they are outputs of the loop.
if [ "$(grep -rn '\.OpenCircuit =' src --include=*.h | grep -vc "$DAS_RUN")" != 1 ] \
   || [ "$(grep -rn '\.ShortCircuit =' src --include=*.h | grep -vc "$DAS_RUN")" != 1 ]; then
    echo "FAIL: the only writers of OpenCircuit/ShortCircuit must be DAS MeasuringRun.h and the Valu's false"
    rail_fail=1
fi
if [ "$(grep -c 'StaticVol.Meas\[Index\].OpenCircuit = false;' "$VALU_RUN")" != 1 ] \
   || [ "$(grep -c 'StaticVol.Meas\[Index\].ShortCircuit = false;' "$VALU_RUN")" != 1 ]; then
    echo "FAIL $VALU_RUN: the Valu has no range chain, so both flags must be set false"
    rail_fail=1
fi
# The flags are outputs: no trigger may be registered for fields 5/6.
if grep -q 'MakeFieldKey(5, 0)' "$MEAS_SRC" && [ "$(grep -c 'MakeFieldKey(5, 0), On' "$MEAS_SRC")" != 0 ]; then
    echo "FAIL $MEAS_SRC: field 5 is Read Only; no trigger may be registered for it"
    rail_fail=1
fi
if [ "$(grep -c 'MakeFieldKey(6, 0), On' "$MEAS_SRC")" != 0 ]; then
    echo "FAIL $MEAS_SRC: field 6 is Read Only; no trigger may be registered for it"
    rail_fail=1
fi
[ $rail_fail = 0 ] && echo "PASS: both flags are computed per sample from the active range and the same raw sample"
fail=$((fail + rail_fail))
echo

echo "### native measurement rail-flag tests (open/short truth table + locked offsets/entries)"
build_run "measrail" "meas_rail_test.cpp"
echo

# t_156125fc: the Raw Resistance unit transform. Built in both Number shapes because the defect it
# pins (multiply-before-divide in Q16.16) wraps in the DAS's 32-bit FixedMul32 path AND in the
# core's 64-bit one - the wrap happens in the assignment back to int32, not in the multiply helper.
echo "### native raw-resistance transform tests (window 1..1022 x 3 references, DAS 32-bit shape)"
build_run "meastransform_das" "meas_transform_test.cpp" -DNUMBER_ONLY_32BIT -DSCALAR_ONLY
echo

echo "### native raw-resistance transform tests (same contract, 64-bit Number shape)"
build_run "meastransform_core" "meas_transform_test.cpp"
echo

exit $fail
