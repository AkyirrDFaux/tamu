#!/usr/bin/env python3
"""Deterministic per-target software version.

Each target (firmware env / app) has its own source set. The version is minted only when that
set's content changes: unchanged content keeps the stored version, a change mints a new one.
Minting checks the current date first - a new day resets the iteration, the same day grows it.

The version is the documented System-block shape YY:MM:DD:II (7 year + 4 month + 5 day + 16
iteration bits). The state lives in `version.json` at the repo root.

    python3 scripts/version.py <target>          # print "year month day iteration"
    python3 scripts/version.py --all             # print every target's version
"""
import hashlib
import json
import os
import sys
from datetime import date

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATE_PATH = os.path.join(ROOT, "version.json")

# Bootloaders are deliberately unversioned (they do not run the Device service).
TARGETS = {
    "DAS_v0_1": {
        "roots": ["firmware/src/Core", "firmware/src/Blocks", "firmware/src/Devices/DAS_v0.1"],
        "files": ["firmware/src/Main.cpp", "firmware/platformio.ini"],
        "exclude": ["firmware/src/Devices/DAS_v0.1/Bootloader.cpp"],
    },
    "Tamu_v2_0A": {
        "roots": ["firmware/src/Core", "firmware/src/Blocks", "firmware/src/Devices/Tamu_v2.0A"],
        "files": ["firmware/src/Main.cpp", "firmware/platformio.ini"],
        "exclude": [],
    },
    "app": {
        "roots": ["app/lib"],
        "files": ["app/pubspec.yaml"],
        # The generated stamp is written from this hash; excluding it avoids a feedback loop.
        "exclude": ["app/lib/core/app_version.g.dart"],
    },
}


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


def source_hash(target):
    h = hashlib.sha256()
    for rel in _source_files(target):
        h.update(rel.encode("utf-8"))
        h.update(b"\0")
        with open(os.path.join(ROOT, rel), "rb") as f:
            h.update(f.read())
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
    """Mints on a content change; a date change resets the iteration, the same day grows it."""
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
    """Returns the target's version dict, minting + persisting if the source changed."""
    if target not in TARGETS:
        raise KeyError("unknown version target %r" % target)
    if today is None:
        d = date.today()
        today = (d.year % 100, d.month, d.day)
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
            v = ensure_version(target)
            print("%s %d %d %d %d" % (target, v["year"], v["month"], v["day"], v["iteration"]))
        return 0
    v = ensure_version(argv[1])
    print("%d %d %d %d" % (v["year"], v["month"], v["day"], v["iteration"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
