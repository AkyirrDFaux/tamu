# TODO

Long-term plan (`Docs/Plan.md`): 1) Scripts, 2) blocks/modules + subscriptions, 3) app backup.

## 1. Scripts
- [x] **Milestone A — walking skeleton**: `SCR_XX` file parser, loaded-script registry,
      Register exposure (block type `0x3FE`), management CIDs `0x0500-0x0507`.
      Verified on Tamu v2.0A (`app/test/hil_script_test.dart`).
- [x] **Milestone B — VM**: preloaded instructions + line/block tables; math/logic
      (`Set/Add/…/Select`), flow (`If/While/EndBlock/Jump/Call/Return/Halt`), time
      (`Delay/WaitUntil/GetTime`), services (`Register read/write` local + foreign with
      ScriptUpdated + per-instance TRID, `Script state`, `Log`, `Nop`) and
      `Compose/Extract`; per-instance script TRID range `0xF000-0xF9FF`; loop guard.
      Verified on Tamu v2.0A (`app/test/hil_script_vm_test.dart`).
- [x] **Milestone B — boot/load**: `Load-on-boot` loads every stored script so flagged;
      `Run-on-load` starts it. Verified across a hard reset.
- [ ] **Script (un)loading from within a script** (a script starting/stopping another) and
      macro-style cross-script calls — not yet implemented.
- [x] **Milestone C — app list page**: `ScriptsPage` with a Loaded/Available switch,
      state chip + Start/Pause/Continue/Stop/Restart, expandable inputs (rendered per UI
      spec) and outputs, a Device view "Scripts" tile (guarded by the Scripts capability),
      and `ScriptEditorPage`.
- [x] **Milestone C — app editor**: opens stored *and* loaded scripts; edits function name +
      properties, inputs (type/style/limits/default), outputs, variables (add/remove/type),
      constants (value/name), and instructions (per-line/per-symbol editing with
      context-filtered pickers, variable/constant shortcuts, active-line highlight);
      validity/type check; upload (file) + apply live (reload).
- [x] **Editor UX**: type-driven sizes (no size field), type-limited input styles with
      conditional min/max, readable reorderable instruction lines (Destination-Instruction-
      Operands), recommendation-first grouped pickers (tap a group / back), instruction-aware
      destination/operand limits and filtering, full predefine set, and Unload actions.
- [x] **Bugfix / docs-alignment round**:
      - Editor active-line highlight fixed (IC is a line index, not a symbol offset).
      - Inputs rendered from their UI specification (Slider/Toggle/Button) in the script
        list expansion (docs: "formatted with the UI specifications").
      - `Log` opcode implemented (custom logs); `Script state` opcode now uses the same
        transition rules as CID 4.
      - Waiting-state fixes: a manual state change cancels an outstanding foreign
        confirmation; a late reply only resumes a script that is still Waiting.
      - Compose rejects a non-writable container.
      - Removed dead code (header register keys, unused app helpers).
- [x] **Recommendations**: instruction-aware instruction-picker ordering (value-producing
      vs control ops), position/type-aware symbol recommendations (register address, device
      address, line target, boolean condition, numeric) and an "expects ..." hint line.
- [x] **Second bugfix / streamline round**: boot only loads `Load-on-boot` scripts (was
      loading every stored script); register read/write buffers sized to the u8 value limit
      (was a 64-byte overflow risk); compose operand scratch aliasing fixed; Number size
      guard; foreign device-address validation; input control re-sync on refresh without
      fighting a drag; single-pass UI-info parse (was five re-walks).
## 2. Blocks/modules + subscriptions
- [x] **Block/module schema alignment** (docs-driven): Button reduced to field 0; LED-Button
      = Button (0) + LEDState (3) with reserved 1-2; Acc&Gyr deadzones removed (Acceleration
      = 5, Angular Velocity = 6); Resistive measurement deadzone removed (Measured Value = 3,
      Current Range = 4); `DataType::Filename` added and used for the LED-Display Layout File
      Name. App `block_registry` mirrored + a drift-guard unit test.
- [x] **Subscription alignment**: `Deadzone` added to both entries (requester 28 B, provider
      32 B, CID 1 payload); trigger types `EdgeRising/Falling/Any` and `DeltaPeriodic`;
      per-trigger hashlike (FNV-1a / raw compare / edge counter / last value / subresolution
      vector); high/low packet priorities; script I/O (`0x3FE`) as source/target; non-doc
      CID 5 manual save removed (auto-save on set/delete). App types/dialog/client updated.
- [x] **Two-device HIL** (Tamu + DAS): DAS Measured Value → Tamu target; Acc&Gyr vector delta
      self-loopback; script-output source; new 28/32 B entry round-trip.
      Verified on hardware (`app/test/hil_subscriptions_test.dart`).

## 3. App backup (`Docs/App/Backup.md`)
- [x] **3A Semantic format**: `backup_value.dart` (type/enum/flag/colour/matrix/dictionary
      words), `backup_format.dart` (per-device archive: register, subscriptions, SNDB,
      files) and `backup_script.dart` (scripts as function/IO/variables/constants/lines
      with type words and semantic values). Numbers only as literal values/indexes. Unit
      tests (`backup_value_test.dart`, `backup_script_test.dart`, `backup_restore_test.dart`).
- [x] **3B Capture/restore**: the ENTIRE register (System + static + dynamic, read-only and
      volatile entries included), the Subscriptions tables, Scripts, SNDB (cores) and every
      device file, all semantic. Restore matches by name then index with a type check
      (string/filename interchangeable, enum names resolved on the target). The zip now
      holds **one JSON per device - no aggregate manifest**.
- [x] **3C Per-part sync UI**: `RestorePlanPage` groups Registry / Scripts / Subscriptions /
      SNDB / Files with per-item selection and target device/block remap; unavailable items
      are flagged and disabled.
- [x] **3D HIL**: whole-registry capture/mutate/restore, semantic script round-trip and file
      restore on Tamu (`app/test/hil_backup_test.dart`).

### Notes
- Backup zip entries must be built from UTF-8 bytes, not `ArchiveFile.string` (archive
  3.6.1 sizes by UTF-16 code units but stores UTF-8, so any non-ASCII character such as the
  "±" in the accelerometer range labels wrote a wrong uncompressed size and strict unzippers
  failed with a CRC error). Guarded by the "zip declares correct sizes for non-ASCII content"
  test.
- Backup format 2 is semantic and per-device (`backup_format.dart`); the zip contains only
  `<id>_<name>.json` files (no aggregate manifest). Archives with a numeric (pre-semantic)
  format are rejected with a migration message.
- Scripts are captured both semantically (`backup_script.dart`) and as their raw `SCR_XX`
  file (storage section); restoring the semantic script re-serialises it via `ScriptDraft`.
- Backup capture skips device files larger than 128 kB by default (the Storage stream
  fragments are small, so large reads are slow); `captureDevice(maxFileBytes:)` raises it.
  System block Net ID (field 7) is not captured (firmware write limitation, see `Issues.md`).
- A write of `Filename`/`String` shorter than the field is space-padded to its declared size.
- Script entities use the ValueInfo packing of `Core/Services/Script.h` /
  `app/lib/core/script_file.dart` (internal convention; the docs leave it open).
- Outputs are read-only and inputs writable in the Register; Variables are script RAM
  (management CID 5/7) and Constants are file data - neither is register content.
- IO lives in the volatile memory space ("dynamic memory without persistence" per docs).
- Loaded scripts are exposed through the Register service as block type `0x3FE`, **I/O only**
  (Inputs and Outputs; inputs writable). Variables live in the script RAM (Script CID 5/7),
  constants are file data and the Header is script metadata (Script CID 3/5/8) - none of
  them are register content.
