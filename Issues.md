# Issues

Open items only. The 2026-10-04 per-area audit findings (register, storage, device/log,
subscriptions, tamu, das, bootloader, app, tooling, tests) were fixed and committed; the
remaining low-value follow-ups live in `TODO.md`.

## Open (2026-10-09)

- **Doc vs code: the `DataType` enum.** `Docs/Data Formats.md` now carries the values, read from
  `firmware/src/Core/Types/Enums.h:40-62`. Three deltas came out of that comparison:
  - **`Name` (16 characters) has no `DataType` member.** The doc defines it as a reusable type; the
    enum has no such entry, so a 16-character name is a `String` or `Filename` in code. Either the
    enum gains a member or the doc drops the type.
  - **`Effect` is documented but absent from the enum**, which has `Geometry` (0x101) and `Texture`
    (0x102) only. `Docs/Modules and Blocks/LED Display.md` specifies effects as well.
  - **Three enum members were undocumented:** `Deleted` (0x0D), `Uint32` (0x0E) and `DevType` (0x0F).
    They are in the doc table now, with descriptions inferred from their names - confirm those.
  - Naming: the doc used `Serial Number`, `ID` and `Generic dictionary` where the enum says `SN`, `Id`
    and `UnknownKeyed`. The table uses the enum names for traceability.

## Open (2026-10-06)

- **Valu v2 bootloader: a sector erase does not take effect.** Full status and measurements in
  `TODO.md`. It is **silent** - the controller sets `EOP` and reports no write-protection error
  while the flash content is unchanged - so the app-facing consequence would be update failures
  that look like nothing happened. Do not build Valu v2 features that depend on re-flashing the
  board until this is resolved.
- **The Valu bootloader can only ever be replaced over the ROM bootloader.** Our own bootloader
  refuses writes below `APP_BASE` by design, and this board has no SWD header, so every bootloader
  iteration needs an ISP session (`wchisp-nightly`, one command per USB reset). Worth keeping in
  mind when deciding how much logic belongs in the bootloader rather than the app.
- **`valu_upload.py` drives `wlink` over SWD, which this board cannot do.** The ISP recipe that
  actually works (USB port reset, then a single `wchisp-nightly` invocation) needs folding into it
  as an `--isp` mode so `./upload.sh boot valu` matches reality.

## Code decisions needed (2026-10-04 docs-conformance sweep)
- **Register write of a longer String/Filename.** `Register.md` says a longer value fails (only a
  shorter one is allowed, space-padded); `MemoryTypes.h` clamps it instead. The app and the "System
  Name clamps to 16 bytes" HIL rely on the clamp. Parked (minor detail).

## Valu v2.0 app (2026-10-06)
- **Resistive measurement reference resistor is undefined.** `Docs/Devices.md` gives three ADC
  inputs (PA6/PA1/PA0) but says "Reference resistor not defined." With no reference and no range
  selectors, the resistance/LDR/NTC transforms in `Devices/Valu_v2.0/MeasuringRun.h` use a
  placeholder `VALU_MEAS_REF_KOHM` (10 kOhm); raw and voltage measurements do not depend on it.
  Needs a hardware value before a resistance-reading module is meaningful.
- **"USB Bootloader" and "Script" services have no capability-bit mapping.** `Docs/Devices.md`
  lists the Valu's services as Mandatory + USB Bootloader + App interface + Dynamic memory +
  Script. `Capabilities::` (Core/Types/Enums.h) has bits for Node/AppInterface/DynamicMemory/
  Scripts/StorageFeatures/Subscriptions but **none for a bootloader** - the code (and the
  bootloader itself) already treats that as "no capability bit" (`TODO.md` Decisions). The Valu
  app advertises `Node | StorageFiles | AppInterface | DynamicMemory | Scripts`.
- **Valu modules not implemented in this pass.** `Docs/Devices.md` also lists "LED Display x2"
  (needs the Vysi1 render/layout stack) and "OLED Display (TODO)" for the Valu. The app implements
  the LED-Button, the three buttons, the one fan output and the three resistive channels (the
  peripheral set the task specified); the two display modules are not built.
- **`MAX_SCRIPTS` is reduced to 16 on the Valu.** The Script service's loaded-script registry is
  `LoadedScript scriptRegistry[256]` (~38 KB) in `ScriptDefs.h`; the CH32V203's 20 KB cannot hold
  it, so the registry is now a `#ifndef`-guarded build knob and the Valu sets `-D MAX_SCRIPTS=16`.
  Higher Scripts-range indices report as unloaded.


## Docs decisions needed (parked unless noted)
- **OS notifications.** `Docs/App/Settings.md` lists "Allow notifications (To OS)" with per-event
  selection; the app persists `notifyOs`/`osEvents`/`suppressOsWhenOpen` but only delivers in-app
  notifications (`app/lib/core/notifications.dart`). Implement OS delivery or mark the setting
  pending.
- **Script UI info format.** `Script.md` describes the UI info as names + per-input limits/UI type;
  the app writes **version 2** with a per-input label list (the custom-enum case). v1 is no longer
  supported, so an old backup's script names fall back to the file name until re-saved - document
  the format.
- **Pre-rotated Position convention.** The renderer samples the geometry/texture masks *forward*,
  so a shape's centre lands at `-L^-1 * t`; both writers (`ScriptExecTransform` and the app's
  `Transform23`) store `t' = L * t` to keep the centre at `-t` under rotation. `LED display.md`
  describes Position as a plain 2x3 matrix - document the convention.
- **Cross-script macro calls.** `Script.md` names "Macro call" but gives no opcode, boundary rule
  or argument passing. The VM runs **one script per tick** and every wait state lives on the
  callee, so it needs a call stack of `(script, line)` pairs and a tick loop that resumes whichever
  script is waiting. Proposal to confirm: a `Call script` flow op with operands
  `(loaded script id, entry line)`, blocking by construction, values exchanged through registers.
- **Script CID 0 lists file IDs, not loaded slots.** CID 1 takes a separate loaded id, so CID 0's
  list is not slot-addressable; the app tracks file->slot and falls back to the file==slot
  convention. Reporting the loaded **slots** (or a slot per entry) would remove the guesswork.
- **Dynamic descriptor doc omits `Name`/`generation`/`present`** (deferred).
- **Dynamic 8.8 enumerate encoding** is only in code comments (`RegisterEnumerate.h`), not in
  `Register.md`.
- **Bootloader LED entry.** `Bootloader.md` (white LED) vs `Devices.md` (core white LED "missing
  hardware") vs the app (red LED). Code drives no LED in the core bootloader.
- **Device-type numbering is undocumented.** `Docs/Data Formats.md` and `System Block and Device
  Commands.md` name the System `Device Type` field but give no numeric table. `Enums.h` /
  `types.dart` assign `Tamu_v2_0A = 0x01`, `Valu_v2_0 = 0x02`, `DualAnalogSensor = 0x03` as a code
  convention (0x02 carried over from the pre-restructure tree) - confirm or document it.

### Docs-conformance sweep wording (2026-10-04)
- **Documented but not implemented** (future/planned, `Plan.md`): WiFi `App Active` values + System
  field 8.1 SSID/Password; Router capability + service (stub only); branch-broadcast address
  `0x3FE`; UDP app transport; the `Mesh` LED-display shape; the `Effect` data
  type. Code-only types (`Deleted`, `Uint32`, `DevType`, `UnknownKeyed`) are undocumented. (The
  **Valu v2.0 app** is now implemented - `[env:Valu_v2_0]`, `Devices/Valu_v2.0/`; its LED-Display x2
  and OLED modules remain unbuilt, see the Valu section above.)
- **Register.md**: dynamic Create request also carries a 16-char name (doc lists only `Index`), and
  the response is `BlockIndex(5)+ack` (doc says `Success`); System struct members are shown as keyed
  positions but only the whole struct is exposed at key 0.
- **Storage.md**: CID 5/6 payload order is frag-info first with the name at +4 (not Name first);
  a file's size is the exact byte size (only allocation is page-aligned); the pointer page's
  *newest* valid slot wins; `MoveFiletable` grow/shrink is `OPTIMIZE_SPEED`-only; utility signatures
  are `bool` + out-params; the flash API is `Storage_FlashX`; size 0 is accepted; `FindSpace` scans
  from an internal wear cursor.
- **Script.md**: the leading varSpace word is a vestigial IC slot; the symbol-subtype list omits
  `Number`; no size table (offsets are prefix sums); CID 5 truncates to 112 B; CID 6 takes a line
  index, not an instruction counter.
- **App Interface.md**: the BLE payload cap is MTU-5, not MTU-2.
- **App docs** were synced to the app on 2026-10-07 (the Update tab, the Connection autoconnect
  toggle, the Subscriptions viewer, the actual Devices graph layout and the Device view data
  source/capability wording are now documented). Remaining item outside `Docs/App`: `Current setup
  v3` lux cap is ~8.85k (not 10k) and the fan is not connected.
- **RSBus/Packets.md**: the Script TRID range is unspecified and the System/Log counter is 8-bit
  (within range) not 12-bit.
- **Subscriptions.md**: the get-subscriptions stream starts with an undocumented count byte.

## Storage / DAS
- **A few reduced-FS doc leftovers.** `fd2ba14` removed the main "Reduced variant" text, but
  `Docs/Services/Storage.md` still says "not in reduced file system" in the Create/Delete/Resize/
  Rename command rows, and `Docs/Devices.md:48` still reads "Memory: 128B (single file from offset
  0)" (the DAS is now a 512 B multi-file region at `0x3E00`). Optional wording cleanup.
- **`.SUBREQ` on a provider-only node** was reported once; the empty-table file is deleted now.
  Re-check if it reappears.

## Android (on-device behaviour untested)
- No Android device/emulator: BLE runtime permission prompt + denied/permanently-denied paths, BLE
  scan/connect/MTU, Storage Access Framework backup save + restore and file download, and the
  compact drawer shell on a phone form factor.

## Evaluation setup
- **LED brightness can brown out the board.** The builder clamps the displays to 5 % and the
  brightness script caps at 70 %, but a firmware-side current cap/ramp would be safer.
- **The LED display has no framebuffer readback**, so visuals are verified by eye only; a render
  snapshot command would make them testable.
- **DAS provider stale entries** - effectively solved by the 120 s provider lease + orphan-cancel
  path; revisit only if a *confirmed* cancel is wanted.
