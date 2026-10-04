"""Tamu_v2_0A upload offset: the main app lives in `ota_0`, not the factory slot.

PlatformIO's espidf builder sets `ESP32_APP_OFFSET` from the boot-default partition (`factory`,
0x10000) - which is the core bootloader. Override it so `pio run -e Tamu_v2_0A -t upload`
writes the main app to `ota_0` and never clobbers the factory bootloader.

The offset is derived from `partitions.csv` (the single source of truth; ESP-IDF assigns the
offsets at build time so they are not written in the file) instead of a hardcoded 0x70000.
"""
import csv
import os
import re

Import("env")  # noqa: F821 - provided by PlatformIO's SCons environment

# SPI flash sector size; ESP-IDF aligns every partition to it (CONFIG_SPI_FLASH_SEC_SIZE).
_ALIGN = 0x1000


def _align_up(value):
    return (value + _ALIGN - 1) // _ALIGN * _ALIGN


def _to_int(text):
    text = (text or "").strip()
    return int(text, 0) if text else None


def _partition_table_offset(project_dir, pioenv):
    """CONFIG_PARTITION_TABLE_OFFSET from the env's sdkconfig (ESP-IDF default 0x8000)."""
    path = os.path.join(project_dir, "sdkconfig.%s" % pioenv)
    try:
        with open(path) as f:
            for line in f:
                m = re.match(r"CONFIG_PARTITION_TABLE_OFFSET\s*=\s*(\S+)", line.strip())
                if m:
                    return int(m.group(1), 0)
    except OSError:
        pass
    return 0x8000


def ota0_offset(csv_path, table_offset):
    """Resolve the first `ota_0` partition offset by replaying the ESP-IDF layout."""
    offset = _align_up(table_offset + _ALIGN)  # the table itself occupies one sector
    with open(csv_path, newline="") as f:
        for row in csv.reader(f):
            row = [cell.strip() for cell in row]
            if not row or not row[0] or row[0].startswith("#"):
                continue
            explicit = _to_int(row[3]) if len(row) > 3 else None
            if explicit is not None:
                offset = explicit
            offset = _align_up(offset)
            if row[0] == "ota_0":
                return offset
            if len(row) > 4:
                offset += _to_int(row[4]) or 0
    raise SystemExit("core_app_offset: no ota_0 entry in %s" % csv_path)


_project_dir = env.subst("$PROJECT_DIR")
_ota0 = ota0_offset(os.path.join(_project_dir, "partitions.csv"),
                    _partition_table_offset(_project_dir, env.subst("$PIOENV")))
print("Tamu_v2_0A: ota_0 upload offset 0x%X (derived from partitions.csv)" % _ota0)
env.Replace(ESP32_APP_OFFSET=_ota0)
