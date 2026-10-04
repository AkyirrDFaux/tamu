# Issues

Open items only. The 2026-10-04 per-area audit findings (register, storage, device/log,
subscriptions, tamu, das, bootloader, app, tooling, tests) were fixed and committed; the
remaining low-value follow-ups live in `TODO.md`.

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

## Storage / DAS
- **`Docs/Devices.md` still documents the DAS "Reduced variant" file system.** The DAS now runs
  the shared full multi-file filesystem (`StorageBlockFS`) with a 384 B region at `0x3E80`
  (pointer page + file table + 256 B data); the reduced `StorageFixedFS.h` and `USE_FIXED_STORAGE`
  were removed. The doc's reduced-variant description, the `.SV` "fixed size, no presence bit"
  wording and the 128 B memory figure need updating.
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
