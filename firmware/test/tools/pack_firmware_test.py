#!/usr/bin/env python3
"""Host test for firmware/tools/pack_firmware.py - the deterministic package and its verifier.

Device-free: it builds packages from synthetic images in a temp directory, so it pins the tool's
behaviour without a build or a rig:

  * determinism - the same inputs (bytes + mtime + metadata) produce the same package bytes, and
    the entries are stored, sorted and stamped 1980-01-01;
  * the manifest - address, size, sha256, crc32, padded_size and crc32_padded against zlib and
    against the 32-byte / 0xFF padding the loaders use;
  * the verifier - OK on a good package, a named file + byte offset on a tampered byte, and exit
    code 1 for a tampered package or a package of the wrong target;
  * the refusals - missing/empty/over-region image, a target with no stamp, a stale stamp, a
    foreign file with no ESP-IDF app descriptor.

    python3 firmware/test/tools/pack_firmware_test.py
"""

import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.dirname(os.path.dirname(HERE))
REPO = os.path.dirname(FIRMWARE)
TOOL = os.path.join(FIRMWARE, "tools", "pack_firmware.py")

failures = 0


def check(name, condition, detail=""):
    global failures
    if condition:
        print("PASS %s" % name)
    else:
        failures += 1
        print("FAIL %s%s" % (name, (": " + detail) if detail else ""))


def load_tool():
    spec = importlib.util.spec_from_file_location("pack_firmware", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run_cli(*args):
    proc = subprocess.run([sys.executable, TOOL] + list(args),
                          capture_output=True, text=True)
    return proc.returncode, proc.stdout, proc.stderr


def version_state(target, year=26, month=11, day=12, iteration=7, digest=None):
    """A version.json holding one target's entry, with the real source hash unless overridden."""
    scripts = os.path.join(REPO, "scripts")
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    import version as version_mod
    return {target: {
        "year": year, "month": month, "day": day, "iteration": iteration,
        "hash": digest if digest is not None else version_mod.source_hash(target),
    }}


def write_json(path, payload):
    with open(path, "w") as f:
        json.dump(payload, f, indent=2, sort_keys=True)
        f.write("\n")


def write_bytes(path, data):
    with open(path, "wb") as f:
        f.write(data)


def idf_app_image(payload, version="testbuild", project="tamu", date="Nov 12 2026",
                  time_text="18:26:10"):
    """A minimal ESP-IDF app image: descriptor magic at 0x20 and the fixed-width text fields."""
    image = bytearray(max(len(payload), 0x100))
    image[:len(payload)] = payload
    image[0x20:0xD0] = bytes(0xD0 - 0x20)   # the descriptor's fields are NUL-padded text
    struct.pack_into("<I", image, 0x20, 0xABCD5432)
    image[0x30:0x30 + len(version)] = version.encode()
    image[0x50:0x50 + len(project)] = project.encode()
    image[0x70:0x70 + len(time_text)] = time_text.encode()
    image[0x80:0x80 + len(date)] = date.encode()
    return bytes(image)


def local_entry_payload(pkg_bytes, name):
    """Offset of a stored entry's data (straight from the local header) and the bytes there."""
    pos = 0
    while True:
        pos = pkg_bytes.find(b"PK\x03\x04", pos)
        if pos < 0:
            raise AssertionError("entry %s not found" % name)
        nlen, xlen = struct.unpack_from("<HH", pkg_bytes, pos + 26)
        csize, = struct.unpack_from("<I", pkg_bytes, pos + 18)
        entry_name = pkg_bytes[pos + 30:pos + 30 + nlen].decode()
        if entry_name == name:
            start = pos + 30 + nlen + xlen
            return start, pkg_bytes[start:start + csize]
        pos += 4


def main():
    tool = load_tool()
    tmp = tempfile.mkdtemp(prefix="fwpack-")
    try:
        # ===== CRC-32 convention =====
        check("crc32 known answer (\"123456789\" -> 0xCBF43926)",
              tool.crc32(b"123456789") == 0xCBF43926, hex(tool.crc32(b"123456789")))
        payload = bytes(range(256)) * 3
        check("crc32 equals zlib.crc32 (poly 0xEDB88320, init/final 0xFFFFFFFF)",
              tool.crc32(payload) == zlib.crc32(payload))
        check("crc32 is reported as a 32-bit hex number, little-endian on the wire",
              tool.hex32(tool.crc32(payload)) == "0x%08X" % zlib.crc32(payload)
              and struct.pack("<I", tool.crc32(b"123456789")) == bytes.fromhex("2639f4cb"))

        # ===== padding convention =====
        check("pad to whole 32-byte chunks with 0xFF (100 -> 128)",
              tool.pad_image(bytes(100)) == bytes(100) + b"\xFF" * 28)
        check("an already-aligned image is not padded (96 -> 96)",
              tool.pad_image(bytes(96)) == bytes(96))

        # ===== determinism + manifest =====
        image = bytes((i * 7 + 3) & 0xFF for i in range(100))
        image_path = os.path.join(tmp, "DAS_v0_1.bin")
        write_bytes(image_path, image)
        os.utime(image_path, (1_700_000_000, 1_700_000_000))
        das_version_file = os.path.join(tmp, "version-das.json")
        write_json(das_version_file, version_state("DAS_v0_1"))

        out_a = os.path.join(tmp, "out-a")
        out_b = os.path.join(tmp, "out-b")
        code_a, stdout_a, stderr_a = run_cli("--target", "DAS_v0_1", "--image", image_path,
                                            "--out-dir", out_a, "--version-file",
                                            das_version_file, "--commit", "0123456789ab")
        code_b, _, _ = run_cli("--target", "DAS_v0_1", "--image", image_path,
                               "--out-dir", out_b, "--version-file", das_version_file,
                               "--commit", "0123456789ab")
        check("packaging a DAS image succeeds", code_a == 0 and code_b == 0,
              stderr_a or stdout_a)
        pkg_a = os.path.join(out_a, "DAS_v0_1-26.11.12.7.fw.zip")
        pkg_b = os.path.join(out_b, "DAS_v0_1-26.11.12.7.fw.zip")
        check("the package name is <target>-<version>.fw.zip",
              os.path.isfile(pkg_a) and os.path.isfile(pkg_b), pkg_a)
        with open(pkg_a, "rb") as f:
            bytes_a = f.read()
        with open(pkg_b, "rb") as f:
            bytes_b = f.read()
        check("building twice from the same inputs gives identical bytes",
              bytes_a == bytes_b,
              "%d vs %d B" % (len(bytes_a), len(bytes_b)))
        check("the built_utc comes from the image mtime and is stable",
              "2023-11-14T22:13:20Z" in stdout_a, stdout_a)

        with zipfile.ZipFile(pkg_a) as zf:
            infos = zf.infolist()
            names = [i.filename for i in infos]
            check("entries are sorted", names == sorted(names), names)
            check("entries are stored (no compression)",
                  all(i.compress_type == zipfile.ZIP_STORED for i in infos))
            check("entries carry the fixed 1980-01-01 timestamp",
                  all(i.date_time == (1980, 1, 1, 0, 0, 0) for i in infos))
            check("entries carry fixed mode bits, not the umask's",
                  all(i.external_attr == 0o644 << 16 and i.create_system == 3 for i in infos),
                  str([(i.external_attr, i.create_system) for i in infos]))
            manifest = json.loads(zf.read("manifest.json").decode())

        check("manifest format is 1", manifest.get("format") == 1, manifest.get("format"))
        check("manifest target is the packaged target", manifest.get("target") == "DAS_v0_1")
        check("manifest version is the stamp the image reports",
              manifest.get("version") == "26.11.12.7", manifest.get("version"))
        check("manifest records the build commit",
              manifest.get("build_commit") == "0123456789ab")
        check("manifest records built_utc", manifest.get("built_utc") == "2023-11-14T22:13:20Z")
        entry = manifest["images"][0]
        check("image file is the stable name", entry["file"] == "DAS_v0_1.bin", entry["file"])
        check("image address is 0x08000800", entry["address"] == "0x08000800", entry["address"])
        check("image size is the shipped size", entry["size"] == len(image), entry["size"])
        check("image sha256 is the shipped bytes' sha256",
              entry["sha256"] == tool.sha256_hex(image))
        check("image crc32 is the shipped bytes' crc32",
              entry["crc32"] == tool.hex32(zlib.crc32(image)), entry["crc32"])
        check("padded_size rounds up to 32 (100 -> 128)", entry["padded_size"] == 128,
              entry["padded_size"])
        check("crc32_padded covers the 0xFF-padded image",
              entry["crc32_padded"] == tool.hex32(zlib.crc32(image + b"\xFF" * 28)),
              entry["crc32_padded"])

        # ===== the verifier: OK =====
        code, stdout, stderr = run_cli("--verify", pkg_a)
        check("verifying an intact package prints OK and exits 0",
              code == 0 and stdout.startswith("OK "), "%d %s%s" % (code, stdout, stderr))
        code, stdout, _ = run_cli("--verify", pkg_a, "--target", "DAS_v0_1")
        check("verifying against the package's own target passes", code == 0, stdout)

        # ===== the verifier: tampered byte, localised against the loose image =====
        tamper_dir = os.path.join(tmp, "tampered")
        os.makedirs(tamper_dir)
        shutil.copy(pkg_a, os.path.join(tamper_dir, os.path.basename(pkg_a)))
        shutil.copy(image_path, os.path.join(tamper_dir, "DAS_v0_1.bin"))
        tampered = os.path.join(tamper_dir, os.path.basename(pkg_a))
        with open(tampered, "rb") as f:
            data = bytearray(f.read())
        data_start, payload = local_entry_payload(bytes(data), "DAS_v0_1.bin")
        data[data_start + 7] ^= 0xFF
        write_bytes(tampered, bytes(data))
        code, stdout, stderr = run_cli("--verify", tampered)
        check("a flipped byte makes the verifier exit non-zero", code == 1,
              "%d %s%s" % (code, stdout, stderr))
        check("the mismatch names the file", "MISMATCH DAS_v0_1.bin:" in stdout, stdout)
        check("the mismatch names the byte offset (0x7)",
              "first differing byte at offset 0x7" in stdout, stdout)
        # The same package without the loose reference still fails, on the digest alone.
        lonely = os.path.join(tmp, "lonely")
        os.makedirs(lonely)
        shutil.copy(tampered, os.path.join(lonely, os.path.basename(tampered)))
        code, stdout, _ = run_cli("--verify", os.path.join(lonely, os.path.basename(tampered)))
        check("without a reference copy the digest mismatch is still named",
              code == 1 and "sha256" in stdout and "DAS_v0_1.bin" in stdout, stdout)

        # ===== the verifier: wrong target =====
        code, stdout, stderr = run_cli("--verify", pkg_a, "--target", "Tamu_v2_0A")
        check("verifying a DAS package against Tamu_v2_0A is refused", code == 1,
              "%d %s%s" % (code, stdout, stderr))
        check("the wrong-target refusal names both targets",
              "DAS_v0_1" in stdout and "Tamu_v2_0A" in stdout, stdout)

        # ===== the verifier: unexpected entry =====
        extra = os.path.join(tmp, "extra.fw.zip")
        with zipfile.ZipFile(pkg_a) as zin, zipfile.ZipFile(extra, "w") as zout:
            for info in zin.infolist():
                zout.writestr(info, zin.read(info.filename))
            zout.writestr("smuggled.bin", b"\x00" * 8)
        code, stdout, _ = run_cli("--verify", extra)
        check("an entry that is not in the manifest is a mismatch",
              code == 1 and "smuggled.bin" in stdout, stdout)

        # ===== refusals =====
        code, _, stderr = run_cli("--target", "DAS_v0_1",
                                 "--image", os.path.join(tmp, "nope.bin"),
                                 "--out-dir", out_a, "--version-file", das_version_file)
        check("a missing image is refused (exit 2)", code == 2 and "not found" in stderr, stderr)

        empty = os.path.join(tmp, "empty.bin")
        write_bytes(empty, b"")
        code, _, stderr = run_cli("--target", "DAS_v0_1", "--image", empty,
                                 "--out-dir", out_a, "--version-file", das_version_file)
        check("an empty image is refused (exit 2)", code == 2 and "empty" in stderr, stderr)

        oversize = os.path.join(tmp, "oversize.bin")
        write_bytes(oversize, bytes(tool.TARGETS["DAS_v0_1"]["max_size"] + 1))
        code, _, stderr = run_cli("--target", "DAS_v0_1", "--image", oversize,
                                 "--out-dir", out_a, "--version-file", das_version_file)
        check("an image larger than the target's region is refused (exit 2)",
              code == 2 and "larger than" in stderr, stderr)

        no_stamp = os.path.join(tmp, "version-empty.json")
        write_json(no_stamp, {})
        code, _, stderr = run_cli("--target", "DAS_v0_1", "--image", image_path,
                                 "--out-dir", out_a, "--version-file", no_stamp)
        check("a target with no stamp entry is refused (exit 2)",
              code == 2 and "stamp cannot be read" in stderr, stderr)

        stale_file = os.path.join(tmp, "version-stale.json")
        write_json(stale_file, version_state("DAS_v0_1", digest="deadbeef"))
        code, _, stderr = run_cli("--target", "DAS_v0_1", "--image", image_path,
                                 "--out-dir", out_a, "--version-file", stale_file)
        check("a stamp that no longer matches the sources is refused (exit 2)",
              code == 2 and "stale" in stderr, stderr)

        incomplete_file = os.path.join(tmp, "version-incomplete.json")
        state = version_state("DAS_v0_1")
        del state["DAS_v0_1"]["iteration"]
        write_json(incomplete_file, state)
        code, _, stderr = run_cli("--target", "DAS_v0_1", "--image", image_path,
                                 "--out-dir", out_a, "--version-file", incomplete_file)
        check("an incomplete stamp entry is refused (exit 2)",
              code == 2 and "iteration" in stderr, stderr)

        code, _, stderr = run_cli("--target", "Not_an_env", "--image", image_path,
                                 "--out-dir", out_a)
        check("an unknown target is refused (exit 2)", code == 2 and "unknown target" in stderr,
              stderr)

        # ===== the ESP-IDF app descriptor check (core images) =====
        core_version_file = os.path.join(tmp, "version-core.json")
        write_json(core_version_file, version_state("Tamu_v2_0A", iteration=6))
        good_core = os.path.join(tmp, "Tamu_v2_0A.bin")
        write_bytes(good_core, idf_app_image(bytes(range(64)) * 4))
        os.utime(good_core, (1_700_000_000, 1_700_000_000))
        code, stdout, stderr = run_cli("--target", "Tamu_v2_0A", "--image", good_core,
                                       "--out-dir", out_a, "--version-file", core_version_file,
                                       "--commit", "0123456789ab")
        check("a core image with an ESP-IDF app descriptor is packaged", code == 0,
              stderr or stdout)
        check("the descriptor's own identity is reported",
              "esp-idf app descriptor" in stdout and "'tamu'" in stdout, stdout)
        core_pkg = os.path.join(out_a, "Tamu_v2_0A-26.11.12.6.fw.zip")
        code, stdout, _ = run_cli("--verify", core_pkg, "--target", "Tamu_v2_0A")
        check("the core package verifies against Tamu_v2_0A", code == 0, stdout)
        with zipfile.ZipFile(core_pkg) as zf:
            core_manifest = json.loads(zf.read("manifest.json").decode())
        check("the core image address is 0x90000",
              core_manifest["images"][0]["address"] == "0x90000",
              core_manifest["images"][0]["address"])

        foreign = os.path.join(tmp, "foreign.bin")
        write_bytes(foreign, bytes(0x400))
        code, _, stderr = run_cli("--target", "Tamu_v2_0A", "--image", foreign,
                                 "--out-dir", out_a, "--version-file", core_version_file)
        check("a file with no app descriptor is refused as not that target's image (exit 2)",
              code == 2 and "descriptor" in stderr, stderr)

        # ===== bootloaders are unversioned, and verify against their target =====
        boot = os.path.join(tmp, "DAS_bootloader.bin")
        write_bytes(boot, bytes(500))
        os.utime(boot, (1_700_000_000, 1_700_000_000))
        code, stdout, stderr = run_cli("--target", "DAS_bootloader", "--image", boot,
                                       "--out-dir", out_a, "--commit", "0123456789ab")
        check("an unversioned bootloader packages as -unversioned",
              code == 0 and os.path.isfile(os.path.join(
                  out_a, "DAS_bootloader-unversioned.fw.zip")), stderr or stdout)
        code, stdout, _ = run_cli("--verify", os.path.join(
            out_a, "DAS_bootloader-unversioned.fw.zip"), "--target", "DAS_bootloader")
        check("the bootloader package verifies (address 0x08000000)",
              code == 0 and "0x08000000" in stdout, stdout)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        print("%d FAILED" % failures)
        return 1
    print("all packaging checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
