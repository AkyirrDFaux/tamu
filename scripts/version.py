#!/usr/bin/env python3
"""Deterministic per-target software version.

Each target (firmware env / app) has its own source set. The version is minted only when that
set's content changes: unchanged content keeps the stored version, a change mints a new one.
Minting grows the iteration while the content keeps changing on the same day; a content change
on a new day starts the iteration at 1. A date change on its own does not reset anything - the
stored version is returned unchanged while the content hash is unchanged.

For firmware targets the shared `firmware/platformio.ini` is not hashed wholesale: only the
`[env]` section, the target's `[env:<name>]` section and the sections it extends, so editing an
unrelated env (or the bootloader env) does not mint a version for this target.

The version is the documented System-block shape YY:MM:DD:II (7 year + 4 month + 5 day + 16
iteration bits; the same shape the firmware packs in Core/Services/RegisterRead.h). The state
lives in `version.json` at the repo root.

    python3 scripts/version.py <target>          # print "year month day iteration"
    python3 scripts/version.py --all             # print every target's version
"""
import hashlib
import json
import os
import re
import sys
from datetime import date

try:
    import fcntl  # POSIX only; Windows has no flock and falls back to an unlocked write.
except ImportError:  # pragma: no cover
    fcntl = None

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATE_PATH = os.path.join(ROOT, "version.json")
LOCK_PATH = os.path.join(ROOT, "version.json.lock")

# The documented System-block field order (7 year + 4 month + 5 day + 16 iteration bits).
VERSION_FIELDS = ("year", "month", "day", "iteration")

# Bootloaders are deliberately unversioned (they do not run the Device service).
TARGETS = {
    "DAS_v0_1": {
        "roots": ["firmware/src/Core", "firmware/src/Blocks", "firmware/src/Devices/DAS_v0.1"],
        "files": ["firmware/src/Main.cpp"],
        "exclude": ["firmware/src/Devices/DAS_v0.1/Bootloader.cpp"],
        "platformio_env": "DAS_v0_1",
    },
    "Tamu_v2_0A": {
        "roots": ["firmware/src/Core", "firmware/src/Blocks", "firmware/src/Devices/Tamu_v2.0A"],
        "files": [
            "firmware/src/Main.cpp",
            # ESP-IDF image inputs (the DAS uses the ch32v noneos-sdk platform, so these are
            # core-only and do not affect the DAS hash).
            "firmware/src/CMakeLists.txt",
            "firmware/CMakeLists.txt",
            "firmware/partitions.csv",
            "firmware/src/idf_component.yml",
            "firmware/sdkconfig.Tamu_v2_0A",
        ],
        "exclude": [],
        "platformio_env": "Tamu_v2_0A",
    },
    "app": {
        "roots": ["app/lib"],
        "files": ["app/pubspec.yaml", "app/pubspec.lock"],
        # The generated stamp is written from this hash; excluding it avoids a feedback loop.
        "exclude": ["app/lib/core/app_version.g.dart"],
    },
}


def version_fields(version):
    """Return (year, month, day, iteration) in the documented field order."""
    return tuple(version[k] for k in VERSION_FIELDS)


def format_version(version):
    """The dotted display form, e.g. "26.10.4.10"."""
    return "%d.%d.%d.%d" % version_fields(version)


def pack_version(version):
    """Pack YY:MM:DD:II into the documented u32 (mirrors RegisterRead.h)."""
    year, month, day, iteration = version_fields(version)
    return (((year & 0x7F) << 25) | ((month & 0x0F) << 21) |
            ((day & 0x1F) << 16) | (iteration & 0xFFFF))


def _source_files(target):
    cfg = TARGETS[target]
    paths = []
    for root in cfg["roots"]:
        for dirpath, _dirs, filenames in os.walk(os.path.join(ROOT, root)):
            for fn in filenames:
                paths.append(os.path.relpath(os.path.join(dirpath, fn), ROOT))
    paths += cfg["files"]
    excluded = set(cfg["exclude"])
    return sorted(p for p in set(paths) if p not in excluded)


def _platformio_sections(env_name):
    """The `[env]` + `[env:<name>]` text plus the target env's `extends` chain.

    Only the target's own configuration (and the bases it extends) is returned, so a change to
    an unrelated environment does not appear in this target's hash.
    """
    path = os.path.join(ROOT, "firmware", "platformio.ini")
    if not os.path.isfile(path):
        return ""
    with open(path) as f:
        lines = f.readlines()

    headers = []
    for i, line in enumerate(lines):
        m = re.match(r"\s*\[([^\]]+)\]\s*$", line)
        if m:
            headers.append((m.group(1), i))
    sections = {}
    for idx, (name, start) in enumerate(headers):
        end = headers[idx + 1][1] if idx + 1 < len(headers) else len(lines)
        sections[name] = "".join(lines[start:end])

    wanted = ["env", "env:%s" % env_name]
    seen = set()
    while wanted:
        name = wanted.pop(0)
        if name in seen or name not in sections:
            continue
        seen.add(name)
        for m in re.finditer(r"^\s*extends\s*=\s*(.+)$", sections[name], re.M):
            wanted.extend(parent.strip() for parent in m.group(1).split(","))
    return "".join(sections[name] for name in sorted(seen))


def source_hash(target):
    cfg = TARGETS[target]
    h = hashlib.sha256()
    for rel in _source_files(target):
        h.update(rel.encode("utf-8"))
        h.update(b"\0")
        with open(os.path.join(ROOT, rel), "rb") as f:
            h.update(f.read())
    env_name = cfg.get("platformio_env")
    if env_name:
        h.update(b"firmware/platformio.ini#target-section\0")
        h.update(_platformio_sections(env_name).encode("utf-8"))
    return h.hexdigest()


def _load_state():
    if not os.path.isfile(STATE_PATH):
        return {}
    with open(STATE_PATH) as f:
        return json.load(f)


def _save_state(state):
    with open(STATE_PATH, "w") as f:
        json.dump(state, f, indent=2, sort_keys=True)
        f.write("\n")


def _mint(previous, digest, today):
    """Mint only on a content change; an unchanged hash keeps the stored version.

    The iteration grows while the content changes on the same day and starts at 1 on the first
    content change of a new day. A moved date with unchanged content mints nothing.
    """
    if previous and previous.get("hash") == digest:
        return dict(previous)  # unchanged: keep the stored version
    same_day = previous and (previous.get("year"), previous.get("month"),
                             previous.get("day")) == today
    iteration = (previous.get("iteration", 0) + 1) if same_day else 1
    return {
        "hash": digest,
        "year": today[0],
        "month": today[1],
        "day": today[2],
        "iteration": iteration,
    }


def ensure_version(target, today=None):
    """Returns the target's version dict, minting + persisting if the source changed.

    The read-modify-write of `version.json` is serialized with a file lock so parallel envs
    (e.g. a multi-env `pio run`) cannot race each other.
    """
    if target not in TARGETS:
        raise KeyError("unknown version target %r" % target)
    if today is None:
        d = date.today()
        today = (d.year % 100, d.month, d.day)
    with open(LOCK_PATH, "w") as lock:
        if fcntl is not None:
            fcntl.flock(lock, fcntl.LOCK_EX)
        state = _load_state()
        updated = _mint(state.get(target), source_hash(target), today)
        if state.get(target) != updated:
            state[target] = updated
            _save_state(state)
    return updated


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: version.py <target>|--all\n")
        return 2
    if argv[1] == "--all":
        for target in TARGETS:
            print("%s %d %d %d %d" % ((target,) + version_fields(ensure_version(target))))
        return 0
    print("%d %d %d %d" % version_fields(ensure_version(argv[1])))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
