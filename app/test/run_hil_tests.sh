#!/usr/bin/env bash
# Run all HIL tests sequentially. Tests require the Tamu core on a USB port.
#
# Usage:
#   TAMU_HIL=/dev/ttyACM1 bash test/run_hil_tests.sh          # serial
#   TAMU_HIL=ble       bash test/run_hil_tests.sh              # BLE
#
# To run a single file:
#   TAMU_HIL=/dev/ttyACM1 bash test/run_hil_tests.sh test/hil_live_test.dart
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="$(dirname "$SCRIPT_DIR")"

if [ -z "${TAMU_HIL:-}" ]; then
  echo "ERROR: TAMU_HIL is not set. Set it to /dev/ttyACM1 (serial) or 'ble' (BLE)."
  exit 1
fi

# libserialport native lib
export LD_LIBRARY_PATH="${APP_DIR}/build/linux/x64/debug/bundle/lib:${LD_LIBRARY_PATH:-}"

# Which files to run
if [ $# -gt 0 ]; then
  FILES=("$@")
else
  FILES=(
    test/hil_test_suite.dart
    test/hil_subscriptions_test.dart
    test/tamu_hardware_verification_test.dart
    test/hardware_register_test.dart
    test/hardware_storage_test.dart
    test/dyn_flow_test.dart
    test/ble_scan_probe_test.dart
    test/hil_ble_probe_test.dart
    test/ble_ping_test.dart
    test/ble_rtt_probe_test.dart
  )
fi

cd "$APP_DIR"

PASSED=0
FAILED=0
SKIPPED=0

for f in "${FILES[@]}"; do
  if [ ! -f "$f" ]; then
    echo "SKIP  $f (file not found)"
    SKIPPED=$((SKIPPED + 1))
    continue
  fi
  echo "--- Running $f ---"
  if flutter test --no-pub "$f" 2>&1; then
    echo "--- $f PASSED ---"
    PASSED=$((PASSED + 1))
  else
    echo "--- $f FAILED ---"
    FAILED=$((FAILED + 1))
  fi
  echo
done

echo "========================================"
echo "Results: $PASSED passed, $FAILED failed, $SKIPPED skipped"
echo "========================================"
[ "$FAILED" -eq 0 ]
