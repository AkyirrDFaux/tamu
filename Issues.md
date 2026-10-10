# Issues

Open items only. The 2026-10-04 per-area audit findings (register, storage, device/log,
subscriptions, tamu, das, bootloader, app, tooling, tests) were fixed and committed; the
remaining low-value follow-ups live in `TODO.md`.

Item codes: **A** = doc-fix rulings, **B** = code-fix items (both from the 2026-10-09 fact check); **E** = the remaining open backlog items, numbered for later handling.

## Planned change: services as classes (2026-10-09, owner request)

**Goal.** One service = one class with one instance, non-virtual methods, no `new`, no vtables. The
CH32V003 has 2 KB of RAM and ~14 KB of flash and shares these sources with the ESP32-C3, so the shape has to
cost nothing. The wire protocol, the block layouts and the CID map do not move.

**Why.** The services are free functions over file-static state today - `HandleRegister`,
`HandleEnumerateBlocks`, `HandleCreateDynamic`, `HandleSetName`, `HandleLogHandler`, `HandleDeviceService`,
`HandleSNDB`, `HandleBootloaderPassthrough` - with 19 file-statics in the subscriptions provider alone and 6
in the script runtime. Nothing owns that state, two services cannot both have an `Init`, and the dispatcher is
a switch rather than a table. Two shapes already exist beside it: `CoreTimeSyncService` is a class with a
`Tick()`, and `StorageBlockFS` is a struct behind a global `Storage`. The tree is inconsistent; this picks one.

**Convention.**

```cpp
// Core/Services/Register.h
class RegisterService
{
public:
    void Init();                              // explicit, called from the device's Main
    bool Handle(const PacketFrame &frame);    // this service's CID dispatch
private:
    // the file-static state, moved in as members
};
extern RegisterService Register;              // one instance, defined in the service's header
```

- Non-virtual methods only, no inheritance.
- No constructor that needs runtime work: `Init()` is called explicitly from the device's `Main()`, so
  static-initialisation order never matters.
- One instance per binary, in `Core/Services/`, not per device. A device enables a service by which `Handle`
  the dispatcher calls (today's `Capabilities` gate), not by allocating one.
- The file splits stay: the class is declared in the service's top header and its methods are defined in the
  sibling files. Register's seven files and Subscriptions' six stay as they are; nothing is merged for tidiness.
- The `Cid` enums stay put; the app mirrors them.
- `Handle` returns whether it consumed the packet, so the dispatch chain becomes explicit.

**Benefits.** State ownership and a real `Init`/reset (testable), no name collisions between services, a small
`(service, handler)` dispatcher table, and a name for the documents' "Implementation Functions" sections.

**Cost, measured not guessed.** Method names live in debug info only, and the state moves from file-static to
member without changing size or order, so both images should stay within a handful of bytes. The real cost is
the diff across ~24 files, and the risk is a missed `static` acquiring a different lifetime - which is what the
native tests and both builds are for.

**Order** (each step builds and passes before the next):
1. `Register` - the biggest, and the one that proves the convention fits a split service.
2. `Storage` - already a struct with an object; mostly renaming and moving `Init` in. This also
   absorbs the storage flash API refactor (Issues A12): the `Storage_Flash*` free functions become the
   service's methods. The same class treatment extends to any other service still written as free
   functions rather than a class (the point of this plan is one shape for every service).
3. `Subscriptions` - 19 file-statics and three files that talk to each other.
4. `Script` - 6 statics plus the VM and a loaded-program table: the riskiest.
5. `LogHandler`, `Device`/SNDB and the bootloader passthrough - small; the dispatcher table lands here.
6. `AppInterface` last. The `Core/Functions/` helpers (`Packet`, `Dispatcher`, `Crc8`, `TimeSync`) stay free
   functions: only the *services* become classes, and `TimeSync` is already one and is the shape to copy.

**Must not change:** the wire protocol, the CID map, the register/block layouts, the `.pio` envs, the
per-device composition, or anything under `Docs/`. No document change is expected.

**Verification per step:** `./test.sh` (native firmware tests + the app suite + analyze), both `pio run` envs,
and the safe HIL sweep once the rig is flashed.

**Sequencing:** after the in-flight wave (six units are editing these files now) and after B10.

## Doc fact check (2026-10-09) - open items

Every firmware document was checked against `firmware/src`; the closed dispositions
(approved/applied or rejected) are in git history. Only the still-open items remain below.
**Doc fixes need approval before they are applied**; code fixes are work for the beta window.

### A. Doc fixes open (cross-reference rerun, 2026-10-10)

The 2026-10-10 rerun confirmed the earlier sweep; these divergences are **doc-only** (the code
matches the spec). **A61-A63 were ruled on and applied** (see git); the rest await a ruling.

**A64 Record type name consistent everywhere.** `Storage.md:61` reads `WriteFilerecord(Filerecord
NewRecord)` and the file-table diagram (`:23`) labels "Filerecord 0..4"; the code type is `FileEntry`
(`StorageDefs.h:58`) while the functions are `WriteFilerecord`/`DeleteFilerecord`
(`StorageBlockFS.h:125,165`). Ruling: make the term consistent - **file record or file entry**
everywhere. **Awaiting the term choice.**

**A65 `Bitmap` sits after the effect list.** `LED Display.md:77` lists `Bitmap (reserved)` in the
texture list, before "The effect list continues the texture enum" (`:79`); the enum value is 8, after
the effects 4-7 (`Render.h:74-86`). Proposed: move the bullet after `- Brightness change`.

**A66 Field name is `Render Block Index`.** `LED Display.md:9` says `Render KeyedBlock Index`; the code
and app say "Render Block Index" (`Vysi1Layout.h:93`, `block_registry.dart:148`). Proposed: drop
"Keyed".

**A67 Boot-indicator LED wording (absorbs E8).** `Bootloader.md:3` says the bootloader "lights the
white LED permanently"; only the DAS has a white LED and the running app uses it as the RS485 TX
indicator, the Valu uses its red LED, and the core drives no LED. `Devices.md:43` calls the DAS white
LED "the bootloader indicator". Proposed: state the per-device LED (DAS white / Valu red / core none)
and describe the DAS white LED as the RS485 TX/communication indicator.

**A68 Windows USB.** `App/General Info.md:7` lists Windows as "BLE, USB"; `supportsUsb` is Linux-only
(`platform_caps.dart`, whose header says Windows = not yet). Proposed:
`| Windows | BLE (USB not implemented yet) |`.

**A69 Requester/Provider capability note.** `App/Service Views/Subscriptions.md:14` claims "the
capability bits do not yet separate Request/Provide"; they do (`types.dart:386-388`, `Enums.h:17,19`).
The gate itself is real (`subscriptions_page.dart` tests `Capability.core`). Proposed: "…gated on the
Core capability - only cores currently act as requesters, though the advertised SubReq/SubProv bits
distinguish the roles."

**A70 Autoconnect toggle label.** `App/Settings.md:3` says the toggle's subtitle shows the target; the
toggle changes its *title* to "Autoconnect: <target>" and the hint lives on the next row
(`settings_page.dart:29-53`). Proposed wording fix.

**A71 `.DV_` is shown as hex.** `App/Service Views/Storage.md:16` lists `.DT_`/`.DV_` as the
human-readable dynamic files; `.DV_` is a raw hex dump (`file_viewers.dart:299-301`). Proposed: note
the `.DV_` value space is shown as hex.

### B. Code fixes still open (the document is the specification)

**B1 Router and branch broadcast are documented and unimplemented.** - **scope settled**: after the beta. The rig has no multi-bus device to test a tree topology on. `RSBus and Packets.md:3-7` and
`Data Formats.md:37` (`0x3FE`) describe a router tree; `Dispatcher.h:36-40` says the multi-bus topology
is not implemented and `Docs/Services/Router.md` is a stub.

**B5 System block field 8.1 has no code.** `System Block...md:24` documents `WiFi connection
information` at 8.1; there is no WiFi code in the firmware.

**B6 The UDP framing has no code.** `App Interface.md:25-27` documents a UDP packet; the firmware
implements USB and BLE only.

**B40 The app's `DataType` enum omits `UnknownKeyed` (0x100).** `Data Formats.md:23` documents
`UnknownKeyed` and the firmware defines it (`Enums.h:60`), but `types.dart` jumps from `name (0x12)`
to `geometry (0x101)`. Add `unknownKeyed(0x100)` so the enum mirrors the firmware. (E10's old claim
that these types are undocumented is now wrong - they are in `Data Formats.md`.)

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
- **E2** **Script UI info format.** `Script.md` describes the UI info as names + per-input limits/UI type;
  the app writes **version 2** with a per-input label list (the custom-enum case). v1 is no longer
  supported, so an old backup's script names fall back to the file name until re-saved - document
  the format.
- **E3** **Pre-rotated Position convention.** The renderer samples the geometry/texture masks *forward*,
  so a shape's centre lands at `-L^-1 * t`; both writers (`ScriptExecTransform` and the app's
  `Transform23`) store `t' = L * t` to keep the centre at `-t` under rotation. `LED display.md`
  describes Position as a plain 2x3 matrix - document the convention.
- **E4** **Cross-script macro calls.** `Script.md` names "Macro call" but gives no opcode, boundary rule
  or argument passing. The VM runs **one script per tick** and every wait state lives on the
  callee, so it needs a call stack of `(script, line)` pairs and a tick loop that resumes whichever
  script is waiting. Proposal to confirm: a `Call script` flow op with operands
  `(loaded script id, entry line)`, blocking by construction, values exchanged through registers.
- **E5** **Script CID 0 lists file IDs, not loaded slots.** CID 1 takes a separate loaded id, so CID 0's
  list is not slot-addressable; the app tracks file->slot and falls back to the file==slot
  convention. Reporting the loaded **slots** (or a slot per entry) would remove the guesswork.
- **E6** **Dynamic descriptor doc omits `Name`/`generation`/`present`** (deferred).
- **E8** **Bootloader LED entry.** `Bootloader.md` (white LED) vs `Devices.md` (core white LED "missing
  hardware") vs the app (red LED). Code drives no LED in the core bootloader. *(Concrete doc wording
  proposed as A67 below.)*
- **E9** **Device-type numbering is undocumented.** `Docs/Data Formats.md` and `System Block and Device
  Commands.md` name the System `Device Type` field but give no numeric table. `Enums.h` /
  `types.dart` assign `Tamu_v2_0A = 0x01`, `Valu_v2_0 = 0x02`, `DualAnalogSensor = 0x03` as a code
  convention (0x02 carried over from the pre-restructure tree) - confirm or document it.

### Docs-conformance sweep wording (2026-10-04)
- **E10** **Documented but not implemented** (future/planned, `Plan.md`): WiFi `App Active` values + System
  field 8.1 SSID/Password; Router capability + service (stub only); branch-broadcast address
  `0x3FE`; UDP app transport; the `Mesh` LED-display shape; the `Effect` data
  type. (The
  **Valu v2.0 app** is now implemented - `[env:Valu_v2_0]`, `Devices/Valu_v2.0/`; its two LED
  displays are registered and the OLED module remains unbuilt, see the Valu section above.)
- **E11** **Register.md**: the System struct members are shown as keyed positions but only the whole
  struct is exposed at key 0. (The dynamic Create request now lists `Name` and its response is
  `Success flag` - that part is resolved.)
- **E12** **Storage.md**: CID 5/6 payload order is frag-info first with the name at +4 (not Name first);
  a file's size is the exact byte size (only allocation is page-aligned); the pointer page's
  *newest* valid slot wins; `MoveFiletable` grow/shrink is `OPTIMIZE_SPEED`-only; utility signatures
  are `bool` + out-params; the flash API is `Storage_FlashX`; `FindSpace` scans
  from an internal wear cursor. (Size 0 is now rejected - B11. The record type name is A64.)
- **E13** **Script.md**: the leading varSpace word is a vestigial IC slot; the symbol-subtype list omits
  `Number`; no size table (offsets are prefix sums); CID 5 truncates to 112 B; CID 6 takes a line
  index, not an instruction counter.
- **E14** **App Interface.md**: the BLE payload cap is MTU-5, not MTU-2.
- **E15** **App docs** were synced to the app on 2026-10-07 (the Update tab, the Connection autoconnect
  toggle, the Subscriptions viewer, the actual Devices graph layout and the Device view data
  source/capability wording are now documented). Remaining item outside `Docs/App`: `Current setup
  v3` lux cap is ~8.85k (not 10k) and the fan is not connected.
- **E17** **Subscriptions.md**: the get-subscriptions stream starts with an undocumented count byte.

## Storage / DAS
- **E18** **A few reduced-FS doc leftovers.** `fd2ba14` removed the main "Reduced variant" text, but
  `Docs/Services/Storage.md` still says "not in reduced file system" in the Create/Delete/Resize/
  Rename command rows, and `Docs/Devices.md:48` still reads "Memory: 128B (single file from offset
  0)" (the DAS is now a 512 B multi-file region at `0x3E00`). Optional wording cleanup.
- **E19** **`.SUBREQ` on a provider-only node** was reported once; the empty-table file is deleted now.
  Re-check if it reappears.

## Android (on-device behaviour untested)
- **E20** No Android device/emulator: BLE runtime permission prompt + denied/permanently-denied paths, BLE
  scan/connect/MTU, Storage Access Framework backup save + restore and file download, and the
  compact drawer shell on a phone form factor.

## Evaluation setup
- **E21** **LED brightness can brown out the board.** The builder clamps the displays to 5 % and the
  brightness script caps at 70 %, but a firmware-side current cap/ramp would be safer.
- **E22** **The LED display has no framebuffer readback**, so visuals are verified by eye only; a render
  snapshot command would make them testable.
- **E23** **DAS provider stale entries** - effectively solved by the 120 s provider lease + orphan-cancel
  path; revisit only if a *confirmed* cancel is wanted.
