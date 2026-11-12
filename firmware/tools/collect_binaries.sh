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
#
# Every image is also packaged next to its .bin as binaries/<env>-<version>.fw.zip: the image plus
# a manifest.json naming the address it belongs at and its sha256 / crc32 / padded length, so a
# flash is checked against a manifest instead of the file that happens to be on disk. The loose
# .bin stays (the flashers use it). A package is checkable without flashing anything:
#     python3 firmware/tools/pack_firmware.py --verify binaries/DAS_v0_1-26.11.12.4.fw.zip
#
# MANIFEST.txt carries each image's version stamp, size and sha256, each package's full sha256,
# plus the commit it was built from, so "which image is on this device" is answerable from the
# file itself.
#
# usage: firmware/tools/collect_binaries.sh [env ...]     (default: the four shipped images)
set -u
cd "$(dirname "$0")/.." || exit 1          # -> firmware/
REPO=$(cd .. && pwd)
OUT="$REPO/binaries"
PIO=${PIO:-$(command -v pio || true)}
[ -n "$PIO" ] || PIO="$HOME/.platformio/penv/bin/pio"
LOGDIR=".pio/collect"                      # build logs stay inside the (ignored) build tree
ENVS=${*:-"Tamu_v2_0A Tamu_bootloader DAS_v0_1 DAS_bootloader"}

mkdir -p "$OUT" "$LOGDIR" || exit 1
fail=0
for e in $ENVS; do
  echo "== building $e"
  if ! "$PIO" run -e "$e" >"$LOGDIR/$e.log" 2>&1; then
    echo "   BUILD FAILED: see firmware/$LOGDIR/$e.log"
    fail=1
    continue
  fi
  src=".pio/build/$e/firmware.bin"
  if [ ! -f "$src" ]; then echo "   no firmware.bin for $e"; fail=1; continue; fi
  # The copy keeps the build artifact's mtime (touch -r rather than cp -p, which cannot preserve
  # the mode of an existing file under this tree's ACLs): the package's built_utc is read from
  # it, so a no-op rebuild produces a byte-identical package with the same sha256.
  cp -f "$src" "$OUT/$e.bin" || { echo "   copy failed"; fail=1; continue; }
  touch -r "$src" "$OUT/$e.bin" || { echo "   could not date the copy"; fail=1; continue; }
  echo "   -> binaries/$e.bin  $(stat -c %s "$OUT/$e.bin") B"
  rm -f "$OUT/$e"-*.fw.zip                 # a re-versioned build must not leave the old package
  if ! python3 tools/pack_firmware.py --target "$e" --image "$OUT/$e.bin" --out-dir "$OUT"; then
    echo "   PACKAGING REFUSED: $e has no package"
    fail=1
  fi
done

{
  echo "# binaries/ — built $(date -u '+%Y-%m-%d %H:%M UTC')"
  echo "# commit:  $(git -C "$REPO" log --oneline -1 2>/dev/null)"
  echo "# dirty:   $(git -C "$REPO" status --porcelain | wc -l) path(s) modified"
  echo "#"
  echo "# images"
  for e in $ENVS; do
    f="$OUT/$e.bin"
    [ -e "$f" ] || continue
    printf '%-30s %8s B  %s\n' "$(basename "$f")" "$(stat -c %s "$f")" \
      "$(sha256sum "$f" | cut -c1-32)"
  done
  echo "#"
  echo "# packages — check without unpacking: firmware/tools/pack_firmware.py --verify <pkg>"
  for p in "$OUT"/*.fw.zip; do
    [ -e "$p" ] || continue
    printf '%-34s %8s B  %s\n' "$(basename "$p")" "$(stat -c %s "$p")" \
      "$(sha256sum "$p" | cut -d' ' -f1)"
  done
} > "$OUT/MANIFEST.txt"

echo "== manifest"
cat "$OUT/MANIFEST.txt"
exit $fail
