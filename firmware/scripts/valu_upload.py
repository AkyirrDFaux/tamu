#!/usr/bin/env python3
"""Flash a Valu v2.0 (CH32V203G8R6) image over the WCH-Link debug probe (SWD).

The chip has no custom bootloader yet, so the first bootloader image cannot arrive over USB or
RSBus - it has to go in over the WCH-Link probe. This is the probe-side flash step for the Valu,
the CH32V203 counterpart of `./upload.sh bin das` (minichlink on the CH32V003); the owner runs it
by hand. Runtime bootloader entry is the documented one: hold the LED-button (PA2, Docs/Devices.md)
at power-up.

It defaults to the built bootloader image at flash offset 0x0 (the bootloader owns 0x0-0x3000, so
the app is linked at 0x3000):

    python3 firmware/scripts/valu_upload.py                # bootloader @ 0x0 (the default)
    python3 firmware/scripts/valu_upload.py app.bin 0x2000 # the app image
    python3 firmware/scripts/valu_upload.py --erase        # first-time flash: erase, then write

`wlink flash --address <abs> <image>` takes the absolute code-flash address (the wlink README
flashes to 0x08000000). The CH32V20x flash is aliased at 0x00000000 and 0x08000000, so an address
below FLASH_BASE is treated as an offset into code flash and rebased onto 0x08000000.
"""
import argparse
import os
import shutil
import subprocess
import sys

# CH32V20x code-flash alias (the second alias over the same flash; the SDK links at 0x0).
FLASH_BASE = 0x08000000
# The bootloader image occupies the bottom of flash; this is what `./upload.sh boot valu` builds.
BOOTLOADER_ENV = "Valu_bootloader"
BOOTLOADER_OFFSET = 0x0

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_FIRMWARE_DIR = os.path.dirname(_SCRIPT_DIR)
DEFAULT_IMAGE = os.path.join(_FIRMWARE_DIR, ".pio", "build", BOOTLOADER_ENV, "firmware.bin")
# PlatformIO's bundled wlink (the Valu counterpart of MINICHLINK/ESPTOOL_PY in upload.sh).
DEFAULT_WLINK = os.path.expanduser("~/.platformio/packages/tool-wlink/wlink")


def _address(text):
    try:
        return int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError("not an address: %r" % text)


def build_parser():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("image", nargs="?", default=DEFAULT_IMAGE,
                        help="image to flash (.bin/.hex/.elf); default: the built bootloader")
    parser.add_argument("address", nargs="?", type=_address, default=BOOTLOADER_OFFSET,
                        help="code-flash offset or address (default: 0x0, the bootloader)")
    parser.add_argument("-e", "--erase", action="store_true",
                        help="erase code flash before writing (a first-time bootloader flash)")
    parser.add_argument("-d", "--device", metavar="INDEX",
                        help="WCH-Link probe index (wlink -d), when several are attached")
    parser.add_argument("--chip", default="CH32V20X",
                        help="chip family passed to wlink (default: CH32V20X)")
    parser.add_argument("--wlink", default=os.environ.get("WLINK", DEFAULT_WLINK),
                        help="path to the wlink binary (default: $WLINK or the PlatformIO package)")
    parser.add_argument("-n", "--dry-run", action="store_true",
                        help="print the command without running it")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)

    if not os.path.isfile(args.image):
        sys.exit("error: no image '%s' (build it first: ./upload.sh boot valu)" % args.image)

    wlink = args.wlink
    if not (os.path.isfile(wlink) and os.access(wlink, os.X_OK)):
        found = shutil.which("wlink")
        if found is None:
            sys.exit("error: wlink not found at '%s' (set --wlink or WLINK; the PlatformIO "
                     "package is tool-wlink)" % wlink)
        wlink = found

    # wlink wants the absolute code-flash address; a bare offset is rebased onto the alias.
    address = args.address
    if address < FLASH_BASE:
        address += FLASH_BASE

    # -d is a *global* wlink option: it must precede the `flash` subcommand (clap rejects it after).
    cmd = [wlink]
    if args.device:
        cmd += ["-d", args.device]
    cmd += ["flash", "--chip", args.chip, "--address", "0x%08X" % address]
    if args.erase:
        cmd.append("--erase")
    cmd.append(args.image)

    print("==> %s" % " ".join(cmd))
    if args.dry_run:
        return 0
    return subprocess.call(cmd)


if __name__ == "__main__":
    sys.exit(main())
