"""Post-link check: the DAS bootloader's published entry stub is where the contract says.

The app jumps to a FIXED address to hand the node back to the bootloader
(Bootloader::BL_ENTRY_ADDR = 0x7FC in Core/Functions/Bootloader.h). That address holds the
jump to BootloaderEntry, placed there by --section-start=.bootentry=0x7FC and kept alive by
--undefined=BootloaderEntryStub. If either flag is dropped, or the image grows into the last
word, the app would call whatever happens to be there. This script fails the build instead of
letting that reach a device.

Checks:
  1. the symbol BootloaderEntryStub exists in the linked ELF and its address is 0x7FC;
  2. the section .bootentry is 0x7FC and does not run past 0x800;
  3. the app image (everything the bootloader hands over) does not reach 0x7FC.

Run from platformio.ini's extra_scripts as `post:scripts/bootloader_entry_check.py`.
"""

Import("env")  # noqa: F821  (SCons injects this)

import subprocess
import sys

ENTRY_ADDR = 0x7FC
REGION_END = 0x800  # the DAS's 2 KB bootloader region


def _fail(msg):
    sys.stderr.write("bootloader_entry_check: FAILED - %s\n" % msg)
    env.Exit(1)


def _nm(elf):
    # The toolchain's prefix is riscv-wch-elf- on this platform; ask the env for the
    # configured nm/objdump so a toolchain change does not silently break the check.
    nm = env.subst("$CC").replace("gcc", "nm")
    try:
        out = subprocess.check_output([nm, elf], stderr=subprocess.STDOUT)
    except (OSError, subprocess.CalledProcessError) as exc:
        _fail("cannot run %s on %s: %s" % (nm, elf, exc))
    return out.decode("utf-8", "replace")


def _sections(elf):
    objdump = env.subst("$CC").replace("gcc", "objdump")
    try:
        out = subprocess.check_output([objdump, "-h", elf], stderr=subprocess.STDOUT)
    except (OSError, subprocess.CalledProcessError) as exc:
        _fail("cannot run %s on %s: %s" % (objdump, elf, exc))
    return out.decode("utf-8", "replace")


def check(target, source, env):  # noqa: F811  (SCons action signature)
    elf = str(target[0])
    syms = _nm(elf)
    addr = None
    for line in syms.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[-1] == "BootloaderEntryStub":
            addr = int(parts[0], 16)
            break
    if addr is None:
        _fail("BootloaderEntryStub is not in %s (dropped by --gc-sections? "
              "--undefined=BootloaderEntryStub missing from build_flags)" % elf)
    if addr != ENTRY_ADDR:
        _fail("BootloaderEntryStub is at 0x%X, not 0x%X" % (addr, ENTRY_ADDR))

    # Section placement: .bootentry must sit at the contract address and stay inside the region.
    sec_name = None
    for line in _sections(elf).splitlines():
        if ".bootentry" in line:
            parts = line.split()
            try:
                size = int(parts[2], 16)
                vma = int(parts[3], 16)
            except (IndexError, ValueError):
                continue
            sec_name = ".bootentry"
            if vma != ENTRY_ADDR:
                _fail(".bootentry VMA is 0x%X, not 0x%X" % (vma, ENTRY_ADDR))
            if vma + size > REGION_END:
                _fail(".bootentry runs past the 2 KB region (0x%X+0x%X > 0x%X)"
                      % (vma, size, REGION_END))
            break
    if sec_name is None:
        _fail(".bootentry section is missing from %s" % elf)

    # The image the bootloader runs must not reach the contract word: a section that ends
    # beyond 0x7FC would be overwritten by the stub, or the stub would be overwritten by it.
    for line in _sections(elf).splitlines():
        parts = line.split()
        if len(parts) < 4 or not parts[0].isdigit():
            continue
        name = parts[1]
        if name.startswith(".debug") or name == ".bootentry":
            continue
        try:
            size = int(parts[2], 16)
            vma = int(parts[3], 16)
        except ValueError:
            continue
        if size and vma < REGION_END and vma + size > ENTRY_ADDR:
            _fail("section %s (0x%X+0x%X) overlaps the entry word at 0x%X"
                  % (name, vma, size, ENTRY_ADDR))

    sys.stdout.write("bootloader_entry_check: OK - BootloaderEntryStub at 0x%X, "
                     ".bootentry inside the 2 KB region\n" % ENTRY_ADDR)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", check)
