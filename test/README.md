# Tamu hardware tests & tools

- `hwtest.py`     - low-level serial console helper (send CLI commands to the core)
- `testsuite.py`  - full automated regression battery over the RSBus (98 checks)

Usage:
    python3 test/testsuite.py [--port /dev/ttyACM0] [--reboot] [--only <groups>]

Flutter app HIL suites live in `app/test/` (must stay there for `flutter test`),
run them with TAMU_HIL + LIBSERIALPORT_PATH set - see the header of
`app/test/hil_live_test.dart`.
