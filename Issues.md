# Issues

## Naming/coverage gaps vs the docs (decision needed)
- **OS notifications.** `Docs/App/Settings.md` lists "Allow notifications (To OS)" with
  per-event selection; the app persists `notifyOs`/`osEvents`/`suppressOsWhenOpen` but only
  delivers in-app notifications (`app/lib/core/notifications.dart`). Implement OS delivery
  or mark the settings as pending.
- **Script UI info carries no enum labels in the docs.** `Docs/Services/Script.md` describes the
  UI info as names plus per-input limits/UI type; the app now writes **version 2** with a label
  list per input (the custom-enum / dropdown case). **v1 is no longer supported** (the app's
  parser and the firmware's function-name read accept version 2 only), so an old backup's script
  names fall back to the file name until re-saved - the format should be documented.
- **`Docs/Current setup v3.md` predates the emote interface.** The implementation adds script 2
  outputs (pupil offset L/R), script 3 inputs 2/3 (Force close / Max opening) and script 5
  (Emote selector) with a custom-enum emote input; the spec still lists only script 2's two
  inputs and no script 5 interface.
- **Position matrices carry a pre-rotated translation.** The renderer samples the geometry and
  texture masks *forward*, so a shape's centre lands at `-L^-1 * t`; with a rotation baked into
  the Position the shape would drift. Both writers (`ScriptExecTransform` and the app's
  `Transform23`) therefore store `t' = L * t`, which keeps the centre at `-t` for any rotation
  (an unrotated transform is unchanged). `Docs/Modules and blocks/LED display.md` describes
  Position as a plain 2x3 matrix, so a hand-written rotated matrix would need to know this.
- **Cross-script macro calls: the encoding and the waiting semantics are unspecified.**
  `Docs/Services/Script.md` lists "Macro call" and "Script (un)loading" in the functions table
  and nothing else - no opcode, no symbol encoding, no statement about what happens when the
  *callee* blocks. The (un)loading half is implemented (service ops 7/8, mirroring management
  CIDs 1/2, with a self-(un)load guard). The macro-call half needs a decision before it can be
  built, because the VM runs **one script per tick** and every wait state (`waitUntil`,
  `pendingForeign`, the 500 ms foreign deadline) lives on the callee:
  - if a callee waits on a foreign register reply, the tick loop must resume the *callee*, not
    the caller - so one script's state can no longer describe the run;
  - `Return` must know which script and line to come back to, i.e. the call stack has to carry
    a script slot as well as an instruction index;
  - it is not stated whether a macro call is blocking (caller waits for the callee to finish) or
    immediate, nor whether arguments/results cross the boundary, nor how recursion depth is
    counted (`SCRIPT_MAX_CALL_DEPTH` is per script today).
  Proposal to confirm: a `Call script` flow op with operands `(loaded script id, entry line)`,
  blocking by construction (the caller's instruction pointer stays on the call line and it
  resumes only on the callee's `Return`/`Halt`), a shared call stack of `(script, line)` pairs,
  and no argument passing (scripts exchange values through registers, as they already do).

## Docs revision: points still to pin
The revision closed two gaps: the **active-flag model is gone** (there is no 4-bit active segment
and no "Subscription Source" bit, and the read no longer combines flags), and the **script CID 8
extension is folded into CID 3** ("Read state" now returns `State, Last error code`). What the new
text still disagrees about, and needs a ruling before the matching code lands:

- **The trigger table's "Function pointer (Static)"** cannot be sent. Proposal: `Field&Key` plus a
  reserved 32-bit word (0 for static, the Script ID for dynamic).
- **Script CID 0 lists "Script File IDs", not slots.** CID 1 takes a separate loaded id, so the
  two may differ - and CID 0's list is then not addressable: the caller cannot recover which slot
  holds which file. The docs were updated to **uint16 file ids** (SCR_XXX, 4096 files) with the
  slot still 6-bit (64 loaded); the app picks a free slot and tracks file->slot itself, and a
  script's block meta carries its function name (not its file id), so an untracked slot can only
  fall back to the file==slot convention. Reporting the loaded **slots** in CID 0 (or a slot in
  each list entry) would remove the guesswork - a docs decision.
- **The app mirrors the firmware's static persistent layout.** `.SV` is a raw space with no
  offsets, so the app recomputes each field's offset from a per-type persistent-field table
  (`device_backup.dart`'s `staticPersistentFields`) + the 32-bit alignment rule. The firmware is
  the source of truth, so a firmware layout change must be mirrored there; a device-side
  "field offsets" read (not in the doc's wire format) would remove the duplication.

Verified against the code (2026-10-03), still to pin:
- **The Static Block Type Table is descriptive, not literal.** The doc lists an explicit
  `Field&Key` per entry and per trigger plus a `Count: Triggers`; the firmware stores positional
  parallel arrays (`Map[]`/`Offsets[]`/`Triggers[]`) indexed by field (field/key implicit, no
  separate trigger count). Equivalent, but the doc should say the entries are positional.
- **The dynamic commands' request shape (deferred).** The doc says `Index (uint16)`; the firmware
  + app send a full 32-bit **BlockInfo** (the index in the upper half). The doc should say
  `BlockInfo`.

Resolved in the 2026-10-03 revision (code now matches): the trigger timing wording, the
`VolatileSize`/`PersistentSize` units, the Dynamic Block Table header (the `.DT_XX` file is now
`Name(16) + count(16) + reserved(16) + entries`, no length prefix and no stored type), the dynamic
Trigger flag, the banked enumerate 8.8 split, and `Command ID table.md`'s Save/Recall swap. The
dynamic descriptor no longer stores a type (it is derived from the global index); a live block is
marked by a `present` flag instead.

Resolved in the register doc-vs-implementation pass (2026-10-03, later):
- **Dynamic `Read Only` is enforced.** `DynamicBlockDescriptor::SetEntry` and `DeleteEntry` reject a
  write/delete of a stored read-only entry (the static and script paths already did).
- **Block names are the documented fixed 16 chars**, space-padded, with no NUL and no truncation
  (`SetBlockName`; create/set/get/meta/persist all use it).
- **System field 8 "App Active" is an enum** (`AppActive`: No/USB/BLE), not a bool.
- **`Register.md` updated**: the BlockInfo split (10/6, dynamic banked), the enumerate-fields
  request (packed 10.6, 4-byte padded), the block-meta read (field 0xFF), the write response
  (echoes the request), and the String/Filename space-padding exception.
- **The System block meta uses the same shape as every other block** (Bi + ValueInfo + 16-char name).
- **`BlockSchema.VolatileSize`/`PersistentSize` dropped** (unused; the flat space structs are the
  layout source of truth).

Still open after the 2026-10-03 cleanup:
- **The `.DT_XX` entry omits the doc's `MemoryOffset`.** The doc's Dynamic Block Table lists
  `Field&Key(16) + MemoryOffset(16) + ValueInfo(32)` per entry, but the firmware writes only
  `Field&Key(16) + ValueInfo(32)` and recomputes the offset (value sizes + 32-bit alignment) on
  load, so the file is a compacted mirror rather than a literal one. Storing the offset would
  match the table 1:1; dropping it is redundant-but-smaller and is what the app decoder assumes.
  The doc should pin which form the `.DT_XX` file uses.
- **The dynamic descriptor doc omits `Name`/`generation`/`present`** (deferred).
- **ResistiveMeasure trigger flags are device-specific.** The doc marks Sampling Rate "(TR)"; the
  firmware gives it (and Filter Coefficient) a trigger function but no `ValueTrigger` flag, so the
  wire flags don't advertise the trigger. Different devices may or may not need one.
## Android (on-device behaviour untested)

- **On-device behavior not yet verified** (no Android device/emulator configured): the BLE
  runtime permission prompt and its denied/permanently-denied paths, BLE scan/connect/MTU,
  the Storage Access Framework backup save + restore and file download, and the compact
  drawer shell on a phone form factor.

## Evaluation setup (`Docs/Current setup v3.md`)
- **LED brightness can brown out the board.** The LED-display driver accepts brightness
  values whose current draw resets the MCU (the board dropped off USB at 60 %; a stored
  brightness script at a high ceiling put it in a boot/brown-out loop). The builder clamps the
  displays to 5 % before anything else, and the brightness script's ceiling is 70 % (reached
  around 10k lux). A firmware-side current cap (or a ramp) would be safer than relying on the app.
- **The LED display has no framebuffer readback.** `Docs/Modules and blocks/LED display.md`
  exposes no way to read the rendered pixels, so a HIL test can only assert the render
  dictionary contents + the refresh rate. The `Cut` mask operation is exercised by the LED
  probe (`hil_led_display_test`); the evaluation scene uses only `Replace` now that dark mode
  is a filled iris, and the *look* is verified by eye only. A render snapshot command would
  make the visuals testable.
- **DAS provider subscriptions accumulate stale entries.** The DAS provider table holds 4, and
  a requester cancel does not always reach the DAS (busy bus / dropped packet), so stale
  providers linger and can block a new subscription (`ProviderFindFree` returns none). The
  setup builder clears both DAS provider tables first as a workaround. The fix is **parked
  pending documentation**: the cause is that the docs' "Transaction ID manager"
  (`Docs/RSBus and Packets.md` - REQACK plus a registered handler with a per-TRID timeout) is
  **not implemented**, so nothing ever confirms that a cancel landed. Scope and design are in
  `TODO.md`.
