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

Verified against the code (2026-10-03), still to pin:
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

Resolved in the register doc-vs-implementation pass 2 (2026-10-03, later):
- **The `.DT_XX` table now stores the `MemoryOffset`** per entry (Field&Key + MemoryOffset +
  ValueInfo); the load uses the stored offset directly (the DV is the compacted persistent space).
- **The static block table is literal** (`BlockEntry`: Field&Key + MemoryOffset + ValueInfo) with
  a literal trigger table (`BlockTrigger`: Field&Key + function pointer) that holds only the fields
  that actually have a trigger - no per-field nullptr padding (the core dropped ~86 B).
- **The app derives the static layout from the read commands**
  (`RegisterClient.readStaticFieldLayout` reads the CID 1 field list + CID 2 per-field ValueInfo);
  the hardcoded `staticPersistentFields` mirror is gone.

Still open after the 2026-10-03 cleanup:
- **The dynamic descriptor doc omits `Name`/`generation`/`present`** (deferred).
- **The static write's over-long String/Filename behaviour is unspecified.** `Register.md` says
  "Write of a different type and/or length fails (String/Filename writes may be shorter and are
  space-padded to the field size)". The code *also* clamps a longer String/Filename write to the
  field size (a 22-char System Name write is clamped to the documented 16, pinned by the
  verification HIL test), which the parenthetical does not state.
- **ResistiveMeasure trigger flags are device-specific.** The doc marks Sampling Rate "(TR)"; the
  firmware gives it (and Filter Coefficient) a trigger function but no `ValueTrigger` flag, so the
  wire flags don't advertise the trigger. Different devices may or may not need one.

Resolved in the TRID-range pass (2026-10-03, later):
- The reserved ranges are defined centrally (`TRID_*` in `Core/Functions/Packet.h`): System/Logs
  `0x0000-0x0FFF`, Subscriptions `0x1000-0x1FFF`, Scripts `0x2000-0x2FFF` (the docs leave the
  script range as "..."), App `0xF000-0xFFFF`. Replies echo the request's TRID
  (`FinalizeReply`), and the app allocates/matches on the full 16-bit App TRID.
- **System/Logs is not yet a blind incrementing counter.** The Device service still discriminates
  its responses by the tag's CID (`MakeService(Device, cid)`), so a plain counter would break
  discover/timesync matching; converting it needs the documented TRID manager (the same gap as
  the DAS provider-cancel entry below). The current Device/Log tags already sit inside
  `0x0000-0x0FFF`.
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
- **DAS provider subscriptions could accumulate stale entries.** The DAS provider table holds 4,
  and a requester cancel is fire-and-forget (docs: "Sent once for deletion"), so a dropped
  cancel left a stale provider that could block a new subscription. The 2026-10-03 subscription
  rework adds a **120 s provider lease** renewed by the requester's keepalive, so an unrenewed
  provider expires on its own, and a value update whose TRID matches no requester entry is
  cancelled back (the orphan path). A missed cancel can still hold a slot for up to 120 s. The
  revised `RSBus and Packets.md` specifies per-service TRID management (an incrementing counter
  or a slot table), which the code implements - there is no handler-table requirement to meet,
  so a confirmed cancel is a low-priority improvement, not a docs gap.
