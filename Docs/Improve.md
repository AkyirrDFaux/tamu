# Improvement suggestions

## New 2026-08-26 (script editor rebuild)

- **Script.md "File blocks" input meta now carries an interaction-style byte per input**
  (5 B/input: dict BlockMeta + per-input [key BlockMeta + style byte]); legacy 4 B/input
  files still parse. The doc's "Input definition meta length" table row should mention the
  per-input style byte.
- **Input interaction styles** are persisted but not yet rendered anywhere outside the
  editor - a future "run" view should show inputs as Button/Switch/Picker/Slider per the
  style. Automatic maps Bool->Switch, Enum->Picker, String->Text, Number->Slider.
- **The editor auto-declares referenced constants with a Number-0 default**; there is no
  way yet to define a constant's value inline in an instruction (constants must be edited
  in the Constants card). The State/Type/MathOp predefine literals are pickable but have
  no presets.
- **MacroCall** has no authoring UI beyond a plain instruction line (pick MacroCall and
  set the target script id via the operand picker - the operand is a Number that holds the
  script id).

## New 2026-08-26 (script service implementation)

- **Script.md's symbol example is illustrative, not a program** (it chains an output
  into another op, which line 51 forbids). The implemented grammar is
  `[Output][Instruction][Operands...] EndLine`; IF/WHILE embed a math/logic condition
  expression. Consider rewriting the example to a valid program.
- **Script.md "Create script | 13 | Script ID | Success"**: the implementation returns
  the *assigned* Script ID (1 byte, 0 = failure) instead of a plain success byte so the
  app can use an auto-assigned ID; the doc should say so.
- **Script "Read script" (CID 15)** streams the whole file; the app reassembles it.
  Script editing rewrites the whole file each save (open stream with expected size ->
  chunks -> close), so a script larger than free storage fails at Open. A block-level
  patch/diff editor would avoid the rewrite.
- **Script MemWrite to static (System Memory) blocks cannot set the ScriptUpdated flag**
  - the schema flags are const; a script write to a static field would be captured by a
  backup. Either document it or add a side-table of script-touched static fields to skip
  in SerializeSystemBlocks.
- **RAM preload**: scripts are always preloaded to heap at start; if the heap is
  insufficient the start fails. The doc's "run from file with read-only pointers"
  fallback is not implemented.
- **Editor gaps**: input/constant defaults are edited as hex; the editor cannot yet
  express State/Type/MathOp predefine literals (only Bool/Char/Index); MacroCall lines
  are editable as text but there is no macro authoring UI.
- **OS notifications** ("Allow notifications (To OS)" in Settings) still have no
  consumer - in-app notifications now fire (Device discovered/lost, Backup finished).

## New 2026-08-25 (consolidation/audit round)

- **Data Formats.md ID model is internally inconsistent**: it says "16bit
  (4 bit net + 12 bit device)" but also defines Broadcast as 0xFFFFFFFF (32-bit), and the
  firmware/app use flat 16-bit addresses (Broadcast 0xFFFF) with dense IDs from 2 - no
  net/device split anywhere. Pick one model (the implementation follows the 16-bit one).
- **Keyed Memory.md only lists CIDs 0-6** but the firmware (and app) implement CID 7
  "read all entries of a dictionary in one round trip" - document it next to the other CIDs.
- **Docs/App/Backup.md promises more than the app implements**: per-part selection of synced
  items, cross-device sync of compatible targets, and file-system (not just System Memory)
  backup. The app currently archives/restores System Memory blocks only (per-device JSON
  zip + live restore). Either trim the doc to what exists or track the rest as a feature.
- **Docs/App/Service views/Storage.md mentions uploading files** from the host; the app only
  downloads/previews (the firmware has Write Stream Open/Close/Write CIDs 7/8/64+, so upload
  is implementable).
- **Docs/App/Settings.md describes in-app and OS notifications**, but the app only persists
  the preference toggles - nothing consumes them. Either implement the notification feed or
  mark the feature not-yet-implemented in the doc.
- **Docs/App/Device view.md lists a Router table viewer and Script editor**; both wait on
  their (not-yet-implemented) firmware services. The capability display also shows all six
  service bits while the doc says "Show only non-services" - clarify what that means (all
  current capability bits ARE service bits).
- **Docs/App/Devices.md wants routers below the core in a tree**; the Router service is not
  implemented, so the graph currently splits core-vs-others. Revisit when routers exist.

## Resolved 2026-08-24

- SNDB deletion is now covered by the updated Docs/Services/Device service.md:
  SNDB Write (CID 14) with ID = 0 deletes the entry carrying the serial number.
  Firmware and app follow this exactly; the temporary CID 15 was removed again.
- Block-type editing via the block-level Write (field index invalid) is implemented
  in both memory services per "type is user editable"; consider noting it next to
  the Write row in both CID tables.
