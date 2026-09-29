#!/usr/bin/env bash
# Runs every suite that does not need the hardware.
#
#   * firmware - the native numeric-core tests (Core/Types/*.h compiled and run on the host,
#                once as the core build and once as the DAS build)
#   * app      - the host test suite (the `hil` suites are excluded; those need the device,
#                and are run with app/test/run_hil_tests.sh)
#   * analyzer - flutter analyze, which must stay clean
#
# Usage: ./test.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"

echo "=== firmware: native numeric-core tests ==="
"$ROOT/firmware/test/native/run.sh"

echo
echo "=== app: host tests ==="
cd "$ROOT/app"
flutter test --exclude-tags hil

echo
echo "=== app: analyzer ==="
flutter analyze
