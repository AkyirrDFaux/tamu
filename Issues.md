# Issues

Open items only. The 2026-10-04 per-area audit findings (register, storage, device/log,
subscriptions, tamu, das, bootloader, app, tooling, tests) were fixed and committed; the
remaining low-value follow-ups live in `TODO.md`.

Item codes: **A** = doc-fix rulings, **B** = code-fix items (both from the 2026-10-09 fact check); **E** = the remaining open backlog items, numbered for later handling.

## Completed 2026-10-09/10 (detail in git history)

**Services as classes.** Every service is one class with one instance, non-virtual, no `new`, no vtable;
`Init()` explicit. The dispatcher keeps its `switch` - the `(service, handler)` table was prototyped and
**rejected** (+204 B Tamu / +128 B DAS / +92 B Valu, over the DAS budget). Eight per-service forwarders
were removed; `HandleScript`/`HandleScriptResponse` are kept (removing them grows Tamu 910 B).

**Symbol sweep.** `PacketFrame` is `cmd`/`trid` only; `SUB_PRIORITY_*` -> `PRIORITY_SUB_*`; `PAGE_SIZE`
collapsed into `STORAGE_BLOCK_SIZE`; the three `-Wno-unused-function` suppressions removed.

**Naming.** PascalCase for every project identifier, no state prefixes; external-library symbols keep
their spelling; a single-letter name is reserved for a short-lived loop index or scratch counter
(`i`, `j`, `k`, `x`, `y`). The single-letter + lowerCamel remainder is in flight (agent running).

**Bootloader protocol v2.** 3-bit command field (000 identify-req ... 101 read-resp), a write response
carrying the last successfully written offset, and an identify request/response (app-space size in the
offset slot + 32-byte null-padded name). Docs approved and applied (`e824825`, `af4eb79`); codec + all
three bootloaders (`331de55`), core passthrough relay + RSBus length/decode fix (`7afc8aa`), async
passthrough + passthrough CID remap (`0c88b33`), app client + native codec test (`6c66172`), app
passthrough-CID alignment (`a894971`), passthrough-ID test (`37f05e2`), app pre-flash size guard
(`67fc816`). Sizes: DAS_bootloader 1980/2048, Tamu_bootloader 171940/393216, Valu_bootloader 10568/12288;
apps unchanged. Offline gate green; firmware and app KATs agree byte-for-byte.

**Resolved 2026-10-10 - HIL.** The v2 wire, the passthrough and the node bootloader now run on
hardware. The core's own bootloader takes the app image end to end (`test/core_bootloader_flash.py`),
and the passthrough reaches the DAS in update mode for identify, whole-image read and rewrite+verify
(3/3). Measured cost of the half-duplex turn-around: 6-8 ms per passthrough request, so `SendRaw`'s
~700 us silence wait is small against the frame it precedes. Still open: the app's update flow end to
end on Android, which gates TODO's beta criterion.

## HIL suite findings (2026-10-10)

From the first full run of the new HIL suite (`app/test/hil/`, spec `Docs/HIL Tests.md`) on the bench
rig (core #1 + DAS #2): 43 passed, 4 failed, 0 skipped. Each failure reproduces in isolation.

**Resolved 2026-10-10 - B1/B2 (subscription value updates dropped) root-caused and fixed.** The app
addresses a local device by its net-0 shorthand (the SNDB stores local devices with NetID 0), but the
wire carries the qualified address: the node adopts `MakeId(localNet, shortId)`. Measured on the bench:
core own ID `0x8C01`, node own ID `0x8C02`, while the requester entry held `providerAddr = 2`. So the
sender's `IdSrc` (`0x8C02`) never matched the stored provider address and `SubscriptionsControl.h`'s
value-update routing dropped every update; the provider entry then also timed out. Fix: qualify the
address at install time with the existing helper already used for the self-loopback check
(compare `SubscriptionsControl.h:33`) -
`Entry->ProviderAddr = NetQualifyLocal(ProviderAddr, DeviceStatus.NetId)`
(`Core/Services/SubscriptionsControl.h`, CID `0x11`). HIL after the fix: **47 passed, 0 failed,
0 skipped**. Tamu flash +32 B, DAS unchanged.

**Resolved 2026-10-10 - B3 (short-write padding) and B4 (SNDB self-entry) fixed, verified on hardware.**
Padding: `ScriptService::SetEntry` now null-fills the tail (`memset(Data + Vlen, 0, ...)`), matching the
static path and `Docs/Services/Register.md`. SNDB: live self-registration was already gone (commit
`5788df2`); the stale `NetID.1` entry survived in the persistent `.SNREG`, so `SNDB::AddDevice` now
rejects `MakeId(0,1)` and boot purges any stale self-entry. HIL after the fix: 45 passed, 2 failed (the
two subscription items above). Sizes: Tamu flash -100 B, Valu -40 B, DAS unchanged.

**Resolved 2026-10-10 - the old StorageFiles capability bit is obsolete.** Removed from code and docs:
the firmware define (`Core/Types/Enums.h`), the app constant, its `describe()` label, the
`backup_restore.dart` file-support gate, and the `firmware_contract_test.dart` entry. Bit 5 survives
only as a historical comment so it is not reused.

## Doc fact check (2026-10-09) - open items

Every firmware document was checked against `firmware/src`; the closed dispositions
(approved/applied or rejected) are in git history. Only the still-open items remain below.
**Doc fixes need approval before they are applied**; code fixes are work for the beta window.

### A. Doc fixes (cross-reference rerun, 2026-10-10) - all resolved

A61-A67 and A69-A71 were ruled on and applied (A69/A70/A71 also required code changes); A68 is a
TODO, not a doc issue.

### B. Code fixes still open (the document is the specification)

**B1 Router and branch broadcast are documented and unimplemented.** - **scope settled**: after the beta. The rig has no multi-bus device to test a tree topology on. `RSBus and Packets.md:3-7` and
`Data Formats.md:37` (`0x3FE`) describe a router tree; `Dispatcher.h:36-40` says the multi-bus topology
is not implemented and `Docs/Services/Router.md` is a stub.

**B5 System block field 8.1 has no code.** `System Block...md:24` documents `WiFi connection
information` at 8.1; there is no WiFi code in the firmware.

**B6 The UDP framing has no code.** `App Interface.md:25-27` documents a UDP packet; the firmware
implements USB and BLE only.

**B7 The `Extract` opcode has no named define.** `ScriptExec.h:551` branches on
`Op == SCRIPT_OP_COMPOSE` and treats anything else as extract, so extract works but its opcode has no
`SCRIPT_OP_*` name - `ScriptDefs.h:139` defines `SCRIPT_OP_COMPOSE 0` alone. The revised `Script.md`
states Compose 0 / Extract 1 as normative, so the second number wants a define beside the others.

### C. Checked and correct

The command ID table matches every service and the code constants. The Bootloader packet layouts, the
Log struct and database entries, the USB framing and CRC coverage, the App TrID range
`0xF000-0xFFFF`, the Register field sizes and version packing, the subscription record sizes, the script
header and block order, the symbol layout, the Acc & Gyr block, the Buttons/LED blocks, the Fan block,
every device pin assignment, and the DAS/Valu storage geometry all verify against the code.

### D. Unverifiable from code

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
  Higher Scripts-range indices report as unloaded.

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
- **E5** **Script CID 0 lists file IDs, not loaded slots.** CID 1 takes a separate loaded id, so CID 0's
  list is not slot-addressable; the app tracks file->slot and falls back to the file==slot
  convention. Reporting the loaded **slots** (or a slot per entry) would remove the guesswork.
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
  compact drawer shell on a phone form factor.

## Evaluation setup
- **E22** **The LED display has no framebuffer readback**, so visuals are verified by eye only; a render
  snapshot command would make them testable.
- **E23** **DAS provider stale entries** - effectively solved by the 120 s provider lease + orphan-cancel
  path; revisit only if a *confirmed* cancel is wanted.
