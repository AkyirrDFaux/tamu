# Real Hardware Verification - Tamu v2.0A + DAS v0.1

Retest session 2026-08-23, firmware updated (storage block size 64 B, `-flto`, core capability
bit, non-blocking LED blink, accumulating time-sync offset). Two devices on the RS-485 RSBus:

- **Tamu v2.0A** (ESP32-C3, USB `/dev/ttyACM0`, console + RSBus) - core, SNDB ID 1, SN `E4B063C820700000000000000000`.
- **DAS v0.1** (CH32V003, WCH-Link `/dev/ttyACM1`, no console) - SNDB ID 2, SN `CDABBD6400000000000000000000`.

The DAS has no console output; it is reached exclusively over the RSBus through the Tamu console
(DAS upload: `pio run -e DAS_v0_1 -t upload --upload-port /dev/ttyACM1`). Re-flashing the DAS
rewrites the whole chip flash, so its storage filesystem is reformatted on every upload.

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
| Device: capability | `dev 1 cap` | PASS (0x00000001, CORE bit set) |
| Device: discover (self) | `dev 1 discover` | PASS (re-registers as ID 1) |
| System Memory: LED-Button (b0) read | `read 1 s 0 0` / `read 1 s 0 1` | PASS |
| System Memory: Fan PWM (b1/b2) read | `read 1 s 1/2 0` / `read 1 s 1/2 1` | PASS (25 kHz, duty 0) |
| System Memory: Acc&Gyr (b3) read | `read 1 s 3 1` / `read 1 s 3 2` | PASS (live accel + gyro) |
| System Memory: LED Display (b4/b5) read | `read 1 s 4 0` / `read 1 s 5 0` | PASS |
| System Memory: fan duty write + callback | `write 1 s 1 1 0x003 0.5` | PASS (fan spins; value persists) |
| System Memory: LED write + callback | `write 1 s 0 0 0x006 1` | PASS (LED toggles) |
| System Memory: backup (save/rmem/recall) | `save/rmem/recall 1 s ...` | PASS |
| Dynamic Memory CRUD | `create/save/rmem/recall/delete 1 d 0x100 dynblock` | PASS |
| Keyed Memory CRUD | `create 1 k 0x101`, `write 1 k 0 0 1 0x003 123`, read/save/rmem/recall/delete | PASS (key persists) |
| Storage: file table | `file 1 table` | PASS |
| Storage: create/read/resize/delete | `file 1 create/read/resize/delete TESTF` | PASS |
| Topology dump | `tree 1` | PASS (System 6, Dynamic 0, Keyed 0) |
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
| Device: uptime | `dev 2 uptime` | PASS (tracks real time after sync) |
| Device: loop time | `dev 2 loop` | PASS (avg 0.06-0.1 ms - non-blocking blink) |
| Device: time sync | `dev 2 time` | PASS (converges to the core time) |
| Device: capability | `dev 2 cap` | PASS (0x00000000 - node has no core bit, expected) |
| System Memory: Meas1 read | `read 2 s 0 0..4` | PASS |
| System Memory: Meas2 read | `read 2 s 1 3` / `read 2 s 1 4` | PASS |
| Measuring (open input) | `read 2 s 0 3` / `read 2 s 0 4` | PASS (~1023 raw, 330 kOhm auto-range) |
| System Memory: write | `write 2 s 0 1 0x003 0.8` | PASS |
| System Memory: backup (save/recall) | `save/recall 2 s -` | PASS (recall restores value) |
| Storage: file create | `file 2 create <name> <size>` | PASS (no longer hangs) |
| Storage: file table after create/save | `file 2 table` | FAIL - only the newest file remains (see notes) |
| Storage: file resize | `file 2 resize <name> <size>` | FAIL ("File resize failed") |
| Storage: file delete | `file 2 delete <name>` | FAIL - reports success but the entry is not removed |
| Topology dump | `tree 2` | FAIL - empty (no summary returned) |

## Not working correctly / notes

1. **DAS file table loses all previous entries on every write.** `file 2 create <name> <size>`
   no longer hangs (the old hang is fixed), but each create - and each `save 2 s -` - leaves the
   table containing *only* the most recently written file. Verified on flash: after creating
   `AAA` then `BBB`, the table page holds a single entry (`BBB` at offset 192); the `.TABLE`
   self-entry and `AAA` are gone, and the pointer page is erased. Consequences:
   - `file 2 table` lists only the last file (no `.TABLE`, no `SYSMEM` until the next save).
   - `file 2 resize` fails for any file that was not the very last one written.
   - `file 2 delete` reports "File deleted" but the entry is either already lost or stays
     visible - the table is not merged/copied correctly.

2. **DAS `file 2 resize` always fails** ("File resize failed"), including for a file that is the
   sole table entry (`resize BBB 40`). The resize path (copy to a new area + commit) does not
   complete on this hardware.

3. **DAS `tree 2` returns an empty topology dump.** The topology request is transmitted (3
   successful transmissions), but no "Registry Summary" is printed. Single-field reads
   (`read 2 s ...`) still work. `tree 1` on the Tamu works fine.

4. **DAS capability is 0x00000000** - expected: the DAS is a plain node (`Capabilities::None`),
   while the Tamu now correctly reports `0x00000001` (CORE bit). Not a bug.

5. **One stray log record** appears in `logs`: `Dev 2 | Src 0x0006 | Code 0x0002` from a
   previous session (keyed-memory service, stale in the core RAM log buffer). Harmless.

## Fixed in this firmware update (verified)

- **DAS storage create no longer hangs** the node (previously required a re-flash to recover).
- **Tamu capability bitfield** now reports `0x00000001` (CORE) instead of `0x00000000`.
- **DAS main loop is fast** (~0.1 ms, non-blocking LED blink) so the bus is serviced every loop.
- **DAS time sync** accumulates the correction and converges to the core time.

## Test artifacts left on the devices

- SNDB: core as ID 1, DAS as ID 2 (expected).
- Tamu storage: `DYNMEM`, `KEYMEM`, `SYSMEM` backup files (expected).
- DAS storage: `SYSMEM` backup (Meas1/Meas2 writable fields; `FilterCoeff` saved at 0.8).
- DAS RAM: `FilterCoeff` = 0.8 (recalled from the backup). Reset on reboot.
- Device names in RAM: "Tamu Node" (core), "DAS v0.1" (DAS) - RAM only.