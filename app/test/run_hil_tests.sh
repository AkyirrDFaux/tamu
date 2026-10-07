#!/usr/bin/env bash
# Run HIL tests in one batch. Tests require the Tamu core on a USB port.
#
# Usage (paths are relative to app/):
#   bash test/run_hil_tests.sh                       # rig profile (default: small)
#   bash test/run_hil_tests.sh --rig full            # evaluation-setup suite (needs 2 DAS)
#   TAMU_HIL=/dev/ttyACM0 bash test/run_hil_tests.sh test/hil_script_test.dart
#   TAMU_HIL=ble bash test/run_hil_tests.sh          # BLE
#
# Rig profiles:
#   small (default)  core + 1 DAS, no display/fan rig. Runs the hardware verification sweep.
#   full             core + 2 DAS + displays/fans. The evaluation-setup suite: it applies the
#                    whole setup and restores it, and both halves need two DAS nodes.
#
# TAMU_HIL is resolved from /dev/serial/by-id when it is unset, because the /dev/ttyACM<N>
# numbering is assigned in enumeration order and changes between sessions and on replug.
# Set TAMU_HIL to override, or to 'ble' for the BLE path.
#
# The feature suites are destructive - they upload/unload their own scripts and reconfigure
# blocks, leaving the device off the evaluation setup. On a `small` rig nothing can put it
# back (restoring needs the two-DAS setup), so run them only when that is acceptable:
#   bash test/run_hil_tests.sh test/hil_script_test.dart \
#       test/hil_script_vm_test.dart test/hil_dynamic_persistence_test.dart \
#       test/hil_backup_test.dart test/hil_storage_files_test.dart \
#       test/hil_led_display_test.dart test/tamu_hardware_verification_test.dart
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="$(dirname "$SCRIPT_DIR")"

RIG="${TAMU_RIG:-small}"
FILES=()

while [ $# -gt 0 ]; do
  case "$1" in
    --rig)
      shift
      RIG="${1:-}"
      if [ -z "$RIG" ]; then
        echo "ERROR: --rig needs a value (small|full)"
        exit 2
      fi
      ;;
    --rig=*) RIG="${1#--rig=}" ;;
    -h|--help)
      # The header comment block: from line 3 up to (not including) the `set -euo` line.
      sed -n '3,/^set -euo pipefail/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) FILES+=("$1") ;;
  esac
  shift
done

# The Espressif USB JTAG device is the core. Resolve by identity, never by number.
resolve_core_port() {
  local link
  for link in /dev/serial/by-id/*Espressif*; do
    [ -e "$link" ] || continue
    readlink -f "$link"
    return 0
  done
  return 1
}

# Count attached WCH-Link *probes*. This is NOT the number of DAS nodes: a node running on the
# RSBus has no USB presence at all, and its probe is only connected while it is being flashed.
# So this can read 0 on a fully populated rig, or 1 while that one node is halted by a probe
# attach. It is only ever a hint - never a gate on whether a run can work.
count_probes() {
  local link n=0
  for link in /dev/serial/by-id/*WCH-Link*; do
    [ -e "$link" ] || continue
    n=$((n + 1))
  done
  echo "$n"
}

if [ -z "${TAMU_HIL:-}" ]; then
  # A flash or a replug re-enumerates the device and /dev/serial/by-id briefly disappears,
  # so poll for the port instead of failing the run on that race.
  port=""
  for _ in $(seq 1 24); do
    if port="$(resolve_core_port)"; then
      break
    fi
    port=""
    sleep 0.5
  done

  if [ -n "$port" ]; then
    export TAMU_HIL="$port"
    echo "resolved core port: $TAMU_HIL (from /dev/serial/by-id)"
  else
    echo "ERROR: no Tamu core found after 12 s."
    echo "  Expected an Espressif USB JTAG device under /dev/serial/by-id - the ports"
    echo "  renumber between sessions. If it is attached under another name, set:"
    echo "    TAMU_HIL=/dev/ttyACM0 bash test/run_hil_tests.sh"
    echo "  (or TAMU_HIL=ble for the BLE path)."
    exit 1
  fi
fi

PROBES="$(count_probes)"

case "$RIG" in
  small)
    if [ "${#FILES[@]}" -eq 0 ]; then
      FILES=(test/tamu_hardware_verification_test.dart)
    fi
    ;;
  full)
    # Not a hard gate: the host cannot see how many DAS nodes are on the bus, only how many
    # probes are attached. Warn and let the suite itself report what it finds.
    if [ "$PROBES" -lt 2 ]; then
      echo "NOTE: rig 'full' runs the evaluation-setup suite, which needs 2 DAS nodes; this host"
      echo "      shows $PROBES WCH-Link probe(s). Probes are not nodes, so this is only a hint -"
      echo "      but if fewer than 2 nodes are attached, expect setUpAll to fail with"
      echo "      'expected 2 DAS nodes, found N'. On a core + 1 DAS rig use:"
      echo "        bash test/run_hil_tests.sh --rig small"
    fi
    if [ "${#FILES[@]}" -eq 0 ]; then
      FILES=(test/hil_current_setup_test.dart)
    fi
    ;;
  *)
    echo "ERROR: unknown rig '$RIG' (expected small|full)"
    exit 2
    ;;
esac

# libserialport native lib ships next to the debug bundle. Resolve the arch directory
# dynamically (the old hardcoded linux/x64 path broke on other hosts).
for lib in "$APP_DIR"/build/linux/*/debug/bundle/lib; do
  if [ -d "$lib" ]; then
    export LD_LIBRARY_PATH="${lib}:${LD_LIBRARY_PATH:-}"
    break
  fi
done

cd "$APP_DIR"

# Keep only files that exist, then run them in a single `flutter test` so the Flutter tool
# starts once instead of once per file.
EXISTING=()
SKIPPED=0
for f in "${FILES[@]}"; do
  if [ -f "$f" ]; then
    EXISTING+=("$f")
  else
    echo "SKIP  $f (file not found)"
    SKIPPED=$((SKIPPED + 1))
  fi
done

if [ "${#EXISTING[@]}" -eq 0 ]; then
  echo "No test files to run."
  echo "Results: 0 passed, 0 failed, $SKIPPED skipped"
  exit 1
fi

echo "--- rig=$RIG  core=$TAMU_HIL  probes=$PROBES  files=${#EXISTING[@]} ---"
if flutter test --no-pub --concurrency=1 "${EXISTING[@]}"; then
  PASSED=${#EXISTING[@]}
  FAILED=0
else
  PASSED=0
  FAILED=${#EXISTING[@]}
fi

echo "========================================"
echo "Results: $PASSED passed, $FAILED failed, $SKIPPED skipped"
echo "========================================"
[ "$FAILED" -eq 0 ]
