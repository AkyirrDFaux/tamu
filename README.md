# Tamu

Tamu is a firmware / micro-OS for 32-bit microcontroller boards, together with a
Flutter app that drives a network of them. Every device runs the same command
interface — a system block, a register file with static and dynamic memory,
storage, logging, and an optional scripting engine — and speaks the same packet
protocol, so one app reaches the core (Tamu v2.0A) and the nodes on its RSBus
(DAS v0.1) without device-specific code.

`Docs/` is the specification. It defines the services, the packets, the data
formats and the device behaviour; the firmware and the app are built against it,
not the other way around.

## Layout

| Path | What it holds |
| --- | --- |
| `firmware/` | The firmware for every supported device, in PlatformIO environments |
| `app/` | The Flutter companion app for Linux and Android |
| `Docs/` | The specification vault (Obsidian markdown), the source of truth |
| `test/` | The hardware-test entry point and its direct protocol client |

## Build and run

Firmware, from `firmware/`:

    pio run -e Tamu_v2_0A -e DAS_v0_1

App, from the repo root:

    ./app.sh

Flashing goes through `./upload.sh app|boot|all [tamu|das|valu]`.

Everything that needs no hardware — the firmware native tests, the app's suites,
the analyzer:

    ./test.sh

Those are pointers. Each command's detail lives in the per-part READMEs below.

## Where to read next

| Document | What it covers |
| --- | --- |
| [`firmware/README.md`](firmware/README.md) | Environments, supported devices, build, flash, memory ceilings |
| [`app/README.md`](app/README.md) | The app, its `lib/` layout, running and testing it |
| [`SCRIPTING.md`](SCRIPTING.md) | The script language: slots, every operation, operands, patterns |
| [`test/README.md`](test/README.md) | The HIL rig: protocol client, suite categories, logs |
| [`Docs/`](Docs/) | The specification — start at `Docs/General Architecture.md` |

The commands above are the ones `test.sh`, `app.sh` and `upload.sh` run or
document (read from those scripts).

## Requirements

- PlatformIO, with the platform versions pinned in `firmware/platformio.ini`.
- A Flutter SDK. `./test.sh` takes `FLUTTER=<path>` and otherwise probes the
  usual install locations; `./app.sh` expects `flutter` on `PATH`.
