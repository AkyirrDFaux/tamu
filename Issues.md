# Issues

## Naming/coverage gaps vs the docs (decision needed)
- **Script file names.** `Docs/Services/Script.md` says `SCR_XXX`; the implementation uses
  `SCR_XX` (`SCR_00..SCR_3F`, 64 slots) in both firmware (`Core/Services/Script.h`) and app
  (`app/lib/core/script_file.dart`). Rename if >64 scripts are wanted.
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

- **`Register.md` "Write ... Respond always"** contradicts the agreed behaviour: **write responses
  are request-gated** (reply only when the request set REQACK), which is what `Set Name`'s
  "Respond only if requested" already says. **Confirmed**: the firmware gates every response on
  `FLAG_REQACK` (`SendResponse` returns early otherwise), so only the doc line needs to go back.
- **The trigger table's "Function pointer (Static)"** cannot be sent. Proposal: `Field&Key` plus a
  reserved 32-bit word (0 for static, the Script ID for dynamic).
- **`ValueInfo` layout.** The Map entry gives `Type(16) + Size(8) + Flags(8)`, while the wire
  format packs `FlagsAndType(16) + Key(8) + Size(8)` (`BLOCK_META_FLAGS_MASK` occupies bits 10-15).
  The block table now puts ValueInfo on the wire, so this needs pinning; with only the three
  passive flags left, the doc's layout is cleaner and would retire the mask hack.
- **`Script.md:82`** still says "Writer sets the script updated flag", but that flag no longer
  exists.
- **`App/Device view.md:8`** still lists **CLI** in the capability bitfield, though the CLI is gone
  and `Docs/Services/CLI.md` was deleted.
- **Script CID 0 lists "Script File IDs", not slots.** CID 1 now takes a separate loaded id, so
  the two may differ - but then CID 0's list is not addressable: the caller cannot recover which
  slot holds which file. The implementation loads a slot explicitly and the app keeps file id ==
  slot, so CID 0 stays meaningful; either CID 0 should report the loaded **slots**, or the docs
  should say the loaded id equals the file id.
- **`Docs/Services/Register.md`'s dynamic trigger table has no backing data.** The row says a
  dynamic trigger's target is a **Script ID**, but nothing stores one: `DynamicEntry` has no
  script id and the descriptor has no trigger array - the `Trigger` flag is only a marker. A
  dynamic block therefore reports **zero** triggers, and that column is unimplemented.
- **The dynamic descriptor's `Name` is 24 characters, not 12.** `BLOCK_NAME_LEN` is 24 and
  `HandleCreateDynamic` accepts 23; the doc's descriptor (and the old Dynamic Block Table) say 12.
  The evaluation setup's own block is named `'Subscriptions'` (13), so clamping to 12 would break
  it - the doc should say 24.
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
