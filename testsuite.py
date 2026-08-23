#!/usr/bin/env python3
"""Automated replication of the standard Tamu v2.0A + DAS v0.1 hardware test battery.

Usage:
    python3 testsuite.py                # run all standard tests (no reboot test)
    python3 testsuite.py --reboot       # include the storage reboot-persistence test
    python3 testsuite.py --only device  # run only the named group(s)

The suite drives the Tamu console over /dev/ttyACM0 (RS-485 bus to the DAS) and verifies
every reply against the expected CLI output format. Tests are non-destructive: all scratch
files/blocks are removed and device defaults restored at the end. A non-zero exit code means
at least one test failed.
"""

import argparse
import os
import re
import serial
import subprocess
import sys
import time

PORT = "/dev/ttyACM0"
BAUD = 115200
WLINK = os.path.expanduser("~/.platformio/packages/tool-wlink/wlink")
WLINK_CHIP = "CH32V003"

DAS_DEFAULTS = (("0", "10"), ("1", "10"))          # (block, SamplingRate) - FilterCoeff 0.5
RESULTS = []                                       # (name, ok, detail)


def report(name, ok, detail=""):
    """Record and print a test result; returns the ok flag."""
    RESULTS.append((name, bool(ok), detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f"  ({detail})" if detail else ""))
    return ok


class Cli:
    """Wraps the Tamu USB serial console with a command/read + retry loop."""

    def __init__(self, port=PORT, baud=BAUD, timeout=3.0):
        self.ser = serial.Serial(port, baud, timeout=0.2)
        self.timeout = timeout

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def _read_until(self, dur):
        deadline = time.time() + dur
        buf = b""
        while time.time() < deadline:
            chunk = self.ser.read(self.ser.in_waiting or 1)
            if chunk:
                buf += chunk
        return buf

    def send(self, command, timeout=None, retries=1):
        """Send a command and return the reply text. Retries once if the command was
        not echoed or the prompt did not come back (known first-command garbling)."""
        t = timeout or self.timeout
        for _ in range(retries + 1):
            self.ser.reset_input_buffer()
            self.ser.write((command + "\n").encode())
            text = self._read_until(t).decode("utf-8", "replace")
            if command.strip() in text and "tamu>" in text:
                return text
            time.sleep(0.2)
        return text


def parse_table(text):
    """Parse a 'file N table' reply into {name: (offset, size)}. Names are space-padded."""
    rows = {}
    for m in re.finditer(r'File "([^"]+)" \| Offset (\d+) \| Size (\d+)', text):
        rows[m.group(1).strip()] = (int(m.group(2)), int(m.group(3)))
    return rows


def reboot_das():
    if not os.path.exists(WLINK):
        return False
    try:
        out = subprocess.run([WLINK, "reset", "--chip", WLINK_CHIP],
                             capture_output=True, text=True, timeout=30).stdout
        return "Reset Quit" in out
    except Exception:
        return False


# ---------------------------------------------------------------------------
# Test groups
# ---------------------------------------------------------------------------

def test_device_service(cli):
    """Device service: ping/type/sn/version/name/uptime/loop/time/cap on both devices."""
    for dev in (1, 2):
        r = cli.send(f"dev {dev} ping")
        report(f"dev {dev} ping", "PONG" in r, r.strip().splitlines()[-1] if r else "no reply")

        r = cli.send(f"dev {dev} type")
        ok = bool(re.search(r"Type 0x[0-9A-Fa-f]{4}", r))
        report(f"dev {dev} type", ok, "0x0001" in r and dev == 1 or "0x0003" in r and dev == 2)

        r = cli.send(f"dev {dev} sn")
        report(f"dev {dev} sn", "SN " in r)

        r = cli.send(f"dev {dev} version")
        report(f"dev {dev} version", "Version" in r)

        r = cli.send(f"dev {dev} name")
        report(f"dev {dev} name", "Name" in r)

        r = cli.send(f"dev {dev} uptime")
        report(f"dev {dev} uptime", bool(re.search(r"Uptime \d+ ms", r)))

        r = cli.send(f"dev {dev} loop")
        report(f"dev {dev} loop", "Loop avg" in r)

        r = cli.send(f"dev {dev} time")
        report(f"dev {dev} time", "Time sync" in r)

        r = cli.send(f"dev {dev} cap")
        report(f"dev {dev} cap",
               ("0x00000001" in r and dev == 1) or ("0x00000000" in r and dev == 2))


def test_tamu_system_memory(cli):
    """Tamu System Memory block reads/writes (LED, fans, accel/gyro, displays)."""
    for block in range(6):
        r = cli.send(f"read 1 s {block} 0")
        report(f"read 1 s {block} 0", "Field [00]" in r)

    r = cli.send("read 1 s 3 1")
    report("read accel (s 3 1)", "Vector[3]" in r)

    r = cli.send("read 1 s 3 2")
    report("read gyro (s 3 2)", "Vector[3]" in r)

    r = cli.send("write 1 s 1 1 0x003 0.0")
    report("fan duty write 0%", "Number: 0.0000" in r)

    r = cli.send("write 1 s 1 1 0x003 2.0")
    report("fan duty write 2% (percent)", "Number: 2.0000" in r)

    r = cli.send("write 1 s 1 1 0x003 150")
    cli.send("read 1 s 1 1")
    r = cli.send("read 1 s 1 1")
    report("fan duty 150 clamps to 100 (stored)", "Number: 100.0000" in r)

    r = cli.send("write 1 s 1 1 0x003 0.0")
    report("fan duty restored 0%", "Number: 0.0000" in r)

    r = cli.send("write 1 s 0 0 0x006 0")
    report("LED colour write (s 0 0)", "Field [00]" in r)

    r = cli.send("save 1 s -")
    report("save 1 s -", "Operation OK" in r)
    r = cli.send("rmem 1 s 0")
    report("rmem 1 s 0", "Block [00]" in r)
    r = cli.send("recall 1 s -")
    report("recall 1 s -", "Operation OK" in r)


def test_das_system_memory(cli):
    """DAS System Memory: measurement fields, writable fields, clamping."""
    r = cli.send("read 2 s 0 3")
    report("read MeasuredValue (s 0 3)", "Field [03]" in r and "[RO]" in r)

    r = cli.send("read 2 s 0 4")
    report("read CurrentRange (s 0 4)", "Field [04]" in r)

    r = cli.send("read 2 s 1 3")
    report("read Meas2 MeasuredValue (s 1 3)", "Field [03]" in r)

    r = cli.send("write 2 s 0 1 0x003 0.5")
    report("write FilterCoeff 0.5", "Number: 0.5000" in r)
    r = cli.send("write 2 s 0 0 0x003 10")
    report("write SamplingRate 10", "Number: 10.0000" in r)

    # Write-time clamping (BUG 2 fix)
    cli.send("write 2 s 0 1 0x003 2.5")
    r = cli.send("read 2 s 0 1")
    report("FilterCoeff 2.5 clamps to 1.0", "Number: 1.0000" in r)
    cli.send("write 2 s 0 1 0x003 -0.5")
    r = cli.send("read 2 s 0 1")
    report("FilterCoeff -0.5 clamps to 0.0", "Number: 0.0000" in r)
    cli.send("write 2 s 0 0 0x003 1000000")
    r = cli.send("read 2 s 0 0")
    report("SamplingRate 1e6 clamps to 1000", "Number: 1000.0000" in r)
    cli.send("write 2 s 0 0 0x003 -1")
    r = cli.send("read 2 s 0 0")
    report("SamplingRate -1 clamps to 1", "Number: 1.0000" in r)

    cli.send("write 2 s 0 1 0x003 0.5")
    cli.send("write 2 s 0 0 0x003 10")
    cli.send("write 2 s 1 1 0x003 0.5")
    cli.send("write 2 s 1 0 0x003 10")
    r = cli.send("save 2 s -")
    report("save 2 s -", "Operation OK" in r)
    r = cli.send("file 2 table")
    report("SYSMEM backup created", "SYSMEM" in r)


def test_dynamic_keyed_memory(cli):
    """Dynamic + Keyed memory CRUD, save/recall, delete-visibility (BUG 4 fix)."""
    cli.send("create 1 d 0x100 TSDY")
    r = cli.send("write 1 d 0 0 0x003 42")
    report("dynamic write 42", "Number: 42.0000" in r)
    r = cli.send("read 1 d 0 0")
    report("dynamic read 42", "Number: 42.0000" in r)
    r = cli.send("save 1 d 0")
    report("dynamic save", "Operation OK" in r)
    r = cli.send("rmem 1 d 0")
    report("dynamic rmem", "Block [00]" in r)
    r = cli.send("recall 1 d 0")
    report("dynamic recall", "Operation OK" in r)
    r = cli.send("read 1 d 0 0")
    report("dynamic read after recall", "Number: 42.0000" in r)
    r = cli.send("delete 1 d 0")
    report("dynamic delete", "Operation OK" in r)
    r = cli.send("read 1 d 0 0")
    report("read deleted block fails", "Operation FAILED" in r)

    cli.send("create 1 k 0x100 TSKE")
    r = cli.send("write 1 k 0 0 7 0x003 3.5")
    report("keyed write key 7", "Key [07]" in r)
    r = cli.send("read 1 k 0 0 7")
    report("keyed read key 7", "Key [07]" in r)
    r = cli.send("read 1 k 0 0 8")
    report("keyed wrong key fails", "Operation FAILED" in r)
    cli.send("save 1 k 0")
    cli.send("recall 1 k 0")
    r = cli.send("read 1 k 0 0 7")
    report("keyed recall restores", "Key [07]" in r)
    cli.send("delete 1 k 0")

    cli.send("save 1 d -")
    cli.send("save 1 k -")
    r = cli.send("tree 1")
    report("tree 1 dynamic/keyed purged",
           "Registry Summary [Dynamic]: 0 blocks." in r and "Registry Summary [Keyed]: 0 blocks." in r)


def test_storage(cli):
    """Storage edge cases on the DAS, including the size-0 reservation fix."""
    t = 5.0  # flash erase + RS485 round trip is slower than the default window
    # Clean scratch from any previous run
    for name in ("TS0", "TS1", "TS2", "TS3", "TSPERS", "TSABCDEF"):
        cli.send(f"file 2 delete {name}", timeout=t)

    r = cli.send("file 2 table", timeout=t)
    report("file 2 table", "File Table" in r)

    r = cli.send("file 2 create TS0 0", timeout=t)
    report("create size-0 file", "File created" in r)
    r = cli.send("file 2 create TS1 64", timeout=t)
    report("create 64B file", "File created" in r)
    r = cli.send("file 2 table", timeout=t)
    table = parse_table(r)
    report("size-0 block reserved (no aliasing)",
           "TS0" in table and "TS1" in table and table["TS0"][0] != table["TS1"][0],
           f"TS0@off{table.get('TS0', (0, 0))[0]} TS1@off{table.get('TS1', (0, 0))[0]}")

    r = cli.send("file 2 create TS1 128", timeout=t)
    report("duplicate name rejected", "File create failed" in r)

    r = cli.send("file 2 create TSABCDEFG 64", timeout=t)
    r = cli.send("file 2 table", timeout=t)
    report("name truncated to 8 chars", "TSABCDEF" in r)

    r = cli.send("file 2 read TS1 0 5", timeout=t)
    report("file read", "File Data" in r)
    r = cli.send("file 2 read NOPE 0 5", timeout=t)
    report("read non-existent file", "File Data" in r)

    r = cli.send("file 2 resize TS1 32", timeout=t)
    report("resize down", "File resized" in r)
    r = cli.send("file 2 resize TS1 64", timeout=t)
    report("resize back up (in place)", "File resized" in r)
    r = cli.send("file 2 resize TS1 9999", timeout=t)
    report("resize out-of-space fails", "File resize failed" in r)
    # Deterministic resize-up success: shrink frees a block, growing back reuses it.
    r = cli.send("file 2 create TS3 128", timeout=t)
    report("create 128B file", "File created" in r)
    r = cli.send("file 2 resize TS3 64", timeout=t)
    report("resize 128 -> 64 (down)", "File resized" in r)
    r = cli.send("file 2 resize TS3 128", timeout=t)
    report("resize 64 -> 128 (up)", "File resized" in r)
    r = cli.send("file 2 resize TS1 0", timeout=t)
    report("resize to 0", "File resized" in r)
    r = cli.send("file 2 create TS2 64", timeout=t)
    report("resize-to-0 keeps block (no aliasing)", "File created" in r)
    r = cli.send("file 2 table", timeout=t)
    table = parse_table(r)
    report("TS1/TS2 distinct after resize-to-0",
           "TS1" in table and "TS2" in table and table["TS1"][0] != table["TS2"][0],
           f"TS1@off{table.get('TS1', (0, 0))[0]} TS2@off{table.get('TS2', (0, 0))[0]}")

    r = cli.send("file 2 resize TS0 64", timeout=t)
    report("resize size-0 file up", "File resized" in r)

    r = cli.send("file 2 delete TS1", timeout=t)
    report("delete file", "File deleted" in r)
    r = cli.send("file 2 delete TS1", timeout=t)
    report("delete non-existent", "File deleted" in r)
    r = cli.send("file 2 table", timeout=t)
    table = parse_table(r)
    report("TS1 gone after delete", "TS1" not in table)

    for name in ("TS0", "TS2", "TS3", "TSABCDEF"):
        cli.send(f"file 2 delete {name}", timeout=t)


def test_memory_edge_cases(cli):
    """Type/range/index validation on the DAS System Memory."""
    cases = [
        ("write 2 s 0 1 0x001 42",          "Number field rejects Integer type"),
        ("write 2 s 0 2 0x003 2",            "Enum field rejects Number type"),
        ("write 2 s 0 2 0x003 9",            "enum out of range rejected"),
        ("write 2 s 0 3 0x003 5",            "RO MeasuredValue write rejected"),
        ("read 2 s 0 9",                     "invalid field rejected"),
        ("read 2 s 9 0",                     "invalid block rejected"),
        ("write 2 s 9 0 0x003 5",            "write invalid block rejected"),
    ]
    for cmd, desc in cases:
        r = cli.send(cmd)
        report(desc, "Operation FAILED" in r, r.strip().splitlines()[-1] if r else "")


def test_device_edge_cases(cli):
    """Dead-address timeout (BUG 5 fix) and name truncation."""
    r = cli.send("dev 5 ping")
    report("ping dead address -> timeout", "no response from device 5" in r)

    r = cli.send("dev 5 uptime")
    report("dead address uptime -> timeout", "no response from device 5" in r)

    r = cli.send("save 2 d -")
    report("save missing service -> timeout", "no response from device 2" in r)

    long_name = "TOOLONGNAME_123456789012345678901234567"
    cli.send(f"dev 2 name {long_name}")
    r = cli.send("dev 2 name")
    m = re.search(r'Name "([^"]*)"', r)
    report("name truncated to 23 chars", bool(m) and len(m.group(1)) == 23, m.group(1) if m else "")
    cli.send("dev 2 name DAS_v0.1")
    r = cli.send("dev 2 name")
    report("name restored", "DAS_v0.1" in r)


def test_cli_parse(cli):
    """Strict Number parsing (BUG 6 fix)."""
    r = cli.send("write 2 s 0 1 0x003 abc")
    report("garbage 'abc' rejected", "Failed to parse value" in r)
    r = cli.send("write 2 s 0 1 0x003 0x10")
    report("hex '0x10' rejected", "Failed to parse value" in r)
    r = cli.send("write 2 s 0 1 0x003 2.5")
    report("valid '2.5' accepted", "Number:" in r)
    cli.send("write 2 s 0 1 0x003 0.5")
    cli.send("write 2 s 0 0 0x003 10")


def test_topology_sndb_logs(cli):
    """Topology dumps, SNDB registry, log service."""
    r = cli.send("tree 1")
    report("tree 1", "Registry Summary [System]" in r and "Registry Summary [Dynamic]" in r)
    r = cli.send("tree 2")
    report("tree 2", "Registry Summary [System]: 2 blocks." in r)
    r = cli.send("sndb 1 read_all")
    report("sndb registry", "SNDB Entry" in r and "ID: 0x0002" in r)
    r = cli.send("logs")
    report("logs service", "No logs recorded." in r or "Dev" in r)


def test_reboot_persistence(cli):
    """Storage reboot persistence (requires the WCH-Link tool)."""
    cli.send("file 2 delete TSPERS")
    r = cli.send("file 2 create TSPERS 64")
    if "File created" not in r:
        report("reboot-persistence setup", False, "create failed")
        return
    if not reboot_das():
        report("reboot-persistence", False, "wchlink reset failed / not found")
        return
    time.sleep(3)
    cli.send("dev 2 ping")
    r = cli.send("file 2 table")
    report("file survives reboot", "TSPERS" in r)
    cli.send("file 2 delete TSPERS")


# ---------------------------------------------------------------------------

GROUPS = {
    "device": test_device_service,
    "tamu": test_tamu_system_memory,
    "das": test_das_system_memory,
    "dynamic": test_dynamic_keyed_memory,
    "storage": test_storage,
    "memory": test_memory_edge_cases,
    "device_edge": test_device_edge_cases,
    "parse": test_cli_parse,
    "topology": test_topology_sndb_logs,
    "reboot": test_reboot_persistence,
}

DEFAULT_ORDER = ["device", "tamu", "das", "dynamic", "storage",
                 "memory", "device_edge", "parse", "topology", "reboot"]


def main():
    ap = argparse.ArgumentParser(description="Automated Tamu/DAS hardware test battery")
    ap.add_argument("--port", default=PORT, help="Tamu USB serial port")
    ap.add_argument("--baud", type=int, default=BAUD)
    ap.add_argument("--timeout", type=float, default=3.0, help="per-command reply window (s)")
    ap.add_argument("--reboot", action="store_true", help="include the storage reboot-persistence test")
    ap.add_argument("--only", help="comma-separated group names to run")
    args = ap.parse_args()

    order = [g for g in DEFAULT_ORDER if args.only is None or g in args.only.split(",")]
    if not args.reboot and "reboot" in order:
        order.remove("reboot")
    if not order:
        print("No groups selected.")
        sys.exit(2)

    print(f"=== Tamu/DAS test battery (port {args.port}, groups: {', '.join(order)}) ===")
    cli = Cli(args.port, args.baud, args.timeout)
    try:
        # Warm up the console: the very first command on a fresh port is often garbled
        # (known quirk) and would otherwise fail its assertion.
        cli.send("dev 1 ping")
        time.sleep(0.5)
        for group in order:
            print(f"\n--- {group} ---")
            try:
                GROUPS[group](cli)
            except Exception as e:  # keep the run going on a hard error
                report(f"{group}: unexpected error", False, repr(e))
    finally:
        # Restore defaults / leave a clean state
        try:
            cli.send("write 1 s 1 1 0x003 0.0")
            cli.send("write 2 s 0 1 0x003 0.5")
            cli.send("write 2 s 0 0 0x003 10")
            cli.send("write 2 s 1 1 0x003 0.5")
            cli.send("write 2 s 1 0 0x003 10")
        except Exception:
            pass
        cli.close()

    passed = sum(1 for _, ok, _ in RESULTS if ok)
    failed = len(RESULTS) - passed
    print(f"\n=== Results: {passed} passed, {failed} failed, {len(RESULTS)} total ===")
    if failed:
        for name, ok, detail in RESULTS:
            if not ok:
                print(f"  FAILED: {name}  {detail}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()