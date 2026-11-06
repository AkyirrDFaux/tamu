# Study: a pairing-free BLE updater in the `factory` image - what it costs (2026-11-06, card t_79a194d5)

Owner's question: *"the tamu bootloader could have a simplified ble interface (with no pairing
etc), that could be used to update the app (if that fits, estimate how much space it would need)."*

This file answers it with a **measured image**, not an estimate from object sizes, and then states
what has to move, who may write, and how a bad image is recovered. Everything below was produced by
the two commands in "Re-run" at the end.

## 0. Update (2026-11-06, card t_3d74f8d1): this study's proposal was superseded, and the updater
   is now the shipped factory image

The number and the mechanisms below still hold; the layout they were measured against does not.
Commit `17e5259` applied a **different** repartition than the one proposed here, and the owner
authorised the promotion:

| | this study (proposed) | `partitions.csv` (applied, **authoritative**) |
| --- | --- | --- |
| `factory` | 0x10000 / 1 MB | 0x10000 / **0x7D000 (500 KiB)** |
| `ota_0` | 0x110000 / 2 MB | **0x90000** / 0x283000 (2.57 MB) |
| `storage` | 0x310000 / 948 KB | **0x313000** / 0xED000 |
| end | 0x3FD000 | 0x400000 |

`partitions.csv` is the only partition table: **`partitions_updater.csv` has been deleted** (its
1 MB `factory` is not what the device has, and two tables that disagree is how the app once landed
at 0x8D000). The measured 430,544 B probe fits the applied 500 KiB `factory` at 84 %, so the 1 MB
proposal was never needed - the probe and the shipped image were merged into
`Bootloader/CoreBootloader.cpp` (one source, no duplicated write/read core; `Tamu_updater_probe`
and its sdkconfig are gone), and `Tamu_bootloader` now builds the BLE updater. Section 5(b)'s
pending-verify/rollback is implemented (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` in
`sdkconfig.Tamu_bootloader` and `sdkconfig.Tamu_v2.0A`), and the on-device demonstration is in
section 8.

## 1. The number

| Image | Size | Partition it lives in | Share |
| --- | --- | --- | --- |
| shipped factory app (`[env:Tamu_bootloader]`) | **172,784 B** | `factory` 0x60000 = 393,216 B | 43.9 % |
| same app + NimBLE host+controller + one GATT service (`[env:Tamu_updater_probe]`) | **430,544 B** | same, if it had to fit today | **109.5 %** |
| ... measured against the proposed 1 MB `factory` | 430,544 B | `factory` 0x100000 = 1,048,576 B | 41.0 % |

**Delta: +257,760 B (+149 %).** So a pairing-free BLE updater does **not** fit the 384 KB factory
partition: it misses by **37,328 B (36.5 KB)** - and that is before anything is added for a phone-side
protocol that is not just the raw frames (a progress/version exchange, an image-length field).
NimBLE host + controller is the bulk of the delta; the updater's own code is small (the probe's
translation unit compiles to 2,680 B of `.text` against the shipped file's 1,151 B).

The 172,784 B baseline did not move: `CoreBootloader.cpp` is untouched by this study, and the probe
duplicates its write/read handlers instead of refactoring them, so the delta above is exactly "the
BLE interface", not "the BLE interface plus a refactor".

## 2. What the measured image contains

`firmware/src/Bootloader/CoreBootloaderBle.cpp`, built by `[env:Tamu_updater_probe]`:

- the USB Serial/JTAG frame path of the shipped image (same raw bootloader frames, same answers);
- NimBLE **host + controller** from ESP-IDF itself (`bt` component, no managed component, no
  `esp-nimble-cpp`), one custom 128-bit service with a write characteristic (frames in, no response)
  and a notify characteristic (responses out), MTU 247, one connection, one CCCD;
- **no security manager, no bonds** (`CONFIG_BT_NIMBLE_SECURITY_ENABLE=n`,
  `CONFIG_BT_NIMBLE_NVS_PERSIST=n`), peripheral + broadcaster + GATT server only;
- the boot budget and the update window described in sections 4 and 5.

It is **not** a finished updater: the transfer still speaks the raw `0xCA ... 0xBC` frames from
`Docs/Services/Bootloader.md` over a GATT characteristic, so a phone has to reassemble and CRC them
the way the app does over USB. A friendlier protocol (a length-prefixed stream, echo of a completed
offset) fits in the 600 KB of headroom the 1 MB partition leaves.

## 3. Where the space has to come from

The table proposed at the time (`partitions_updater.csv`, since deleted - see section 0):

| Name | Offset | Size | vs today |
| --- | --- | --- | --- |
| `nvs` | 0x9000 | 16 KB | unchanged |
| `otadata` | 0xD000 | 8 KB | unchanged |
| `phy_init` | 0xF000 | 4 KB | unchanged |
| `factory` | 0x10000 | **1 MB** | was 384 KB - the updater's home, 2.4x the measured 430,544 B |
| `ota_0` | 0x110000 | **2 MB** | was 2,688 KB - still 3.0x the 698,550 B app |
| `storage` | 0x310000 | 948 KB | size unchanged, and the same offset it has today |

Offsets are what ESP-IDF's own `gen_esp32part.py` derives from that CSV (not hand arithmetic); the
table ends at 0x3FD000, so the whole 4 MB flash is accounted for. `ota_0` at 2 MB rather than the
1.25 MB the card sketched: 1.25 MB would leave 0xC0000 unallocatable (app partitions align to 64 KB
and `storage` comes after), and there is nothing else to spend it on. The alternative - `ota_0`
1.25 MB, 780 KB left unallocated - also fits, if a smaller app slot is preferred.

The shipped table for comparison: `factory` 384 KB at 0x10000, `ota_0` 2,688 KB at 0x70000,
`storage` 948 KB at 0x310000. `storage` keeps its **offset** as well as its size in the proposal,
which is a coincidence of the two tables' arithmetic, not a promise: **repartitioning erases the
device.**

**One-way costs, plainly:**

- Flashing a new partition table erases `ota_0` (the app) and `storage` (the `.SV` files and the
  stored setup). The device comes back as a factory-fresh device and needs a setup upload
  afterwards - that is Akyirr's step, not a worker's.
- The first flash of the new table **must be USB** (esptool/`./upload.sh`): a BLE updater cannot
  write itself into a partition that does not exist yet. Order: new table + factory image over USB,
  then the app over USB once, and from then on the app can be updated over BLE.
- The bootloader in the tree today (`CoreBootloader.cpp`, `OTA_APP_SIZE = 0x2A0000`) still refuses
  writes when the runtime `ota_0` is *larger* than its bitmap. With a 2 MB `ota_0` that check passes
  (2 MB < 2,688 KB), so the current image keeps working across the migration - only the probe's copy
  of the constant (`0x200000`) is exact.

## 4. Who may write, with no pairing

No pairing means no key the device could hold that a peer in radio range could not read off the air,
so a "passphrase in the protocol" over an unencrypted link authenticates nothing - it is a secret
whispered in a room. The design therefore uses the only proof a device without a display has:
**physical presence**.

- The radio only exists in update mode. A device running the app has no updater service on the air at
  all (the factory image is not advertising anything while `ota_0` runs).
- Update mode is entered by B1 held at reset, by an empty `ota_0`, or by a spent boot budget
  (section 5) - never by anything a link can ask for.
- Inside update mode the advertisement lives for a bounded window (300 s, `UpdaterAdmission.h`) and
  then stops, so a device parked in update mode does not keep offering itself to the room.
- A frame is accepted only from the single live connection while that window is open; a second peer
  is disconnected, and a write after the window closed is refused at the attribute
  (`BLE_ATT_ERR_WRITE_NOT_PERMITTED`).

What stops a stranger: they cannot find the device (no service on the air outside update mode), and
inside the window they must be in radio range *while the owner is holding the button*. If they do
catch a window, the blast radius is bounded by construction: `factory` is never written, an update
only ever lands in `ota_0`, and an image that does not prove itself falls back to update mode. The
worst case is another update, not a brick - and the owner can always re-enter update mode with the
button.

The trade, stated: a stranger who is present at the press can flash. Pairing (or a passkey) would
cost the security manager back - a few KB and the whole bond store - and still be defeated by a
device with no display and no out-of-band channel. If that trade is ever unacceptable, the cheap
upgrade is not a passphrase but an encrypted link with a key the phone derives from a code the
*owner* reads off the device - which needs a display.

## 5. Recovery: how the factory image decides to stay in update mode

Two mechanisms, both needed:

**(a) The boot budget (`Bootloader/BootDecision.h`).** Every boot of the main app spends one
attempt from an NVS counter; the app clears it once it is demonstrably alive; at 3 attempts the
factory image stops booting the app and stays in update mode. Staying in update mode spends nothing,
so the device stays parked there across further resets instead of re-arming the broken image. A new
write pass (offset 0) clears the counter, so a repaired image gets the full budget again.

**(b) The hole this study found - closed 2026-11-06 (card t_3d74f8d1).** `Main.h` re-arms `otadata`
to `factory` at the top of `app_main`, and the factory image points `otadata` at `ota_0` before it
restarts. So while `ota_0` is the boot partition, the **second-stage bootloader boots the app
directly and the factory image never runs**: an image that dies before the re-arm line is a reboot
loop with no button catch and no updater - reachable only over USB (download mode). The boot budget
cannot help, because the code holding it is not being started.

The fix is ESP-IDF's own pending-verify/rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`), now
enabled in `sdkconfig.Tamu_bootloader` and `sdkconfig.Tamu_v2.0A` (both, because PlatformIO flashes
the app env's `bootloader.bin` on every `-t upload`, so a disagreement would silently downgrade the
bootloader). With it:

- the factory image selects `ota_0` with state NEW; the second-stage bootloader marks it
  PENDING_VERIFY when it boots it;
- the app's existing otadata erase (the re-arm) is the confirmation - an image that reaches its own
  first statement has proved it can run;
- an image that dies before that erase stays unconfirmed, so the next reset marks the slot ABORTED
  and the bootloader falls back to `factory`;
- the factory image reads the slot's state (`BootDecision.h`'s `ImageRolledBack`) and refuses to
  boot an ABORTED image, so it parks in update mode instead of re-arming the crash;
- a new write pass (offset 0) clears the state along with the boot budget, so a repaired image is
  bootable again.

The budget of (a) still matters for the other half: an image that *does* confirm and then dies later
is caught by the attempts. The app refunds the budget only after 10 s of its main loop
(`Main.h`), which is what makes "healthy" a measurement rather than the first line of `app_main`.

**What is proven** - `test/native/updater_boot_test.cpp`, run by `test/native/run.sh`
(`PASS: 60 checks, 0 failures`): the decision table on a simulated device across resets -
no image never boots; a healthy app refunds its budget; an image that looks valid but never gets
healthy is booted 3 times and then parked in update mode *and stays parked*; a rolled-back slot is
never booted and does not spend an attempt; a new write pass clears the rollback; the button is a
free override; the counter cannot wrap into "bootable"; the window opens, expires and survives the
49.7-day uptime wrap; a write needs both an open window and a link.

**On-device half:** section 8.


## 6. Re-run

```
cd firmware
export PLATFORMIO_CORE_DIR=/home/akyirr/.platformio
pio run -e Tamu_bootloader             # .pio/build/Tamu_bootloader/firmware.bin  = the BLE updater (430,976 B)
pio run -e Tamu_v2_0A                  # the main app for ota_0
pio run -e Tamu_badimage               # the rig fixture that crashes before app_main
./test/native/run.sh                   # includes updaterboot: 60 checks
```

The probe env, its sdkconfig and `partitions_updater.csv` are gone (section 0): the factory image
that carries the updater is `[env:Tamu_bootloader]`, built against `partitions.csv` - the only
partition table - so the size check is the real one (430,304 B of 512,000 B).


## 7. Known gaps / follow-ups

- ~~The probe's write/read core is duplicated~~ - the probe and the shipped image are one source
  now (`Bootloader/CoreBootloader.cpp`), so there is nothing left to factor out.
- The transfer is raw bootloader frames over GATT. A phone-side protocol (chunking, completed-offset
  echo, image length) is still unbuilt; the writes themselves are 30-33 KiB/s measured (section 8).
- ~~The rollback mechanism of section 5(b) is designed, not implemented~~ - implemented and
  exercised on the rig (section 8).
- **The GATT link has not been established from a host yet.** The updater's advertisement is on the
  air and is received (`Tamu updater` at -58 dBm, name from the scan response), but a connect from
  this laptop is aborted locally (`bluetoothctl`: `le-connection-abort-by-local`) and the device
  logs no `BLE_GAP_EVENT_CONNECT` at all, so the CONNECT_IND is never answered. A BLE *write* is
  therefore unproven end to end; every write measured in section 8 went over the USB transport.
  Issues.md F66 carries the evidence and the candidates.

## 8. On the rig: a bad image is recovered, and the repair is a write pass (2026-11-06)

Rig: Tamu v2.0A on `/dev/ttyACM0` (USB Serial/JTAG), flashed by the owner's authorization of
2026-11-06. The partition table read back from 0x8000 is byte-identical to the one this tree builds
(sha256 `e2cef874…`), so `partitions.csv` is what the device has: `factory` 0x10000/0x7D000,
`ota_0` 0x90000/0x283000, `storage` 0x313000/0xED000.

**What was flashed.** `[env:Tamu_bootloader]` 432,176 B at 0x10000 and `[env:Tamu_v2_0A]`
699,536 B at 0x90000, each ending with esptool's `Hash of data verified.`; both regions read back
byte-identical to the built `.bin`. The factory reports 430,304 B of the 512,000 B partition (84 %)
and 16,052 B of RAM.

**The broken image** (`[env:Tamu_badimage]`, `src/Test/BadImage.cpp`, 133,904 B) is a real ESP image
- the loader validates it and jumps to it - whose first `.flash.init_array` entry aborts before
`app_main`, i.e. before the app can erase otadata to confirm itself:

```
.flash.text  4200002e <_GLOBAL__sub_I_app_main>:
4200002e: 1141                addi  sp,sp,-16
42000032: fe384097            auipc ra,0xfe384
42000036: e6e080e7            jalr  -402(ra)      # 40383ea0 <abort>
```

**The recovery, as measured.** The image was written into `ota_0` *through the updater's own USB
frames* (4,185 chunks, 133,904 B, 30.7 KiB/s, last written 0x20b00) - an offset-0 pass, which
clears the boot budget and otadata - and the board was reset with the button released:

- boot 1: `UPDATER: decide: button=0 image=1 rolled_back=0 attempts=0` -> the factory selected the
  slot with state NEW and restarted into it;
- the second-stage bootloader marked it PENDING_VERIFY, the image aborted before its first
  statement, so the reset that followed marked it ABORTED and fell back to `factory`
  (`boot: Defaulting to factory image`);
- boot 2: `UPDATER: decide: button=0 image=1 rolled_back=1 attempts=1` -> **the factory refused the
  image and stayed in update mode**, logged `update mode: LED on, updater on USB and (window 300 s)
  BLE`, and NimBLE logged `GAP procedure initiated: advertise`;
- read back afterwards: `otadata sector 0: ota_seq=0x00000001 state=ABORTED`.

No button, no cable beyond the reset that any power cycle gives: the device parked with the updater
on the air, which is the whole point of the BLE transport.

**The repair, and the trap.** A *raw* esptool write into `ota_0` does not clear that state
(Issues.md F64): after one, the factory logged `rolled_back=1` and refused the freshly written
image. Writing the same app through the updater instead - an offset-0 pass - gave
`decide: rolled_back=0 attempts=0`, `Loaded app from partition at offset 0x90000`, the app's own
`APPBLE: radio up: name='Akyirr'`, and `ota_0` read back byte-identical to the built `.bin`
(sha256 `c6e50e8d…`). 699,536 B took 21,861 chunks at 33.2 KiB/s.

**The budget half, on its own.** Three resets inside a few seconds (the app never gets its 3 s of
health, so nothing is refunded) park the device the same way - `update mode: LED on …`. That is the
path a device with a *valid but dying* image takes, and it is also how the updater was brought up
for the BLE scan without touching the image.

**On the air.** An independent scan (`bleak`, this laptop's adapter) saw
`E4:B0:63:C8:20:72 name='Tamu updater' rssi=-58`. The name is there because it rides the **scan
response**: flags + the complete 128-bit service UUID already fill 21 of the 31 advertisement bytes,
and adding the name made 35, which NimBLE refused - the first build advertised nothing at all
(Issues.md F65). The GATT link itself is the open gap of section 7.

