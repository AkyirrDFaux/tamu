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
#                     (Core/Functions/MemoryBlocks.h), so the target keeps its value - except a
#                     zero-length write of a dictionary type, which CREATES the field's key-0 marker
#                     row (ValueMarkerCreatable, Core/Functions/MemoryTypes.h) so an appended render
#                     part can be classified
#   register_read_test.cpp  a row with no bytes is unreadable - a read of it fails the access
#                     (REGISTER), it is not an empty value whose type then complains (the read-side
#                     ValueReadable, Core/Functions/MemoryTypes.h)
#   vysi_render_kind_test.cpp  the real Vysi1Render.h: a render part is classified by its field's
#                     key-0 dictionary marker (kind 1 geometry / 2 texture), so a field whose marker
#                     was created by an empty host write applies and one without a marker does not
#   lease_test.cpp    the requester row's lifetime and install/keepalive cadence (Core/Services/
#                     SubscriptionsLease.h): rows are never dropped on time, an unanswered install
#                     retries at SUB_RETRY_MS, capped at 5, then the keepalive, and an answer
#                     resets the counter; the once-per-episode give-up line and its exact text -
#                     plus a source guard that the only removal of a row is the explicit-cancel
#                     branch of RequesterSet
#   meas_transform_test.cpp  the Raw Resistance unit transform over the whole usable ADC window
#                     (Core/Devices/MeasuringResistance.h), in both Number shapes
#   wifi_settings_test.cpp  the WiFi Settings block's entry table (types/offsets/flags/triggers), the
#                     write-only password (a read answers empty, never the stored text) and the
#                     configuration-only State rule; plus System field 8 as an interface BIT FIELD
#                     (Core/Types/Enums.h AppActiveBits)
#   netid_test.cpp    the core's net-ID rule (Core/Functions/NetId.h): a write ARMS the move and
#                     keeps the old identity until its reply is out (a reply from the new address
#                     cannot be matched by the client that addressed the old one), then moves the
#                     live net and the core's NetID.1 address together, 0/0x3F are refused, and the
#                     value the save paths mirror equals the one stored - so a Save All cannot
#                     revert it
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

    # t_4c9c5fa8: bytes taken off the UART while a transmit waits for the line are kept for the
    # assembler (BusRxKeepRing), not drained away. Built in both Crc8 shapes because the assembler
    # validates with it.
    echo "### native RX-keep tests (a reply kept across a transmit), $label"
    build_run "rxkeep_${SPEED:+speed}" "bus_rx_keep_test.cpp" $SPEED
    echo

    echo "### native geometry tests (LED display mask shapes), $label"
    build_run "geometry_${SPEED:+speed}" "geometry_test.cpp" $SPEED
    echo
done

# t_763b4cde / t_d7802076: retransmission is why a duplicated Save All filled a node's file table.
# The guard (Core/Functions/BusRepeat.h) and the address-phase backoff (Bus.h RS485BackoffMs) are
# host-clean, so the real code is driven here: which CIDs are guarded, that a repeat inside the
# window answers from the record while anything else is a new transaction, and that two nodes whose
# addresses differ draw different delays while they are still in phase (the lockstep case).
echo "### native bus-retransmission tests (repeat guard + backoff decorrelation)"
build_run "busrepeat" "bus_repeat_test.cpp"
echo

# t_8dead2f1: the same guard, but exercised through a RECEIVING SERVICE - the real
# StorageService::Handle over the real StorageBlockFS on a fake 512 B NOR flash in the DAS geometry -
# so "the request executed once" is measured on the store (the file table's append position: the
# first slot whose offset is 0xFFFFFFFF), which is exactly what a duplicated file-table command
# spends. GREEN: a repeat appends nothing and is answered with the original reply, while the same
# bytes under a new TRID append +1 (so the second frame was not merely dropped). The RED run
# (.pio/probe/red_repeat_path.py) fails those assertions with the ledger switched off.
echo "### native retransmission dedupe through the storage service (a repeat appends nothing)"
build_run "storagerepeat" "storage_repeat_test.cpp" -DBOARD_DAS_v0_1 -DSTORAGE_FLASH_SIZE=512 -DSTORAGE_BLOCK_SIZE=64 -DMEMORY_BACKUP_CAP=128
echo

# The dispatcher half is device-layer (it pulls every service) and cannot be linked on the host, so
# the wiring is pinned on the source: a repeat must be ANSWERED from the record and must SKIP the
# service switch, the outcome must be recorded where the reply is built, and no retry site may still
# draw its backoff from the lockstep expression.
echo "### source guard: a retransmission is answered, never executed twice"
REPEAT_FAIL=0
REGISTER_SRC="src/Core/Services/RegisterDispatch.h"
STORAGE_SRC="src/Core/Services/Storage.h"
REPLY_SRC="src/Core/Functions/MemoryBackup.h"
# The two services that own the guarded CIDs must ask the guard BEFORE executing, answer from the
# record, and not fall through to the handler.
for SVC in "$REGISTER_SRC" "$STORAGE_SRC"; do
    if ! grep -q 'BusRepeatGuarded(Frame.Cmd) && BusRepeatHit(Frame, (uint16_t)TimeFromBoot())' "$SVC"; then
        echo "FAIL $SVC: a repeat is no longer consulted before the command executes"
        REPEAT_FAIL=1
    fi
    if ! grep -q 'SendResponse(Frame, nullptr, 0, BusRepeatStatus())' "$SVC"; then
        echo "FAIL $SVC: a repeat must be ANSWERED from the record, or the requester times out"
        REPEAT_FAIL=1
    fi
    if ! grep -A 6 'BusRepeatHit(Frame, (uint16_t)TimeFromBoot())' "$SVC" | grep -q 'return true;'; then
        echo "FAIL $SVC: the replay must return, or the command executes anyway"
        REPEAT_FAIL=1
    fi
done
if ! grep -q 'BusRepeatNote(Frame, Status, Len' "$REPLY_SRC"; then
    echo "FAIL $REPLY_SRC: SendResponse no longer records the outcome a repeat is answered from"
    REPEAT_FAIL=1
fi
# No retry site may still draw its backoff from the lockstep low-nibble expression. The pattern is
# anchored to lines that do not start a comment, so the headers may keep explaining the old bug.
for RSBUS in src/Devices/Tamu_v2.0A/RSBus.h src/Devices/DAS_v0.1/RSBus.h; do
    if grep -qE '^[^/]*RawRand\(\) *% *16' "$RSBUS"; then
        echo "FAIL $RSBUS: the lockstep low-nibble backoff is back"
        REPEAT_FAIL=1
    fi
done
# Every retry site uses the shared helper: Tamu 2 (packet + raw bootloader), DAS 1.
if [ "$(grep -c 'RS485BackoffMs(' src/Devices/Tamu_v2.0A/RSBus.h)" != 2 ] ||
   [ "$(grep -c 'RS485BackoffMs(' src/Devices/DAS_v0.1/RSBus.h)" != 1 ]; then
    echo "FAIL: every retry site must use RS485BackoffMs (Tamu 2: packet + raw bootloader, DAS 1)"
    REPEAT_FAIL=1
fi
# t_d7802076: the helper's address term must FOLD the whole short address. A group read from the low
# address bits only (`(Addr * 5 + Attempt) & 7`, which shipped in 52c07aa) is a lockstep of period 8 -
# nodes 1 and 9, 0.2 and 0.10, every pair 8 apart, drew the identical delay for ever.
BACKOFF_SRC="src/Core/Functions/Bus.h"
if ! grep -q 'Addr >> 5' "$BACKOFF_SRC" || ! grep -q 'Addr >> 10' "$BACKOFF_SRC"; then
    echo "FAIL $BACKOFF_SRC: the backoff no longer folds the whole short address"
    REPEAT_FAIL=1
fi
if grep -qE 'Addr \* 5u? *\+ *\(uint32_t\)Attempt' "$BACKOFF_SRC"; then
    echo "FAIL $BACKOFF_SRC: the mod-8 group backoff is back"
    REPEAT_FAIL=1
fi
if [ "$REPEAT_FAIL" = 0 ]; then
    echo "PASS: both services answer a repeat from the record, SendResponse records it, and no retry site keeps the lockstep draw"
fi
if [ "$REPEAT_FAIL" != 0 ]; then fail=$((fail + 1)); fi
echo

echo "### native alignment tests (PacketFrame alignment + unaligned buffer access)"
build_run "align" "align_test.cpp"
echo

# The panel's LED count is the layout file's: 86-index and 87-index layouts must load on the SAME
# binary (the count is the highest index + 1, and the per-panel send follows it), while a cell one
# past the chain capacity is a named refusal instead of a silent 0xFFFF.
echo "### native Vysi layout-file tests (86- and 87-LED layouts in one binary; over-capacity is named)"
build_run "vysilayout" "vysi1_layout_test.cpp"
# The test models the board's chain capacity as its own constant because Vysi1Layout.h pulls the
# register bus and cannot be linked on the host. Keep the modelled figure equal to the header's.
if ! grep -q 'static const uint32_t LedNum = 87;' src/Blocks/Vysi1Layout.h; then
    echo "FAIL src/Blocks/Vysi1Layout.h: LedNum is no longer 87 - update Capacity in test/native/vysi1_layout_test.cpp"
    fail=$((fail + 1))
fi
echo

# t_10273fcc, structural half: NO layout is pre-loaded. Each panel's map is the FILE its "Layout
# File Name" register names - loaded at boot (after the persistent recall, which restores the field
# but does not re-run its write trigger) and on every write of that register. The board's Main.h
# cannot be linked on the host (it pulls the ESP-IDF, the app link and the whole static memory), so
# the invariant is asserted on the source:
#   - the display starts with NO map, the only writer of a map is LoadLayoutFromStorage (fed by a
#     file), and ClearLayout writes "no LED" only - no built-in table is ever installed;
#   - the compiled-in table is referenced only outside the class (PreloadVysiLayout writes it into
#     STORAGE as LAY_1 on a Valu; MaxBuiltinLayoutIndex checks it at compile time), never by a
#     member that could install it;
#   - the boot load runs once per panel, after LoadAllBackups() and before the first Render;
#   - a refusal (blank name, missing/bad file) clears the map, so the panel renders nothing.
echo "### source guard: no layout is pre-loaded - each panel's map is the file its register names"
VYSI_MAIN_SRC="src/Devices/Tamu_v2.0A/Main.h"
VYSI_LAYOUT_SRC="src/Blocks/Vysi1Layout.h"
vysi_fail=0

if grep -q 'LoadDefaultLayout' "$VYSI_LAYOUT_SRC"; then
    echo "FAIL $VYSI_LAYOUT_SRC: a compiled-in layout install path is back (LoadDefaultLayout)"
    vysi_fail=1
fi

# The class body (the only code that can touch a display's map) must not name the built-in table.
class_start=$(grep -n '^class Vysi1Display' "$VYSI_LAYOUT_SRC" | cut -d: -f1)
class_end=$(awk -v s="$class_start" 'NR > s && /^};/ {print NR; exit}' "$VYSI_LAYOUT_SRC")
if [ -z "$class_start" ] || [ -z "$class_end" ]; then
    echo "FAIL $VYSI_LAYOUT_SRC: could not delimit class Vysi1Display"
    vysi_fail=1
else
    inside=$(grep -n 'LayoutVysiv1_0' "$VYSI_LAYOUT_SRC" | cut -d: -f1 |
             awk -v s="$class_start" -v e="$class_end" '$1 > s && $1 < e')
    if [ -n "$inside" ]; then
        echo "FAIL $VYSI_LAYOUT_SRC: the built-in table is used inside the display class (line(s) $(echo $inside)) -"
        echo "      a member could install it on a panel; only PreloadVysiLayout (a storage write) may read it"
        vysi_fail=1
    fi
fi

# Every write to the map is either ClearLayout's "no LED" or the loader's committed file cells.
map_writes=$(grep -cE '^ +Layout\[[^]]*\] = ' "$VYSI_LAYOUT_SRC" || true)
if [ "$map_writes" != 2 ] ||
   ! grep -qE '^ +Layout\[[^]]*\] = Vysi1LayoutFile::Unused;' "$VYSI_LAYOUT_SRC" ||
   ! grep -qE '^ +Layout\[[^]]*\] = Pending\[i\];' "$VYSI_LAYOUT_SRC"; then
    echo "FAIL $VYSI_LAYOUT_SRC: the map must only be written by ClearLayout (no LED) and LoadLayoutFromStorage (file cells) - found $map_writes write(s)"
    vysi_fail=1
fi

# The constructor starts the panel with no map, and the boot apply clears it on a refusal.
if [ "$(grep -c 'ClearLayout();' "$VYSI_LAYOUT_SRC" || true)" != 2 ] ||
   ! sed -n '/^    Vysi1Display(Vysi1Volatile &Vol/,/^    }/p' "$VYSI_LAYOUT_SRC" | grep -q 'ClearLayout();'; then
    echo "FAIL $VYSI_LAYOUT_SRC: the display must start with no map and the boot apply must clear on a refusal (expected exactly ClearLayout() in the constructor and in ApplyLayoutFromStorage)"
    vysi_fail=1
fi
apply_body=$(sed -n '/^inline Vysi1LayoutFile::Result ApplyLayoutFromStorage/,/^}/p' "$VYSI_LAYOUT_SRC")
if ! printf '%s\n' "$apply_body" | grep -q 'Disp.LoadLayoutFromStorage();' ||
   ! printf '%s\n' "$apply_body" | grep -q 'Disp.ClearLayout();'; then
    echo "FAIL $VYSI_LAYOUT_SRC: ApplyLayoutFromStorage must load the named file and clear the map (not install a default) when it cannot"
    vysi_fail=1
fi
if ! sed -n '/Vysi1LayoutFile::Result LoadLayoutFromStorage()/,/^    }/p' "$VYSI_LAYOUT_SRC" | grep -q 'Result::NoName'; then
    echo "FAIL $VYSI_LAYOUT_SRC: a blank Layout File Name must answer NoName (no map), not a built-in default"
    vysi_fail=1
fi

# Boot order: one apply site, both panels, after the persistent recall and before the first Render.
if [ "$(grep -c 'ApplyLayoutFromStorage(' "$VYSI_MAIN_SRC" || true)" != 1 ] ||
   ! grep -q 'Vysi1Display \*Panels\[2\] = {&Display1, &Display2};' "$VYSI_MAIN_SRC"; then
    echo "FAIL $VYSI_MAIN_SRC: the boot must apply each panel's layout in exactly one place, covering BOTH displays"
    vysi_fail=1
fi
recall_line=$(grep -n '^    LoadAllBackups();' "$VYSI_MAIN_SRC" | cut -d: -f1)
boot_layout_line=$(grep -n 'ApplyLayoutFromStorage(' "$VYSI_MAIN_SRC" | cut -d: -f1)
render_line=$(grep -n 'Display1.Render();' "$VYSI_MAIN_SRC" | head -1 | cut -d: -f1)
if [ -z "$recall_line" ] || [ -z "$boot_layout_line" ] || [ -z "$render_line" ] ||
   [ "$boot_layout_line" -le "$recall_line" ] || [ "$boot_layout_line" -ge "$render_line" ]; then
    echo "FAIL $VYSI_MAIN_SRC: the layout must be loaded from storage after the persistent recall and before the first Render"
    vysi_fail=1
fi
[ $vysi_fail = 0 ] && echo "PASS: both panels load the file their register names at boot, nothing built-in is installed, and a refusal leaves no map"
fail=$((fail + vysi_fail))
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

# t_79a194d5: the pairing-free BLE updater's recovery path. The factory image's boot decision
# and the update window are pure functions, so "a bad image stays recoverable" is run here as a
# sequence of resets rather than only being read off the source.
echo "### native updater tests (boot budget / recovery decision + the update window)"
build_run "updaterboot" "updater_boot_test.cpp"
echo

echo "### native TRID tests (System/Log counter range + increment/wrap)"
build_run "trid" "trid_test.cpp"
echo

echo "### native log layout tests (LogMessage source/code/timestamp)"
build_run "log" "log_test.cpp"
echo

echo "### native value-update tests (a 0-length value is refused; a dictionary marker is created)"
build_run "valueupdate" "value_update_test.cpp"
echo

echo "### native register-read tests (a row with no bytes is unreadable, not a type mismatch)"
build_run "registerread" "register_read_test.cpp"
echo

# t_1e301bfb: a DAS Save All is one `.SV` write, and the file table is append-only - so a save
# that needs a table move needs two CONTIGUOUS 64 B blocks while its own staging file holds one.
# A store whose free blocks are single-block islands therefore refuses every save, for ever
# (measured on node 0.4: 0x4E three times in a row). The real StorageBlockFS.h runs here over a
# fake 512 B NOR flash, in the DAS geometry, and the test builds those stores: an ordinary store
# keeps the atomic path; a node-shaped store (`.SV` + two 1-block files, full table) must RECLAIM
# - compacted table, a plain CreateFile working again - and then take a 20-save run; the island
# store, where no move is placeable at all, must save 20 times in place and is asserted to stay
# stuck for other writers (the stated residual of that last resort).
echo "### native storage backup-write tests (a stuck store reclaims or still saves)"
build_run "storagebackup" "storage_backup_test.cpp" -DBOARD_DAS_v0_1 -DSTORAGE_FLASH_SIZE=512 -DSTORAGE_BLOCK_SIZE=64 -DMEMORY_BACKUP_CAP=128
echo

# t_2da784f0, renderer half: the REAL Blocks/Vysi1Render.h is compiled here (unmodified) over a real
# DynamicBlockDescriptor, with the four device-layer symbols its headers reference but the render path
# never calls (DeviceStatus/TxFrame/DispatchPacket/DeviceLog) stubbed. It pins the two facts the card
# needs measured rather than read:
#   - a field whose key-0 row was created by an EMPTY host write (a Geometry/Texture ValueInfo with
#     Size 0, no value bytes) is classified kind 1 / kind 2 and its geometry/texture APPLY;
#   - with those key-0 rows gone - the measured rig symptom - the fields are still listed and
#     classified 0, the overlay is never applied and the picture shows through.
# A block whose key-0 rows already exist must read the same in both passes (fields 0/1 keep their
# kind), and re-sending an existing marker must change nothing (a restore is idempotent).
# -Wno-unused-function: MemoryDynamic.h's file helpers are static and unused in a TU that only
# drives the block model - they warn under -Werror without being reachable here.
echo "### native Vysi render-kind tests (a created key-0 marker is classified, a missing one is not)"
build_run "vysirender" "vysi_render_kind_test.cpp" -DUSE_DYNAMIC_BLOCKS -Wno-unused-function
echo

# t_a706d64a: the render mask cache is per PART, not per block. A write to one render part (the
# emote script rewrites the lid/iris keys while it animates) must not cost the other parts' shape
# passes, and a write that re-sends a part's own bytes must cost nothing at all - while the frames
# stay exactly what a full rebuild produces. "Recomputed" is observed with a sentinel written into
# each GeoMask slot: a slot the renderer rebuilt no longer holds it.
echo "### native Vysi render-cache tests (only a changed part's mask is recomputed)"
build_run "vysicache" "vysi_render_cache_test.cpp" -DUSE_DYNAMIC_BLOCKS -Wno-unused-function
echo

echo "### native BLE Settings tests (Just Works vs passkey policy, and the block table's offsets)"
build_run "blesettings" "ble_settings_test.cpp"
echo

echo "### native BLE admission tests (identity compare across type spaces, who may bond, who may hold a session)"
build_run "bleadmission" "ble_admission_test.cpp"
echo

echo "### native BLE window tests (the automatic window follows an owner event, never the bond count - E69)"
build_run "blewindow" "ble_window_test.cpp"
echo

echo "### native GPIO8 reset tests (timing table, LED sequence, button suppression)"
build_run "blereset" "ble_reset_test.cpp"
echo

echo "### native BLE Settings write-rule tests (the cable, or a secured BLE session)"
build_run "blewrite" "ble_write_test.cpp"
echo

# t_44676a52: the radio's four survival mechanisms, each a host-clean state machine.
#
#   ble_rx_test.cpp      the RX chunk assembler (Blocks/BleRxAssembler.h): a declared length beyond the
#                        link is refused, a chunk that stops arriving is abandoned on its clock, both are
#                        counted, and the frame that follows a deliberately truncated write PARSES.
#   ble_latch_test.cpp   the single-session latch (Blocks/BleLinkLatch.h): a lost onDisconnect is
#                        repaired and the next connect is admitted; a teardown the stack refuses is
#                        retried and then forced; a second peer can never release the live session.
#   usb_tx_stall_test.cpp  the USB TX stall detector (Blocks/UsbTxStall.h): a port that takes nothing is
#                        reported once per episode and cleared the moment the host sends a frame.
echo "### native BLE RX assembler tests (a truncated chunk resyncs; the next frame parses)"
build_run "blerx" "ble_rx_test.cpp"
echo

echo "### native BLE session-latch tests (a lost teardown heals; a refused one is retried then forced)"
build_run "blelatch" "ble_latch_test.cpp"
echo

echo "### native USB TX stall tests (a port that takes nothing is reported once and skipped)"
build_run "usbtxstall" "usb_tx_stall_test.cpp"
echo

# The naming rule (Blocks/AutoName.h): a device names itself "<Prefix>-<serial tail>" on its first
# bond, and only while its System `Name` field is undisturbed - so a name the app or a setup restore
# wrote wins, whichever side of the pairing it landed on. The rule, the field shape and the decision
# table are driven here; the device's own write is guarded on the source below.
echo "### native naming tests (a device names itself after its serial on its first bond)"
build_run "autoname" "auto_name_test.cpp"
echo

# The naming rule's device half, structural half. The rule is host-tested above; what the device
# wires it to cannot be linked on the host (NimBLE, the static memory, the storage partition), so it
# is asserted on the source:
#   - the ONLY site that raises the flag is the security callback, and its gate is the bond store
#     having been EMPTY when the link came up (BleLinkBondCount == 0) - the device's first bond,
#     never a reconnect and never a later bond (the store's snapshot, AppBLELinkBondSnapshot);
#   - the flag is consumed before the write, so one first bond names the device once - and the
#     `AutoNameDue` call repeats the same gate plus "a bond still exists";
#   - the write runs from the app task's per-loop hook (BleSettingsSync -> AppBLEAutoNameService),
#     never from the NimBLE host callback, because it mirrors `.SV`;
#   - the factory name the rule compares against is the initialiser's own constant (KFactorySystem),
#     not a second copy of the string.
echo "### source guard: the auto-name is owed to the FIRST bond and written from the app task"
NAME_APPBLE="src/Devices/Tamu_v2.0A/AppBLE.h"
NAME_DEVICE="src/Devices/Tamu_v2.0A/BleAutoName.h"
NAME_GLUE="src/Devices/Tamu_v2.0A/BleSettingsGlue.h"
NAME_MAIN="src/Devices/Tamu_v2.0A/Main.h"
name_fail=0

if [ "$(grep -c 'BleAutoNameOwed = true' "$NAME_APPBLE" || true)" != 1 ] ||
   ! grep -A1 'if (BleLinkBondCount == 0)' "$NAME_APPBLE" | grep -q 'BleAutoNameOwed = true;'; then
    echo "FAIL $NAME_APPBLE: the auto-name must be raised exactly once, gated on an empty bond store at connect (the first bond)"
    name_fail=1
fi
if ! grep -q 'AutoNameDue(BleLinkBondCount == 0' "$NAME_DEVICE" ||
   ! grep -q 'BleAutoNameOwed = false; // one attempt per first bond' "$NAME_DEVICE"; then
    echo "FAIL $NAME_DEVICE: the write must repeat the first-bond gate and consume the one-shot flag before writing"
    name_fail=1
fi
if [ "$(grep -c 'AppBLEAutoNameService();' "$NAME_GLUE" || true)" != 1 ]; then
    echo "FAIL $NAME_GLUE: the auto-name must have exactly one call site - the per-loop hook"
    name_fail=1
fi
if ! grep -q 'AutoNameUndisturbed(StaticPer.System.Name' "$NAME_DEVICE" ||
   ! grep -q '\.System = KFactorySystem,' "$NAME_MAIN" ||
   ! grep -q 'constexpr SystemPersistent KFactorySystem' "$NAME_MAIN"; then
    echo "FAIL: the factory name must be one constant (KFactorySystem) - shared by the initialiser and the undisturbed check"
    name_fail=1
fi
[ $name_fail = 0 ] && echo "PASS: the name is owed to the first bond, fills only an undisturbed System/Name, and is written from the app task"
fail=$((fail + name_fail))
echo

# The GAP Device Name characteristic (0x2A00), structural half. This is the string a bonded peer's
# OS reads and caches, and it is a DIFFERENT value from the advertised name NimBLEAdvertising sets.
# NimBLE initialises it from CONFIG_BT_NIMBLE_SVC_GAP_DEVICE_NAME ("nimble"), and the server's first
# start re-initialises the GAP service from that same config value
# (`NimBLEServer::start()` -> `resetGATT()` -> `ble_svc_gap_init()`), so a name set before the first
# advertisement is thrown away - measured on the rig as advertisement "Akyirr" / 0x2A00 "nimble"
# (card t_2983d5c5). What has to hold, asserted on the source because NimBLE cannot be linked here:
#   - the advertisement and the characteristic are set from the SAME effective-name string, in
#     StartAppAdvertising, and the characteristic AFTER Adv->start();
#   - every set goes through BleSetGapName - one place, so the two values cannot be set from
#     different strings - and the rename path uses it too, for a rename that cannot re-advertise yet.
echo "### source guard: the GAP Device Name (0x2A00) follows the advertised name, set after the server starts"
GAPNAME_SRC="src/Devices/Tamu_v2.0A/AppBLE.h"
gapname_fail=0
gapname_adv=$(sed -n '/^static void StartAppAdvertising()/,/^}/p' "$GAPNAME_SRC")
if ! printf '%s\n' "$gapname_adv" | grep -q 'BleSetGapName(AdvName);'; then
    echo "FAIL $GAPNAME_SRC: StartAppAdvertising must set the GAP device name from the advertised name"
    gapname_fail=1
fi
if ! printf '%s\n' "$gapname_adv" | grep -A4 'Adv->start()' | grep -q 'BleSetGapName(AdvName);'; then
    echo "FAIL $GAPNAME_SRC: the GAP device name must be set AFTER Adv->start() - the server's start resets it to \"nimble\""
    gapname_fail=1
fi
if [ "$(grep -c 'NimBLEDevice::setDeviceName(' "$GAPNAME_SRC")" != 1 ]; then
    echo "FAIL $GAPNAME_SRC: NimBLEDevice::setDeviceName must have exactly one call site (BleSetGapName)"
    gapname_fail=1
fi
gapname_apply=$(sed -n '/^void AppBLESettingsApply()/,/^}/p' "$GAPNAME_SRC")
if ! printf '%s\n' "$gapname_apply" | grep -q 'BleSetGapName(Name);'; then
    echo "FAIL $GAPNAME_SRC: a rename must set the GAP device name too (a live session blocks the re-advertisement)"
    gapname_fail=1
fi
[ $gapname_fail = 0 ] && echo "PASS: 0x2A00 is set from the advertised name's string, after the server's start, and on rename"
fail=$((fail + gapname_fail))
echo

# The GPIO8 reset and the write rule, structural half. The sequencer's decision is host-tested above;
# what the device wires it to cannot be linked on the host (GPIO, NimBLE, the static memory), so it is
# asserted on the source:
#   - GPIO8 is the reset input and is not claimed by anything else;
#   - the input is an input with the pull-up, and the notification pin is only ever driven LOW or back
#     to an input - never HIGH, which a closed contact would short to ground;
#   - the button read is the GPIO2 line (!PinRead(LED_NOTIFICATION_PIN)), suppressed only while the
#     sequencer owns that pin (BleResetOwnsPin) - never routed through the GPIO8 sequencer, because a
#     contact on the reset pad is not a press of the button;
#   - the reset input is sampled on every pass with no gate on the LED driver: GPIO8 is its own pad,
#     so the notification LED cannot blind the reset;
#   - the pin is handed back to the notification driver (OnLEDStateChange) when the sequence ends;
#   - the boot brings the input up and the main loop services it once per pass;
#   - the `BLE Settings` write rule is BleWriteAllowed over the frame's own link plus the session flag,
#     and the old "USB only" rule is gone.
echo "### source guard: GPIO8 reset wired to the button, the LED and the stack restart; writes by link"
RST_SRC="src/Blocks/BleResetHold.h"
RSTPIN_SRC="src/Devices/Tamu_v2.0A/BleResetPin.h"
RSTBTN_SRC="src/Devices/Tamu_v2.0A/Button.h"
RSTMAIN_SRC="src/Devices/Tamu_v2.0A/Main.h"
RSTGLUE_SRC="src/Devices/Tamu_v2.0A/BleSettingsGlue.h"
rst_guard_fail=0

# The four numbers the card names, in the one header that owns them.
for pinned in \
    "#define BLE_RESET_IGNORE_MS 2000u" \
    "#define BLE_RESET_CLEAR_MS 10000u" \
    "#define BLE_RESET_BLINK_HALF_MS 250u" \
    "#define BLE_RESET_BLINK_COUNT 5u"; do
    if [ "$(grep -cF "$pinned" "$RST_SRC")" != 1 ]; then
        echo "FAIL $RST_SRC: '$pinned' is missing or duplicated (the timing table's own constant)"
        rst_guard_fail=1
    fi
done

# GPIO8: the reset input, and nothing else claims it.
if [ "$(grep -c 'BLE_RESET_PIN = GPIO_NUM_8' "$RSTPIN_SRC")" != 1 ]; then
    echo "FAIL $RSTPIN_SRC: the reset input must be GPIO8"
    rst_guard_fail=1
fi
if [ "$(grep -rn 'GPIO_NUM_8' src --include=*.h | grep -vc "$RSTPIN_SRC")" != 0 ]; then
    echo "FAIL: GPIO8 is claimed somewhere other than the reset input"
    rst_guard_fail=1
fi

# The pin's own use: input with the pull-up, and the LED only ever LOW or INPUT (never driven HIGH).
if [ "$(grep -c 'PinModeInputPullUp(BLE_RESET_PIN)' "$RSTPIN_SRC")" != 1 ]; then
    echo "FAIL $RSTPIN_SRC: the reset input must be an input with the pull-up"
    rst_guard_fail=1
fi
if [ "$(grep -c 'PinHigh(LED_NOTIFICATION_PIN)' "$RSTPIN_SRC")" != 0 ]; then
    echo "FAIL $RSTPIN_SRC: the notification pin must never be driven HIGH (a held contact would short it)"
    rst_guard_fail=1
fi
if [ "$(grep -c 'PinLow(LED_NOTIFICATION_PIN)' "$RSTPIN_SRC")" != 1 ] ||
   [ "$(grep -c 'PinModeInputPullUp(LED_NOTIFICATION_PIN)' "$RSTPIN_SRC")" != 1 ]; then
    echo "FAIL $RSTPIN_SRC: the LED phases must be exactly 'drive low = lit' and 'input pull-up = dark'"
    rst_guard_fail=1
fi
if [ "$(grep -c 'OnLEDStateChange(StaticBlockRegistry\[0\], 3' "$RSTPIN_SRC")" != 1 ]; then
    echo "FAIL $RSTPIN_SRC: the pin must be handed back to the notification driver when the sequence ends"
    rst_guard_fail=1
fi
if [ "$(grep -c 'AppBLEStackReset(Clear)' "$RSTPIN_SRC")" != 1 ]; then
    echo "FAIL $RSTPIN_SRC: the action must be the one stack restart (AppBLEStackReset)"
    rst_guard_fail=1
fi

# The button is read from GPIO2 and only SUPPRESSED while the sequencer drives that pin: the report
# must still come from the physical line, and nothing may route it through the GPIO8 sequencer.
if [ "$(grep -c '|| BleResetOwnsPin()' "$RSTBTN_SRC")" != 1 ] ||
   [ "$(grep -c '= !PinRead(LED_NOTIFICATION_PIN);' "$RSTBTN_SRC")" != 1 ]; then
    echo "FAIL $RSTBTN_SRC: ButtonUpdate must suppress while the sequencer owns the pin and otherwise read GPIO2 (!PinRead(LED_NOTIFICATION_PIN))"
    rst_guard_fail=1
fi
if [ "$(grep -rc 'BleResetButtonPressed' src --include=*.h | grep -vc ':0$')" != 0 ]; then
    echo "FAIL: a GPIO8 contact is not a button press - nothing may report the button through the reset sequencer"
    rst_guard_fail=1
fi
# GPIO8 is a pad of its own: the reset input is sampled on every pass, never gated on the LED driver.
if [ "$(grep -c 'BleResetStep(BleResetState, PinRead(BLE_RESET_PIN), NowMs)' "$RSTPIN_SRC")" != 1 ] ||
   [ "$(grep -c 'Readable' "$RSTPIN_SRC")" != 0 ]; then
    echo "FAIL $RSTPIN_SRC: the reset input must be sampled unconditionally (GPIO8 is not the LED pin)"
    rst_guard_fail=1
fi

# Boot brings the input up; the main loop services it once per pass, after the button.
if [ "$(grep -c 'BleResetInit();' "$RSTMAIN_SRC")" != 1 ] ||
   [ "$(grep -c 'BleResetService();' "$RSTMAIN_SRC")" != 1 ]; then
    echo "FAIL $RSTMAIN_SRC: the reset input must be set up once and serviced once per pass"
    rst_guard_fail=1
fi
btn_line=$(grep -n 'ButtonUpdate();' "$RSTMAIN_SRC" | head -1 | cut -d: -f1)
svc_line=$(grep -n 'BleResetService();' "$RSTMAIN_SRC" | head -1 | cut -d: -f1)
if [ -z "$btn_line" ] || [ -z "$svc_line" ] || [ "$svc_line" -le "$btn_line" ]; then
    echo "FAIL $RSTMAIN_SRC: the reset must be serviced after ButtonUpdate in the same loop pass"
    rst_guard_fail=1
fi

# The write rule: the frame's own link plus the session flag, and no USB-only shortcut left.
if [ "$(grep -c 'BleWriteAllowed(AppFrameCameOverUsb(), AppFrameCameOverBle(), AppBLELinkSecured())' "$RSTGLUE_SRC")" != 1 ]; then
    echo "FAIL $RSTGLUE_SRC: BleBlockWritable must ask BleWriteAllowed with the frame's link and the session flag"
    rst_guard_fail=1
fi
if [ "$(grep -c 'return AppFrameCameOverUsb();' "$RSTGLUE_SRC")" != 0 ]; then
    echo "FAIL $RSTGLUE_SRC: the USB-only write rule is back"
    rst_guard_fail=1
fi
[ $rst_guard_fail = 0 ] && echo "PASS: GPIO8 times the reset, the button is suppressed through it, the LED is handed back, writes are decided by the link"
fail=$((fail + rst_guard_fail))
echo

echo "### native app-console guard tests (idle link writes the line, a busy one suppresses it - E67)"
build_run "appconsole" "appconsole_test.cpp"
echo

# t_7a1870db round 2 (E75). The TX ring's indexes are 16-bit free-running counters, and 65536 is
# not a multiple of the 24 KiB ring, so a position taken from a wider sum lands 16384 bytes away
# from where the writer put it. The model here drives the real header through twelve back-to-back
# whole-file replies across the counter wrap (both link chunk sizes) and must deliver every byte;
# the same model with the pre-fix `(Tail + Count) % SIZE` expression must fail, so the defect
# stays pinned and the helper is not "simplified" back.
echo "### native TX ring geometry tests (a whole reply survives the 16-bit counter wrap - E75)"
build_run "txring" "txring_test.cpp"
echo

# t_367f86d0 round 2, structural half. Two rules the radio code owns, neither of which the host can
# link (AppBLE.h pulls NimBLE, the host task and NVS), asserted on the source:
#   - a link's security deadline comes from the policy, not from one fixed figure: a passkey pairing
#     is a person typing six digits, so the 5 s budget that suits Just Works dropped it mid-pairing;
#   - a passkey change revokes the bonds it invalidates. A phone reconnecting with a bond made under
#     the old code comes up encrypted and bonded but NOT authenticated, which the connect gate
#     refuses forever, so the change has to forget the store - at the write (policy applied) AND at
#     radio start (the passkey can be changed while the radio is off, where the store is unreachable).
echo "### source guard: policy-shaped secure deadline, and a passkey change revokes its bonds"
APPBLE_SRC="src/Devices/Tamu_v2.0A/AppBLE.h"
BLESTORE_SRC="src/Devices/Tamu_v2.0A/BlePasskeyStore.h"
blessec_guard_fail=0

# The fixed 5 s deadline is gone, and onConnect grants the budget the policy selects.
if [ "$(grep -c 'BLE_SECURE_DEADLINE_MS' "$APPBLE_SRC")" != 0 ]; then
    echo "FAIL $APPBLE_SRC: the single fixed secure deadline is back (it must come from the policy)"
    blessec_guard_fail=1
fi
connect_body=$(sed -n '/void onConnect(NimBLEServer/,/^    }$/p' "$APPBLE_SRC")
# t_44676a52: the arming moved into BleArmSecureDeadline (so every refusal path arms it too), so the
# budget is granted through that one helper - and the deadline arithmetic lives there with it.
if ! printf '%s\n' "$connect_body" | grep -q 'BleArmSecureDeadline(ConnInfo.getConnHandle(), BleSecureBudgetMs(Policy), Policy.Mitm);' ||
   ! grep -q 'BleSecureDeadlineMs   = TimeFromBoot() + BudgetMs;' "$APPBLE_SRC"; then
    echo "FAIL $APPBLE_SRC: onConnect must grant the policy's budget (BleSecureBudgetMs)"
    blessec_guard_fail=1
fi
# ... and the drop says which case it was, so a dropped passkey pairing is not silent.
if ! sed -n '/^void AppBLETick()/,/^}/p' "$APPBLE_SRC" | grep -q 'the code was not entered'; then
    echo "FAIL $APPBLE_SRC: the secure-deadline drop must name the passkey case"
    blessec_guard_fail=1
fi

# The bond revocation is reached from both places a passkey can change, and it really forgets.
apply_body=$(sed -n '/^void AppBLEApplySecurityPolicy()/,/^}/p' "$APPBLE_SRC")
if ! printf '%s\n' "$apply_body" | grep -q 'AppBLEEnforceBondPolicy();'; then
    echo "FAIL $APPBLE_SRC: AppBLEApplySecurityPolicy must revoke bonds the new passkey invalidates"
    blessec_guard_fail=1
fi
radio_body=$(sed -n '/^static bool AppBLERadioStart()/,/^}/p' "$APPBLE_SRC")
if ! printf '%s\n' "$radio_body" | grep -q 'AppBLEEnforceBondPolicy();'; then
    echo "FAIL $APPBLE_SRC: the radio start must apply a passkey change made while the radio was down"
    blessec_guard_fail=1
fi
enforce_body=$(sed -n '/^bool AppBLEEnforceBondPolicy()/,/^}/p' "$APPBLE_SRC")
if ! printf '%s\n' "$enforce_body" | grep -q 'AppBLEForgetAllBonds();'; then
    echo "FAIL $APPBLE_SRC: AppBLEEnforceBondPolicy must forget the invalidated bonds"
    blessec_guard_fail=1
fi
# The stamp of "which code the bonds were made under" has to be persistent: a passkey can be changed
# with the radio off, and the store cannot be touched then.
if [ "$(grep -c 'nvs_set_blob(H, BLE_BOND_EPOCH_NVS_KEY' "$BLESTORE_SRC")" != 1 ]; then
    echo "FAIL $BLESTORE_SRC: the bond-policy stamp must be written to NVS"
    blessec_guard_fail=1
fi

# t_df5c6520: the epoch is stamped only when the store is actually EMPTY after the delete. Stamping
# regardless of the outcome records a surviving bond as made under the current code, so the next
# AppBLEEnforceBondPolicy finds them equal and never retries - the phone a passkey change was meant
# to free stays locked out. Checked structurally (brace depth, not a literal snippet) so a re-ment
# of the body cannot sneak the stamp back out of the guard:
#   - the count the guard tests is read back with getNumBonds() AFTER deleteAllBonds();
#   - every stamp (memcpy into BleBondEpoch / BleBondEpochSave) sits inside an `if (<that count> == 0)`
#     block that comes after the delete;
#   - a surviving-count log line exists, so the operator sees why the retry is coming.
forget_body=$(sed -n '/^bool AppBLEForgetAllBonds()/,/^}/p' "$APPBLE_SRC")
forget_verdict=$(printf '%s\n' "$forget_body" | awk '
    BEGIN { depth = 0; del = 0; guard_line = 0; guard_depth = -1; guard_pending = 0;
            bad = 0; stamps = 0; counted = 0; n = 0 }
    {
        n++
        line = $0
        # The opening brace of the guard, on its own line (Allman style in this file).
        if (guard_pending && line ~ /^[ \t]*\{[ \t]*$/) {
            guard_depth = depth + 1; guard_line = n; guard_pending = 0
        }
        if (del > 0 && match(line, /^[ \t]*if \([A-Za-z_][A-Za-z0-9_]* == 0\)[ \t]*$/)) {
            v = line; sub(/^[ \t]*if \(/, "", v); sub(/ == 0\).*$/, "", v)
            if ((v in readback) && readback[v] > del) guard_pending = 1
        }
        if (match(line, /[A-Za-z_][A-Za-z0-9_]*[ \t]*=[ \t]*NimBLEDevice::getNumBonds\(\)/)) {
            v = substr(line, RSTART, RLENGTH); sub(/[ \t]*=.*$/, "", v); readback[v] = n
        }
        if (line ~ /NimBLEDevice::deleteAllBonds\(\)/) del = n
        if (line ~ /memcpy\(BleBondEpoch/ || line ~ /BleBondEpochSave\(BleBondEpoch\)/) {
            stamps++
            if (guard_depth < 0 || depth < guard_depth || n < guard_line) bad = 1
        }
        if (n > del && del > 0 && line ~ /bond\(s\)/ && line ~ /%d/) counted = 1
        o = gsub(/\{/, "{", line); c = gsub(/\}/, "}", line); depth += o - c
    }
    END {
        if (del == 0) print "the function no longer calls deleteAllBonds()"
        else if (guard_depth < 0) print "no `if (<count> == 0)` guard on a count read back after the delete"
        else if (guard_line <= del) print "the empty-store guard sits before the delete"
        else if (bad) print "a stamp sits outside the empty-store guard"
        else if (stamps < 2) print "the store is no longer stamped at all"
        else if (!counted) print "a surviving store is not logged with its count"
        else print "ok"
    }')
if [ "$forget_verdict" != "ok" ]; then
    echo "FAIL $APPBLE_SRC: a failed bond forget must leave the epoch alone - $forget_verdict"
    blessec_guard_fail=1
fi
[ $blessec_guard_fail = 0 ] && echo "PASS: deadline from the policy, passkey change revokes its bonds (stamp in NVS), and a failed forget is not stamped"
fail=$((fail + blessec_guard_fail))
echo

# t_1b380cee (Issues.md E65), structural half: `ble_gap_unpair()` refuses with `BLE_HS_EBUSY` while
# advertising or discovery is active, and since E64 this device advertises whenever no session is up
# - which is the state a USB `Action = 1` arrives in. So the forget itself has to take the
# advertisement down for the call, and the tick has to put it back. On the source because AppBLE.h
# pulls NimBLE, the host task and NVS (the host cannot link it). What must hold:
#   - the stop comes BEFORE deleteAllBonds(): a stop after it is no stop at all, the unpair has
#     already been refused;
#   - both the stop and the restart are gated on an advertisement that is actually up, so the paths
#     that never needed one (a live link, a disabled radio) lose nothing;
#   - the restart request (`BleAdvStateApplied = false`) comes AFTER the delete, and it is what makes
#     AppBLETick rebuild the advertisement - a forget that leaves it stopped takes the device off
#     the air for good, which is the opposite of E64's rule.
echo "### source guard: a bond forget works on an idle, advertising device (E65)"
e65_guard_fail=0
forget_body=$(sed -n '/^bool AppBLEForgetAllBonds()/,/^}/p' "$APPBLE_SRC")
adv_stop_line=$(printf '%s\n' "$forget_body" | grep -n 'Adv->stop()' | head -1 | cut -d: -f1)
del_line=$(printf '%s\n' "$forget_body" | grep -n 'NimBLEDevice::deleteAllBonds()' | head -1 | cut -d: -f1)
restart_line=$(printf '%s\n' "$forget_body" | grep -n 'BleAdvStateApplied = false;' | head -1 | cut -d: -f1)
if [ -z "$adv_stop_line" ] || [ -z "$del_line" ] || [ "$adv_stop_line" -ge "$del_line" ]; then
    echo "FAIL $APPBLE_SRC: the advertisement must be stopped BEFORE deleteAllBonds() (the unpair refuses on the air, E65)"
    e65_guard_fail=1
fi
if [ -z "$restart_line" ] || [ "$restart_line" -le "$del_line" ]; then
    echo "FAIL $APPBLE_SRC: the advertisement must be requested back AFTER the delete (BleAdvStateApplied = false)"
    e65_guard_fail=1
fi
if [ "$(printf '%s\n' "$forget_body" | grep -c 'if (AdvWasUp')" -lt 2 ]; then
    echo "FAIL $APPBLE_SRC: the stop and the restart must both be gated on an advertisement that is up"
    e65_guard_fail=1
fi
if ! printf '%s\n' "$forget_body" | grep -q 'Adv->isAdvertising()'; then
    echo "FAIL $APPBLE_SRC: the forget must ask the stack whether advertising is up, not assume it"
    e65_guard_fail=1
fi
[ $e65_guard_fail = 0 ] && echo "PASS: the bond forget clears the air first, then asks the tick to rebuild the advertisement"
fail=$((fail + e65_guard_fail))
echo

# t_c1376256 (Issues.md E68), structural half. The revoke is only a revoke if three things hold, and
# each of them failed once on the rig or in review:
#   - a passkey write that does not CHANGE the code revokes nothing (the epoch comparison decides,
#     not "was the write accepted"), and it must not drop a live session either: the old drop test
#     was `Policy.Mitm`, which is true for a re-written passkey and disconnected a good session;
#   - a real change drops whatever link is up, whichever direction the code moved - passkey ->
#     `000000` left MITM off and kept the old peer's session over a bond the same apply deleted;
#   - the epoch is never stamped from `onAuthenticationComplete`: a bond made under an old code
#     still authenticates from its stored LTK, and stamping on that reconnect erased the mismatch
#     the boot-time retry depends on (the rig observed exactly this, Issues.md E65).
echo "### source guard: a passkey write revokes only a changed code, and drops its live link (E68)"
e68_guard_fail=0
apply_body=$(sed -n '/^void AppBLEApplySecurityPolicy()/,/^}/p' "$APPBLE_SRC")
revoked_var=$(printf '%s\n' "$apply_body" | sed -n 's/.*bool \([A-Za-z_][A-Za-z0-9_]*\) = AppBLEEnforceBondPolicy();.*/\1/p' | head -1)
if [ -z "$revoked_var" ]; then
    echo "FAIL $APPBLE_SRC: the policy apply must take the revocation decision (a no-op write revokes nothing)"
    e68_guard_fail=1
fi
# The drop is gated on that decision - never on the MITM bit of the policy just applied.
if printf '%s\n' "$apply_body" | grep -q 'if (Policy.Mitm'; then
    echo "FAIL $APPBLE_SRC: the session drop is gated on the new policy, not on the code having changed (E68)"
    e68_guard_fail=1
fi
revoke_line=$(printf '%s\n' "$apply_body" | grep -n '= AppBLEEnforceBondPolicy();' | head -1 | cut -d: -f1)
drop_line=$(printf '%s\n' "$apply_body" | grep -n 'BleDropLink(StaticBleLatch.Session, "passkey changed under it");' | head -1 | cut -d: -f1)
if [ -z "$drop_line" ] || [ -z "$revoke_line" ] || [ "$drop_line" -le "$revoke_line" ]; then
    echo "FAIL $APPBLE_SRC: a changed passkey must drop the live session after revoking its bonds (E68)"
    e68_guard_fail=1
elif ! printf '%s\n' "$apply_body" | sed -n "${revoke_line},${drop_line}p" | grep -q "if ($revoked_var &&"; then
    echo "FAIL $APPBLE_SRC: the session drop must be gated on '$revoked_var' (the revocation decision)"
    e68_guard_fail=1
fi
# The decision itself is host-tested (ble_settings_test.cpp); the device must ask it, not re-derive
# it with a memcmp of its own.
if ! printf '%s\n' "$enforce_body" | grep -q 'BleBondsInvalidated(BleBondEpochKnown'; then
    echo "FAIL $APPBLE_SRC: AppBLEEnforceBondPolicy must decide with BleBondsInvalidated (host-tested)"
    e68_guard_fail=1
fi
# A reconnect must never re-stamp the epoch.
auth_body=$(sed -n '/void onAuthenticationComplete/,/^    }$/p' "$APPBLE_SRC")
if printf '%s\n' "$auth_body" | grep -q 'AppBLEBondEpochStamp'; then
    echo "FAIL $APPBLE_SRC: onAuthenticationComplete re-stamps the bond epoch - a reconnect with a stale bond would silence the retry (E68)"
    e68_guard_fail=1
fi
# ... and the two places that may stamp are the empty store after a forget and the nothing-to-revoke
# case in the enforce path.
if [ "$(grep -c 'BleBondEpochSave(BleBondEpoch)' "$APPBLE_SRC")" != 2 ]; then
    echo "FAIL $APPBLE_SRC: the bond epoch may only be written where the store is wholly under the current code (forget + enforce)"
    e68_guard_fail=1
fi
[ $e68_guard_fail = 0 ] && echo "PASS: a changed passkey revokes the bonds and drops the link; a no-op write does neither; a reconnect never re-stamps"
fail=$((fail + e68_guard_fail))
echo

# E64 ruling C (owner, 2026-10-28), with E66 (2026-10-28) on top: advertising is ONE condition - no
# live connection - and does not depend on the bond store or on a window. Who may HOLD A SESSION is
# the separate rule, and E66 moved it out of the controller: the accept list is matched against the
# address the peer uses on the air, so a phone under LE privacy (a rotating RPA the controller
# resolves for the connection but not for the filter) is dropped by the very list that exists to
# admit it. AppBLE.h therefore uses no accept list at all. AppBLE.h cannot be linked on the host
# (NimBLE, the host task, NVS), so the rules are asserted on the source. What must hold:
#   - advertising is wanted whenever the radio is up and enabled, with NO bond or window term;
#   - no advertising filter policy is set anywhere, and the accept list is never programmed (E66);
#   - a new bond may only be made inside a pairing window or while the store is empty, decided per
#     link (BleBondingAllowed) and handed to the stack before the security procedure starts - so a
#     stranger cannot pair outside a window and come back bonded;
#   - the session gate refuses a link that is encrypted and bonded but is NOT one of the bonds from
#     before the link (BleSessionAllowed), and forgets the bond such a link made;
#   - the SAME question is asked at the link, before the security procedure (BleConnectRefused, E73),
#     so a peer that could never be admitted never starts a pairing - no OS dialog, no wasted attempt;
#   - the advertisement is rebuilt when it has to start and only dropped when the radio is off.
echo "### source guard: advertising runs whenever no device is connected - window, bonds or none (E64 C/E66)"
adv_guard_fail=0

advwanted_body=$(sed -n '/^static bool BleAdvWanted()/,/^}/p' "$APPBLE_SRC")
if ! printf '%s\n' "$advwanted_body" | grep -q 'return BleRadioUp && BleEnabled;' ||
   printf '%s\n' "$advwanted_body" | grep -Eq 'AppBLEWindowOpen|BondCount'; then
    echo "FAIL $APPBLE_SRC: BleAdvWanted must be radio-up && enabled - no window or bond term"
    adv_guard_fail=1
fi

# E66: no accept list, in either direction. Nothing may set an advertising filter policy (the
# controller matches the list against the on-air address), and nothing may program the white list.
if [ "$(grep -c 'setScanFilter' "$APPBLE_SRC")" != 0 ]; then
    echo "FAIL $APPBLE_SRC: an advertising filter policy is set again - it drops an LE-privacy peer (E66)"
    adv_guard_fail=1
fi
if [ "$(grep -c 'whiteList' "$APPBLE_SRC")" != 0 ]; then
    echo "FAIL $APPBLE_SRC: the accept list is programmed again - nothing may filter on it (E66)"
    adv_guard_fail=1
fi

# The admission rule itself (Blocks/BleAdmissionPolicy.h) is driven by ble_admission_test.cpp; what
# is asserted here is that the radio code asks it, at the two points that matter.
connect_body=$(sed -n '/void onConnect(NimBLEServer/,/^    }$/p' "$APPBLE_SRC")
if ! printf '%s\n' "$connect_body" | grep -q 'AppBLELinkBondSnapshot();' ||
   ! printf '%s\n' "$connect_body" | grep -q 'NimBLEDevice::setSecurityAuth(!BleLinkBondingClosed, Policy.Mitm, true);'; then
    echo "FAIL $APPBLE_SRC: onConnect must snapshot the store and decide whether this link may bond"
    adv_guard_fail=1
fi
# ... and the snapshot comes before the security procedure, or it would describe a store the link
# has already changed.
snapshot_line=$(printf '%s\n' "$connect_body" | grep -n 'AppBLELinkBondSnapshot();' | head -1 | cut -d: -f1)
bonding_line=$(printf '%s\n' "$connect_body" | grep -n 'setSecurityAuth(!BleLinkBondingClosed' | head -1 | cut -d: -f1)
init_line=$(printf '%s\n' "$connect_body" | grep -n 'ble_gap_security_initiate' | head -1 | cut -d: -f1)
if [ -z "$snapshot_line" ] || [ -z "$bonding_line" ] || [ -z "$init_line" ] ||
   [ "$snapshot_line" -ge "$bonding_line" ] || [ "$bonding_line" -ge "$init_line" ]; then
    echo "FAIL $APPBLE_SRC: the snapshot must be taken first, then the bonding rule, then security"
    adv_guard_fail=1
fi

# E73: the same rule is asked at the LINK, before the security procedure. Asking it only in
# onAuthenticationComplete lets a phone that can never be admitted be walked through a pairing - the
# stack sends a Security Request, the OS raises its pairing dialog, the pairing completes without a
# bond and the link is dropped at the end, which is what the owner saw from his phone. The gate is
# BleConnectRefused (host-tested above) and it must come BEFORE ble_gap_security_initiate.
refuse_line=$(printf '%s\n' "$connect_body" | grep -n 'if (BleConnectRefused(' | head -1 | cut -d: -f1)
if [ -z "$refuse_line" ]; then
    echo "FAIL $APPBLE_SRC: onConnect must refuse an inadmissible peer at the link (BleConnectRefused, E73)"
    adv_guard_fail=1
elif [ -z "$init_line" ] || [ "$refuse_line" -ge "$init_line" ]; then
    echo "FAIL $APPBLE_SRC: the connect-time refusal must come BEFORE ble_gap_security_initiate - otherwise the peer is shown a pairing prompt it cannot complete (E73)"
    adv_guard_fail=1
fi
# ... and it must not be narrowed by address type again: a phone under LE privacy connects from a
# private address, which is exactly the case the owner saw the pairing dialog in. The identity the
# controller resolved is what the store is asked about (BlePeerWasBonded).
if printf '%s\n' "$connect_body" | grep -q 'getType() & 0x01'; then
    echo "FAIL $APPBLE_SRC: the connect-time refusal is narrowed by address type again - an LE-privacy peer then reaches the pairing exchange (E73)"
    adv_guard_fail=1
fi

# The session gate: a link that is encrypted and bonded is still refused when the peer is not one of
# the bonds this link found, and the bond it made is forgotten in the same callback.
auth_body=$(sed -n '/void onAuthenticationComplete(NimBLEConnInfo &ConnInfo)/,/^    }$/p' "$APPBLE_SRC")
if ! printf '%s\n' "$auth_body" | grep -q 'BleSessionAllowed(AppBLEWindowOpen(), BleLinkBondCount,'; then
    echo "FAIL $APPBLE_SRC: onAuthenticationComplete must gate the session on BleSessionAllowed"
    adv_guard_fail=1
fi
if ! printf '%s\n' "$auth_body" | grep -q 'NimBLEDevice::deleteBond(ConnInfo.getIdAddress())'; then
    echo "FAIL $APPBLE_SRC: a bond made on a refused link must be forgotten"
    adv_guard_fail=1
fi

tick_body=$(sed -n '/^void AppBLETick()/,/^}/p' "$APPBLE_SRC")
if ! printf '%s\n' "$tick_body" | grep -q 'if (BleAdvRestart && !BleConnected && AdvWanted)'; then
    echo "FAIL $APPBLE_SRC: the deferred restart must follow the advertising rule, not the window"
    adv_guard_fail=1
fi
# ... and the only stop left in the tick is the radio being switched off.
if [ "$(printf '%s\n' "$tick_body" | grep -c -- '->stop()')" != 1 ] ||
   [ "$(printf '%s\n' "$tick_body" | grep -c 'advertising stopped: the radio is disabled')" != 1 ] ||
   printf '%s\n' "$tick_body" | grep -q 'advertising stopped: no window and no bond stored'; then
    echo "FAIL $APPBLE_SRC: the tick may only stop advertising when the radio is disabled"
    adv_guard_fail=1
fi
# Nothing re-aims a filter and nothing mirrors the store into the controller any more (E66).
if printf '%s\n' "$tick_body" | grep -Eq 'ConnectFilterWhitelistOnly|SyncBleWhitelist|WhitelistOnly'; then
    echo "FAIL $APPBLE_SRC: the tick re-aims a connect filter that no longer exists (E66)"
    adv_guard_fail=1
fi

[ $adv_guard_fail = 0 ] && echo "PASS: advertising runs whenever no device is connected; the session gate admits the bonded peer with no window and refuses a stranger"
fail=$((fail + adv_guard_fail))
echo

# The E66 "last judged link" read-out (fields 9..11 of the block) is GONE: the owner simplified the
# block to six fields on 2026-10-29, and the link record went with them. Its source guard is removed
# with it; what stays host-tested is the admission rule itself (ble_admission_test.cpp) and that the
# radio asks it (the guard above).

# t_c1a0678e (Issues.md E69), structural half: the automatic pairing window follows an EVENT, not a
# reading of the bond store. Measured on the rig: a peer that presents a bonded identity and asks to
# pair makes the vendored stack delete that bond (NimBLEServer.cpp, BLE_GAP_EVENT_REPEAT_PAIRING ->
# ble_store_util_delete_peer + RETRY, before any application callback), and the then-empty store used
# to make AppBLETick open 300 s of window in which any peer that pairs is admitted - a locked device
# could be made to offer itself. The rule now lives in Blocks/BleWindowPolicy.h and is driven by
# ble_window_test.cpp; this asserts the radio code goes through it, at exactly the two owner events
# (the radio starting with no bond, an explicit forget that emptied the store) and nowhere else. The
# guard is its own script so it can be re-run against the pre-fix source and watched to FAIL:
#   bash test/native/ble_window_guard.sh src/Devices/Tamu_v2.0A/AppBLE.h
echo "### source guard: the automatic window follows an owner event, never 'the store reads empty' (E69)"
if bash "$ROOT/test/native/ble_window_guard.sh" "$APPBLE_SRC"; then
    :
else
    fail=$((fail + 1))
fi
echo

# Issues.md E67, route (d) (owner ruling 2026-10-28, "D is sufficient"). The console copy is
# suppressed while the USB link is BUSY, not while a cable is ATTACHED: `AppInterface.Connected`
# is true ~0.4 s after a reset whenever a host merely holds the port, which is why a rig session
# read the boot banner only. Log.h and the device USB glue cannot be linked on the host (esp_log.h,
# the USJ driver), so the guard's shape is asserted on the source; the guard script re-runs against
# the pre-fix header and is watched to FAIL:
#   bash test/native/log_guard.sh src/Devices/Tamu_v2.0A/Log.h src/Devices/Tamu_v2.0A/AppUSB.h \
#       src/Core/Functions/AppConsole.h
echo "### source guard: the console copy follows the USB link being idle, not a cable being attached (E67)"
APPUSB_SRC="src/Devices/Tamu_v2.0A/AppUSB.h"
APPCONSOLE_SRC="src/Core/Functions/AppConsole.h"
if bash "$ROOT/test/native/log_guard.sh" "src/Devices/Tamu_v2.0A/Log.h" "$APPUSB_SRC" "$APPCONSOLE_SRC"; then
    :
else
    fail=$((fail + 1))
fi
echo

echo "### native WiFi Settings tests (block table, write-only password, configuration-only State, field-8 bits)"
build_run "wifisettings" "wifi_settings_test.cpp"
echo

# t_ce642f02, structural half: the WiFi Settings block is a compile-in option (absent when off), its
# password is a secret that must never reach the register file, and every write is gated on the USB
# link. The registry, the dead-link gate and the write path all live in headers the host cannot link
# (they pull the board's static memory, the app interface and NVS), so the facts are asserted on the
# source:
#   - NO environment defines USE_WIFI_SETTINGS: the block is off by default, and off means absent
#     (no registry entry, so it cannot appear in an enumeration) rather than present-but-empty;
#   - the block's registry entry, and its two value spaces, exist only inside `#ifdef
#     USE_WIFI_SETTINGS` in the board's Main.h;
#   - the password's entry is a Trigger with NO Persistent flag (it lives in NVS, not `.SV`), and the
#     glue never copies the text into the volatile register buffer - the field reads back empty;
#   - the write rule is `AppFrameCameOverUsb()`, not "which links are up", and all three writable
#     fields go through it;
#   - System field 8 is built from AppActiveBits (both bits may be set), and the old single-value
#     `AppActive` enum is gone.
echo "### source guard: WiFi Settings is an option (absent when off), write-only password, USB-only writes"
WIFI_BLOCK_SRC="src/Blocks/WifiSettings.h"
WIFI_STORE_SRC="src/Devices/Tamu_v2.0A/WifiPasswordStore.h"
WIFI_GLUE_SRC="src/Devices/Tamu_v2.0A/WifiSettingsGlue.h"
WIFI_MAIN_SRC="src/Devices/Tamu_v2.0A/Main.h"
APPACT_READ_SRC="src/Core/Services/RegisterRead.h"
PIO_SRC="platformio.ini"
wifi_guard_fail=0

# Default off, every environment: no uncommented `-D USE_WIFI_SETTINGS` anywhere in platformio.ini.
if [ "$(grep -cE '^[[:space:]]*-D[[:space:]]+USE_WIFI_SETTINGS' "$PIO_SRC")" != 0 ]; then
    echo "FAIL $PIO_SRC: USE_WIFI_SETTINGS must stay off in every shipped env (it is a compile-in option)"
    wifi_guard_fail=1
fi

# The block is registered exactly once, inside the flag, and so are its two value spaces.
reg_body=$(sed -n '/^const StaticBlockDescriptor StaticBlockRegistry\[\] = {/,/^ *};/p' "$WIFI_MAIN_SRC")
if [ "$(printf '%s\n' "$reg_body" | grep -c '&WifiSettingsSchema')" != 1 ]; then
    echo "FAIL $WIFI_MAIN_SRC: the WiFi Settings block must be registered exactly once"
    wifi_guard_fail=1
fi
if ! printf '%s\n' "$reg_body" | grep -q '^#ifdef USE_WIFI_SETTINGS'; then
    echo "FAIL $WIFI_MAIN_SRC: the registry entry must be guarded - with the flag off the block is ABSENT"
    wifi_guard_fail=1
fi
if [ "$(grep -c 'WifiSettingsPersistent WifiSet' "$WIFI_MAIN_SRC")" != 1 ] ||
   [ "$(grep -c 'WifiSettingsVolatile WifiSet' "$WIFI_MAIN_SRC")" != 1 ]; then
    echo "FAIL $WIFI_MAIN_SRC: both value spaces must carry the block (behind the same flag)"
    wifi_guard_fail=1
fi

# The password: a Trigger and NOT persistent (a Persistent field would be mirrored into `.SV`, in the
# storage partition the Storage service streams out by name), and the text goes to NVS.
if ! grep -q 'DataType::Text, WIFI_PASSWORD_LEN, (uint8_t)ValueTrigger' "$WIFI_BLOCK_SRC"; then
    echo "FAIL $WIFI_BLOCK_SRC: the password entry must be a plain Trigger (no ValuePersistent)"
    wifi_guard_fail=1
fi
if ! grep -q 'MakeFieldKey(WIFI_SET_FIELD_PASSWORD, 0), OnWifiPasswordWrite' "$WIFI_BLOCK_SRC"; then
    echo "FAIL $WIFI_BLOCK_SRC: the password write must reach its trigger"
    wifi_guard_fail=1
fi
if [ "$(grep -c 'nvs_set_blob' "$WIFI_STORE_SRC")" != 1 ]; then
    echo "FAIL $WIFI_STORE_SRC: the password must be stored with one nvs_set_blob (NVS, not the register file)"
    wifi_guard_fail=1
fi
# Nothing may copy the text back into the register buffer: the field is write-only.
if [ "$(grep -cE '(memcpy|strncpy|strcpy)\((Vol|StaticVol)\.WifiSet\.Password' "$WIFI_GLUE_SRC")" != 0 ] ||
   [ "$(grep -c 'Vol->Password' "$WIFI_GLUE_SRC")" != 1 ]; then
    echo "FAIL $WIFI_GLUE_SRC: the password buffer may only be cleared (one memset), never filled"
    wifi_guard_fail=1
fi

# Who may write: the real link the frame arrived on, and every writable field goes through it.
if [ "$(grep -c 'return AppFrameCameOverUsb();' "$WIFI_GLUE_SRC")" != 1 ]; then
    echo "FAIL $WIFI_GLUE_SRC: the write rule must be the frame's own link (AppFrameCameOverUsb)"
    wifi_guard_fail=1
fi
if [ "$(grep -c 'if (!WifiBlockWritable())' "$WIFI_GLUE_SRC")" != 3 ]; then
    echo "FAIL $WIFI_GLUE_SRC: Enabled, SSID and Password must each be gated on WifiBlockWritable"
    wifi_guard_fail=1
fi

# System field 8: both interface bits, and the old single-value enum is gone.
if [ "$(grep -c 'AppActiveBits::Of(AppUSBActive(), AppBLEActive())' "$APPACT_READ_SRC")" != 1 ]; then
    echo "FAIL $APPACT_READ_SRC: field 8 must be composed from AppActiveBits (USB and BLE can both be set)"
    wifi_guard_fail=1
fi
if [ "$(grep -rc 'AppActive::' src --include=*.h | grep -vc ':0$')" != 0 ]; then
    echo "FAIL: the old single-value AppActive enum is back (field 8 is a bit field now)"
    wifi_guard_fail=1
fi
[ $wifi_guard_fail = 0 ] && echo "PASS: option off by default, password write-only in NVS, writes USB-only, field 8 a bit field"
fail=$((fail + wifi_guard_fail))
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

echo "### native net-ID rule tests (a write arms the move; the live identity moves with its reply already sent)"
build_run "netid" "netid_test.cpp"
echo

# The System f7 trigger itself cannot be linked on the host (it pulls the board's static memory),
# so its wiring is asserted on the source. Three things have to hold together:
#   - the trigger ARMS the move (ArmNetId) and still stores the value - a write that only stored
#     the space was reverted by the next Save All, so a restore could not move a wiped core onto
#     the setup's net and every subscription install went to a net no node answers (t_ab3e764a);
#   - the trigger does NOT move the live identity itself: the reply to that very write is built
#     from, and routed by, the address the requester used, so applying the new one first made the
#     acknowledgement leave from an address the client never used - the write landed and the
#     client reported a timeout (t_cac38f19, rig: netid_set.py 39 -> "no matched reply");
#   - the write handler applies the armed move only AFTER sending the reply for that write.
echo "### source guard: the NetID write arms the move, and it lands after that write's reply"
NETID_TRIGGER="$ROOT/src/Core/Services/RegisterRead.h"
NETID_WRITE="$ROOT/src/Core/Services/RegisterWrite.h"
STATIC_WRITE_ORDER="$(awk '/^inline void RegisterService::HandleStaticBlockWrite/{f=1} f{print} f&&/^}/{exit}' "$NETID_WRITE" \
    | grep -n 'SendResponse(Frame, Frame.Payload, PayloadBytes(Frame));\|ApplyArmedNetId(DeviceStatus.NetId, DeviceStatus.ShortAddress);' \
    | sed 's/^[0-9]*:[[:space:]]*//' | tr '\n' '|')"
if [ "$(grep -c 'if (!ArmNetId(Value)) return false;' "$NETID_TRIGGER")" != 1 ] \
   || [ "$(grep -c 'memcpy(&StaticPer.System.NetId, Data, 1);' "$NETID_TRIGGER")" != 1 ] \
   || [ "$(grep -c 'ApplyArmedNetId\|ApplyNetId' "$NETID_TRIGGER")" != 0 ] \
   || [ "$STATIC_WRITE_ORDER" != "SendResponse(Frame, Frame.Payload, PayloadBytes(Frame));|ApplyArmedNetId(DeviceStatus.NetId, DeviceStatus.ShortAddress);|" ]; then
    echo "FAIL: the NetID trigger must arm the move + store it (never move the live identity itself),"
    echo "      and HandleStaticBlockWrite must apply the armed move after its reply (got '$STATIC_WRITE_ORDER')"
    fail=$((fail + 1))
else
    echo "PASS: the NetID write arms the live move, stores it, and applies it after its own reply"
fi
echo

# t_4c9c5fa8, structural half: the board's RSBus cannot be linked on the host (it pulls the UART
# driver, esp_timer and the app link), so the parts of the fix that ARE the fix are asserted on the
# source. A regression here is silent on the host and only shows up as "a node that never answered"
# on the rig, which is exactly what cost a whole restore cycle to find:
#   - nothing on this path may DISCARD received bytes: no uart_flush_input (it ran after the
#     silence wait, whose cap can expire mid-frame with the tail still in the driver ring) and no
#     buffered-length-then-read drain loop;
#   - the silence wait observes the line through the keep path, and the assembler's byte source
#     reads that keep ring BEFORE the UART (byte order: the kept bytes are the older ones);
#   - the drop counter exists and is reported in the BUSGATE totals line beside rxrejects.
echo "### source guard: RSBus keeps received bytes, drops none silently, and reports what it loses"
TAMU_RSBUS="$ROOT/src/Devices/Tamu_v2.0A/RSBus.h"
BUS_H="$ROOT/src/Core/Functions/Bus.h"
APP_IFACE="$ROOT/src/Core/Functions/AppInterface.h"
rxbus_fail=0
if grep -q 'uart_flush_input' "$TAMU_RSBUS"; then
    echo "FAIL $TAMU_RSBUS: uart_flush_input is back on the transmit path - it can remove a reply"
    rxbus_fail=1
fi
if grep -q 'uart_get_buffered_data_len' "$TAMU_RSBUS"; then
    echo "FAIL $TAMU_RSBUS: a buffered-length/drain loop is back - it consumed the bytes it counted"
    rxbus_fail=1
fi
if [ "$(grep -c 'RS485KeepBufferedRx' "$TAMU_RSBUS")" -lt 2 ]; then
    echo "FAIL $TAMU_RSBUS: the silence wait and the pre-TX read must both go through RS485KeepBufferedRx"
    rxbus_fail=1
fi
# The byte source: ReceivePacket() must take the keep ring first, then the UART.
approx_src=$(sed -n '/^int ReceivePacket(PacketFrame \*Data)/,$p' "$TAMU_RSBUS")
if ! printf '%s\n' "$approx_src" | grep -q 'RxKeep.Pop(Byte)' ||
   ! printf '%s\n' "$approx_src" | grep -q 'uart_read_bytes(UART_NUM_1, &Byte, 1, 0)'; then
    echo "FAIL $TAMU_RSBUS: ReceivePacket must read the keep ring first and then the UART"
    rxbus_fail=1
fi
if ! grep -q 'uint32_t BusRxDestroyed = 0;' "$BUS_H"; then
    echo "FAIL $BUS_H: BusRxDestroyed is gone - a dropped RX byte would be invisible again"
    rxbus_fail=1
fi
if ! grep -q 'rxdestroyed=%lu' "$APP_IFACE"; then
    echo "FAIL $APP_IFACE: the BUSGATE totals line must report rxdestroyed= beside rxrejects="
    rxbus_fail=1
fi
# The transmit stint: one deadline for the whole send, and the app link served inside it.
if ! grep -q 'RS485_TX_STINT_MS' "$TAMU_RSBUS" || ! grep -q 'RS485AppLinkTick' "$TAMU_RSBUS"; then
    echo "FAIL $TAMU_RSBUS: the send must be bounded by RS485_TX_STINT_MS and must tick the app link"
    rxbus_fail=1
fi
[ $rxbus_fail = 0 ] && echo "PASS: no RX byte is flushed or drained away, the keep ring feeds the assembler first, and the loss is counted and reported"
fail=$((fail + rxbus_fail))
echo

# t_e5f46574. The bus transaction gate's decisions (Core/Functions/BusGate.h) run here: which
# frames are transactions at all, the window per request kind, the wrap-safe expiry, and which
# queued frame the pump may dispatch. The device half - the slot, the deferral ring, the
# diagnostics ring - cannot be linked on the host (FreeRTOS, the app link, the storage layer), so
# the two things it must not lose are asserted on the source below.
#
# What the test pins, in the card's terms: the gate's scope is the wire and not the frame's origin
# (a core-origin request carrying REQACK takes the same slot as an app frame - two of them used to
# leave 3-10 ms apart with the first answer still owed); the window is per request kind (Save All
# keeps its own, so the reclaim path is not cut off by a read's figure); and the head-of-line block
# is gone (a core-local frame behind a blocked forward is dispatched while it waits).
echo "### native bus-gate tests (one answered unicast at a time, per-kind windows, no head-of-line block)"
build_run "busgate" "bus_gate_test.cpp"
echo

# The device half, structurally. Each of these was a real defect on this card:
#   - the gate must be asked at the ONE door a frame leaves by (RSBus.h SendAndVerifyPacket), not
#     in the dispatcher, which only saw the app's frames and never the core's own sends;
#   - the dispatcher must no longer decide by AppFrameSource - that is the app-only scope - and
#     must no longer leave its RX loop on a blocked head;
#   - the pump must pick with BusGateQueuePick (in order, except past a blocked head to a frame
#     that needs no bus) and the slot must expire through BusGateHeld;
#   - the BLE reader must count what its queue refuses, like the USB reader, and the totals line
#     must print both transports' counters.
echo "### source guard: the gate is asked at the transmit door, and the RX queue has no head-of-line block"
BG_RSBUS="$ROOT/src/Devices/Tamu_v2.0A/RSBus.h"
BG_IFACE="$ROOT/src/Core/Functions/AppInterface.h"
BG_DISPATCH="$ROOT/src/Core/Functions/Dispatcher.h"
BG_BLE="$ROOT/src/Devices/Tamu_v2.0A/AppBLE.h"
BG_GATE="$ROOT/src/Core/Functions/BusGate.h"
bg_fail=0

# One gated transmit function, and the transmit body behind it.
if [ "$(grep -c '^bool SendAndVerifyPacket(const PacketFrame &Data)$' "$BG_RSBUS")" != 1 ] ||
   [ "$(grep -c '^static bool RS485TransmitVerified(const PacketFrame &Data)$' "$BG_RSBUS")" != 1 ]; then
    echo "FAIL $BG_RSBUS: expected exactly one gated SendAndVerifyPacket over one RS485TransmitVerified body"
    bg_fail=1
fi
if ! sed -n '/^bool SendAndVerifyPacket(const PacketFrame &Data)$/,/^}$/p' "$BG_RSBUS" |
        grep -q 'if (!AppBusGateOffer(Data))'; then
    echo "FAIL $BG_RSBUS: SendAndVerifyPacket must offer the frame to the gate before transmitting it"
    bg_fail=1
fi
if ! sed -n '/^bool SendAndVerifyPacket(const PacketFrame &Data)$/,/^}$/p' "$BG_RSBUS" |
        grep -q 'AppBusForwardFinish();'; then
    echo "FAIL $BG_RSBUS: a transmit that never reached the wire must release the slot it opened"
    bg_fail=1
fi
if ! sed -n '/^bool SendAndVerifyPacket(const PacketFrame &Data)$/,/^}$/p' "$BG_RSBUS" |
        grep -q 'AppBusForwardSent();'; then
    echo "FAIL $BG_RSBUS: the window must start when the frame is out (AppBusForwardSent), not before the CSMA wait"
    bg_fail=1
fi

# The dispatcher no longer gates by origin, and no longer blocks the queue on its head.
if [ "$(grep -c 'AppFrameSource != AppLinkSource::None' "$BG_DISPATCH")" != 0 ]; then
    echo "FAIL $BG_DISPATCH: the gate's scope is back to app-origin frames only (AppFrameSource)"
    bg_fail=1
fi
if [ "$(grep -c 'AppLinkFrameNeedsBus(Frame)) break' "$BG_DISPATCH")" != 0 ]; then
    echo "FAIL $BG_DISPATCH: the RX loop leaves on a blocked head again (the head-of-line block)"
    bg_fail=1
fi
if [ "$(grep -c 'AppBusForwardSawReply(Frame.Trid, Frame.Cmd);' "$BG_DISPATCH")" != 1 ] ||
   grep -q 'if ((Frame.Flags & FLAG_TYPE) && Frame.IdSrc != DeviceStatus.ShortAddress)' "$BG_DISPATCH"; then
    echo "FAIL $BG_DISPATCH: any frame from another device matching the open TRID releases the slot (a value update's confirmation is not FLAG_TYPE)"
    bg_fail=1
fi
# ... and the release matches the CMD as well as the TRID: a provider's value update carries the
# subscription TRID of a Change that is still owed its answer (BusGateIsAnswer in BusGate.h).
if ! grep -q 'AppInterface.BusPendingCmd == Cmd' "$BG_IFACE"; then
    echo "FAIL $BG_IFACE: the slot must not close on a frame that echoes the TRID but not the CMD (a subscription value update)"
    bg_fail=1
fi

# The device half of the gate: the offer, the pick and the expiry all go through BusGate.h.
if ! grep -q 'switch (BusGateVerdictFor(AppBusForwardPending(), AppBusDeferredCount < APP_BUS_DEFER_DEPTH))' "$BG_IFACE" ||
   ! grep -q 'if (AppBusDefer(Frame))' "$BG_IFACE" ||
   ! grep -q 'BusGateNeedsAnswer(Frame.IdTgt, Frame.Flags, Frame.Cmd)' "$BG_IFACE" ||
   ! grep -q 'BusGateWindowMs(Frame.Cmd, AppFrameSource == AppLinkSource::None)' "$BG_IFACE"; then
    echo "FAIL $BG_IFACE: AppBusGateOffer must decide with the host-tested helpers (BusGateVerdictFor/NeedsAnswer/WindowMs)"
    bg_fail=1
fi
if ! grep -q 'Pick = BusGateQueuePick(NeedsBus, RxCount, GateOpen);' "$BG_IFACE"; then
    echo "FAIL $BG_IFACE: Pump must pick its frame with BusGateQueuePick"
    bg_fail=1
fi
if ! grep -q 'if (!BusGateHeld(DeviceStatus.UptimeMs, AppInterface.BusPendingSince,' "$BG_IFACE"; then
    echo "FAIL $BG_IFACE: the slot must expire through BusGateHeld (wrap-safe, per-kind window)"
    bg_fail=1
fi

# Both link loss paths, counted and reported.
if [ "$(grep -c 'if (!AppInterface.Enqueue(BleParser.Frame, AppLinkSource::BLE))' "$BG_BLE")" != 1 ] ||
   [ "$(grep -c 'AppLinkEnqueueDropsBle = AppLinkEnqueueDropsBle + 1;' "$BG_BLE")" != 1 ]; then
    echo "FAIL $BG_BLE: a BLE frame the inbound queue refuses must be counted, not dropped silently"
    bg_fail=1
fi
if [ "$(grep -c 'AppInterface.Enqueue(BleParser.Frame, AppLinkSource::BLE);' "$BG_BLE")" != 0 ]; then
    echo "FAIL $BG_BLE: the BLE reader still ignores the result of AppInterface.Enqueue"
    bg_fail=1
fi
if ! grep -q '"link usb rejects=%lu drops=%lu | ble rejects=%lu drops=%lu"' "$BG_IFACE"; then
    echo "FAIL $BG_IFACE: the BUSGATE totals line must report both transports' rejects and drops"
    bg_fail=1
fi
[ $bg_fail = 0 ] && echo "PASS: one gated door onto the bus, no origin-based scope, no head-of-line block, both drop counters reported"
fail=$((fail + bg_fail))
echo

# ---- the four regressions of the second-pass review (t_ab6c7bdc) ----
#
# Two of the four live in wiring that cannot be linked on the host - the deferred ring's origin and the
# latch's stack read - so they are asserted here on the source, while the decisions they feed run in
# bus_gate_test.cpp and ble_latch_test.cpp above. The guards are separate scripts because each names
# the defect it pins and can be run on its own.
echo "### source guard: a deferred frame carries its origin, and an answer-less hold is counted"
if ! bash "$ROOT/test/native/bus_defer_guard.sh" "$BG_IFACE" "$BG_DISPATCH" "$BG_GATE"; then
    fail=$((fail + 1))
fi
echo

echo "### source guard: the latch's liveness read allocates nothing, and its heal ends the session"
if ! bash "$ROOT/test/native/ble_liveness_guard.sh" "$BG_BLE"; then
    fail=$((fail + 1))
fi
echo

# ---- the radio's survival fixes (t_44676a52) ----
#
# The decisions live in three host-tested headers; what the source guards pin is the WIRING - that the
# device code goes through them rather than keeping a second copy of the old behaviour beside them.
BLE_SRC="$ROOT/src/Devices/Tamu_v2.0A/AppBLE.h"
USB_SRC="$ROOT/src/Devices/Tamu_v2.0A/AppUSB.h"
IFACE_SRC="$ROOT/src/Core/Functions/AppInterface.h"
ls_fail=0

# (A/B) The single-session latch: no bare handle, every teardown routed through the supervisor, the
# pairing deadline armed on the refusal paths too, and the tick supervising the latch.
if ! grep -q 'BleLatchTake(StaticBleLatch, ConnInfo.getConnHandle())' "$BLE_SRC" ||
   ! grep -q 'BleLatchSupervise();' "$BLE_SRC" ||
   ! grep -q 'BleLatchTick(StaticBleLatch, BleHandleLive(StaticBleLatch.Drop),' "$BLE_SRC"; then
    echo "FAIL $BLE_SRC: the connect gate and the tick must use the host-tested latch (BleLinkLatch.h)"
    ls_fail=1
fi
# The self-heal is checked against the STACK's own connection table, not against our own flags, and a
# link that never held the session must not clear the session's state when it ends. The stack read
# itself (one allocation-free ble_gap_conn_find) and the heal's WasSession hand-off are pinned by
# test/native/ble_liveness_guard.sh below - the greps here only say the latch is asked at all.
if ! grep -q 'BleLatchHeld(StaticBleLatch) && !BleHandleLive(StaticBleLatch.Session)' "$BLE_SRC"; then
    echo "FAIL $BLE_SRC: a stale latch must be detected from the stack's live-connection table"
    ls_fail=1
fi
if [ "$(grep -c 'BleConnHandle' "$BLE_SRC")" != 0 ]; then
    echo "FAIL $BLE_SRC: the bare BleConnHandle is back - the latch must own the session handle"
    ls_fail=1
fi
# Two disconnect call sites only: BleDropLink (which records and checks the answer) and the
# supervisor's retry. Anywhere else is a refusal that throws its answer away again.
if [ "$(grep -c -- '->disconnect(' "$BLE_SRC")" != 2 ]; then
    echo "FAIL $BLE_SRC: a teardown bypasses BleDropLink, so its result is unchecked again"
    ls_fail=1
fi
# The refusals arm the pairing deadline before returning (the six connect/auth refusal sites).
if [ "$(grep -c 'BleArmSecureDeadline(ConnInfo.getConnHandle(), BLE_REFUSAL_GRACE_MS' "$BLE_SRC")" != 6 ]; then
    echo "FAIL $BLE_SRC: a refusal path leaves a link up without a deadline (want 6 armed refusals)"
    ls_fail=1
fi
# The deadline is assigned in exactly one place - BleClearSecureDeadline - so it can never be zeroed
# before the drop has been handed to the supervisor (which is what B was).
if [ "$(grep -c '^    BleSecureDeadlineMs = 0;' "$BLE_SRC")" != 1 ]; then
    echo "FAIL $BLE_SRC: the deadline is cleared somewhere other than BleClearSecureDeadline (it must follow the drop, not precede it)"
    ls_fail=1
fi

# (C) The RX assembler: bounded, timed, and its resync resets the packet parser with it.
if ! grep -q 'StaticBleRxAssembler.Feed((uint8_t)Value\[i\], TimeFromBoot(), &Byte)' "$BLE_SRC"; then
    echo "FAIL $BLE_SRC: the assembler is fed without its clock, so a truncated chunk desyncs for ever"
    ls_fail=1
fi
if ! grep -q 'if (F.Resynced)' "$BLE_SRC" || ! grep -q 'BleParser.Reset();' "$BLE_SRC"; then
    echo "FAIL $BLE_SRC: a resync must reset the packet parser too (the caller's half of the realignment)"
    ls_fail=1
fi
if ! grep -q 'StaticBleRxAssembler.MaxNeed = BleMaxChunkForMtu(MTU);' "$BLE_SRC"; then
    echo "FAIL $BLE_SRC: the assembler's bound does not follow the negotiated MTU"
    ls_fail=1
fi

# (D) The USB TX path: a bounded pass, and a skip that does not depend on BLE being active.
if grep -q 'pdMS_TO_TICKS(50)' "$USB_SRC" || ! grep -q 'pdMS_TO_TICKS(USB_TX_WRITE_MS)' "$USB_SRC"; then
    echo "FAIL $USB_SRC: a 50 ms write is back - one pass could block 200 ms again"
    ls_fail=1
fi
if ! grep -q '#define USB_TX_PASS_BUDGET_MS 20' "$USB_SRC" || ! grep -q 'Attempt < 2' "$USB_SRC"; then
    echo "FAIL $USB_SRC: the pass budget must stay 20 ms (two writes of 10 ms)"
    ls_fail=1
fi
if ! grep -q 'AppUSBActive() && TxRing && !AppUsbSilent() && !AppUsbTxStalled()' "$IFACE_SRC"; then
    echo "FAIL $IFACE_SRC: TxFlush must skip a silent or stalled USB port regardless of BLE"
    ls_fail=1
fi
if ! grep -q 'UsbTx.TakeReport(Now)' "$USB_SRC" || ! grep -q 'UsbTx.Clear();' "$USB_SRC"; then
    echo "FAIL $USB_SRC: the stall must be purged once per episode and cleared when the host speaks"
    ls_fail=1
fi

# (E) A radio start that fails is backed off, not re-initialised every pass.
if ! grep -q 'BleRadioStartAttempt();' "$BLE_SRC" || ! grep -q 'if (BleStartGaveUp)' "$BLE_SRC"; then
    echo "FAIL $BLE_SRC: a failed radio start must go through the backoff/give-up path"
    ls_fail=1
fi
if grep -q 'AppBLERadioStart(); // first enable' "$BLE_SRC"; then
    echo "FAIL $BLE_SRC: the unbounded start site is back"
    ls_fail=1
fi
[ $ls_fail = 0 ] && echo "PASS: the latch is supervised, the assembler bounded and timed, the USB pass capped and skipped, the radio start backed off"
fail=$((fail + ls_fail))
echo

exit $fail
