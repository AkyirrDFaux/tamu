# Hardware tests

Two layers, both driving the *protocol* rather than any printed output.

## `tamu_proto.py` — direct protocol client (the rig entry point)

Python client for the App Interface over USB Serial/JTAG: it builds and parses the real wire
frames (`0xFA | CRC8 | len | packet stream | 0xBF`, packets crc8-first) and reaches a node
through the core. Use it for quick checks and for writing protocol-level tests:

```
python3 tamu_proto.py 1        # ping, enumerate block types, read the System Name
```

```python
from tamu_proto import Tamu, BlockType
with Tamu() as t:
    print(t.enumerate_types())                     # [0x03, 0x04, 0x05, 0x06]
    print(t.read_field(2, BlockType.ResistiveMeasure, 0, 3))  # DAS temperature
```

Two device-side behaviours to respect: the TX pump treats the port as dead if no app frame has
arrived for ~500 ms (so send and read within that window), and a malformed packet costs a
stream resync (compare against `app/lib/core/protocol.dart`).

## `app/test/hil/` — the HIL suite

One entry point drives the protocol through the app's own clients, so it verifies what the app sees.
It scans the bus, builds a rig inventory and runs every test whose prerequisite the rig satisfies; an
unmet prerequisite is a **skip, never a failure**. `Docs/HIL Tests.md` is the spec. Run it from `app/`:

```
bash test/run_hil_tests.sh                  # every category except bootloader
bash test/run_hil_tests.sh --scan           # list the rig and stop
bash test/run_hil_tests.sh --category module
bash test/run_hil_tests.sh --device 2
bash test/run_hil_tests.sh --bootloader     # a device in bootloader mode
TAMU_HIL=/dev/ttyACM0 bash test/run_hil_tests.sh
TAMU_HIL=ble bash test/run_hil_tests.sh
TAMU_HIL_BOOTLOADER=1 bash test/run_hil_tests.sh --bootloader  # a device is in update mode
```

The core port is resolved from `/dev/serial/by-id` when `TAMU_HIL` is unset. Categories are generic,
service, module and partner; the bootloader category runs on its own because it takes its device off
the bus. Every run writes `app/test/hil/logs/hil_<stamp>.log` (gitignored) with the rig inventory,
each result and the reason behind every skip.

The tests are **destructive**: they change blocks, files, subscriptions and addresses, and the device
is not restored afterwards. Each test clears the temporaries it creates. Bootloader tests need the DAS
built with `-D BOOTLOADER_FORCE` and declared with `TAMU_HIL_BOOTLOADER=<id>`.

## Host-only tests

`./test.sh` at the repo root runs everything that needs no hardware: the firmware's native
numeric/geometry/stride/CRC tests plus the app's unit tests and analyzer.
