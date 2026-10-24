# Tamu Firmware

Firmware / micro-OS for 32-bit microcontroller boards.

Companion app: [`../app/`](../app/README.md)

One codebase and one packet protocol (the spec is `Docs/RSBus and Packets.md`);
the parts that must differ per chip — GPIO, flash, startup code, storage geometry
— live in `src/Devices/<device>/` and are selected by a `BOARD_*` define. The
services are specified in `Docs/Services/`, the devices in `Docs/Devices.md`.

## Supported devices

| Environment | Device | What it is |
| --- | --- | --- |
| `Tamu_v2_0A` | Tamu v2.0A (ESP32-C3) | Core board: RSBus, IMU, two fan PWM outputs, LED display, LED-button, App Interface over USB and BLE. Dynamic memory and scripts enabled. |
| `DAS_v0_1` | DAS v0.1 (CH32V003) | Dual analog sensor node: two resistive measurement channels with switchable range selectors, a button, a red and a white LED. RSBus only, no allocator, no scripts. |

Each application has a bootloader environment beside it: `[env:Tamu_bootloader]`
and `[env:DAS_bootloader]`. They run first, own the bottom of flash, and hand
over to the application unless a button is held at reset.

## Build

From `firmware/`:

    pio run -e Tamu_v2_0A
    pio run -e Tamu_v2_0A -e DAS_v0_1

Build only, no upload. Platform and framework versions are pinned in
`platformio.ini` — the DAS toolchain to a commit, the ESP32 platform to `7.0.1`
— so a fresh clone builds the same image.

## Flash

From the repo root:

    ./upload.sh app [tamu|das|valu]      # application slot(s); bootloaders preserved
    ./upload.sh boot [tamu|das|valu]     # overwrite a bootloader
    ./upload.sh all [tamu|das|valu]      # bootloaders, then applications
    ./upload.sh bin das [file] [offset]  # raw image through minichlink

`./upload.sh` is Akyirr's to run. The PlatformIO targets work directly too:
`pio run -e DAS_bootloader -t upload`, then `pio run -e DAS_v0_1 -t upload`.

## Memory ceilings

Where the windows come from: `board_upload.maximum_size` in `platformio.ini`
for the CH32V entries below, and the partition table for the ESP32 ones —
`board_build.partitions` loads `partitions.csv`, and `app_partition_name`
points the app env at `ota_0` while the bootloader keeps `factory`. A CH32V
build that outgrows its region fails to link instead of overrunning the next.

| Environment | Window | Notes |
| --- | --- | --- |
| `Tamu_v2_0A` | 2688 KiB (`ota_0`) | `pio run` reports 24 % of the window used (~2.0 MB unused), so this target trades size for speed |
| `Tamu_bootloader` | 384 KiB (`factory`) | serves bootloader frames into `ota_0` |
| `DAS_v0_1` | 14336 B; code space to `0x3E00` = 13824 B | the tight one: RAM exactly full; a couple of hundred bytes of code space spare |
| `DAS_bootloader` | 2048 B | deliberately link-fails above 2 KB |

## Tests

From the repo root:

    ./test.sh

It builds and runs the host-side native tests — the numeric core in its three
shapes, and the crc and geometry suites, each in both `OPTIMIZE_SPEED` states,
plus the alignment, stride, script, bootloader, TRID and log suites — then the
app's suites and analyzer. Tests that need the physical rig are the `hil` tag;
`test/README.md` at the repo root describes them.
