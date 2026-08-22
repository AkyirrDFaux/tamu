# Real Hardware Verification - Tamu v2.0A + DAS v0.1

Final session 2026-08-22. Two devices on the RS-485 RSBus:

- **Tamu v2.0A** (ESP32-C3, USB `/dev/ttyACM0`, console + RSBus) - core, SNDB ID 1, SN `E4B063C820700000000000000000`.
- **DAS v0.1** (CH32V003, WCH-Link `/dev/ttyACM1`, no console) - SNDB ID 2, SN `CDABBD6400000000000000000000`.

The DAS has no console output; it is reached exclusively over the RSBus through the Tamu console.
DAS firmware is uploaded over the WCH-Link (`pio run -e DAS_v0_1 -t upload --upload-port /dev/ttyACM1`).
Note: re-flashing the DAS rewrites the whole chip flash, including the 2 KB storage reservation at
the top of flash, so its storage filesystem is reformatted on every upload.

## Tamu v2.0A (ID 1) - service checklist

| Service / test | Command | Result |
| --- | --- | --- |
| Device: ping | `dev 1 ping` | PASS |
| Device: type | `dev 1 type` | PASS (0x0001 Tamu_v2_0A) |
| Device: serial number | `dev 1 sn` | PASS |
| Device: version | `dev 1 version` | PASS ("Tamu v2.0A") |
| Device: name | `dev 1 name` | PASS ("Tamu Node") |
| Device: uptime | `dev 1 uptime` | PASS |
| Device: loop time | `dev 1 loop` | PASS (avg 10 ms) |
| Device: time sync | `dev 1 time` | PASS (offset ~0 ms) |
| Device: capability | `dev 1 cap` | FAIL (0x00000000, deferred - see notes) |
| Device: discover (self) | `dev 1 discover` | PASS (re-registers as ID 1) |
| System Memory: LED-Button (b0) read | `read 1 s 0 0` / `read 1 s 0 1` | PASS |
| System Memory: Fan PWM (b1/b2) read | `read 1 s 1/2 0` / `read 1 s 1/2 1` | PASS (25 kHz, duty 0) |
| System Memory: Acc&Gyr (b3) read | `read 1 s 3 1` / `read 1 s 3 2` | PASS (live accel + gyro) |
| System Memory: LED Display (b4/b5) read | `read 1 s 4 0` / `read 1 s 5 0` | PASS |
| System Memory: fan duty write + callback | `write 1 s 1 1 0x003 0.5` | PASS (fan spins; value persists) |
| System Memory: LED write + callback | `write 1 s 0 0 0x006 1` | PASS (LED toggles) |
| System Memory: backup (save/rmem/recall) | `save/rmem/recall 1 s ...` | PASS (recall restores value) |
| Dynamic Memory CRUD | `create/save/rmem/recall/delete 1 d 0x100 dynblock` | PASS |
| Keyed Memory CRUD | `create 1 k 0x101`, `write 1 k 0 0 1 0x003 123`, read/save/rmem/recall/delete | PASS (key persists) |
| Storage: file table | `file 1 table` | PASS |
| Storage: create/read/resize/delete | `file 1 create/read/resize/delete TESTF` | PASS |
| SNDB | `sndb 1 read_all` | PASS (ID 1 core, ID 2 DAS) |
| Log service | `logs` | PASS |

## DAS v0.1 (ID 2) - service checklist

| Service / test | Command | Result |
| --- | --- | --- |
| Discovery | boot | PASS (assigned ID 2) |
| Device: ping | `dev 2 ping` | PASS |
| Device: type | `dev 2 type` | PASS (0x0003 DualAnalogSensor) |
| Device: serial number | `dev 2 sn` | PASS (CDABBD64...) |
| Device: version | `dev 2 version` | PASS ("DAS v0.1") |
| Device: name | `dev 2 name` | PASS ("DAS v0.1") |
| Device: uptime | `dev 2 uptime` | PASS (tracks real time after sync fix) |
| Device: loop time | `dev 2 loop` | PASS (~1000 ms) |
| Device: time sync | `dev 2 time` | PASS (converges, offset < 1 s) |
| Device: capability | `dev 2 cap` | FAIL (0x00000000, deferred - see notes) |
| System Memory: Meas1 read | `read 2 s 0 0..4` | PASS |
| System Memory: Meas2 read | `read 2 s 1 3` / `read 2 s 1 4` | PASS |
| Measuring (open input) | `read 2 s 0 3` / `read 2 s 0 4` | PASS (~1023 raw, 330 kOhm auto-range) |
| System Memory: write | `write 2 s 0 1 0x003 0.8` | PASS |
| System Memory: backup (save/recall) | `save/recall 2 s -` | PASS (recall restores value) |
| Storage: file table | `file 2 table` | PASS |
| Storage: create/resize/delete | `file 2 create <name> <size>` | FAIL - hangs the DAS (see notes) |

## Not working correctly / notes

1. **DAS storage `create`/`resize`/`delete` hang the DAS.** `file 2 create <name> <size>`,
   `resize` and `delete` make the CH32V003 stop responding to anything on the bus (even `ping`)
   until it is re-flashed. `save 2 s -` (System Memory backup) and `file 2 table` work, so the
   flash controller and the table read path are fine; the failure is inside `CreateFile` /
   `WriteTable`. Bisected on hardware: the request is received and dispatched (Storage service,
   CID 2), then the DAS hangs in the create logic. Not yet root-caused; the DAS fast-erase was
   replaced with the bounded normal erase (fast erase leaves cells at 0x00 on the CH32V003,
   which corrupts the erased-state representation) but the hang persists. See `Docs/Issues.md`
   (Open) for the full write-up.

2. **Capability bitfield is 0x00000000 on both devices.** `dev 1 cap` and `dev 2 cap` report no
   capabilities. Known deferred doc mismatch (`Docs/Issues.md`): no CORE capability bit is
   defined yet.

3. **DAS file-table display prints the table's own entry as "........"** (the `.TABLE` entry's
   name). Cosmetic; the table itself is read/written correctly.

4. **DAS discovery is sensitive to bus congestion.** With heavy broadcast logging (5 reports/s)
   the DAS failed to acquire an ID - its own transmission echoes pile up in the 256 B RX ring,
   `ReceivePacket` re-syncs on its own `0xAA` echoes, and the core's ID-assignment reply is
   swallowed. Latent risk for any chatty node; documented in `Docs/Improve.md`.

5. **One unexplained Tamu reboot during the session** (while hammering the bus + WCH-Link
   operations). Not reproduced since; loop time and all services recovered cleanly on boot.

## Fixes verified this session (details in `Docs/Issues.md`)

- Core -> node time-sync offset sign (node uptime no longer diverges; `dev 2 time` converges).
- DAS default name (now "DAS v0.1", not "Tamu Node").
- DAS `Now()` baseline priming (no stale-SysTick uptime chunk after debugger reboots).
- DAS storage erase (normal page erase instead of fast erase + no-op 0xFF "restore").

## Test artifacts left on the devices

- SNDB: core as ID 1, DAS as ID 2 (expected).
- Tamu storage: `DYNMEM`, `KEYMEM`, `SYSMEM` backup files (expected).
- DAS storage: `SYSMEM` backup (Meas1/Meas2 writable fields, saved defaults).
- Device names in RAM: "Tamu Node" (core), "DAS v0.1" (DAS) - RAM only, reset on reboot.