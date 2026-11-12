#!/bin/bash
# Build the shipped images and refresh ../binaries with the current ones.
#
# The folder is git-ignored on purpose: these are build outputs, not sources. What belongs in git
# is the source and the version stamp; what a person needs when flashing a device from a laptop is
# the .bin. Names are stable so a flashing instruction never changes:
#     binaries/Tamu_v2_0A.bin        core application image   (flash at ota_0: 0x90000)
#     binaries/Tamu_bootloader.bin   core factory image       (flash at factory: 0x10000)
#     binaries/DAS_v0_1.bin          DAS node application     (through the core's passthrough)
#     binaries/DAS_bootloader.bin    DAS node bootloader      (WCH-Link / one-time flash)
# MANIFEST.txt carries each image's version stamp, size and sha256, plus the commit it was built
# from, so "which image is on this device" is answerable from the file itself.
#
# usage: firmware/tools/collect_binaries.sh [env ...]     (default: the four shipped images)
set -u
cd "$(dirname "$0")/.." || exit 1          # -> firmware/
REPO=$(cd .. && pwd)
OUT="$REPO/binaries"
PIO="$HOME/.platformio/penv/bin/pio"
ENVS=${*:-"Tamu_v2_0A Tamu_bootloader DAS_v0_1 DAS_bootloader"}

mkdir -p "$OUT" || exit 1
fail=0
for e in $ENVS; do
  echo "== building $e"
  if ! "$PIO" run -e "$e" >/tmp/collect_$e.log 2>&1; then
    echo "   BUILD FAILED: see /tmp/collect_$e.log"
    fail=1
    continue
  fi
  src=".pio/build/$e/firmware.bin"
  if [ ! -f "$src" ]; then echo "   no firmware.bin for $e"; fail=1; continue; fi
  cp -f "$src" "$OUT/$e.bin" || { echo "   copy failed"; fail=1; continue; }
  echo "   -> binaries/$e.bin  $(stat -c %s "$OUT/$e.bin") B"
done

{
  echo "# binaries/ — built $(date -u '+%Y-%m-%d %H:%M UTC')"
  echo "# commit:  $(git -C "$REPO" log --oneline -1 2>/dev/null)"
  echo "# dirty:   $(git -C "$REPO" status --porcelain | wc -l) path(s) modified"
  echo "#"
  for f in "$OUT"/*.bin; do
    [ -e "$f" ] || continue
    printf '%-28s %8s B  %s\n' "$(basename "$f")" "$(stat -c %s "$f")" "$(sha256sum "$f" | cut -c1-32)"
  done
} > "$OUT/MANIFEST.txt"

echo "== manifest"
cat "$OUT/MANIFEST.txt"
exit $fail
