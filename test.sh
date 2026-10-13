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

# Flutter is often installed outside PATH. Use an explicit FLUTTER, then PATH, then the known
# install locations (the project's canonical one first - it may live in another user's home),
# so the gate runs the same from any shell.
FLUTTER="${FLUTTER:-$(command -v flutter || true)}"
if [[ -z "$FLUTTER" ]]; then
  for _c in /home/akyirr/Programs/Flutter/flutter/bin/flutter \
            "$HOME/Programs/Flutter/flutter/bin/flutter" "$HOME/flutter/bin/flutter" \
            /opt/flutter/bin/flutter /usr/local/flutter/bin/flutter; do
    if [[ -x "$_c" ]]; then FLUTTER="$_c"; break; fi
  done
fi
if [[ -z "$FLUTTER" ]]; then
  echo "test.sh: flutter not found - set FLUTTER=<path to flutter> or put it on PATH" >&2
  exit 1
fi

echo "=== firmware: native numeric-core tests ==="
"$ROOT/firmware/test/native/run.sh"

echo
echo "=== app: host tests ==="
cd "$ROOT/app"
"$FLUTTER" test --exclude-tags hil

echo
echo "=== app: analyzer ==="
"$FLUTTER" analyze
