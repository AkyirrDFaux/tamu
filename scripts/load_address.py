#!/usr/bin/env python3
"""Resolve the flash address of a Tamu core image - from the package manifest when there is one.

Why this exists (Issues.md E86 / F63, card `t_9c6dbc90`)
-------------------------------------------------------
`upload.sh` carried `TAMU_APP_OFFSET=0x70000` labelled "ota_0". It is not: `firmware/partitions.csv`
puts `factory` at 0x10000 (size 0x7D000, so it ends at 0x8D000) and `ota_0` at 0x90000. A raw write
at 0x70000 therefore started inside `factory` and ran over into `ota_0`; the core answered nothing
until the same image was written at 0x90000. A default that writes an app where it cannot boot is
the footgun this tool removes.

The rules, in order
-------------------
1. A package (`.fw.zip`) -> the address is the one its `manifest.json` names for its image. The
   image is extracted for the caller (esptool cannot read a zip) and its size/sha256 are checked
   against the manifest before anything is written.
2. A loose `.bin` that a sibling package names with the same size and sha256 -> that address.
3. A loose `.bin` that no manifest names -> `--default`, and the output says the address was
   **assumed, not read**.
4. An explicit `--offset` is **refused** when the manifest names a different address. It is never
   "helpfully" patched to the right one: a silent correction teaches nothing and hides the mistake.

Partition names always come from `partitions.csv`, resolved row by row, and the row that matched is
echoed in every message - so the text cannot drift from the layout.

stdout: `KEY=VALUE` lines for the caller (`ADDRESS`, `IMAGE`, `ADDRESS_SOURCE`, `PARTITION`,
`PARTITION_ROW`, `TARGET`, `VERSION`).
stderr: the provenance or the refusal, for a human.
exit:   0 flash this, 2 refused (nothing was written), 1 usage error.

    scripts/load_address.py binaries/Tamu_v2_0A-26.11.12.6.fw.zip --extract-dir /tmp/x
    scripts/load_address.py binaries/Tamu_v2_0A.bin --default 0x90000
    scripts/load_address.py pkg.fw.zip --offset 0x70000     # -> refuses, exit 2
"""

import argparse
import csv
import hashlib
import json
import os
import sys
import zipfile

# SPI flash sector size; ESP-IDF aligns every partition to it (CONFIG_SPI_FLASH_SEC_SIZE).
ALIGN = 0x1000
# CONFIG_PARTITION_TABLE_OFFSET: the partition table occupies the first sector after it.
TABLE_OFFSET = 0x8000

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_PARTITIONS = os.path.join(REPO, "firmware", "partitions.csv")
MANIFEST_NAME = "manifest.json"


def _align_up(value):
    return (value + ALIGN - 1) // ALIGN * ALIGN


def _to_int(text):
    text = (text or "").strip()
    return int(text, 0) if text else None


def _hex(value):
    return "0x%X" % value


def _sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


class Refusal(Exception):
    """Nothing was written; the message is what a human needs to see."""


# ---------------------------------------------------------------------------- partitions


def parse_partitions(path):
    """Rows of partitions.csv with their resolved `[offset, offset+size)` window.

    Rows may leave the offset empty (ESP-IDF then places them after the previous one), so the
    layout is replayed the same way `firmware/scripts/core_app_offset.py` replays it.
    """
    rows = []
    offset = _align_up(TABLE_OFFSET + ALIGN)
    with open(path, newline="") as handle:
        for cells in csv.reader(handle):
            cells = [cell.strip() for cell in cells]
            if not cells or not cells[0] or cells[0].startswith("#"):
                continue
            explicit = _to_int(cells[3]) if len(cells) > 3 else None
            if explicit is not None:
                offset = explicit
            offset = _align_up(offset)
            size = _to_int(cells[4]) if len(cells) > 4 else None
            rows.append({
                "name": cells[0],
                "type": cells[1] if len(cells) > 1 else "",
                "subtype": cells[2] if len(cells) > 2 else "",
                "offset": offset,
                "size": size,
                # The row as written, for the message: it must be quotable against the CSV.
                "row": ", ".join(cell for cell in cells if cell),
            })
            offset += size or 0
    if not rows:
        raise Refusal("no partitions in %s" % path)
    return rows


def partition_at(rows, address):
    for row in rows:
        if row["size"] and row["offset"] <= address < row["offset"] + row["size"]:
            return row
    return None


def describe_partition(row):
    span = "%s..%s" % (_hex(row["offset"]), _hex(row["offset"] + (row["size"] or 0)))
    return "partition '%s' (%s/%s) %s" % (row["name"], row["type"], row["subtype"], span)


def csv_row_note(row):
    return "partitions.csv: %s" % row["row"]


def default_partition(rows, default_address):
    """The partition the documented default lands in - what an app image is supposed to sit in."""
    return partition_at(rows, default_address)


# ---------------------------------------------------------------------------- packages


def read_manifest(package):
    try:
        with zipfile.ZipFile(package) as archive:
            return json.loads(archive.read(MANIFEST_NAME))
    except (KeyError, zipfile.BadZipFile, json.JSONDecodeError) as error:
        raise Refusal("%s carries no readable %s (%s)" % (package, MANIFEST_NAME, error))


def package_image(manifest, package, wanted_file=None):
    """The one image entry to flash: by name when the package holds several."""
    images = manifest.get("images") or []
    if not images:
        raise Refusal("%s names no images in its %s" % (package, MANIFEST_NAME))
    if wanted_file:
        base = os.path.basename(wanted_file)
        for image in images:
            if os.path.basename(image.get("file", "")) == base:
                return image
        named = ", ".join(image.get("file", "?") for image in images)
        raise Refusal("%s names %s, not %s" % (package, named, base))
    if len(images) > 1:
        named = ", ".join(image.get("file", "?") for image in images)
        raise Refusal("%s holds several images (%s); name the one to flash" % (package, named))
    return images[0]


def source_note(package, manifest, image):
    manifest = manifest or {}
    bits = []
    if manifest.get("target"):
        bits.append("target %s" % manifest["target"])
    if manifest.get("version"):
        bits.append("version %s" % manifest["version"])
    bits.append("image %s" % image.get("file", "?"))
    return "%s in %s (%s)" % (MANIFEST_NAME, package, ", ".join(bits))


def matching_package(image_path):
    """A sibling package whose manifest names this exact file (size and sha256 agree)."""
    directory = os.path.dirname(os.path.abspath(image_path))
    base = os.path.basename(image_path)
    size = os.path.getsize(image_path)
    digest = _sha256_file(image_path)
    for name in sorted(os.listdir(directory)):
        if not name.endswith(".fw.zip"):
            continue
        package = os.path.join(directory, name)
        try:
            manifest = read_manifest(package)
        except Refusal:
            continue
        for image in manifest.get("images") or []:
            if (os.path.basename(image.get("file", "")) == base
                    and image.get("size") == size
                    and image.get("sha256") == digest):
                return package, manifest, image
    return None


def extract_image(package, image, out_dir):
    """Write the packaged image out for esptool, after checking it is the manifest's image."""
    member = image.get("file", "")
    try:
        with zipfile.ZipFile(package) as archive:
            data = archive.read(member)
    except (KeyError, zipfile.BadZipFile) as error:
        raise Refusal("%s: cannot read %s (%s)" % (package, member, error))
    digest = hashlib.sha256(data).hexdigest()
    if image.get("size") is not None and len(data) != image["size"]:
        raise Refusal("%s: %s is %d B, the manifest says %d B"
                      % (package, member, len(data), image["size"]))
    if image.get("sha256") and digest != image["sha256"]:
        raise Refusal("%s: %s sha256 %s, the manifest says %s"
                      % (package, member, digest, image["sha256"]))
    os.makedirs(out_dir, exist_ok=True)
    destination = os.path.join(out_dir, os.path.basename(member))
    with open(destination, "wb") as handle:
        handle.write(data)
    return destination


# ---------------------------------------------------------------------------- resolution


def resolve(args):
    """-> (facts dict, provenance lines). Raises Refusal."""
    partitions = parse_partitions(args.partitions)
    default_address = _to_int(args.default)
    if default_address is None:
        raise Refusal("--default is required: the address to assume when no manifest names one")

    path = args.path
    if not os.path.isfile(path):
        raise Refusal("no such image: %s" % path)

    is_package = path.endswith(".fw.zip")
    package = manifest = image = None
    if is_package:
        package = path
        manifest = read_manifest(package)
        image = package_image(manifest, package, args.image)
    else:
        found = matching_package(path)
        if found:
            package, manifest, image = found

    manifest_address = None
    if image is not None:
        raw_address = image.get("address")
        if not raw_address:
            # A manifest that names no address cannot be checked against anything: say so instead
            # of falling back to the default and calling it "assumed".
            raise Refusal("%s names no address for %s"
                          % (source_note(package, manifest, image), image.get("file", "?")))
        try:
            manifest_address = _to_int(raw_address)
        except ValueError:
            raise Refusal("%s names address %r for %s, which is not a number"
                          % (source_note(package, manifest, image), raw_address,
                             image.get("file", "?")))
    requested = None
    if args.offset:
        try:
            requested = _to_int(args.offset)
        except ValueError:
            raise Refusal("--offset %r is not a number" % args.offset)

    lines = []
    if requested is not None:
        # An offset the caller named. The manifest either agrees or this is a refusal - never a
        # silent correction.
        address = requested
        where = partition_at(partitions, requested)
        if manifest_address is not None and requested != manifest_address:
            raise Refusal(
                "%s is inside %s\n"
                "  %s\n"
                "  the package names %s for %s - %s\n"
                "  nothing written; the offset is not patched for you"
                % (_hex(requested),
                   describe_partition(where) if where else "no partition in partitions.csv",
                   csv_row_note(where) if where
                   else "partitions.csv: no row covers %s" % _hex(requested),
                   _hex(manifest_address), image.get("file", "?"),
                   source_note(package, manifest, image)))
        if manifest_address is not None:
            lines.append("address: %s - you named it, and %s names it for %s"
                         % (_hex(address), source_note(package, manifest, image),
                            image.get("file", "?")))
            source = "named by you and by %s" % source_note(package, manifest, image)
        else:
            lines.append("address: %s - you named it; no package names this image, so it is "
                         "used as-is" % _hex(address))
            source = "named by you; no manifest names this image"
            slot = default_partition(partitions, default_address)
            if where is not None and slot is not None and where["name"] != slot["name"]:
                lines.append(
                    "warning: %s is inside %s, not the core app's slot '%s' (%s) - the core "
                    "boots from '%s'"
                    % (_hex(address), describe_partition(where), slot["name"],
                       _hex(slot["offset"]), slot["name"]))
    elif manifest_address is not None:
        address = manifest_address
        source = "read from %s" % source_note(package, manifest, image)
        lines.append("address: %s - read from %s"
                     % (_hex(address), source_note(package, manifest, image)))
    else:
        address = default_address
        source = ("assumed, not read - no package or manifest names %s; using the documented "
                  "default %s" % (os.path.basename(path), _hex(address)))
        lines.append("address: %s - assumed, not read: no package or manifest names %s; using "
                     "the documented default for the core app"
                     % (_hex(address), os.path.basename(path)))

    where = partition_at(partitions, address)
    if where is None:
        raise Refusal(
            "%s is not inside any partition in %s\n"
            "  nothing written"
            % (_hex(address), args.partitions))
    lines.append("slot: %s" % describe_partition(where))
    lines.append("      %s" % csv_row_note(where))

    facts = {
        "ADDRESS": _hex(address),
        "IMAGE": path,
        "ADDRESS_SOURCE": source,
        "PARTITION": where["name"],
        "PARTITION_ROW": where["row"],
    }

    if is_package:
        # The image the manifest describes lives inside the package: extract it for esptool.
        if image is None:
            raise Refusal("%s names no image to flash" % path)
        out_dir = args.extract_dir or os.path.dirname(os.path.abspath(path))
        facts["IMAGE"] = extract_image(package, image, out_dir)
        lines.append("image: %s -> %s (size and sha256 match the manifest)"
                     % (image.get("file", "?"), facts["IMAGE"]))
    elif package:
        lines.append("image: %s matches the packaged copy in %s (size and sha256)"
                     % (os.path.basename(path), package))
    return facts, lines


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Resolve the flash address of a Tamu core image from its package manifest.")
    parser.add_argument("path", help="a .fw.zip package or a loose .bin")
    parser.add_argument("--offset", help="the address the caller asked for; refused on a mismatch")
    parser.add_argument("--default", default="0x90000",
                        help="address to assume when no manifest names the image (default 0x90000)")
    parser.add_argument("--partitions", default=DEFAULT_PARTITIONS,
                        help="partition table to resolve names from (default firmware/partitions.csv)")
    parser.add_argument("--image", help="image name inside the package, when it holds several")
    parser.add_argument("--extract-dir", dest="extract_dir",
                        help="where to write the packaged image (default: beside the package)")
    args = parser.parse_args(argv)

    try:
        facts, lines = resolve(args)
    except Refusal as refusal:
        print("refuse: %s" % refusal, file=sys.stderr)
        return 2

    print("load address: %s -> %s" % (os.path.basename(args.path), facts["ADDRESS"]),
          file=sys.stderr)
    for line in lines:
        print("  %s" % line, file=sys.stderr)
    for key, value in facts.items():
        if value:
            print("%s=%s" % (key, value))
    return 0


if __name__ == "__main__":
    sys.exit(main())
