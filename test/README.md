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

## `app/test/hil_*_test.dart` — Flutter HIL suites

The feature suites drive the same protocol through the app's own clients, so they verify what
the app sees. Set the port and run one or more files:

```
TAMU_HIL=/dev/ttyACM0 bash app/test/run_hil_tests.sh \
    app/test/tamu_hardware_verification_test.dart \
    app/test/hil_subscriptions_test.dart \
    app/test/hil_dynamic_persistence_test.dart
```

They are non-destructive by design (scratch blocks/files are removed and defaults restored).
The evaluation-setup suite (`hil_current_setup_test.dart`) needs displays and fans; it applies
the setup and also restores it, so run it last when the full rig is attached.

## Host-only tests

`./test.sh` at the repo root runs everything that needs no hardware: the firmware's native
numeric/geometry/stride/CRC tests plus the app's unit tests and analyzer.
