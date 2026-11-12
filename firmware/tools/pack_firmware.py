#!/usr/bin/env python3
"""Pack a built firmware image and its checksums into a deterministic .fw.zip, and verify it.

Why: a flash is only as trustworthy as the file that happens to be on disk. The package carries
the image plus a manifest that names, per image, the address it belongs at and the digests a
device can be compared against - so "the file I am about to write" is checkable instead of
assumed.

Package layout, `binaries/<target>-<version>.fw.zip`, deterministic:
  * entries sorted by name, stored (no compression), fixed 1980-01-01 timestamps, no extra
    fields, no unix mode bits - so the same inputs give the same bytes and the same sha256;
  * `manifest.json` (format 1) plus the `.bin` files under their stable names.

"Same inputs" includes the image's modification time, because `built_utc` is read from it: copy
build artifacts with `cp -p` (collect_binaries.sh does) or pass `--built-utc`.

The version stamp:
  * `Tamu_v2_0A`, `DAS_v0_1` - the System block's `YY.MM.DD.II` stamp. `scripts/version.py`
    mints it and `scripts/auto_version.py` compiles it in. The image carries it as instruction
    immediates, not as a literal 4-byte constant (measured: neither shipped image contains the
    packed u32), so the stamp is read from `version.json` and the tool refuses unless that
    entry's recorded source hash still equals the current sources - i.e. the stamp describes the
    tree this image comes from. The file itself is corroborated from its own bytes, see
    `inspect_image`.
  * `Tamu_bootloader`, `DAS_bootloader` - `unversioned`: bootloaders run no Device service and
    nothing stamps them (`scripts/version.py` says the same).

Verification: `pack_firmware.py --verify <pkg> [--target NAME] [--reference-dir DIR]` re-checks
the packaged bytes against the manifest, prints `OK`, or names the first mismatch and exits
non-zero. With a reference copy of the loose `.bin` (the sibling file next to the package, or
`--reference-dir`) whose sha256 matches the manifest, a mismatch is localised to the first
differing byte offset.

Exit codes: 0 ok, 1 verification mismatch, 2 refusal (nothing was written).

    firmware/tools/pack_firmware.py --target DAS_v0_1 --image binaries/DAS_v0_1.bin
    firmware/tools/pack_firmware.py --verify binaries/DAS_v0_1-26.11.12.4.fw.zip
"""

import argparse
import hashlib
import json
import os
import struct
import sys
import zipfile
from datetime import datetime, timezone

TOOLS = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.dirname(TOOLS)
REPO = os.path.dirname(FIRMWARE)
DEFAULT_OUT_DIR = os.path.join(REPO, "binaries")
DEFAULT_VERSION_FILE = os.path.join(REPO, "version.json")

FORMAT = 1

# Every loader here writes the image in 32-byte chunks (the DAS bootloader's write payload and
# the app's `Bootloader.payloadSize` both), padding the tail with 0xFF - erased flash - so the
# digest a device computes covers a whole number of chunks. `padded_size`/`crc32_padded` record
# that padded image; `crc32` is the image as shipped, unpadded.
PAD_TO = 32
PAD_BYTE = 0xFF

# address: where the image belongs, from the build config:
#   Tamu_v2_0A       ota_0 partition   (firmware/partitions.csv, scripts/core_app_offset.py)
#   Tamu_bootloader  factory partition (firmware/partitions.csv)
#   DAS_v0_1         CH32 flash 0x08000000 + board_upload.offset_address 0x800
#   DAS_bootloader   CH32 flash base; the bootloader owns 0x0-0x800
# max_size: the region the loader may write (a bigger file is not this target's image).
TARGETS = {
    "Tamu_v2_0A": {
        "image": "Tamu_v2_0A.bin",
        "address": "0x90000",
        "versioned": True,
        "idf_app": True,
        "max_size": 0x283000,           # ota_0 partition
    },
    "Tamu_bootloader": {
        "image": "Tamu_bootloader.bin",
        "address": "0x10000",
        "versioned": False,
        "idf_app": True,
        "max_size": 0x7D000,            # factory partition
    },
    "DAS_v0_1": {
        "image": "DAS_v0_1.bin",
        "address": "0x08000800",
        "versioned": True,
        "idf_app": False,
        # App code space [0x800, 0x3E00): the 512 B storage at 0x3E00 is pinned and stripped
        # from the image, so a file that does not fit here is not this target's app image.
        "max_size": 0x3E00 - 0x800,
    },
    "DAS_bootloader": {
        "image": "DAS_bootloader.bin",
        "address": "0x08000000",
        "versioned": False,
        "idf_app": False,
        "max_size": 2048,
    },
}

# ESP-IDF app descriptor: magic word at 0x20 of the app image, then secure_version, a reserved
# pair, and the fixed-width text fields this tool reads (esp_app_desc_t in esp_app_format.h).
IDF_DESC_OFFSET = 0x20
IDF_DESC_MAGIC = 0xABCD5432
IDF_DESC_VERSION = 0x10             # version[32]
IDF_DESC_PROJECT = 0x30             # project_name[32]
IDF_DESC_TIME = 0x50                # time[16]
IDF_DESC_DATE = 0x60                # date[16]
IDF_DESC_MIN_LEN = 0x90             # through date[16]


class Refusal(Exception):
    """The package cannot be built from what is on disk. Nothing is written."""


# ===== CRC-32 =====

def _crc32_table():
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
        table.append(c)
    return tuple(table)


_CRC32_TABLE = _crc32_table()


def crc32(data):
    """CRC-32/ISO-HDLC: reflected polynomial 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF.

    The returned value is the 32-bit number; on the wire it travels little-endian (4 bytes,
    lowest first), which is what a device-side digest compares against. Equal to zlib.crc32
    and to the device's CRC digest command for the same bytes.
    """
    c = 0xFFFFFFFF
    for b in data:
        c = _CRC32_TABLE[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


def sha256_hex(data):
    return hashlib.sha256(data).hexdigest()


def hex32(value):
    return "0x%08X" % (value & 0xFFFFFFFF)


# ===== images and stamps =====

def _text_field(data, offset, width):
    raw = data[offset:offset + width]
    return raw.split(b"\0", 1)[0].decode("latin-1")


def inspect_image(path, target):
    """Read and corroborate one image. Returns (bytes, info).

    Refuses a missing, empty, over-region or foreign file: the point of packaging is that the
    bytes in the package are provably this target's image.
    """
    cfg = TARGETS[target]
    if not os.path.isfile(path):
        raise Refusal("image not found: %s" % path)
    with open(path, "rb") as f:
        data = f.read()
    if not data:
        raise Refusal("image is empty: %s" % path)
    if len(data) > cfg["max_size"]:
        raise Refusal("%s is %d B, larger than %s's %d B region - not that target's image"
                      % (path, len(data), target, cfg["max_size"]))
    info = {}
    if cfg["idf_app"]:
        if len(data) < IDF_DESC_OFFSET + IDF_DESC_MIN_LEN:
            raise Refusal("%s is too short (%d B) to be an ESP-IDF app image"
                          % (path, len(data)))
        magic = struct.unpack_from("<I", data, IDF_DESC_OFFSET)[0]
        if magic != IDF_DESC_MAGIC:
            raise Refusal("%s carries no ESP-IDF app descriptor (magic 0x%08X at 0x%X, got "
                          "0x%08X) - not a built %s image"
                          % (path, IDF_DESC_MAGIC, IDF_DESC_OFFSET, magic, target))
        info = {
            "project": _text_field(data, IDF_DESC_OFFSET + IDF_DESC_PROJECT, 32),
            "descriptor_version": _text_field(data, IDF_DESC_OFFSET + IDF_DESC_VERSION, 32),
            "descriptor_built": ("%s %s" % (
                _text_field(data, IDF_DESC_OFFSET + IDF_DESC_DATE, 16),
                _text_field(data, IDF_DESC_OFFSET + IDF_DESC_TIME, 16))).strip(),
        }
    return data, info


def _version_module():
    """scripts/version.py - the single source of the stamp's shape and of the source hash."""
    scripts = os.path.join(REPO, "scripts")
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    import version  # noqa: E402
    return version


def read_stamp(target, version_file):
    """The stamp string the image reports, and the packed u32 behind it.

    Refuses when the stamp cannot be established: no entry, an incomplete entry, or a recorded
    source hash that no longer matches the tree (the stored stamp then describes other sources
    and the image on disk is stale).
    """
    cfg = TARGETS[target]
    if not cfg["versioned"]:
        return {"stamp": "unversioned", "packed": None}
    try:
        with open(version_file) as f:
            state = json.load(f)
    except OSError as exc:
        raise Refusal("cannot read %s: %s" % (version_file, exc))
    except ValueError as exc:
        raise Refusal("%s is not valid JSON: %s" % (version_file, exc))
    entry = state.get(target)
    if not isinstance(entry, dict):
        raise Refusal("%s has no version entry in %s - the image's stamp cannot be read"
                      % (target, version_file))
    missing = [k for k in ("year", "month", "day", "iteration", "hash") if k not in entry]
    if missing:
        raise Refusal("%s's version entry in %s lacks %s - the stamp cannot be read"
                      % (target, version_file, ", ".join(missing)))
    version_mod = _version_module()
    stamp = version_mod.format_version(entry)
    packed = version_mod.pack_version(entry)
    fresh = version_mod.source_hash(target)
    if entry["hash"] != fresh:
        raise Refusal("%s's stamp %s is stale: %s records source hash %s, the tree hashes to %s "
                      "- rebuild before packaging"
                      % (target, stamp, version_file, entry["hash"], fresh))
    return {"stamp": stamp, "packed": packed}


def git_commit():
    import subprocess
    try:
        out = subprocess.run(["git", "-C", REPO, "rev-parse", "--short=12", "HEAD"],
                             capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise Refusal("cannot read the build commit from git: %s" % exc)
    return out.stdout.strip()


def built_utc_of(path, override=None):
    if override:
        return override
    seconds = int(os.stat(path).st_mtime)
    return datetime.fromtimestamp(seconds, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def pad_image(data):
    """Pad to a whole `PAD_TO` chunks with 0xFF, exactly like the loaders' tail chunk."""
    padded = len(data)
    if padded % PAD_TO:
        padded += PAD_TO - padded % PAD_TO
    return data + bytes([PAD_BYTE]) * (padded - len(data))


# ===== writing =====

def manifest_for(target, image_name, data, commit, built):
    cfg = TARGETS[target]
    padded = pad_image(data)
    return {
        "format": FORMAT,
        "target": target,
        "version": built["stamp"],
        "build_commit": commit,
        "built_utc": built["built_utc"],
        "images": [{
            "file": image_name,
            "address": cfg["address"],
            "size": len(data),
            "sha256": sha256_hex(data),
            "crc32": hex32(crc32(data)),
            "padded_size": len(padded),
            "crc32_padded": hex32(crc32(padded)),
        }],
    }


def _fixed_zipinfo(name):
    """One stored entry with every field that could vary between runs pinned.

    `create_system`/`external_attr` are set explicitly (not left to the library's default for a
    zero attribute) so the mode bits in the archive are the same constant on every host: a
    package built on one machine must have the same sha256 as one built on another.
    """
    info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_STORED
    info.create_system = 3          # Unix
    info.create_version = 20
    info.extract_version = 20
    info.flag_bits = 0
    info.internal_attr = 0
    info.external_attr = 0o644 << 16
    info.comment = b""
    info.extra = b""
    return info


def write_package(path, manifest, images):
    """Write the package: manifest + images, entries sorted, stored, fixed timestamps."""
    manifest_bytes = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")
    entries = sorted([("manifest.json", manifest_bytes)] + list(images))
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        with zipfile.ZipFile(f, "w", zipfile.ZIP_STORED, allowZip64=False) as zf:
            for name, data in entries:
                zf.writestr(_fixed_zipinfo(name), data)
    os.replace(tmp, path)
    return entries


def build_package(target, image_path, out_dir=DEFAULT_OUT_DIR, version_file=None,
                  commit=None, built_utc=None):
    """Build one package and return (path, manifest, image info)."""
    if target not in TARGETS:
        raise Refusal("unknown target %r (known: %s)" % (target, ", ".join(sorted(TARGETS))))
    version_file = version_file or DEFAULT_VERSION_FILE
    data, info = inspect_image(image_path, target)
    stamp = read_stamp(target, version_file)
    built = {"stamp": stamp["stamp"], "built_utc": built_utc_of(image_path, built_utc)}
    commit = commit or git_commit()
    image_name = TARGETS[target]["image"]
    manifest = manifest_for(target, image_name, data, commit, built)
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, "%s-%s.fw.zip" % (target, stamp["stamp"]))
    write_package(path, manifest, [(image_name, data)])
    info["path"] = path
    info["manifest"] = manifest
    return path, manifest, info


# ===== reading and verifying =====

def read_stored_zip(path):
    """Read a stored (uncompressed) zip straight from its bytes, in central-directory order.

    Deliberately not zipfile's reader: it validates each entry's recorded CRC and raises, which
    would hide the tampered bytes this verifier has to report. Reading the stored payload lets a
    damaged package be described instead of rejected with a library message.
    """
    with open(path, "rb") as f:
        data = f.read()
    eocd = data.rfind(b"PK\x05\x06")
    if eocd < 0 or len(data) - eocd < 22:
        raise ValueError("not a zip: no end-of-central-directory record")
    cd_size, cd_off = struct.unpack_from("<II", data, eocd + 12)
    entries = {}
    order = []
    pos = cd_off
    while pos < cd_off + cd_size:
        if data[pos:pos + 4] != b"PK\x01\x02":
            raise ValueError("corrupt central directory at 0x%X" % pos)
        method, = struct.unpack_from("<H", data, pos + 10)
        csize, usize = struct.unpack_from("<II", data, pos + 20)
        nlen, elen, clen = struct.unpack_from("<HHH", data, pos + 28)
        lho, = struct.unpack_from("<I", data, pos + 42)
        name = data[pos + 46:pos + 46 + nlen].decode("utf-8", "replace")
        if method != zipfile.ZIP_STORED:
            raise ValueError("entry %s is compressed (method %d); a package is stored-only"
                             % (name, method))
        if csize != usize:
            raise ValueError("entry %s: stored entry with size %d != %d" % (name, csize, usize))
        if data[lho:lho + 4] != b"PK\x03\x04":
            raise ValueError("entry %s: bad local header at 0x%X" % (name, lho))
        hlen, xlen = struct.unpack_from("<HH", data, lho + 26)
        start = lho + 30 + hlen + xlen
        entries[name] = data[start:start + csize]
        order.append(name)
        pos += 46 + nlen + elen + clen
    return entries, order


def _first_difference(a, b):
    """Index of the first differing byte, or None when the prefixes are equal."""
    limit = min(len(a), len(b))
    for i in range(limit):
        if a[i] != b[i]:
            return i
    return None if len(a) == len(b) else limit


def _reference_bytes(pkg_path, name, expected_sha, reference_dir):
    """The untampered loose image for `name`, if one is available and provably untampered."""
    candidates = []
    if reference_dir:
        candidates.append(os.path.join(reference_dir, name))
    candidates.append(os.path.join(os.path.dirname(os.path.abspath(pkg_path)), name))
    for cand in candidates:
        try:
            with open(cand, "rb") as f:
                data = f.read()
        except OSError:
            continue
        if sha256_hex(data) == expected_sha:
            return data, cand
    return None, None


def verify_package(path, expect_target=None, reference_dir=None):
    """Check the manifest against the packaged bytes. Returns a list of mismatch strings."""
    problems = []

    def bad(text):
        problems.append(text)

    try:
        entries, order = read_stored_zip(path)
    except (OSError, ValueError) as exc:
        return ["%s: %s" % (path, exc)]
    if "manifest.json" not in entries:
        return ["manifest.json is missing from %s (entries: %s)"
                % (os.path.basename(path), ", ".join(order) or "none")]
    try:
        manifest = json.loads(entries["manifest.json"].decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        return ["manifest.json is not valid JSON: %s" % exc]

    if manifest.get("format") != FORMAT:
        bad("manifest format is %r, this verifier implements format %d"
            % (manifest.get("format"), FORMAT))
    target = manifest.get("target")
    if target not in TARGETS:
        bad("manifest target %r is not a known target" % (target,))
    for field in ("version", "build_commit", "built_utc"):
        if not manifest.get(field):
            bad("manifest has no %s" % field)
    if expect_target is not None:
        if target != expect_target:
            bad("package is for target %s, expected %s" % (target, expect_target))
        else:
            expected = TARGETS[expect_target]
            for entry in manifest.get("images") or []:
                if entry.get("file") != expected["image"]:
                    bad("package carries %r, %s's image is %r"
                        % (entry.get("file"), expect_target, expected["image"]))
                if entry.get("address") != expected["address"]:
                    bad("package gives %r address %s, %s is at %s"
                        % (entry.get("file"), entry.get("address"), expect_target,
                           expected["address"]))

    images = manifest.get("images")
    if not isinstance(images, list) or not images:
        bad("manifest lists no images")
        images = []
    listed = set()
    for entry in images:
        name = entry.get("file")
        if not name:
            bad("an image entry has no file name")
            continue
        listed.add(name)
        payload = entries.get(name)
        if payload is None:
            bad("%s is in the manifest but not in the package (entries: %s)"
                % (name, ", ".join(order)))
            continue
        expected_size = entry.get("size")
        if expected_size != len(payload):
            hint = ""
            if isinstance(expected_size, int):
                at = min(len(payload), expected_size)
                hint = ", first difference at offset 0x%X" % at
            bad("%s: size %d B, manifest says %s B%s" % (name, len(payload), expected_size, hint))
        digest = sha256_hex(payload)
        if entry.get("sha256") != digest:
            detail = "%s: sha256 %s, manifest says %s" % (name, digest, entry.get("sha256"))
            reference, ref_path = _reference_bytes(path, name, entry.get("sha256"),
                                                  reference_dir)
            if reference is not None:
                at = _first_difference(payload, reference)
                if at is not None:
                    detail += (", first differing byte at offset 0x%X (packaged 0x%02X, %s "
                               "0x%02X)" % (at, payload[at] if at < len(payload) else 0,
                                            ref_path, reference[at] if at < len(reference) else 0))
            bad(detail)
        if entry.get("crc32") != hex32(crc32(payload)):
            bad("%s: crc32 %s, manifest says %s"
                % (name, hex32(crc32(payload)), entry.get("crc32")))
        padded = pad_image(payload)
        if entry.get("padded_size") != len(padded):
            bad("%s: padded size %d B, manifest says %s B"
                % (name, len(padded), entry.get("padded_size")))
        if entry.get("crc32_padded") != hex32(crc32(padded)):
            bad("%s: crc32_padded %s, manifest says %s"
                % (name, hex32(crc32(padded)), entry.get("crc32_padded")))
        if target in TARGETS and name == TARGETS[target]["image"]:
            if entry.get("address") != TARGETS[target]["address"]:
                bad("%s: address %s, %s is at %s"
                    % (name, entry.get("address"), target, TARGETS[target]["address"]))
            if len(payload) > TARGETS[target]["max_size"]:
                bad("%s: %d B is larger than %s's %d B region"
                    % (name, len(payload), target, TARGETS[target]["max_size"]))
    for name in order:
        if name not in listed and name != "manifest.json":
            bad("%s is in the package but not in the manifest" % name)
    return problems


# ===== command line =====

def _print_package(path, manifest, info):
    image = manifest["images"][0]
    print("packaged: %s" % os.path.relpath(path, REPO))
    print("  target   %s  version %s  commit %s  built %s"
          % (manifest["target"], manifest["version"], manifest["build_commit"],
             manifest["built_utc"]))
    print("  image    %s  %d B @ %s" % (image["file"], image["size"], image["address"]))
    print("           sha256 %s  crc32 %s  padded %d B  crc32_padded %s"
          % (image["sha256"], image["crc32"], image["padded_size"], image["crc32_padded"]))
    if info.get("descriptor_version"):
        print("           esp-idf app descriptor: project %r version %r built %r"
              % (info.get("project"), info.get("descriptor_version"),
                 info.get("descriptor_built")))
    with open(path, "rb") as f:
        package_sha = sha256_hex(f.read())
    print("           package sha256 %s" % package_sha)


def main(argv):
    parser = argparse.ArgumentParser(
        description="Pack a firmware image with its checksums into a deterministic .fw.zip, "
                    "or verify a package.")
    parser.add_argument("--verify", metavar="PKG",
                        help="check a package's manifest against its bytes")
    parser.add_argument("--target", help="target env; with --verify, the expected one")
    parser.add_argument("--image", help="built image to package")
    parser.add_argument("--out-dir", default=DEFAULT_OUT_DIR,
                        help="package directory (default: %(default)s)")
    parser.add_argument("--version-file", default=DEFAULT_VERSION_FILE,
                        help="where the stamp state lives (default: %(default)s)")
    parser.add_argument("--commit", help="build commit to record (default: git HEAD)")
    parser.add_argument("--built-utc", help="build time to record (default: image mtime)")
    parser.add_argument("--reference-dir",
                        help="where to look for an untampered loose image to localise a mismatch")
    args = parser.parse_args(argv[1:])

    if args.verify:
        problems = verify_package(args.verify, expect_target=args.target,
                                  reference_dir=args.reference_dir)
        if problems:
            for problem in problems:
                print("MISMATCH %s" % problem)
            return 1
        entries, order = read_stored_zip(args.verify)
        manifest = json.loads(entries["manifest.json"].decode("utf-8"))
        images = ", ".join("%s %d B @ %s" % (i["file"], i["size"], i["address"])
                           for i in manifest["images"])
        print("OK %s: format %d, target %s, version %s, commit %s, %s"
              % (os.path.basename(args.verify), manifest["format"], manifest["target"],
                 manifest["version"], manifest["build_commit"], images))
        return 0

    if not args.target or not args.image:
        parser.error("packaging needs --target and --image (or use --verify PKG)")
    try:
        path, manifest, info = build_package(args.target, args.image, out_dir=args.out_dir,
                                            version_file=args.version_file, commit=args.commit,
                                            built_utc=args.built_utc)
    except Refusal as exc:
        print("REFUSED: %s" % exc, file=sys.stderr)
        return 2
    _print_package(path, manifest, info)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
