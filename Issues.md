# Issues

Open items only. Resolved findings, the 2026-10-04 per-area audits and the doc-fact-check rulings
that were applied are in the git history.

Item codes: **A** = doc-fix rulings, **B** = code-fix items (both from the 2026-10-09 fact check); **E** = the remaining open backlog items, numbered for later handling.

## A. Doc fixes still open

**A72 The Duty field is not marked persistent in the doc.** `Docs/Modules and Blocks/Generic System
Blocks.md:8` lists the fan output's Duty as `TR`; the firmware now makes it trigger **and**
persistent, because a fan running at a duty has to come back up at it like the frequency. The Flags
column should read `TR, P` (the block header, `Blocks/PWM.h:5`, carries the change).

**A71 The DAS bootloader's identify reports only [0x800, 0x3E00).** The uploadable area is the
whole app region, storage included: [0x800, 0x4000), 14336 B. The app's binary carries no
`.fixed_data` storage reservation. The bootloader doc's size statement needs the same correction.

**A70 A file with a semantic form is not archived as a binary.** The backup now stores a `SCR_XXX`
script only in its semantic form; the file is rebuilt on restore. The archive-format docs do not
state that rule.

**A69 The backup's semantic block address carries the global index.** The semantic `blockInfo`
value stores `instance` as the GLOBAL index - the decoder rebuilds the bank type from it - but no
doc says so. One line is missing: a global index >= 64 is what keeps a banked block in its own bank
on restore, where the wire's 6-bit instance would send it to bank 0.

**A68 Windows USB.** `App/General Info.md:7` lists Windows as "BLE, USB"; `supportsUsb` is Linux-only
(`platform_caps.dart`). Proposed row: `| Windows | BLE (USB not implemented yet) |`. Windows is dropped
for this window, so this is doc accuracy only.

## B. Code fixes still open (the document is the specification)

The 2026-10-14 audit of the storage system, the subscriptions service and the backup/restore loop
(carried out read-only; the entries below are that audit's findings, the systemic rule stated once:
**a failed or partial read must never delete, overwrite, or produce a wrong result**).

**B11 A reply larger than the ring still cannot complete over BLE.** `Send` now drains the ring
during a stream through `TxFlush`, but that flush is USB-only: the BLE link paces itself in
`AppBLETick` through `TxPeek`/`TxCommit` (a failed notify must not lose data), so a BLE session has
no way to drain mid-stream and a whole-file read above ~5952 B still loses its tail fragments.
Measured 2026-10-14 over USB: 20000 B round-trips intact and the core's `.TABLE` (16384 B) now
enumerates 41 files including every `SCR_*`. Reaching the same over BLE needs the BLE tick to run
during a stream, or a bounded reply size the app honours.

**B12 A failed flash read inside the read handler sends the previous fragment's bytes as content.** *(Fixed in `1ba7a02`: a short read fails the read instead of sending the reused frame's bytes.)*
`Storage.h:126` discards `StorageFlashRead`'s result and sends `ContentLen` bytes from the shared
`TxFrame`, so a partial read becomes a wrong result with no error signal.

**B13 The write handler acknowledges fragments it never wrote.** *(Fixed in `2008fd2`: every rejection - an unopened stream, a name change mid-stream, a missing target, an out-of-order fragment, a fragment past the end, a failed flash write - now replies `FLAG_FAIL` with a log, and the ack is only sent for a fragment that was actually written.)* `Storage.h:161-182`: an
out-of-order/lost fragment or one past the file's end advances `WriteSeq` without writing, and the
app advances its offset on any valid reply, so data is silently lost or written at the wrong offset.

**B14 The restore's result contract hides dropped items.** *(Fixed in `7866b63`: every planned item now accounts for itself.)* `backup_restore.dart:703` skips any
unready item before `done()`/`failItem()`, so it is counted in neither `written` nor `failed`:
`failed 0` cannot distinguish a dropped entry from a successful one.

**B15 Save All is issued only to devices that carried subscriptions.** *(Fixed in `7866b63`: Save All goes to every device the restore wrote.)* `backup_restore.dart:578`
skips a device with no subscription entries, so values restored to a subscription-less device stay
in RAM and are lost at the next power cycle while the restore reports success.

**B16 Helper failures are counted as success.** *(Fixed in `7866b63`/`bfa7a3e`: helper failures land in the failure count, and the provider half is confirmed by a read-back.)* The provider-side install
(`backup_restore.dart:864`), the dynamic-entry create (`:517`) and the script load
(`:599`) all log a failure and continue; the item is still counted written.

**B17 `.SV`/`.DT_*`/`.DV_*` are captured and restored as raw files as well as semantically.** *(Fixed in `1362c43`: `isDevicePersistence()` excludes them from the file capture, so they travel semantically only.)*
`backup_capture.dart:46` excludes only `Script` files and `.SUBREQ`, so a foreign static layout can
be written over a target's persistent space - the opposite of the rule the same function's comment
states ("A file is archived only when it has no semantic form").

**B18 The subscription re-registration chain.** *(Boot-order half fixed in `8bf61c1` - the re-registration runs after the 500 ms bus window; presence half in `ReRegisterForNode` (LastValueMs cleared, the fast window keyed to unserved), fixed in `6cd1f1a`.)* `Main.h:191` re-registers at boot *before the bus
is ever polled*, so those sends are guaranteed lost; the only other trigger is address assignment
(`Device.h:536`), which never fires for a node that returns already addressed; the retry cadence
then collapses from 100 ms to 60 s after 10 s of uptime and `LastValueMs` is never cleared, so a
node that reboots after receiving one value gets no fast window. A network/presence trigger is
missing. Measured 2026-10-14: the stored provider addresses are *not* stale (all four carry net 39,
matching the live NetID 39, the `.SV` NetId byte 39 and both nodes' addresses), so the addresses are
not the cause.

**B19 An unrelated set rewrites `.SUBREQ` from the post-expiry table.** *(Fixed in `6cd1f1a`: the set saves with KeepFileEntries, so an expired entry survives and an empty table does not delete the file.)* `SubscriptionsControl.h:219`
saves on every set, so an entry dropped by a lease timeout is persisted by the next unrelated set -
defeating the expiry fix - and if the timeout removed the last entry the save deletes the file.

**B20 TRIDs are not range-checked.** *(Fixed in `6cd1f1a`: a requester TRID outside the subscription range is refused.)* `Dispatcher.h:80` routes a TRID >= `TRID_APP_BASE` to the app
link instead of the requester, and `SubscriptionsControl.h:190` accepts any TRID, so a subscription
whose TRID is out of `0x1000-0x1FFF` looks healthy but never writes its target. The project's own
HIL tests use `0xFA00`/`0xFC00` (`hil_tests_partner.dart:166`), which is this hole.

**B21 The DAS file table holds 4 records and never reuses holes.** *(Resolved 2026-10-14 in `77e3fa8`.
Hole reuse turns out to be impossible on this medium, so the "never reuses holes" half is a
consequence of flash, not a defect: flash only clears bits and neither backend can program over a
record's bytes without erasing its page - on the DAS the erase page IS the whole table, which an
attempt that reused 0x00 slots measured on the board (every create failed, nothing written; the
read-back stopped it corrupting anything). The fix is capacity: the DAS table is now 2 pages, 8
records, and a device picks the size up at its next format or table move.)* `StorageDefs.h:35` gives the DAS a
1-page (64 B) table = 4 records; `GetEndOfFiletable` (`StorageBlockFS.h:113`) accepts only 0xFFFFFFFF
so invalidation holes are never reused, and the app's create-temp-then-rename needs 5 records. The
measured "16 B writes, 48 B fails" is therefore a *record* limit, not a size limit.

**B22 Round-trip fidelity gaps in the archive.** *(Confirmed 2026-10-14: the dropdown option lists
are in the file codec but not in the semantic model - `BackupInputSpec` carries only
`style/min/max/step` (`backup_script.dart:18-44`), so `fromDraft` drops `options` (`:441-448`) and
`toDraft` rebuilds it empty (`:480-487`). After a restore the input is still a `dropdown` with no
labels, and `script_widgets.dart:410-424` renders nothing for that type. Fix: carry `options`
through the semantic model like the other fields, then regenerate the archive's script blobs from
it - a codec change plus a tamu-setup migration, since the current semantic form has no field to
hold them.)* Script dropdown choices are not in the semantic
model (`backup_script.dart:18`); provider subscriptions are captured but never restored
(`backup_format.dart:199`); a script whose image fails to parse is kept as a binary and then dropped
by the restore, which skips every `kind == 'Script'` file (`backup_capture.dart:33`,
`backup_restore.dart:441`); and "Include device files" off silently drops all scripts, which are
captured only from the file list.

**Resolution (2026-10-14, `1ddb081`):** the codec now carries `options` in both directions and omits
the key when empty, so every capture from here on keeps a dropdown's choices; a semantic round trip
is covered by a test and the host suite is unchanged. The existing archive could not be repaired
from its own content: `SCR_004`'s image (4676 B) holds the script, its variables and its names but
no choice labels, so slot 4's options were already gone when the archive was captured and have to
be re-authored in `1_Tamu_v2.0A.json`'s `inputs[].ui.options` (the images are a cache;
'rebuild.dart' regenerates them). The other three parts of this entry - provider subscriptions
captured but never restored, a script whose image fails to parse being dropped by the restore, and
"Include device files" off dropping all scripts - are untouched and still open.

**Second resolution (2026-10-14, `9a18b6c`), all three parts:** provider-side subscriptions are now
planned and installed via CID 0x21 (ProviderSet), which takes the requester address from the payload
and the TRID from the frame, unlike the 0x01 Change path that cannot name a remote requester;
`restoresAsRawFile()` writes a Script file raw when no semantic script exists for its slot, so an
unparsable image is no longer dropped; and "Include device files" off now governs only raw device
files, never scripts. Host suite 230/1 (baseline 227/1). On the rig the plan grew by the four
DAS provider entries - the fix is live - but each reported `Unknown subscription id`: the archive
predates the recorded `trid`, so nothing can key an install. Same shape as the emote labels: that
data exists only on the device, so the remedy is a fresh capture from the rig, after which the
provider items should install instead of failing.

**B23 The read handler's fragment-count arithmetic overflows for an untrusted file size.** *(Fixed in `1ba7a02`: the count is computed without the overflow and an inexpressible size fails the read.)*
`Storage.h:114` narrows `(TotalContent + ContentCap - 1) / ContentCap` to uint16 and turns a
wrapped 0 into a single 64-byte fragment, so a corrupt size reads as a complete short file.

**B24 Boot restores dynamic blocks with an O(256 x table-capacity) scan.** *(Fixed in `c728acf` (the recall enumerates the DT/DV files) and `3b12ed7` (the table is staged: 4 flash calls per scan, not 1024); the block measured 8.907 s -> 0.067 s.)* `Dispatcher.h:167` probes
all 256 slots and every probe walks all 1024 table records (`StorageBlockFS.h:94`). Not a data-loss
defect, but it is the cost behind the slow boot and it interacts with any retry added for B7/B8.


**B26 The provider-restore path keys an entry to the archive's literal requester address.** *(Open - the archive was corrected to the measured addresses so it works today, but the code still trusts it.)* `backup_restore.dart:1005` resolves `s.requester` with `idFromString()` without remapping it through the source->live device match the way `source` is remapped on the next line. Restoring an archive whose addresses predate the rig's NetID therefore keys the provider entry to an address no device has - the archive said `0.1` while the rig runs `39.1`, and the archive's own device files carry a third value (51), so it cannot be remapped from the address alone. Fix: take the requester device from the matching requester-side subscription (same target device + same source ref), whose owner the plan already knows.

**B27 Script-file length validation wraps in uint32.** *(Fixed in `474489e`: the blob-size sum and every offset derived from it are now 64-bit, so a wrapped total can no longer pass the bounds check.)* `ScriptProgram.h:209-217` summed four header-supplied uint32 lengths, so a crafted `SCR_XXX` header passed `Need > Size` with the running offsets ~4 GB past the buffer.

**B28 ContainerElement's vector index overflows and bypasses the bounds check.** *(Fixed in `474489e`: the magnitude is tested against `Size / 4` before any arithmetic.)* `ScriptExec.h:538-539` did `(uint32_t)(Index + 1) * 4 > Size` on a signed operand, so `Index+1` overflowed and the x4 product wrapped for any index >= 2^30, reaching the pointer arithmetic below.

**B29 The subscription-table stream drops its tail entries.** *(Fixed in `474489e`: the fragment count is sized from the per-fragment packing.)* `SubscriptionsControl.h:54-56` computed `ceil(Total/Capacity)` while the emitting loop packs whole entries - 3 per fragment for 32-byte provider entries - so the last `3*Frags` entries were sent behind a FLAG_STOP. The requester side was immune only because 28 divides 112.

**B30 A write fragment over 64 bytes is silently clipped and still acknowledged.** *(Fixed in `474489e`: refused like the other invalid fragments.)* `Storage.h:189` - the one clip-instead-of-refuse branch left after B13.

**B31 A table move never adopts the current table geometry.** *(Fixed in `474489e`.)* `StorageBlockFS.h:277` took `NewSize` from `FileTableSize`, so a device formatted by the older single-page build kept a 64-byte table through every move and never reached the capacity B21 promised.

**Sweep report (2026-10-14) - reported by a five-way read-only firmware sweep, NOT yet confirmed line-by-line.** The five above were checked against the source and fixed; the rest are recorded as reported, each needing confirmation at the cited line before it is acted on. Spot-checking five of the sweep's high-severity citations found all five exact, which is why the remainder are worth taking seriously.

Core/wire: the DAS and Valu bootloader bounds checks add `(APP_BASE+Offset)+PAYLOAD_SIZE` in uint32 and can wrap past `APP_LIMIT` (`DAS_v0.1/Bootloader.cpp:302,277`, `Valu_v2.0/Bootloader.cpp:407,503`) - the Core bootloader already uses the wrap-safe subtraction (`CoreBootloader.cpp:91,217`); medium. Subscriptions Save replies SUCCESS when the persistence write failed (`SubscriptionsControl.h:250-253`, `SubscriptionsPersist.h:87`); medium.

Storage: a failed read of `.SUBREQ` during a carry-over save is treated as "absent" and the file is overwritten, dropping lease-expired entries the B19 save exists to protect (`SubscriptionsPersist.h:40`); medium. A failed static recall is reported as Success (`RegisterPersist.h:29,203-206`); low. `PreloadVysiLayout` ignores its write result, leaving a present-but-invalid `LAY_1` that the FileExists guard then skips forever (`Blocks/Vysi1Layout.h:51`); low.

Devices: **the parked Valu's likely root cause** - JumpToApp masks interrupts but leaves the USBD interrupt enabled in the PFIC and the controller attached, and the app's startup sets mstatus.MIE before installing its own trap vector, so TinyUSB runs against state the CRT just zeroed (`Valu_v2.0/Bootloader.cpp:527-537`, `startup_ch32v20x_D6.S:253-260`); high, medium confidence, would be settled by adding the PFIC/USBD teardown to JumpToApp. Valu fan never comes up on its persisted Duty (`Valu_v2.0/PWM.h:35-58`); medium. Valu `OnPWMFrequencyChange` stores the raw request while programming a clamped frequency (`Valu_v2.0/PWM.h:61-71`); low. Tamu `SetupFanPWM` discards every LEDC error return (`Tamu_v2.0A/PWM.h:39-82`); low. Tamu fan frequency falls back to 25 kHz but leaves the field at 0 (`Tamu_v2.0A/PWM.h:21-24`); low. DAS/Valu Sensor Type is persisted unvalidated and the sample loop's default publishes raw counts as the requested unit (`DAS_v0.1/Measuring.h:47`, `MeasuringRun.h:71-110`); low. `app_main` ignores the factory-slot re-arm result (`Tamu_v2.0A/Main.h:327-329`); low.

Blocks/script: Reg-write-foreign silently truncates a value to 64 bytes (`ScriptExec.h:473-478`); medium. Expression recursion is unbounded by SCRIPT_EXPR_MAX_DEPTH for prefix chains and right-associative `^` (`ScriptExpr.h:268-338`); medium.

Docs (A-side, for the doc pass): `Docs/App/Update.md:5-6` still says the node gives no write acknowledgement, but both bootloaders acknowledge every write and the core relay returns it - contradicting `Docs/Services/Bootloader.md:7` and the app; medium. `Docs/App/Device View.md:14` lists a "Files" capability label the app never emits; low. `Docs/Devices.md:3` tells authors to use `TYPE_NODE`, which no code references (nodes are the default, the core define is the switch); low. `RegisterEnumerate.h:156-160` carries a stale "Doc gap" comment - `Docs/Services/Register.md:60` does document the 8.8 dynamic word; low.

**B1 Router and branch broadcast are documented and unimplemented.** - **scope settled**: after the beta. The rig has no multi-bus device to test a tree topology on. `RSBus and Packets.md:3-7` and
`Data Formats.md:37` (`0x3FE`) describe a router tree; `Dispatcher.h:36-40` says the multi-bus topology
is not implemented and `Docs/Services/Router.md` is a stub.

**B5 System block field 8.1 has no code.** `System Block...md:24` documents `WiFi connection
information` at 8.1; there is no WiFi code in the firmware.

**B6 The UDP framing has no code.** `App Interface.md:25-27` documents a UDP packet; the firmware
implements USB and BLE only.

## D. Unverifiable from code

`RSBus and Packets.md:1` supply voltage and USB PD (hardware), `Data Formats.md:7` "14 byte UUID"
(14 bytes confirmed, UUID loose), `Data Formats.md:53` the generic dictionary marker encoding,
`Register.md:34` the Internal/External Access columns, `Register.md:48` "sorting by the whole uint32",
`Storage.md:86-96` the dynamic block descriptor ordering (runtime only), and the Valu feature rows that
are hardware-only.

## Open (2026-10-06)

- **E25** **The Valu bootloader can only ever be replaced over the ROM bootloader.** Our own bootloader
  refuses writes below `APP_BASE` by design, and this board has no SWD header, so every bootloader
  iteration needs an ISP session (`wchisp-nightly`, one command per USB reset). Worth keeping in
  mind when deciding how much logic belongs in the bootloader rather than the app.
- **E26** **`valu_upload.py` drives `wlink` over SWD, which this board cannot do.** The ISP recipe that
  actually works (USB port reset, then a single `wchisp-nightly` invocation) needs folding into it
  as an `--isp` mode so `./upload.sh boot valu` matches reality.

## Code decisions needed (2026-10-04 docs-conformance sweep)
- **E24** **Register write of a longer String/Filename.** `Register.md` says a longer value fails (only a
  shorter one is allowed, space-padded); `MemoryTypes.h` clamps it instead. The app and the "System
  Name clamps to 16 bytes" HIL rely on the clamp. Parked (minor detail).

## Valu v2.0 app (2026-10-06)
- **E27** **Resistive measurement reference resistor is undefined.** `Docs/Devices.md` gives three ADC
  inputs (PA6/PA1/PA0) but says "Reference resistor not defined." With no reference and no range
  selectors, the resistance/LDR/NTC transforms in `Devices/Valu_v2.0/MeasuringRun.h` use a
  placeholder `VALU_MEAS_REF_KOHM` (10 kOhm); raw and voltage measurements do not depend on it.
  Needs a hardware value before a resistance-reading module is meaningful.
- **E28** **"USB Bootloader" and "Script" services have no capability-bit mapping.** `Docs/Devices.md`
  lists the Valu's services as Mandatory + USB Bootloader + App interface + Dynamic memory +
  Script. `Capabilities::` (Core/Types/Enums.h) has bits for Node/AppInterface/DynamicMemory/
  Scripts/StorageFeatures/Subscriptions but **none for a bootloader** - the code (and the
  bootloader itself) already treats that as "no capability bit" (`TODO.md` Decisions). The Valu
  app advertises `Node | StorageFiles | AppInterface | DynamicMemory | Scripts`.
- **Valu LED displays are registered; OLED still TODO.** `Docs/Devices.md` lists "LED Display x2"
  and "OLED Display (TODO)" for the Valu. The two LED-display instances are now registered in
  `Devices/Valu_v2.0/Main.h` (the generic Vysi1 block; the strip geometry comes from the layout
  file at runtime). The OLED module is still not built.
- **E29** **`MAX_SCRIPTS` is reduced to 16 on the Valu.** The Script service's loaded-script registry is
  `LoadedScript scriptRegistry[256]` (~38 KB) in `ScriptDefs.h`; the CH32V203's 20 KB cannot hold
  it, so the registry is now a `#ifndef`-guarded build knob and the Valu sets `-D MAX_SCRIPTS=16`.
  Higher Scripts-range indices report as unloaded. **Update 2026-10-11:** the mask word count is now
  `(MAX_SCRIPTS + 63) / 64`, so the Valu's 16 slots get one mask word (it was 0 - an out-of-bounds write
  that also left the script tick path uncompiled); restoring that path costs the Valu ~9 KB flash
  (32 788 -> 42 176, 93.6%), parked.

## Docs decisions needed (parked unless noted)
- **E1** **OS notifications.** `Docs/App/Settings.md` lists "Allow notifications (To OS)" with per-event
  selection; the app persists `notifyOs`/`osEvents`/`suppressOsWhenOpen` but only delivers in-app
  notifications (`app/lib/core/notifications.dart`). Implement OS delivery or mark the setting
  pending.
- **E4** **Cross-script macro calls.** `Script.md` names "Macro call" but gives no opcode, boundary rule
  or argument passing. The VM runs **one script per tick** and every wait state lives on the
  callee, so it needs a call stack of `(script, line)` pairs and a tick loop that resumes whichever
  script is waiting. Proposal to confirm: a `Call script` flow op with operands
  `(loaded script id, entry line)`, blocking by construction, values exchanged through registers.
- **E6** **Dynamic descriptor doc omits `Name`/`generation`/`present`.** The `DynamicBlockDescriptor`
  (`MemoryBlocks.h:35-49`) has `char Name[`BLOCK_NAME_LEN`]`, `uint32_t generation` and `bool present`
  beyond the documented pointers and sizes (`Register.md:88-98`). **Under review** (owner wants a
  closer look).

### Docs-conformance sweep wording (2026-10-04)
- **E12** **Storage.md flash API names.** The doc lists `Init/Read/Write/Erase/Format` (`Storage.md:40-48`);
  the code is `Storage_FlashInit/…Read/…Write/…Erase/…Format` with `(offset, data, size)` order.
  **Deferred**: no rename now — the services-as-classes refactor turns these free functions into
  Storage methods, which supersedes this wording.

## Storage / DAS
- **E19** **`.SUBREQ` on a provider-only node** was reported once; the empty-table file is deleted now.
  Re-check if it reappears.

## Android (on-device behaviour untested)
- **E20** No Android device/emulator: BLE runtime permission prompt + denied/permanently-denied paths, BLE
  scan/connect/MTU, Storage Access Framework backup save + restore and file download, and the
  compact drawer shell on a phone form factor. The app's **update flow end to end** is the other
  Android item, and it gates the beta criteria in `TODO.md`.

## Evaluation setup
- **E22** **The LED display has no framebuffer readback**, so visuals are verified by eye only; a render
  snapshot command would make them testable.
- **E23** **DAS provider stale entries** - effectively solved by the 120 s provider lease + orphan-cancel
  path; revisit only if a *confirmed* cancel is wanted.