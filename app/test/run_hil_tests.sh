#!/usr/bin/env bash
# Unified HIL entry point (Docs/HIL Tests.md).
#
#   bash test/run_hil_tests.sh                       # every category except bootloader
#   bash test/run_hil_tests.sh --scan                # list the rig and stop
#   bash test/run_hil_tests.sh --category module     # one category
#   bash test/run_hil_tests.sh --bootloader          # the bootloader category (runs separately)
#   bash test/run_hil_tests.sh --device 2            # one device
#   bash test/run_hil_tests.sh --category module --device 2
#   TAMU_HIL=/dev/ttyACM0 bash test/run_hil_tests.sh # explicit port
#   TAMU_HIL=ble bash test/run_hil_tests.sh          # BLE
#   TAMU_HIL_BOOTLOADER=1 bash test/run_hil_tests.sh # a device is in bootloader mode
#
# The core port is resolved from /dev/serial/by-id when TAMU_HIL is unset, because
# the /dev/ttyACM<N> numbering changes between sessions and on replug. Every run
# writes a log into test/hil/logs/ (gitignored) and prints the rig inventory.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="$(dirname "$SCRIPT_DIR")"

while [ $# -gt 0 ]; do
  case "$1" in
    --scan) export TAMU_HIL_SCAN=1 ;;
    --bootloader) export TAMU_HIL_CATEGORY=bootloader ;;
    --category) shift; export TAMU_HIL_CATEGORY="${1:-}" ;;
    --category=*) export TAMU_HIL_CATEGORY="${1#--category=}" ;;
    --device) shift; export TAMU_HIL_DEVICE="${1:-}" ;;
    --device=*) export TAMU_HIL_DEVICE="${1#--device=}" ;;
    -h|--help)
      sed -n '2,/^set -euo pipefail/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "ERROR: unknown option '$1' (use --scan, --category <name>, --device <id>)" >&2
      exit 2
      ;;
  esac
  shift
done

if [ -z "${TAMU_HIL:-}" ]; then
  # A flash or a replug re-enumerates the device and /dev/serial/by-id briefly
  # disappears, so poll for the port instead of failing on that race.
  port=""
  for _ in $(seq 1 24); do
    for link in /dev/serial/by-id/*Espressif*; do
      [ -e "$link" ] || continue
      port="$(readlink -f "$link")"
      break
    done
    [ -n "$port" ] && break
    sleep 0.5
  done
  if [ -n "$port" ]; then
    export TAMU_HIL="$port"
    echo "resolved core port: $TAMU_HIL (from /dev/serial/by-id)"
  else
    echo "ERROR: no Tamu core found after 12 s. Set TAMU_HIL=/dev/ttyACM<N>," >&2
    echo "       or TAMU_HIL=ble for the BLE path." >&2
    exit 1
  fi
fi

# libserialport native lib ships next to the debug bundle; resolve the arch dir.
for lib in "$APP_DIR"/build/linux/*/debug/bundle/lib; do
  if [ -d "$lib" ]; then
    export LD_LIBRARY_PATH="${lib}:${LD_LIBRARY_PATH:-}"
    break
  fi
done

cd "$APP_DIR"
exec flutter test --no-pub --concurrency=1 test/hil/hil_runner.dart
