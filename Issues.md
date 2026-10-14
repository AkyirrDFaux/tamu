# Issues

Open items only. Resolved findings, the 2026-10-04 per-area audits and the doc-fact-check rulings
that were applied are in the git history.

Item codes: **A** = doc-fix rulings, **B** = code-fix items (both from the 2026-10-09 fact check); **E** = the remaining open backlog items, numbered for later handling.

## A. Doc fixes still open

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

**B12 A failed flash read inside the read handler sends the previous fragment's bytes as content.**
`Storage.h:126` discards `StorageFlashRead`'s result and sends `ContentLen` bytes from the shared
`TxFrame`, so a partial read becomes a wrong result with no error signal.

**B13 The write handler acknowledges fragments it never wrote.** `Storage.h:161-182`: an
out-of-order/lost fragment or one past the file's end advances `WriteSeq` without writing, and the
app advances its offset on any valid reply, so data is silently lost or written at the wrong offset.

**B14 The restore's result contract hides dropped items.** `backup_restore.dart:703` skips any
unready item before `done()`/`failItem()`, so it is counted in neither `written` nor `failed`:
`failed 0` cannot distinguish a dropped entry from a successful one.

**B15 Save All is issued only to devices that carried subscriptions.** `backup_restore.dart:578`
skips a device with no subscription entries, so values restored to a subscription-less device stay
in RAM and are lost at the next power cycle while the restore reports success.

**B16 Helper failures are counted as success.** The provider-side install
(`backup_restore.dart:864`), the dynamic-entry create (`:517`) and the script load
(`:599`) all log a failure and continue; the item is still counted written.

**B17 `.SV`/`.DT_*`/`.DV_*` are captured and restored as raw files as well as semantically.**
`backup_capture.dart:46` excludes only `Script` files and `.SUBREQ`, so a foreign static layout can
be written over a target's persistent space - the opposite of the rule the same function's comment
states ("A file is archived only when it has no semantic form").

**B18 The subscription re-registration chain.** `Main.h:191` re-registers at boot *before the bus
is ever polled*, so those sends are guaranteed lost; the only other trigger is address assignment
(`Device.h:536`), which never fires for a node that returns already addressed; the retry cadence
then collapses from 100 ms to 60 s after 10 s of uptime and `LastValueMs` is never cleared, so a
node that reboots after receiving one value gets no fast window. A network/presence trigger is
missing. Measured 2026-10-14: the stored provider addresses are *not* stale (all four carry net 39,
matching the live NetID 39, the `.SV` NetId byte 39 and both nodes' addresses), so the addresses are
not the cause.

**B19 An unrelated set rewrites `.SUBREQ` from the post-expiry table.** `SubscriptionsControl.h:219`
saves on every set, so an entry dropped by a lease timeout is persisted by the next unrelated set -
defeating the expiry fix - and if the timeout removed the last entry the save deletes the file.

**B20 TRIDs are not range-checked.** `Dispatcher.h:80` routes a TRID >= `TRID_APP_BASE` to the app
link instead of the requester, and `SubscriptionsControl.h:190` accepts any TRID, so a subscription
whose TRID is out of `0x1000-0x1FFF` looks healthy but never writes its target. The project's own
HIL tests use `0xFA00`/`0xFC00` (`hil_tests_partner.dart:166`), which is this hole.

**B21 The DAS file table holds 4 records and never reuses holes.** `StorageDefs.h:35` gives the DAS a
1-page (64 B) table = 4 records; `GetEndOfFiletable` (`StorageBlockFS.h:113`) accepts only 0xFFFFFFFF
so invalidation holes are never reused, and the app's create-temp-then-rename needs 5 records. The
measured "16 B writes, 48 B fails" is therefore a *record* limit, not a size limit.

**B22 Round-trip fidelity gaps in the archive.** Script dropdown choices are not in the semantic
model (`backup_script.dart:18`); provider subscriptions are captured but never restored
(`backup_format.dart:199`); a script whose image fails to parse is kept as a binary and then dropped
by the restore, which skips every `kind == 'Script'` file (`backup_capture.dart:33`,
`backup_restore.dart:441`); and "Include device files" off silently drops all scripts, which are
captured only from the file list.

**B23 The read handler's fragment-count arithmetic overflows for an untrusted file size.**
`Storage.h:114` narrows `(TotalContent + ContentCap - 1) / ContentCap` to uint16 and turns a
wrapped 0 into a single 64-byte fragment, so a corrupt size reads as a complete short file.

**B24 Boot restores dynamic blocks with an O(256 x table-capacity) scan.** `Dispatcher.h:167` probes
all 256 slots and every probe walks all 1024 table records (`StorageBlockFS.h:94`). Not a data-loss
defect, but it is the cost behind the slow boot and it interacts with any retry added for B7/B8.


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
