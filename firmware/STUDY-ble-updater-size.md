# Study: a pairing-free BLE updater in the `factory` image - what it costs (2026-11-06, card t_79a194d5)

Owner's question: *"the tamu bootloader could have a simplified ble interface (with no pairing
etc), that could be used to update the app (if that fits, estimate how much space it would need)."*

This file answers it with a **measured image**, not an estimate from object sizes, and then states
what has to move, who may write, and how a bad image is recovered. Everything below was produced by
the two commands in "Re-run" at the end.

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

`partitions_updater.csv` (already in the tree, used by the probe env, **not** yet the active table):

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

**(b) The hole this study found - and it is not closed yet.** `Main.h` re-arms `otadata` to
`factory` at the top of `app_main`, and the factory image points `otadata` at `ota_0` before it
restarts. So while `ota_0` is the boot partition, the **second-stage bootloader boots the app
directly and the factory image never runs**: an image that dies before the re-arm line is a reboot
loop with no button catch and no updater - reachable only over USB (download mode). The boot budget
cannot help, because the code holding it is not being started.

The fix is ESP-IDF's own pending-verify/rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`): the
factory image marks `ota_0` as pending before restarting it, the app confirms once healthy, and an
image that dies before confirming is rolled back to `factory` by the second-stage bootloader - which
is exactly the "no cable" recovery this card is about. It belongs with the implementation, not with
this measurement, because it changes the app's boot sequence (and the app's "healthy" point has to
be defined).

**What is proven here** - `test/native/updater_boot_test.cpp`, run by `test/native/run.sh`
(`PASS: 49 checks, 0 failures`): the decision table on a simulated device across resets -
no image never boots; a healthy app refunds its budget; an image that looks valid but never gets
healthy is booted 3 times and then parked in update mode *and stays parked*; the button is a free
override; a new write pass restarts the budget; the counter cannot wrap into "bootable"; the window
opens, expires and survives the 49.7-day uptime wrap; a write needs both an open window and a link.

**What is not proven:** the on-device half - flash a deliberately broken image (valid magic, crashes
early), watch the LED come back and the updater advertise - needs a flash and a device reset, and
this card carried no authorization for either. The recipe is the `[env:Tamu_updater_probe]` image,
then an `ota_0` written with a corrupt body whose first byte is still `0xE9`, then a reset with the
button released.

## 6. Re-run

```
cd firmware
export PLATFORMIO_CORE_DIR=/home/akyirr/.platformio
pio run -e Tamu_updater_probe          # .pio/build/Tamu_updater_probe/firmware.bin = 430,544 B
pio run -e Tamu_bootloader             # .pio/build/Tamu_bootloader/firmware.bin  = 172,784 B
./test/native/run.sh                   # includes updaterboot: 49 checks
python3 /home/akyirr/.platformio/packages/framework-espidf/components/partition_table/gen_esp32part.py \
        --flash-size 4MB partitions_updater.csv .pio/probe/partitions.bin
```

`firmware/sdkconfig.Tamu_updater_probe` is `sdkconfig.Tamu_bootloader` with the Bluetooth options
flipped (that file is otherwise byte-for-byte the shipped one, so the compiler and partition settings
are identical and the size comparison is like for like).

## 7. Known gaps / follow-ups

- The probe's write/read core is duplicated from `CoreBootloader.cpp` on purpose (to leave the
  shipped image byte-identical); factor it into a shared header when the updater is really built.
- The transfer is raw bootloader frames over GATT. A phone-side protocol (chunking, completed-offset
  echo, image length) is unbuilt and its cost is inside the 1 MB headroom.
- The rollback mechanism of section 5(b) is designed, not implemented, and the shipped image still
  has the hole.
- The device-side recovery demonstration needs a flash.
