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
- [x] **Readability / docs-alignment round**: math ops now fold **N operands**
      (`Add x = a + b + c`); a Vector/Matrix destination gets **element-wise math** (scale +
      clamp a gyro vector in one op); register targets use a dedicated **`BlockInfo`** type
      (0x10 - the Register's type|inst|field|key) with a block/field/key editor instead of a
      raw uint32; trivial constants are **inline predefines** (integers via the existing
      `Index` predefine, fractions via a new `Number` predefine, Q8.8). The four setup scripts
      were rewritten with these (behaviour unchanged) and the builder now validates each
      script before upload. Wire-compatible except the BlockInfo type tag and the Number
      predefine (firmware + app ship together).
- [x] **Script follow-up**: a **`Limit`** (clamp) math op (`Limit x = v, min, max`, scalar and
      element-wise); the BlockInfo editor is a **tiered block -> field -> key dialogue**
      (reusing the Subscriptions pickers) and the register operand picker creates BlockInfo
      constants; the lid script is a single **time-interpolated loop** (open, then a 100 ms
      close/open every period) using `Limit`; the editor shows a retry instead of an endless
      spinner when a script file cannot be loaded.
- [x] **Single-line math expressions**: `Set` now evaluates an **infix expression** with
      precedence (`^` > unary `-` > `* /` > `+ -`) and parentheses, in one line
      (`Set y = (A + B) * C - D / 2`). Full scalar/**vector**/**matrix** support
      (element-wise, scalar broadcast). Operators are inline `Math op` predefines
      (`+ − * / ^`, parens 18/19); `^` takes an integer exponent or `0.5` (sqrt).
      `Add/Subtract/Multiply/Divide/Negate` were removed (incorporate into the expression);
      `Modulo/Minimum/Maximum/Absolute/Limit` stay instructions. The editor offers an
      Operators group for a `Set` operand and validates balanced parens + alternation.
- [x] **Editor UX**: instruction lines **wrap** instead of scrolling horizontally; symbol chips
      are **colour-coded by category** (input/output/variable/constant/predefine/instruction)
      with a legend; symbols can be **reordered within a line** by long-press + drag onto
      another symbol.
- [x] **Editor UX follow-up**: symbols are reordered by a normal **drag** (was long-press);
  each operand/destination shows a **role hint** under it (e.g. Limit: value/min/max,
  Transform: rot/offset X/offset Y/scale X/scale Y/skew, Select: condition/if true/if false)
  plus a hover tooltip with the full symbol.
- [x] **Logic/comparisons in the processor + boolean flow**: the expression gained `AND OR XOR
  NOT`, `== != < <= > >=` and `%` (Modulo); comparisons/logic are scalar and yield a Bool.
  `If` / `While` / `Wait until` now take an expression that yields a Bool (`While i < 10`), so
  the standalone `And/Or/Xor/Not/Compare*` (`Shift*`) instructions were removed - only `Select`
  remains in Logic. Shifts are not supported. The lid script dropped its `cond` variable
  (now `While (now - t0) < BLINK_MS`).
- [x] **Vector/matrix functions + transform helper**: prefix functions in the expression -
  `size v` (Euclidean norm), `transpose m` (matrix R×C -> C×R), `dot a b`, `cross a b`
  (Vector3 -> Vector3) - plus a standalone **`Transform`** instruction
  (`Transform m = rot, ox, oy, sx, sy[, skew]`) producing a 2×3 matrix in the render's
  Position format (rotation in radians). The eye/lid scripts now build their Position with
  `Transform` instead of `IDENT` + Compose.
- [x] **Cleanup / streamlining round**: removed the retired instruction defines
  (`Add/Sub/Mul/Div/Neg` math ops, the standalone logic ops except `Select`, unused
  `SCRIPT_FIELD_*`/`SCRIPT_SYMBOL_SIZE`) and the now-unreachable vector-math branches (only
  `Modulo/Minimum/Maximum/Absolute/Limit` reach a container destination; arithmetic goes
  through the `Set` expression). `ScriptsTick`/`HandleScriptResponse` walk only the loaded
  slots (64-bit active mask); the expression evaluator resolves elements through
  `ScriptVectorElement` and folds in place (no `ExprValue` copy); the app's full/compact label
  helpers were unified, `Select`'s condition no longer carries dead If/While/Wait branches,
  and the instruction picker no longer recommends the removed ops. The 1,892-line editor was
  split into `script_editor_page` + `script_symbol_picker` + `script_instruction_picker` +
  `script_value_dialog`, with the shared `ScriptValueCategory` moved to `script_draft.dart`.
  Behaviour unchanged; re-verified on hardware (`hil_script_vm_test`, the four setup scripts).
- [x] **Dynamic-memory hot path**: `DynamicBlockDescriptor::SetEntry` overwrites an existing
  entry **in place** when its size and persistence are unchanged, skipping the tail append +
  full-space compaction (two `malloc`/`free` pairs per write). The scripts rewrite the same
  render matrices every tick, so this is the core loop's hottest write. Size/persistence
  changes still take the append + `RebuildSpaces` path. Verified on hardware
  (`hil_dynamic_persistence_test`, `hil_led_display_test`, `hil_subscriptions_test`,
  `hil_backup_test`).
- [x] **Bug-hunt fixes**: `Number::RoundToInt` rounded **every negative value down by one**
  (`-1.0 -> -2`, `-0.4 -> -1`); it now adds half and floors for both signs (guarded 32-bit, so
  the DAS pulls in no 64-bit helper). `Get time` is now **limited to integer destinations**
  (Index/Uint32) in the VM, the editor's picker + validator and the lid setup script (a Q16.16
  Number overflows the absolute ms count past ~32767 ms). The Log database growth commits each
  `realloc` as it succeeds (a partial failure used to leave `LogBuffer`/`LogUsed`/`LogSeq`
  dangling and then double-free). A malformed `DT_XX` name length is **rejected** instead of
  overflowing `DynamicBlockDescriptor::Name`. The Register write path **clamps
  `BlockMeta.Size`** to the value bytes actually present (the System-Name path already did) and
  the app declares the real value length for script entries; a short write now fills the rest
  of a fixed-size input (spaces for strings, zero otherwise). `Storage_FlashErase` and the
  file read/write clamps use overflow-safe bounds. New regressions: HIL negative rounding +
  `Get time` type, malformed-DT and oversized-write (both verified to fail before the fix),
  short-string padding; app unit test for the `Get time` destination. All 10 HIL suites +
  the four setup scripts re-verified on hardware.
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
- TimeSync accuracy (target: within 10 ms of the core):
  1. The node applied the reply's offset with `TimeOffsetMs += offset` while sending/reading
     RAW `TimeFromBoot()` timestamps, so `offset` was the ABSOLUTE core-node difference and
     accumulated on every sync - the node's clock drifted and, once the accumulated offset
     exceeded the sync interval, the correction itself satisfied the re-sync gate and caused
     a TimeSync storm. All four timestamps now use the SYNCHRONIZED `Now()` (NTP-correct with
     `+=`), and the re-sync interval is measured in raw time so a correction cannot trigger
     the next sync.
  2. Rate discipline (`Core/Functions/SysFunctions.h`): a step-only offset is seconds off
     again within one interval (the DAS's internal RC drifts ~1%), so the node also estimates
     its clock drift (Q16.16, from the offset change since the previous sync) and extrapolates
     the offset between syncs; `Now()` applies it. A large step (core restart) is treated as a
     discontinuity: the offset is stepped but the drift (a property of this oscillator) kept.
  3. The re-sync interval is **60-75 s** (30 s warm-up first) rather than the docs' 2-3 min:
     the DAS's HSI drift changes by ~0.02% between syncs (~10 ms per 60 s), so 60 s is needed
     to hold the target. `tamu_hardware_verification_test` waits for convergence and asserts
     the node clock is within 10 ms of the core (measured ~3 ms).
- TimeSync reworked to the documented model: **synchronized-device initiated**. Nodes (and
  a core syncing to the longest-running core) send Device CID 3 and apply the offset to
  their own clock; the core only answers and never pushes offsets. Removed the old
  `TimeSyncService` push + undocumented CID 4; added `CoreTimeSyncService` (core
  self-sync via Core-discover) and DAS periodic re-sync (2-3 min, jittered).
- Third cleanup round: fixed the SNDB serial validation (`int.tryParse` overflowed 64-bit),
  capped the device-name editor at 16 bytes, made `formatValue` size-flexible for vectors,
  gated the System version decoder, fixed the DAS `Storage_FlashWrite/Read` bounds, the
  `RebuildSpaces` OOM guard, unused-variable/dangling-else/sign-compare/deprecated warnings,
  and a batch of dead code (`LogEntry.detail`, `RegisterValueSlice`, `AppStrayFrames`).
- Second cleanup round: clamped the System Name write to the documented 16 bytes (the
  old clamp of 24 could write the terminating NUL one byte past `DeviceNameBuffer[24]`),
  routed the TimeSync response through `PacketFinalize` instead of a hand-rolled CRC,
  removed dead code (`AlignTo4`, `MakeBlockInfo`, `Pulse` TEMP-DEBUG, `RemoveBlock`,
  the unused dynamic-block type picker), reused `RegisterClient` in `DeviceDatabase`,
  made `dataTypeLabel` delegate to `dataTypeWord`, and fixed every stale doc-path/format
  comment (SYSMEM/DYNMEM references, wrong `Docs/...` locations).
- Packet wire order now matches `Docs/RSBus and Packets.md` (CRC8 | Flags | Priority |
  Length | SRC | CMD | TGT | TRID | Payload); static_asserts pin the offsets and the app
  mirrors it. **Wire-breaking: Tamu + DAS + app must be flashed together.**
- Dynamic memory files: the app now recognises `DT_<hex2>` (table) / `DV_<hex2>` (values)
  and decodes the DT table; the legacy `SYSMEM`/`DYNMEM`/`KEYMEM` viewers were removed.
- System NetID (field 7) write is accepted: the value is stored to STATLOG for the next boot
  without changing the live NetID (docs: applies only after reboot), so the backup can
  restore it and the app link is not broken. 0/0x3F are rejected.
- Review/cleanup round: fixed the SUBREQ viewer entry size (26 B) and the u8 layout header,
  removed the `BlockType` `none`/`system` value collision and the dead `DataType.idx` /
  `FieldFlags.valid`, consolidated the System-block schema (`core/system_schema.dart`) and
  address/hex/BlockInfo helpers, guarded every HIL `setUpAll`, wired `AppDiagnostics` into
  the Log page, documented the remaining doc-vs-code gaps in `Issues.md`, and made the
  Subscriptions CID 1 response return the current value per docs.
- LED display layout file renamed to `LAY_1` (firmware `Blocks/Vysi1Display.h`): the
  preload migrates a `VYSIV1` file (keeping any customization) or creates it from the
  compiled-in 11x10 grid, and removes the obsolete `LAY5X5`/zero-padded `VYSIV1` records
  (`DeleteFileExact`); `LayoutFile` now defaults to `LAY_1` and `Vysi1BootLayout` re-applies
  the file after the boot recall. App layout viewer fixed: the header is u8 width + u8
  height (2 bytes), not uint16 x2 - the old parser rejected the real file.
- Storage name handling fixed (firmware `Core/Functions/Storage.h`): records are space-padded
  but lookups/deletes/rename-invalidation compared them against NUL-terminated C strings, so
  short names ("SUBREQ", "DT_000") never matched - every save appended another record and the
  oldest was read. Now `NameMatch` packs the plain name first, `CreateFile` refuses an
  existing name, `DeleteFilerecord` clears all matches, `FindInFiletable` prefers the newest,
  and `Init` runs `DeduplicateFiletable` + `RemoveObsoleteFiles` (deletes the pre-release
  `DYNMEM`) to heal existing devices.
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

## 4. Android app (`Docs/App/General info.md`: Android = BLE)
- [x] **Platform gating**: `core/platform_caps.dart` (`isAndroid`/`isMobile`/`supportsUsb`
  from `defaultTargetPlatform`, test-overridable). The manager defaults to the BLE source on
  Android, skips USB enumeration, and refuses USB links off desktop; the Connection source
  menu only offers BLE there.
- [x] **Storage / file IO**: Android settings persist via `path_provider`
  (`getApplicationSupportDirectory`); `main()` awaits the async settings load. Backup save
  and Storage download use `file_picker` with `bytes:` so the SAF writes them on mobile
  (the old `$HOME/Downloads` / path-write path was broken on Android). New
  `core/host_files.dart` holds the pick/save helpers.
- [x] **BLE permissions**: `permission_handler` requests `BLUETOOTH_SCAN`/`CONNECT` on the
  first scan (maps to the legacy location permission on Android 11 and below); a denied
  request stops scanning, and the Connection page shows a banner with a settings shortcut
  and an Android Bluetooth-off/unsupported warning. Manifest cleaned (dropped
  `MANAGE_EXTERNAL_STORAGE` + legacy storage permissions; added the BLE feature + versioned
  location permissions).
- [x] **Adaptive phone shell**: below 600 dp the shell uses a navigation Drawer (hamburger on
  the four tab pages); wider screens keep the NavigationRail. Desktop-sized dialogs/panels
  (`DialogBody`) now shrink to the phone width.
- [x] **Host tests**: `test/android_platform_test.dart` (capability gating, BLE-only source,
  USB refusal, file IO round-trip, `DialogBody` shrink, compact-shell decision). Full suite
  76 pass; `flutter build linux --debug` still builds.
- [x] **Android build verified** with JDK 21 (`flutter config --jdk-dir=/usr/lib/jvm/java-21-openjdk`):
  `flutter build apk --debug` and `--release` both succeed. `aapt2 dump badging` confirms
  minSdk 24 / targetSdk 36, the BLE feature, the BLE runtime permissions and no storage
  permissions. `permission_handler` is pinned to `^11.3.1` (its Android impl 12.1.0 uses
  compileSdk 34); 13.x pulls `permission_handler_android` 14.1.0, which needs `compileSdk 37`
  and the SDK ships that platform as `android-37.0`, which AGP 8.11 cannot resolve.
- [x] **Beta identity**: both Android builds are labelled **Tamu App (beta)** and use the
  application id `tamu.app.beta` (`applicationIdSuffix = ".beta"`), so the beta installs
  alongside the previous `tamu.app` release instead of updating it. Remove the suffix to
  promote it.
- [x] **Register System block display fix**: the Register page hid every block whose meta type
  equalled the dynamic "None" tombstone (`0x00`) - which also hid the System block, whose
  meta type is `0x00` (`BlockType::System`). The filter (`isHiddenRegisterSlot` in
  `core/types.dart`) now checks the slot type, so the System block shows again while empty
  dynamic slots stay hidden. Regression test `test/register_slots_test.dart`.
- [ ] **On-device verification** (pending a phone): runtime permission prompt, BLE
  scan/connect/MTU, SAF backup save + restore, download, and the drawer on a real phone.

## 5. Evaluation setup (`Docs/Current setup.md`)
- [x] **Setup builder** (`app/test/current_setup.dart`): builds the whole scenario through the
  existing clients - dynamic block 0 "Subscriptions" (four DAS value targets), dynamic blocks
  1/2 "Left Eye"/"Right Eye" (8-part render dictionaries: white fill, solid green iris,
  double-parabola pupil, half-fill lid), the display render-block/layout wiring, the fan duty,
  the Acc&Gyr sampling/filters, the DAS sensor types, four `DeltaPeriodic` subscriptions
  (each DAS ch1 NTC -> temp field, ch2 LDR -> lux field), and the four scripts.
- [x] **Scripts** (built as `ScriptDraft`s, uploaded to `SCR_00..SCR_03`, load-on-boot +
  run-on-load): 1 temperature -> fan duty (average of both NTCs), 2 gyro XY -> iris/pupil
  `Position` matrices (pupil moves 2x the iris), 3 lid blink (10 s open, 200 ms close/open
  sweep), 4 LDR -> display brightness (each display uses its own DAS LDR).
- [x] **Verification** (`app/test/hil_current_setup_test.dart`, 9 tests): block/field
  structure, display wiring, four subscriptions, DAS values reaching the block (measured
  26.1 degC / ~49-76 lux), all four scripts Running/Waiting (not Error), fan duty 30 %,
  valid animated 2x3 render matrices, and the semantic backup round-trip.
- [x] **Artifact**: `Tamu_current_setup.zip` in the project root - the app's semantic backup
  (one JSON per device: core + 2 DAS), restorable through the app's Backup tool.
- [x] **Device tuning round 1** (applied to the builder): display mounting rotations preserved
  (left ~180 deg, right ~5 deg); left-eye look copied to both eyes (dark-green solid iris,
  iris fade 0.6, pupil half-size 2.4x5.0, lid fade 4.0); iris+pupil base offset inward+up;
  brightness now **increases** with ambient light and the displays use the **crossed** LDRs
  (left display <- right DAS LDR); blink movement shortened to 100 ms; the lux subscriptions
  got a larger deadzone (10 lux) and a longer period (2 s) so the noisy LDR stops streaming.
- [x] **Device tuning round 2**: the vertical eye offset is flipped ("up" is negative y in the
  render space of the mounted displays); the iris is now a `GradientLinear` (left brighter ->
  right darker) whose `Position` follows the pupil (the eye script writes the texture Position
  from the pupil matrix); the lux subscriptions were sped back up (period 500 ms, deadzone
  2 lux, min 200 ms) and the brightness script delay cut to 100 ms, so the brightness reacts
  in ~0.6 s without streaming.
- [ ] **Tuning** (later): temperature->duty curve, gyro->pixel scale, and the brightness
  range (kept low to avoid a brown-out), plus a physical check of the eyes/lid.

### Notes
- The DAS ch1 NTC is a **100 kohm** part (`MeasNTC100K`); writing `NTC10K` misreads it (the
  auto-range jumps to 330 kohm and the temperature reads ~-18 degC). The builder uses the
  firmware default.
- The LED strips can brown out the board at high brightness; the builder clamps the displays
  to 5 % first and caps the brightness script at 15 %.
- **A `While` re-reads its operand each iteration, so a condition computed once before the
  loop never updates.** The lid script originally did `cond = step < STEPS` before the While
  and then looped forever (the lid jammed shut, `step` ran to thousands). The comparison is
  now recomputed inside the loop. Worth surfacing in the editor/VM docs.
