#!/usr/bin/env bash
# Run HIL tests in one batch. Tests require the Tamu core on a USB port.
#
# Usage (paths are relative to app/):
#   TAMU_HIL=/dev/ttyACM1 bash test/run_hil_tests.sh          # serial
#   TAMU_HIL=ble       bash test/run_hil_tests.sh              # BLE
#
# To run a single file:
#   TAMU_HIL=/dev/ttyACM1 bash test/run_hil_tests.sh test/tamu_hardware_verification_test.dart
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="$(dirname "$SCRIPT_DIR")"

if [ -z "${TAMU_HIL:-}" ]; then
  echo "ERROR: TAMU_HIL is not set. Set it to /dev/ttyACM1 (serial) or 'ble' (BLE)."
  exit 1
fi

# libserialport native lib ships next to the debug bundle. Resolve the arch directory
# dynamically (the old hardcoded linux/x64 path broke on other hosts).
for lib in "$APP_DIR"/build/linux/*/debug/bundle/lib; do
  if [ -d "$lib" ]; then
    export LD_LIBRARY_PATH="${lib}:${LD_LIBRARY_PATH:-}"
    break
  fi
done

# The default is the evaluation-setup suite: it applies the full setup and restores it, so it
# needs the displays and fans attached. Run the feature suites explicitly when a change touches
# them, e.g.
#   TAMU_HIL=/dev/ttyACM0 bash test/run_hil_tests.sh test/hil_script_test.dart \
#       test/hil_script_vm_test.dart test/hil_dynamic_persistence_test.dart \
#       test/hil_backup_test.dart test/hil_storage_files_test.dart \
#       test/hil_led_display_test.dart test/tamu_hardware_verification_test.dart
# NOTE: those suites are destructive - they upload/unload their own scripts and reconfigure
# blocks, leaving the device off the evaluation setup. Run the setup suite afterwards (or
# last) to restore it:
#   TAMU_HIL=/dev/ttyACM0 bash test/run_hil_tests.sh test/hil_current_setup_test.dart
if [ $# -gt 0 ]; then
  FILES=("$@")
else
  FILES=(
    test/hil_current_setup_test.dart
  )
fi

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

echo "--- Running ${#EXISTING[@]} file(s) in one flutter test ---"
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
