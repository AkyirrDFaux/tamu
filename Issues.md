# Issues

Open items only. The 2026-10-04 per-area audit findings (register, storage, device/log,
subscriptions, tamu, das, bootloader, app, tooling, tests) were fixed and committed; the
remaining low-value follow-ups live in `TODO.md`.

## Code decisions needed (2026-10-04 docs-conformance sweep)
- **Register write of a longer String/Filename.** `Register.md` says a longer value fails (only a
  shorter one is allowed, space-padded); `MemoryTypes.h` clamps it instead. The app and the "System
  Name clamps to 16 bytes" HIL rely on the clamp. Parked (minor detail).

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
- **Device view wording.** `Docs/App/Device view.md` says it "Interacts with the device service
  only", but the app reads identity (type/SN/version/capability/name) via the Register System block
  (which matches Device Commands.md). Stale side is the doc.
- **Bootloader LED entry.** `Bootloader.md` (white LED) vs `Devices.md` (core white LED "missing
  hardware") vs the app (red LED). Code drives no LED in the core bootloader.

### Docs-conformance sweep wording (2026-10-04)
- **Documented but not implemented** (future/planned, `Plan.md`): WiFi `App Active` values + System
  field 8.1 SSID/Password; Router capability + service (stub only); branch-broadcast address
  `0x3FE`; Valu v2.0 device; UDP app transport; the `Mesh` LED-display shape; the `Effect` data
  type. Code-only types (`Deleted`, `Uint32`, `DevType`, `UnknownKeyed`) are undocumented.
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
- **App docs**: `General info` omits the Update tab; `Devices` graph layout differs (no net
  structure / router tree); `Device view` lists a nonexistent Bootloader capability and omits the
  Subscriptions viewer; `Connection` has an undocumented autoconnect toggle; `Current setup v3` lux
  cap is ~8.85k (not 10k) and the fan is not connected.
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
