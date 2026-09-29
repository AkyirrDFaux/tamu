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
- [x] **Script (un)loading from within a script** (A10 part 1, done): service ops 7/8 mirror
      management CIDs 1/2, with a self-(un)load guard. **Macro-style cross-script calls** are
      still not implemented - the encoding and the waiting semantics are unspecified, see the
      proposal in `Issues.md` and TODO A10 part 2.
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
- [x] **Current setup v3** (`Docs/Current setup v3.md`, supersedes v1): the eye render blocks
  gained a 9th part - a **Circle Cut** that carves the iris into the dark-mode "edge only"
  ring (`Vysi1Display::MaxCachedFields` 8 -> 12). The blocks are built in **light** mode and
  script 4 switches each eye to dark on its own (black background, ring enabled, light-green
  wider pupil). The four scripts now take their tunables as **inputs** (Register/UI-drivable,
  with slider/toggle specs): temperature target + P constant (`duty = clamp(P*(temp-target),
  0, 100)`, both-NTC average), eye offset (Vector2, x mirrored for the right eye) + sensitivity
  (Matrix 2x3, a plain XYZ->XY matrix multiply via `Extract`), blink delay (10 s) + movement
  time (200 ms), and manual/auto light-dark + per-eye manual selection. New HIL coverage: a
  `Cut` ring and a 9-part scene in `hil_led_display_test`, and the setup test drives the mode
  (dark -> light) and the P controller through the inputs. Fixed `ScriptClient.writeEntry`,
  which reported a failed key-0 write as success and a successful key != 0 write as failure
  (the CID 2 reply is a payload echo, not a status byte).
- [x] **LED transfer curve + ring look**: `Vysi1Display`'s `GammaTable` applied `in^(1/1.8)` -
  the *brightening* direction - so dark colours were badly lifted (32 -> 80). It now applies
  the stated gamma in the correct direction (`255*(in/255)^1.8`, 32 -> 6). The per-frame
  Brightness is applied **after** the curve (it used to scale first, so "5%" landed at ~19%
  duty); the same brightness value is now the true LED duty, so the setup's 5-20% range reads
  ~2x dimmer than before. The dark-mode ring cut was widened (6.4 -> 7.2 px inner diameter) for
  a thinner "edge only" band.
- [x] **v2 look capture**: the eye values hand-tuned on the device (left eye) were folded into
  the builder and applied to both eyes - the iris gradient brightened to (0,242,0)/(0,153,0)
  and the ring's inner (Cut) edge given `Fade = 0.6` (it had been left at the default, so the
  ring's inside was hard). The pupil now keeps **one** size (the original 2.4 x 5.0) in both
  modes, and the dark-mode pupil is a **desaturated dark green** (120,150,120) rather than a
  wider light green - both matching the updated `Docs/Current setup v3.md`. The captured
  values are pinned by setup-test assertions so they cannot drift.
- [x] **LDR calibration (v2)**: the lux subscription deadzone tightened to **0.1 lux**
  (`luxDeadzone`), the dark/light line to **~1 lux** (`darkLux`), and the brightness map to
  **5..70 %** over **0..50 000 lux** (`luxBrightMin`/`luxBrightMax`/`luxSpan`). The display's
  initial value is now the floor (brightness == LED duty after the transfer-curve fix), and the
  setup test bounds the driven brightness to the band.
- [x] **Delta-subscription scalar deadzone**: the trigger never applied the deadzone to scalar
  Numbers - `SubscriptionsDeltaHash` returned the raw 16.16 bits, so a noisy scalar (the LDR
  lux) sent on every bit of jitter (up to the minimum interval) while a stable one (the NTC)
  stayed quiet, and the deadzone value had no effect on scalars at all. Scalars
  (Number/Index/Uint32) now compare the change magnitude against the deadzone (Docs: "Checks
  distance ... Last scalar value"), with the last sent value kept in Hash/Hashlike. New HIL
  test drives a script input as the source: +1 with deadzone 5 is not sent, +10 is (verified
  to fail before the fix). The root cause was compounded by the requester confirming **every**
  value update: the confirmation carries an FNV hash and `HandleProviderConfirmation` writes it
  straight into the provider's Hash/Hashlike, clobbering the delta trigger's last-scalar state
  on any **remote** provider (a self-loopback never confirms, so a core-local test passed while
  the DAS kept streaming). The requester now confirms only for `OnChangeConfirm`, per the docs
  ("request is confirmation if needed") - which also removes a redundant packet per update for
  every other trigger (edge counters were being clobbered the same way). A second HIL test
  checks that the DAS provider's Hash equals the last sent scalar (verified to fail before).
- [x] **Delta-subscription vector deadzone**: the vector path sent on any change of its
  subresolution hashlike. Per the docs ("Checks distance (euclidian for vectors)" ... "sends
  sooner if the threshold is reached") it now gates on the euclidean distance: the provider
  keeps the last sent vector (3 int32 axes; +48 B on the DAS's 4-entry table) and sends when
  the squared distance reaches the squared deadzone (saturating 32-bit maths, no libgcc on the
  DAS). The subresolution pack stays the reported hashlike. New HIL test (AccGyr source, long
  period): a huge deadzone sends only the first value, a tiny one keeps updating (verified to
  fail before).
- [x] **LDR lux recalibration** (`datasheet/dsh.520-084.1.pdf`): the board's part is the GL55
  **5-10 kOhm** variant (R at 10 lux), and `MeasLDR10K` no longer uses the old `180/R`
  approximation (which assumed R10 = 18 kOhm, gamma = 1). It now implements the datasheet
  relation `R(E) = R10*(E/10)^-gamma` => `E = 10^(1 + (log10(R10) - log10(R_ref) -
  log10(ratio))/gamma)`, with `LDR_R10_KOHM` / `LDR_GAMMA` as the calibration knobs
  (defaults 7.5 and 0.6, the latter read off Fig. 2). Everything stays in the log domain so no
  intermediate overflows Q16.16, and `Number.h` gained `log10`/`pow10` (the antilog is a binary
  expansion of precomputed `10^(2^-i)` constants, multiplies only - the DAS pulls in no
  libgcc). Verified with a host build of `Number.h` against the closed form (agrees within the
  existing `log` approximation's ~10%). The DAS reports lux as a Q16.16 Number so it saturates
  at ~31623; the setup's `luxSpan` is 30000 so the 70% cap is reachable. Flashed to the DAS
  and verified on hardware (the same ADC sample: firmware 3.65 lux, closed form 3.98, old
  formula 13.8). Still wants a lux-meter calibration of R10 (the part is only specified as
  5-10 kOhm).
- [x] **Brightness curve**: the lux -> brightness map was linear, so a phone flashlight
  (~10k lux) only reached ~27%. It is now `MIN + (MAX-MIN)*(lux/luxSpan)^0.25` - a compressive
  curve (with `^0.25` spelled as two `^0.5`s, since the expression only has `^0.5` and integer
  powers). Measured on hardware by driving the lux inputs directly: 1 lux -> 9.8%, 100 -> 20.6%,
  1000 -> 32.8%, **10000 -> 54.4%**, 30000 -> 70% (the cap).
- [x] **Dark-mode look revision**: dark mode now uses a regular **filled** iris - the Circle
  Cut ring is gone, so the eye block is 8 parts again (the eye script no longer writes a ring
  position) - with a really dark green gradient (0,90,0)/(0,45,0) and a slightly lighter
  desaturated dark-green pupil (140,170,140). The pupil size stays 2.4x5.0 in both modes. A
  mode change now rewrites the iris gradient, the background and the pupil colour. Added
  `app/test/current_setup_test.dart` (host-only) so the setup builder's script structure and
  constant wiring are validated without hardware; verified the switch on the device (dark: bg
  black, iris (0,90,0), pupil (140,170,140); light: bg white, iris (0,242,0)) and that the two
  eyes switch independently.
- [x] **Dark-mode iris/pupil tuning + pupil-edge fix**: captured the tuned dark colours (iris
  `0x008C00`/`0x006600`, pupil `0xB3AD74`) and fixed the "edges go to a completely different
  tone" artefact. It was **not** the gamma table (that is per-channel identical, so it cannot
  shift a hue): the pupil's half-height was **5.0** against the iris radius **4.5**, and the
  eye script wrote the pupil at **twice** the iris's travel (half-offset parallax), so the
  pupil's tone slid outside the iris circle - most visible once the pupil became a light khaki
  on black, and only when the eye looked sideways ("sometimes"). The iris, pupil and iris fade
  now share **one** position and the pupil's half-height is **4.0**, so it stays inside. Both
  are asserted in the setup test (shared position + `halfH < iris radius`).
- [x] **Correct compositing order (low-brightness colour quality)**: the renderer filled and
  alpha-blended the authored (sRGB-ish) colours and applied the transfer curve only at the very
  end, so blends and gradients mixed *encoded* values and low-brightness partial-alpha edges
  quantised to the wrong tones - which is what the "edge goes to a different tone" report was
  really about. Colours are now linearised **once**, as they enter the render buffer
  (`Vysi1Display::Linearise`), so all alpha compositing happens in linear light and the frame
  ends with just the Brightness scale. Flat (alpha-1) fills are numerically identical to
  before, so the tuned colours are unchanged; only blended/anti-aliased pixels improve. (The
  Contrast/Brightness *effects* now act on linear values too, which is physically right but
  visually stronger.) The pupil size was restored to 2.4x5.0 as requested.
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
  (The Android on-device verification item that sat here was dropped as a non-issue by the user.)

## 5. Evaluation setup (`Docs/Current setup v3.md`)
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
- [x] **Static settings now persist across a reboot**: `applyCurrentSetup` wrote the display
  render-block/layout, fan, gyro and DAS measurement settings but never issued a Register
  **Save** (CID 3), so they only lived in RAM - a reboot reverted the displays to render
  block -1 and the eyes stopped rendering (the "config did not persist" report). Added
  `RegisterClient.saveStatic` (CID 3, field 0xFF) and `saveStaticBlock` in the builder, which
  saves each configured static block and throws if the save fails. Verified by a new
  reboot-persistence test in `hil_current_setup_test` (the core is hard-reset, then the
  display render blocks, layout, render-dictionary values, dynamic blocks, scripts and
  subscriptions are all checked).
- [x] **STATLOG save truncated the log after every update (firmware bug).** `LogEntryWrite`
  "replaced" an existing (block, field) entry *in place* and returned a length ending at that
  entry; `HandleStaticSaveRecall` then wrote back exactly that many bytes, silently dropping
  every entry that followed. So saving a block whose entry sat in the middle of the log wiped
  the saved settings of the blocks after it - the saves still reported success. This is what
  swapped the displays and moved the fan: the saved instance-1 render block was truncated away
  by a later save of instance 0. Fixed in `Register.h`: remove the old entry (shift the tail
  down) and append the new one, so all other entries survive. Verified by the reboot test:
  both display instances now restore (`display 0 -> Right Eye`, `display 1 -> Left Eye`) and
  the STATLOG holds entries for both instances.
- [x] **Render-dictionary entries are now Persistent.** The eye blocks were built with flag 0,
  so `saveDynamic` stored the entry *structure* but the values were zeroed on reload (only the
  scripts' positions/colours came back, not the shapes, texture types, sizes or fades - the
  scene rendered wrong after a reboot). `setDynEntry` now sets `FieldFlags.persistent` by
  default; the live-value "Subscriptions" block opts out. The reboot test asserts the restored
  shapes/texture types/pupil size/lid fade.
- [x] **Rig mapping: displays swapped L<->R and the fan moved to the second output.** The
  physical left display answers on core instance 1 and the fan on PWM instance 1:
  `dispLeft = 1`, `dispRight = 0`, `fanInst = 1`. The mount rotations are keyed by CORE
  INSTANCE now (`dispOffsetInst0`/`dispOffsetInst1`) - a rotation is a property of the panel and
  its mount, so it stays with the output instead of following the side. The two constants were
  also swapped to match the device's current values: the panels are mounted differently
  (instance 0 ~5 deg, instance 1 ~175 deg), so a single rotation for both is wrong. The LDR
  crossing (left eye <- right DAS LDR) was left as-is; if the panels physically moved, that
  pairing should flip too.
- [x] **Gyro reaction sense inverted (rig fact).** The mount's gyro axes read opposite to the
  render space, so the eye script negates the two in-plane components (`gx`/`gy` = 0 - g) right
  after reading them; a tilt now moves the pupils the way the rig leans. The axis mapping
  (gx->x, gy->y) and the right-eye x mirror are unchanged. Guarded by a host test that finds
  both negations in the emitted script.
- [x] **New brightness curve.** `lux -> %` is now `MIN + RANGE*((1-w)*t + w*t^4)` with
  `t = (lux/10000)^0.2` (a mix of x^0.2 and x^0.8, w = 0.45), clamped to [5, 70]. Measured on
  the device by silencing the feeds and driving the lux inputs: 0 -> 5.0 %, 200 -> 20.0 %,
  1000 -> 29.2 %, 3000 -> 41.7 %, 5000 -> 51.0 %, 10000 -> 70.0 % (cap). Replaces the old
  `^0.25` / span-30k curve whose cap was only reached at 30k. Covered by a HIL test that drives
  200 and 10000 lux and asserts ~20 % / 70 %.
- [x] **Script VM: general fractional powers.** `^` used to accept only 0.5 and integer
  exponents (which is why the old curve spelled `^0.25` as two square roots). `ScriptExprPow`
  now expands any exponent into its binary fraction and multiplies nested square roots
  (`x^(sum 2^-k) = prod x^(2^-k)`), so `x^0.2` works. First attempt advanced the root chain
  only when a strictly-lower bit was set, leaving it one sqrt too shallow - fixed by testing
  every bit at or below the next one. Exponent literals are Q8.8 (1/256 resolution: 0.2 ->
  0.19922, negligible here).
- [x] **v3 emote system** (Docs/Current setup v3.md). The eye render block grew to **9 parts**:
  pupil base (A) + pupil modifier (B) + the pupil fill, so an emote can build a shape from two
  geometries (B is a no-op - Shape None + Add - for the emotes that need one).
  - **Script 2 "Eye movement"** is now a pure calculator: it publishes the per-eye pupil
    transform as **Output 0/1 (Matrix 2x3)** and writes no render fields.
  - **Script 5 "Emote selector"** (new) owns the eye geometry: it reads those outputs and
    writes the iris, iris fade and both pupil positions (pupil A +tilt, B -tilt) plus the
    per-emote shapes. It is loaded into a **higher slot** than script 2, so it consumes the
    same pass's fresh offset and the displays render in that pass - no cross-script loop delay.
    Writes happen on change with a 0.05 px deadzone (the gyro jitter is below it, so at rest
    nothing is written and the mask cache stays valid).
  - Emotes (custom enum input): Normal = the tuned DoubleParabola; Happy = a triangle with a
    smaller cut triangle (hollow caret); Dead = two rectangles added at +/-45 deg (the tilt
    rides in the Position, radians); Annoyed = DoubleParabola + the lid held 25 % closed.
    Pupil colours stay with script 4 (mode colours).
  - An emote change is applied **behind a forced blink**: the emote script sets script 3's
    `Force close`, waits for the lid to reach its closed position (or a 2 s timeout), swaps the
    pupil/tilt/Max opening, then releases. All non-blocking (a state machine, not a wait loop).
  - **Script 3 "Lid timer"**: + Input 2 `Force close`, + Input 3 `Max opening`; the blink is now
    close -> hold while forced -> open to the max-opening rest -> interruptible delay, and it
    parks at the rest position each cycle.
  - **Custom enum inputs (app)**: `ScriptInputSpec.options`, UI-info **version 2** (labels per
    input; v1 blobs still parse), a real dropdown in the input panel, and `_inEnum` in the
    builder. The firmware reads the function name from v1 *or* v2 and ignores the rest.
  - Tests: host (5 scripts validate, enum labels, script 2's outputs/lack of writes, a
    file round-trip of the names/labels) + HIL (every emote's shapes in both eyes, the
    +/-45 deg tilt, the forced blink, the annoyed lid rest, colours untouched, 5 scripts in
    the archive). The setup suite dropped the fan/P-control and the lux->brightness anchor
    tests (the fan is not connected and the curve is verified by driving lux) and the emote
    waits were shortened; a bare `run_hil_tests.sh` now runs just the setup suite (the
    feature suites are listed in the script for when a change touches them).
- [x] **Emote tuning round 1** (from the first look): Happy's triangle band is thicker
  (outer 6.6 / cut 3.4 instead of 5.6 / 4.0); Dead's bars are thicker and shorter
  (`deadBarWidth` 2.4, `deadBarLength` 7.0); the annoyed lid rests ~2 px lower
  (`lidAnnoyedOpen` 0.58); the fully-closed lid line moved past the bottom edge
  (`lidClosedTy` 6.5) so it covers the last LED row. **UI info v1 support was dropped**
  (app parser and the firmware's name read now accept version 2 only).
- [x] **Emote tuning round 2** (second look) + the scripts page/editor fixes:
  - **Happy** is now a true caret: apex 55 deg (pointier), outer 7.0 / cut 3.0 (thicker band),
    and the cut triangle is **shifted 1.9 px down** (a per-emote `CUT_OFFSET_HAPPY`, applied to
    the pupil B translation) so it removes the base and leaves the two upper edges instead of a
    triangle ring.
  - **Dead** bars rebalanced to 1.9 x 9.0 px (2.4 x 7.0 made the arms merge into a blob; the
    matrices were already correct - both bars share the centre at +/-45 deg).
  - **Scripts page**: `ScriptIoSection._row` only routed slider/toggle/button to the
    spec-driven control, so every other input fell through to a generic "Input N" tile that
    opened the numeric value editor. Dropdown (and `auto` with labels) now render the labelled
    dropdown, and titles come from the file's input names (`ScriptIoSection.names`, wired from
    `scripts_page`).
  - **Editor**: the value dialog gained an "Enum values" section (add/remove/name) so a custom
    enum can be defined; the labels are stored in the UI info and the value stays the index.
  - Rebuilt the Linux app (`build/linux/x64/debug/bundle/tamuapp`) - the earlier symptoms were
    the stale binary, which only understood UI-info v1.
- [x] **Emote tuning round 3** + the transform fix (from the second look):
  - **The real cause of the cross drift**: the renderer samples the geometry mask *forward*
    (`pp = Position * coord`), so a rotated Position also rotates its translation and the
    shape's centre lands at `-L^-1 * t` instead of `-t`. For a pure translation that is
    invisible (everything tuned so far), but with a rotation the two bars drifted apart. Fixed
    in the Transform (firmware `ScriptExecTransform` and the app's `Transform23.toCells/
    fromCells`): the stored translation is pre-rotated by the linear part, so the centre stays
    put for any rotation. Verified on the device: the Happy and Dead pupils' centres now equal
    the iris's to <0.01 px (was ~1 px with the rotation).
  - **Happy**: the whole pupil is lifted 1.5 px (`happyOffsetY`, applied to both pupil parts),
    on top of the cut triangle being pushed down 1.9 px (the caret). Asserted in the HIL test.
  - **Dead**: bars 1.9 x 9.0 (2.4 x 7.0 read as a blob).
  - **Normal <-> Annoyed no longer blinks**: only a change that alters the pupil *shape*
    (Normal/Annoyed vs Happy/Dead) forces the blink; a lid-only change is applied at once
    (a shape class compared against the applied emote's).
  - The emote script was one `EndBlock` short after the rewrite (the run ran off the end and
    the script sat in Finished, so no emote ever applied) - caught by extending the script-state
    test to slot 4, then by counting the flow blocks.
  (The "Tuning" item that sat here - temperature->duty curve, gyro->pixel scale, brightness
  range, physical eye/lid check - was dropped as a non-issue by the user.)

### Notes
- **A script loop advances at most once per main-loop tick.** `ScriptRun` stamps every line it
  executes and yields when a line is revisited within the same tick, so a `While` body that
  contains no `Delay` runs one iteration per tick. The lid's 200 ms movement therefore takes
  200 ticks (~0.2-1 s) and the *nominal* `Movement time` is a lower bound, not a real duration.
  Fine here, but worth knowing before tightening any blink timing.
- **Fixed-point `^` accuracy.** A non-integer exponent is evaluated as a product of nested
  square roots (see the entry above). Each `sqrt` truncates, so a long chain can drift by a
  fraction of a percent - measured under 0.1 % for the brightness curve's `x^0.2`/`x^0.8`.
  Fine for a brightness curve; keep chains short where precision matters. Exponent and weight
  literals are Q8.8, so they are quantised to 1/256.
- The DAS ch1 NTC is a **100 kohm** part (`MeasNTC100K`); writing `NTC10K` misreads it (the
  auto-range jumps to 330 kohm and the temperature reads ~-18 degC). The builder uses the
  firmware default.
- The LED strips can brown out the board at high brightness; the builder clamps the displays
  to 5 % first and caps the brightness script at 15 %.
- **A `While` re-reads its operand each iteration, so a condition computed once before the
  loop never updates.** The lid script originally did `cond = step < STEPS` before the While
  and then looped forever (the lid jammed shut, `step` ran to thousands). The comparison is
  now recomputed inside the loop. Worth surfacing in the editor/VM docs.

## 6. Cleanup / optimization / bugfix pass

- [x] **Trig accuracy (core)**: the fixed-point `sin`/`cos` used the plain parabola
  (`Bx + Cx|x|`, ~5.6% amplitude error - visible as an oversized shape when a transform is
  rotated). Added the standard improved-parabola correction
  (`y' = 0.225*(y*|y| - y) + y`, ~0.1% error). The HIL emote test now pins the 45 deg cells to
  0.7071 +/- 0.02 (was 0.75).
- [x] **"Not Saved" active flag (core)**: a written persistent field now reports the active
  `Not Saved` flag until it is saved (Docs/Services/Register.md). Small bitset next to the
  STATLOG helpers (48 B; the DAS's RAM went 79.3% -> 81.6%), set on a static write, cleared by
  Save, folded into the read meta on both static read paths. The app already renders the `NS`
  chip; HIL regression added.
- [x] **App is only a subscription manager** (verified): `SubscriptionClient` writes the
  *devices'* requester tables; the app never registers itself as requester/provider. A guard
  test asserts `appSourceId` is never used as a subscription address.
- [x] **PWM Frequency**: verified working (1 kHz and 25 kHz accepted with the channel live and
  idle) - the old "always refused" note was stale. HIL regression added; the issue was dropped.
- [x] **Issues.md cleaned**: dropped/rewrote the stale entries (PWM, the "opposite rotation
  senses" misreading, the LDR calibration per the user, the subscription-trigger default, the
  SNDB re-register design note) and stated the app's manager-only role.
- [x] **Splits done** (mechanical, `part`-based, zero privacy/behaviour change):
  `ui/value_editor.dart` 1167 -> +scalars/containers/visual; `core/backup.dart` 889 ->
  +capture/restore; `test/current_setup.dart` 1296 -> +scripts/apply. Analyzer clean; 92 app
  tests + 16 setup + 5 feature suites green.
- [x] **`Script.h` split done** (2060 -> 30 + 6 guarded parts): `ScriptDefs.h`, `ScriptProgram.h`,
  `ScriptVm.h`, `ScriptExpr.h`, `ScriptExec.h`, `ScriptRuntime.h`, each re-wrapped in its own
  `#ifdef USE_SCRIPTS` with explicit includes. The split was verified *lossless* (the parts'
  bodies concatenate byte-for-byte to the original lines) and the core binary is byte-size
  identical (652384), so it is codegen-neutral. Both targets build.
- [x] **Splits done — all of them** (mechanical; every file verified lossless + codegen-neutral).
  Recipe: slice at top-level boundaries (checking the preprocessor nesting depth is 0 so each
  part is guard-balanced), reassemble and assert the parts equal the whole original *and* that
  braces/#if are balanced per part, then confirm both targets build with the **same binary size**
  (core 652384 B, DAS 13932 B) and the same DAS RAM/flash.
  - firmware: `Vysi1Display.h` (855) -> Vysi1Gamma/Layout/Render (this one was also missing its
    `#pragma once`); `Memory.h` (678) -> MemoryBackup/Blocks/Dynamic; `Storage.h` (948) ->
    StorageDefs/BlockFS/FixedFS (the `#ifndef USE_FIXED_STORAGE` branch is regenerated in the
    parent); `Register.h` (928) -> Defs/Enumerate/Read/Write/Persist/Dispatch;
    `Subscriptions.h` (884) -> Defs/Requester/Provider/Persist/Control.
  - app: `register_page.dart` (1748) -> register_page (633) + register_page_tiles (601) +
    register_page_edit (517); `script_editor_page.dart` (991) -> 436 + script_editor_view (312) +
    script_editor_line (249); `subscriptions_dialog.dart` (690) -> 491 + subscriptions_pickers
    (208).
  - **Technique note for the page files**: a Dart `part` cannot split a *class body*, so the
    State's methods moved into `extension on _XState { ... }` blocks in part files. Dart resolves
    an unqualified call inside the class to a same-library extension, and the extension keeps
    private access - so this is a pure move. Two gotchas found and handled: `setState` is
    `@protected` (not callable from an extension -> routed through a `_rebuild` helper on the
    State), and extension bodies cannot declare `static` members or reference the type's statics
    unqualified (the statics stayed in the class and the parts' calls are qualified).
  - A first attempt at `Vysi1Display.h` dropped its final `}`: `wc -l` counts newlines, and the
    file's last line had none. The verification now compares against the whole file, not a
    `wc`-derived prefix. Worth remembering.
- [x] **Register Current/Backup view** (docs gap, implemented): the appbar now has the documented
  Current/Backup toggle; Current shows the live RAM values with the Save actions, Backup shows
  what a Save persisted with the Recall action (per-field recall included). The stored values are
  decoded from the device's own backup files by a new pure module
  (`app/lib/core/device_backup.dart`): `STATLOG` for static/System fields and `DT_`/`DV_` per
  dynamic slot - including a `DV_` decoder the app previously lacked (it rendered `DV_` as raw
  hex). `ui/file_viewers.dart` now renders through that same module, so the two paths cannot
  drift. Fields with no stored entry are shown as "not backed up" and volatile dynamic entries as
  "not persisted", so a missing save is visible. 17 decoder tests + 7 Backup-view widget tests +
  a page toggle test; `app/test/ui_smoke_test.dart` adds the first runtime coverage for
  `ScriptEditorPage` and `SubscriptionDialog`.
  Note: the firmware's CID 0x15 "read backup" is dynamic-only, which is why static/System come
  from the file decode.
- [ ] **Optimization - per-field geometry-mask versioning** (A11, deferred to the rig batch):
  any write to an eye block bumps the block generation and the renderer recomputes all 9 masks;
  the emote script writes on gyro motion. Measured headroom says it is not urgent (the display
  readback sits at the panel cap, ~127-132 FPS), so it stays planned rather than done.
  Unverifiable without the rig. Sketch for when it is picked up: the per-frame pass still has to
  run (`ApplyGeometryField` combines the cached mask), so the win is only in skipping the
  per-LED `RenderGeometryField`; that needs a *per-field* invalidation token rather than the
  block-wide `generation` - either a per-field version array in the block descriptor (RAM per
  field) or comparing the cached geometry inputs (shape/size/position/rounding/angles/fade/
  alpha/point/noise) each frame and recomputing only the fields whose inputs moved. The latter
  needs no block-model change and keeps the host-visible behaviour identical, which is what
  makes it checkable off-device - the rig would confirm the frame time.
- Deferred by request: SNDB re-registration, the LED brightness cap, the subscription trigger
  default, the LDR calibration (considered done).
- [x] **Brightness curve re-fitted to the updated v3 table** (`<10 lux -> 5 %`, `100 -> 10 %`,
  `3000 -> 40 %`, `>10k -> 70 %`): the low end is now much dimmer than the old 200-lux anchor,
  so the curve became a *shifted* power law with a dead zone below 10 lux -
  `MIN + RANGE * ((Max(lux, 10) - 10) / 8840)^0.5721`, clamped. Measured on the device by
  driving the lux inputs: 0/5/10 -> 5.00 %, 100 -> 9.75 %, 1000 -> 23.7 %, 3000 -> 40.0 %,
  10k -> 70.0 %. Complexity dropped too: one term instead of the old 0.2/0.8 mix.
  **Bug caught on the way**: the `^` bound to the *divisor* instead of the quotient (the
  expression's precedence), which turned the curve into a linear ramp - fixed with an explicit
  group around the division. Worth remembering when writing expressions.

### Native numeric-core tests (host-only, no hardware needed)
- [x] **Host test harness for the firmware numeric core**: `firmware/test/native/numeric_test.cpp`
  + `run.sh`, compiled and run in **three** shapes - the core build (64-bit), the DAS build
  (`NUMBER_ONLY_32BIT` + `SCALAR_ONLY`) and 32-bit with Vector/Matrix (`NUMBER_ONLY_32BIT`
  alone, a combination neither target selects but which is otherwise never compiled).
  Zero dependencies (plain g++, no Unity, no network).
  Covers construction/rounding/divide-by-zero, `FixedMul32`/`MulHigh32`/`FixedDiv32` against an
  exact int64 reference (hand-picked edges + a 200k randomised sweep), `sqrt`, `sin`/`cos`
  (including the 45-degree regression and the old 5.6 % amplitude error), `atan2`, `log`/`log10`/
  `pow10`, the `Vector`/`Matrix` transforms (pre-rotated translation convention) and `Colour`
  blending. ~3600 checks per build. Run it with `firmware/test/native/run.sh`.
  This is the net whose absence let the trig regression ship in a beta.
- [x] **`MulHigh32` was wrong for two negative operands** (off by exactly 2^16): the unsigned
  decomposition needs 33 bits for `aH*bL + aL*bH` and silently dropped the carry. `FixedMul32`
  was unaffected (it only reads the low 16 bits of the high word), so it never showed on the
  device - but the function's documented contract was broken for any other caller. Rewritten
  with signed high halves (Hacker's Delight `mulhs`); `FixedMul32` output is unchanged
  (the randomised sweep proves it). DAS flash 13912 -> 13916 B.
- [x] **`Vector::operator*` double-scaled its scalar** (`Data[i] * scalar.Value` fed a raw
  Q16.16 value into `Number`'s int constructor, which shifts it again - any scalar above ~32767
  overflowed to 0). Unused in the firmware today, but public API with a comment that said the
  opposite of what the code did. Fixed to `Data[i] * scalar`.
- [x] **`atan2` accuracy fixed**: replaced the crude rational form (worst case 0.071 rad = 4 deg
  at ~163 deg) with the standard `min/max` reduction plus a cubic in `a^2`. Measured worst case
  is now **0.0002 rad** (363x better); the quadrant boundaries and `atan2(0,0) = 0` are exact.
  Affects the `Polygon`/`Star` sector lookup - the rendered look wants an eyeball with the rig.
- [x] **`log`/`log10` accuracy fixed**: centring the range reduction on 1 (target
  [sqrt(1/2), sqrt(2))) instead of [1, 2) cuts the 4-term series' worst-case error from
  **0.109 to 0.002** (59x); `pow10(log10(x))` round-trip **10.4 % -> 0.17 %**. Powers of two
  stay exact. **The DAS LDR lux output changes** (more accurate), so the LDR calibration needs
  re-checking against a reference and the DAS needs a re-flash - recorded in `Issues.md`.
- [x] **32-bit `sqrt` (`isqrt32`) fixed and now actually compiled**: it is only selected by
  `NUMBER_ONLY_32BIT` without `SCALAR_ONLY`, which no target uses, so it was dead *and* off by
  up to 2.3 %. One Newton step brings it to 0.04 %; a third native test configuration
  (32-bit with Vector/Matrix) now compiles and runs it so it cannot rot again.

### Cross-implementation contracts and host coverage (host-only)
- [x] **App<->firmware contract tests** (`app/test/firmware_contract_test.dart`, 13 tests): the
  script encoding (symbol/predefine/category enums, every inline math op, every instruction
  opcode per category, header size and slot count), the core enums (`DeviceType`, `DataType`,
  `BlockType`, `TriggerType`, `FieldFlags`, `Capability` bits) and the render dictionary
  (`Geometries`, `GeometryOperation`, `Textures2D`) are compared directly against the firmware
  headers. Extra/renumbered members on *either* side fail, so a new firmware enum member cannot
  be silently unknown to the app. Verified against a deliberate mutation (changing `BlockType.led`
  fails the test). Skips cleanly when the firmware tree is not next to the app.
- [x] **Analyzer to zero**: a nested `app/test/analysis_options.yaml` disables `avoid_print` for
  tests only (test output is deliberate), so `flutter analyze` reports **no issues** instead of
  41 infos while `lib/` keeps the lint.
- [x] **`diagnostics_schema_test.dart`**: pins the diagnostics ring (cap, oldest-first order,
  formatting, dump) and the System schema (every field/key named, unknown values fall back
  rather than throwing).
- [x] **`./test.sh`**: one entry point for everything that does not need hardware - the native
  numeric tests, the app host suite (HIL excluded) and the analyzer.
- Note: the dead-code sweeps found nothing. The firmware has no unused static functions (only
  `app_main`, the framework entry point) and the app has no unreferenced public declaration, so
  there was nothing to remove.

### DAS flash reduction + Tamu speed build (measured, step by step)
Target: the DAS has 13 928/16 384 B (85.0 %) and 1 672/2 048 B RAM; Tamu has a 3 MB app
partition with 652 KB used, so flash is free there and only speed matters.

**DAS (each step measured; baseline 13 928 B):**
- [x] **`pow10` -> range-reduced polynomial** (no table): `10^f = 2^(f*log2 10)` with a quintic
  in the fraction. Removes the 64-byte `10^(2^-i)` table and 16 conditional multiplies; 5
  multiplies instead, faster on both targets, worst-case error 0.013 %.
  **Measured: -24 B** (the function grew 40 B, the table saved 64 B). Kept for the speed.
- [x] **`LoadAllBackups` one pass instead of nested O(blocks x fields x entries) + a second
  System pass**. Same restore semantics (last entry wins, same persistent/read-only/size
  filter). **Measured: -136 B** (410 -> 272 B). This is the one genuine algorithmic win.
- [x] **`pow10`'s 2 runtime `log10` constants** noted for later; the lux path is the biggest
  remaining DAS item (measured ceiling: removing `log`+`pow10` entirely = **-504 B**, but the
  NTC genuinely needs a log, so only part is capturable).
- **Rejected after measuring** (kept out):
  - `noinline` on the dispatcher's handlers (`HandleRegister`/`Storage`/`Device`/`LogHandler`):
    **+92 B** - inlining was already the size-optimal choice, and `DispatchPacket`'s 2446 B is
    their combined bodies, not a comparison chain.
  - Unifying the 4+ copies of the STATLOG walk behind one finder: **+4/-4 B (i.e. nothing)** -
    GCC's identical-code-folding already merges identical loops. Reverted to keep the
    save/recall path untouched.
  - `HandleLogHandler` on nodes is already compiled to nothing (`#ifndef TYPE_CORE`), and
    `DeviceLog` is already a no-op on TEXTLESS builds - no hidden wins there.
- Net so far: **13 928 -> 13 768 B (-160 B, 85.0 % -> 84.0 %)**, RAM unchanged, DAS ELF still
  has **no `*di3` symbols** (NUMBER_ONLY_32BIT doing its job).

**Tamu (speed):**
- [x] **Release-speed build profile**: the env was compiling with `-Og` plus the platform's
  debug-friendly `-fno-shrink-wrap -fno-jump-tables -fno-tree-switch-conversion` (PlatformIO
  appends its flags *after* `build_flags`, so a later `-Og` won). Fixed with **`build_unflags`**
  + `-O2 -fwrapv`. `-fwrapv` keeps the fixed-point arithmetic's two's-complement wraparound
  defined (-O2 would otherwise exploit the signed-overflow UB).
  `firmware.bin` 652 384 -> **666 944 B** (+14.5 KB, still ~21 % of the 3 MB partition).
- [x] `-O2` surfaced a **false-positive** `-Warray-bounds`/`-Wstringop-overflow` in
  `CreateDynamicBlock` (GCC cannot follow the realloc'd registry; sizes are clamped and the
  source is payload-bounded). Suppressed narrowly at that site so the warning stays live
  everywhere else.
- Host benchmark of the same integer math (x86-64 proxy, 300 k iters): the per-LED
  transform+norm2 is 2.5x faster at -O2 and 3.3x at -O3 vs -Og; sqrt 2.7x; sin+cos 1.4-2x.
- [x] Remaining Tamu candidates - all now covered: render invariant hoisting (A8 parts 1+2,
  done), shape specialisation (A8 part 3, still rig-gated), `Crc8` table via `OPTIMIZE_SPEED`
  (A9, done), `-O3` vs `-O2` (A4, measured; `-O2` kept).

### Near-identical functions merged (code cleanliness)
Found with a body-similarity scan (comments stripped, identifiers/strings normalised), merged
where the result is clearer, verified by both firmware builds + the full host suite.

**Firmware:**
- [x] `Vysi1Display::IdentityAffine` was a byte-identical copy of the free `IdentityAffine23()`
  (the scan scored it 1.00) - the member now returns it.
- [x] `ScriptExprFnSize` is `sqrt(dot(v, v))` - now calls `ScriptExprFnDot` (the two shared the
  same accumulation + result shape). Reordered so Dot precedes Size.
- [x] The four subscription finders (`RequesterFindByTrid/Free`, `ProviderFindByTrid/Free`) were
  0.91-0.97 identical over two different tables - now two shared templates in
  `SubscriptionsDefs.h` plus a per-role `Occupied` predicate. **This also shrank the DAS by
  another 28 B** (13 768 -> 13 740 B), the only size win in this batch.
- [x] The two dynamic-block read handlers (`HandleDynamicBlockRead`, `HandleReadBackup`) shared
  their reply tail (field 0xFF = block meta, else one keyed entry) - now
  `ReplyDynamicBlockOrField` in `RegisterDefs.h`.
- [x] CLI `CmdPing`/`CmdIdentify` (0.99) and `CmdSave`/`CmdRecall` (0.99) - merged into
  `CmdDeviceRequest` and `CmdSaveRecall`; the printed text and command ids are unchanged.
- [x] SNDB registry access: the "read entry `i`" expression was repeated at 8 sites and the
  index-based write at 3 - now `SNDB::ReadEntry`/`WriteEntry`. The four scan functions stay
  separate (they differ in what a failed read means, and `IterNext` uses a persistent cursor).

**App:**
- [x] `subscription_client.getProviderSubscriptions`/`getRequesterSubscriptions` (0.98) - one
  `_getSubscriptionList` decoder parameterised by CID/entry size.
- [x] `script_client.enumerateInstances`/`enumerateKeys` (0.98) - one `_enumerate(level, ...)`.
- [x] `backup_value._enumRaw`/`_deviceTypeRaw`/`_blockTypeRaw` (0.87-0.91) - one `_rawFromWord`
  with a label table. **First bump: lower the similarity threshold to 0.80**; the field's
  `blockInfoBytes` (the same 4-byte packing as `uint32ToBytes`) and
  `subscriptions_page._show{Add,Edit}SubscriptionDialog` also merged.

**Deliberately left (merge would need a role flag/bool and reads worse, not better):**
`subscriptions_dialog._loadFieldsFor{Target,Source}`, `script_editor_line.addDestination/
addOperand`, `devices_page._cycle{Net,Type}`, `RequesterEntrySerialize`/`ProviderEntrySerialize`
(different wire layouts), `ScriptResolveOperandScalar`/`ScriptOperandBlockInfo` (the `scratch`
buffer's lifetime is load-bearing - returning `data` from a helper would dangle),
`PinModeOutput`/`PinModeInputPullUp` (different register writes).
Note: the 1.00-scoring int->string `switch` tables (`predefineName`, `categoryName`, etc.) are a
scan artefact - normalising string literals made distinct tables look identical; they are not
duplicates.

## 7. Size / speed / cleanup pass - consolidated plan

Baseline now: **DAS** 13 740 / 16 384 B flash (83.9 %), 1 672 / 2 048 B RAM (81.6 %), stack 576 B,
no `__*di3` helpers; **core** 666 368 B of a 3 MB partition (~21 %), built `-O2 -fwrapv`.
Host suite (no rig needed): `./test.sh` = native numeric core in 3 configs + 137 app tests +
analyzer.

### Track A - host-verifiable

- [x] **A1 Active-flag array aligned to the documentation** (done). `Docs/Services/Register.md`
  "Flag RAM" specifies **one 4-bit segment per static entry**, blocks stacked by instance, and
  the section covers **"System + Static"**. The array now follows that: 4 bits per entry, the
  entry offset is `sum of the preceding registry entries + field`, the System block has its own
  trailing segment, and the size is per board (`-D STATIC_ACTIVE_ENTRIES`: DAS 24 slots = 12 B,
  core 48 = 24 B; the arithmetic is documented next to each board's registry) with a runtime
  bounds check so a board that outgrows the cap cannot corrupt memory. The array is `inline`
  (a `static` array in a header would give every TU its own copy - it only works today because
  both targets are single-TU).
  **Two real bugs fixed on the way:**
  1. The **System block was excluded entirely** (the array was addressed by static-registry
     index, and System has none), and `HandleSystemBlockRead` built its reply by hand without
     folding in the flag - so editing the device **Name** (persistent, waits for an explicit
     Save) never reported *Not Saved*. Now set on the Name write, cleared by Save, and folded
     into the reported meta. (NetID was only ever safe because its write path auto-saves.)
  2. **Recall did not clear *Not Saved*** for static blocks either - a recalled value matches
     its backup, so the flag must clear. Now done inside `StaticFieldRecall`/`SystemFieldRecall`
     (one site each, not per call site).
  **Measured trade (DAS, re-measured after the flag-scoping fix below)**: RAM
  **1 672 -> 1 636 B (81.6 -> 79.9 %, -36 B)**; flash **13 740 -> 13 992 B (84.6 -> 85.4 %,
  +252 B)** for A1 + D2 + A3 together (the offset walk is genuine new code; `-flto` already
  merged the duplicated inlined copies, so `noinline` changed nothing). Both still have headroom
  (412 B RAM / 2.4 KB flash free). If the flash is wanted back, a boot-built prefix-sum table
  would trade ~10 B RAM for ~50 B flash - not done, since it needs a boot init hook.
  **Corrected claim**: the promised "host test for the index formula" is not feasible - the
  formula reads `static_block_registry`/its schemas, and neither is constexpr-usable (GCC rejects
  their addresses in a constant expression: `void* const` pointers to mutable blocks), so a
  `static_assert` is out too. Verification is the two builds + the runtime bound check.
  **D2 done - the remaining active flags are implemented as write provenance.** Per your rule
  ("they clear when writing a new value of different specification - manual user's input vs
  local/foreign script"): a `WriteOrigin` (Manual / LocalScript / Foreign) is derived from the
  writer - the app (0xFFFE) and the local CLI are Manual, a frame from a peer on the bus is
  Foreign - or from the flags the writer already passes (a local script sends `ScriptUpdated`,
  a subscription update sends `External`). Every write clears the provenance it does not match
  and sets its own, and reads fold the whole active set in
  (`StaticActiveReported` covers Not Saved + Script Updated + External).
  Two details worth knowing:
  - Static block schemas are `const`, so their active flags can only live in the array - which
    is exactly why the doc has one. The old `ScriptUpdated`/`External` bits folded into a
    static meta were silently **discarded** by `StaticBlockDescriptor::Set` (it only validates
    the type), so the array is what makes them observable at all.
  - **Subscription Source cannot be reported**: the flags field is 6 bits (bits 10-15) and
    already holds ReadOnly/Persistent/Trigger + Not Saved/Script Updated/External, so the
    documented 4th active flag has no wire bit. Stored-but-unreportable would be dead weight,
    so it is left unimplemented and recorded in `Issues.md`.
  **Cost**: DAS flash +104 B; an attempt to fold both flags into one offset lookup was *larger*
  (+36 B), and a `#ifdef USE_SCRIPTS` narrowing (the DAS has no scripts, so the Script-Updated
  bit can never be set there) clawed 48 B back.

- [x] **A2** done - see the record below (the register view no longer re-enumerates each tick).

- [x] **A3** done - see the record below (`MEMORY_BACKUP_CAP` 256 -> 128 on the DAS).

- [x] **A4** done - see the record below (`-O3` measured, `-O2` kept).

- [x] **A5** done - see the record below (dead `DAS_LED_*` defines removed).

- [x] **A6 Script VM symbol resolution made O(1)** (done). The hot cost was not the resolution
  *logic* but the offset it needs: `InputOffset`/`OutputOffset`/`VarOffset`/`ConstOffset` walked
  the preceding meta entries (`for k < i: off += align4(size)`), i.e. **O(index)** - and the VM
  resolves operands **per line per tick** at ~200 Hz, so a variable late in a script cost a full
  walk on every use. They are now **prefix sums** built once at load
  (`LoadedScript::BuildOffsets` → `Core/Functions/StrideOffsets.h`), so a symbol resolves to a
  table index. The layout cannot change while a script is loaded (only values change, never the
  strides), so nothing needs invalidating - which is also why the pointer cache the plan
  floated is **not** needed: with O(1) resolution there is nothing left to cache, and register
  targets keep resolving per execution as they must (a dynamic block's pointers move).
  Three things fell out of the same change:
  - the load path's two input/constant default copies also walked the strides (O(n²) parsing) -
    they now reuse the same offsets, and the file's packing *is* the space's packing;
  - `ScriptSumStrides` went away (the builder produces the totals as the last entry of each
    segment, so there is one place that computes them);
  - `ScriptAlign4` moved into the shared header as `StrideAlign4`.
  **Measured: core 667 840 → 667 552 B (-288 B)**, DAS untouched (no scripts there), and the
  per-tick work drops from a walk to a load.
  **Constant folding was considered and is not worth doing**: the scripts' expressions are
  symbol-driven (a "constant" operand is a read from `constSpace`, e.g. `luxT - LUX_MIN` in the
  brightness script), so there are no literal-only subexpressions in the token stream - folding
  would need a load-time constant-propagation pass to buy one subtraction per tick.
  **New host test**: `test/native/stride_test.cpp` (wired into `test/native/run.sh`) checks the
  table against the naive walk the code replaced, the saturating out-of-range read, the totals
  and the four segment bases - using the same inline base helpers as the getters, so a mistake
  in the layout arithmetic fails there. 45 checks.

- [x] **A7 DAS measuring math - dedicated (distilled) rewrite tried, does not pay; a real
  win found in the same place instead.** Measured ceiling for deleting `log` + `pow10`
  entirely is **504 B**, but neither can go: the **NTC needs a logarithm** and the **LDR's lux
  needs an exponential**.
  **Attempt 1 (base-2 generic pair).** Fold the datasheet constants so the lux is
  `2^(A(range) - log2(ratio)/gamma)`, `A = log2(10) + (log2(R10) - log2(Rref))/gamma`. The
  folding is *correct* - a host test matched it against the old log10/pow10 path to
  **0.04-0.11 %** across the band the brightness curve uses - but it is **+24 B**: the natural
  `log` must stay for the NTC, so the lux *adds* a second logarithm while `pow10` is merely
  swapped for a similar `exp2`.
  **Attempt 2 (dedicated, "ignores Number").** A distilled integer-domain pair
  (`Core/Types/SensorMath.h`): `SensorLog2` for the integer ADC counts (four conditional
  shifts to find the MSB - `__builtin_clz` was tried first and is a trap on this core, pulling
  in `__clzsi2` plus a **256-byte `__clz_tab`**) and `SensorExp2` (the same quintic `pow10`
  uses), both serving the LDR *and* the NTC so the trio could drop out entirely. With the
  interpolation the tables can be 33 bytes each; without it the lux error is ~5 % and the exp
  ~2 %, i.e. too coarse. Measured, best variant (quintic exp2, no exp table): **+8 B**, with
  the transform code at **863 B against the baseline's 840 B**. The trio is simply compact:
  `log` **114 B**, `log10` **22 B** (a wrapper), `pow10` **164 B**, and `log` is already shared
  by both sensors. Reverted, header deleted.
  **What does pay (`A7` fix, applied).** The LDR recomputed `log10(R10)` and `log10(R_ref)`
  *every sample* - two `log()` calls and two fixed-point divisions. They are constants, so
  `log10(R10) - log10(R_ref) = log10(R10/R_ref)` folds into a per-range table
  (`kLdrLog10R10OverRref`), leaving one `log()` call plus a constant division and the shared
  `pow10`. **DAS 13 972 -> 13 960 B, and `log10` drops out of the image** - equivalent maths
  (the same numbers to within one Q16.16 step, 0.006 % in lux), fewer per-sample calls, and the
  NTC path is left bit-identical.
  **Found on the way (worth knowing):** the Steinhart-Hart sum `A + ln(R/R0)/B ~ 0.0034` is
  only ~**222 Q16.16 steps**, so `T = 1/sum` turns *one* step into ~**1.3 K**. The temperature
  reading is resolution-limited by the form itself, which is why it must be re-calibrated
  after any change to that path - so it was left alone.
  Conclusion: do not rewrite the trio; fold the constants that the compiler cannot.

- [x] **A8 (part 1+2) Core render hot loop: per-LED divisions and shape invariants hoisted.**
  Every per-LED division and trig call in `ShapeAlpha` is now a per-*geometry* invariant
  (`PrepareGeometry`, called once per geometry field): `HalfX/HalfY`, `InvHalfX/InvHalfY`,
  `InvFade`, `InvSizeX/InvSizeY`, the rounded-rect inner extents, `min(HalfX,HalfY)`,
  `ParabolaK`, the trapezoid's `tan(slant)`/top/slope, the triangle's `h/2` and `2h/w`, the
  polygon/star sector + `cos(half)` + inner radius, and the Noise cell divisors. `FadeAlpha` now
  takes the params and multiplies by `InvFade`; the gradient's per-LED `/extent` is a hoisted
  reciprocal too. **Measured: the only per-LED division left is the polygon/star `cos(ang)`**
  (the divisor genuinely depends on the pixel), down from ~6-8; core `.text` **-432 B**
  (668 272 -> 667 840) because the divide code disappeared.
  Accuracy: the reciprocals differ by at most 1 LSB from the divide (accepted: functional
  equivalence), everything else is bit-identical (the same value computed once).
  Note the host micro-benchmark (divide vs reciprocal-multiply) shows only ~20 % because x86 has
  a hardware divide and hides its latency; on the C3 a `Number` divide is a 64-bit ROM software
  routine (`__divdi3`), so the saving in cycles is far larger - the structural change (divide
  count) is the meaningful one there.
- [x] **A8 (part 3) loop specialisation - measured, not worth doing (closed).** The geometry
  shape math now lives in its own pure header (`Blocks/GeometryMath.h`), so it can be measured
  and tested on the host instead of being eye-verified only; the move is behaviour-neutral (the
  image size is unchanged). A host benchmark of today's form (the shape switch inside the
  per-LED loop, through `ShapeAlpha`) against a hand-specialised loop with the switch hoisted
  gives **-8.8 % per 86-LED field pass** (`-O2` and `-O1` alike, byte-identical output) - so GCC
  does *not* unswitch it - but that pass is a small slice of a frame that already runs at the
  panel cap (~127-132 FPS, measured earlier): the absolute saving is a fraction of a percent of
  the frame, for twelve duplicated loop bodies. Not worth the churn; the readable dispatcher
  stays. Revisit only if an on-device profile says otherwise.
  **The extraction paid for itself immediately** - it caught a real bug in the rounded-rectangle
  signed distance. The standard form is `d = r - length(max(q,0)) - min(max(q.x,q.y), 0)` with
  `q = |P| - (Half - r)`: the `min(max(...))` term corrects the *interior* and must not be
  subtracted out in the side band. The code tested `m > 0` instead of `m < 0`, so a
  `Square`/`Rectangle` with `Rounding > 0` lost ~Rounding px from **all four straight sides**
  instead of only rounding the corners. Fixed (one comparison). The evaluation scene sets only
  Shape/Operation/Position/Size/Fade/Angles on its geometries, so no shipped look changes; the
  rig look-check is noted in Track B for anyone who does use Rounding.
  **New host test** `test/native/geometry_test.cpp` (**122 733 checks**, wired into `run.sh` in
  both `OPTIMIZE_SPEED` states): per-shape extent/inside/outside, the mirror and rotation axes
  each shape actually has (a polygon's every sector boundary *and* bisector; a star's bisectors
  only, since its outer points sit half a sector off the axes), fade monotonicity, noise
  determinism, a range sweep over size/fade/point-count/angle, and the rounded-rect sides the
  bug broke. Samples in the fade band or on a sector step are skipped - the alpha is genuinely
  discontinuous there - with a floor on how many points were compared, so a check cannot pass by
  skipping everything.

- [x] **A9 `OPTIMIZE_SPEED` duals - the flag, the sites, and the harness** (done).
  `OPTIMIZE_SPEED` is now a real build-level choice: **the size variant is the default** and
  `Tamu_v2_0A` defines it in `platformio.ini` (next to `USE_SCRIPTS`, with the rationale in a
  comment). `NUMBER_ONLY_32BIT` stays what it is - a **hardware** capability flag (the CH32V003
  has neither a multiplier nor a divider), never an optimisation switch.
  **Sites, and why only one:**
  - **`Crc8` - implemented** (the genuine divergence). Moved out of `Packet.h` into
    `Core/Functions/Crc8.h` so it is host-testable (`Packet.h` pulls in `DeviceStatus`, which a
    host test cannot provide). The speed shape is a **256-byte lookup table generated by
    `constexpr` from the very step function the loop uses**, so the two shapes cannot disagree;
    it lands in flash-mapped rodata (DROM), not RAM. The CRC covers every packet and every USB
    chunk, so the core pays 256 B of flash for it: **core 667 552 → 667 840 B (+288 B, flash
    free)**, DAS unchanged at 13 960 B and no table in its image.
  - **`Fnv1a`** - no useful second shape. The hash is inherently sequential (each byte's
    multiply depends on the previous), so a table or an unroll buys nothing meaningful.
  - **The `Number` division fast path** - not an `OPTIMIZE_SPEED` site at all: the compact
    48-iteration `FixedDiv32` versus the 64-bit divide the core's hardware compiles to is
    already selected by `NUMBER_ONLY_32BIT`, which is exactly the hardware flag this item says
    to keep separate.
  - **`log`/`pow10` tables** - their only caller is the DAS's measuring block, and the DAS is
    the *size* target, so a table shape would never be selected. (A7 also showed the polynomial
    form is already both the smaller and the faster choice there.)
  **Harness**: `test/native/run.sh` now builds **every** config in both `OPTIMIZE_SPEED` states
  and runs the same assertions, so a future dual cannot silently change a result:
  numeric core/32-bit/DAS (3665/3665/3405 checks) plus the new `crc_test.cpp`
  (**486 checks** each way) and the stride-offset test (45). The CRC test's reference is
  independent of the firmware (its own shift-register implementation) and pins the standard
  check value `"123456789" → 0xF4`, i.e. the *parameters* are tested, not just the equivalence
  of the two shapes - as are single-bit error detection over a 12-byte frame.

- [x] **A10 (part 1) Script (un)loading from within a script** (done). `Docs/Services/Script.md`
  lists "Script (un)loading" among the VM's functions, so the VM now has service ops **7
  (`Load script`)** and **8 (`Unload script`)** that perform exactly management **CID 1/CID 2**:
  load answers the loaded id (the file id, as the CID replies; `0xFF` on failure) and unload
  takes a loaded id and treats "not loaded" as a no-op. Two guards, both deliberate:
  - **a script cannot (un)load itself** - `ScriptLoad`/`ScriptUnload` release the program and
    spaces that would be executing, so this returns a script error instead of corrupting memory
    (self-restart is what the `Script state` op is for);
  - loading replaces the target slot (the same idempotent reload the app's "apply live" uses),
    and a script loaded this way starts **Stopped** - loading is not running.
  App side: both instructions are in `script_instructions.dart` with role hints, and the
  app↔firmware contract test now maps ops 7/8 to the new firmware defines, so the encoding
  cannot drift. New app test covers the arity/destination rules (Load needs a destination to
  report the id, Unload does not). **Core +688 B** (two cases plus their inlined resolution in
  the VM), DAS unaffected (no scripts). For reference, PlatformIO's partition report on the core
  now reads **667 842 B / 21.2 %** of the 3 MB app partition (the `firmware.bin` deltas quoted
  across A6/A9/A10 are measured on the same artifact, so they are comparable to each other).
- [ ] **A10 (part 2) Cross-script macro calls - blocked on a docs decision**, recorded in
  `Issues.md` with the proposal. The VM runs **one script per tick** and every wait state
  (`waitUntil`, `pendingForeign`, the foreign deadline) lives on the callee, so "macro call"
  cannot be built as a local `Call` variant: the call stack has to carry a script slot, and the
  tick loop has to resume whichever script is actually waiting. The docs name "Macro call" but
  give no opcode, no boundary-crossing rule and no argument passing; the proposal (a `Call
  script` flow op taking `(loaded id, entry line)`, blocking by construction, a shared
  `(script, line)` stack, values exchanged through registers) is waiting on confirmation.
- [ ] **A11** Per-field geometry-mask versioning - **deferred to the rig batch by design**:
  the measured headroom says it is not urgent (the display readback sits at the panel cap) and
  its correctness cannot be checked without hardware, so it is Track B/D3, not Track A. The
  plan is in §6.

- [x] **A2 App: the register view no longer re-enumerates every 0.5 s** (done). The documented
  auto-refresh is about *values*, but every tick was calling `readBlocks()` = enumerate block
  types + per-type instances + a meta read per block (~8-12 bus round-trips on a bus the DAS
  shares) for topology that only changes on create/delete/reorder/re-type - and each of those
  paths refreshes explicitly anyway. Now: the tick re-reads only the *values* of the expanded
  blocks, and the topology every 10th tick (a counter, not the wall clock, so it is deterministic
  and testable). A view with nothing expanded costs **no traffic at all**. Explicit refresh (the
  Refresh button, edit actions) still does a full read.
  Verified by a new behavioural test: `readBlocks` is called once on load, **not** during ticks
  inside the interval, and again after the 10th tick. That needed two small test seams on
  `RegisterPage` (`isConnected`, `clientFactory`) because the refresh paths require a live link,
  which a widget test cannot fake (`ConnectionManager.isConnected` is `_transport != null`).
- [x] **A2b (noted) - checked and closed: the eager tile building is not the cost it looked
  like.** The page does build a card per block eagerly (`SingleChildScrollView` + `Column`), but
  a card is only cheap *until* it is expanded, and the expansion is already lazy: `_blockCard`
  returns just a `Card`/`ListTile` plus a few `ChipLabel`s when collapsed, and the field list
  (with its grid, keys and edit affordances) is built only for blocks in `_expanded`, whose
  fields are fetched on demand (`_loadBlockFields`). So the 0.5 s value refresh rebuilds ~N
  collapsed tiles - tens of widgets each - every half second, which is far below anything a
  phone notices. Slivers would only pay off if a registry grew to hundreds of *expanded* blocks,
  and they would mean restructuring the page (including the reorderable dynamic section, which
  needs `shrinkWrap` inside the current scroll view) for that hypothetical. Left as is.
- [x] **A3 DAS stack cap**: `MEMORY_BACKUP_CAP` 256 -> **128** for the DAS. The STATLOG for its
  two resistive-measure blocks needs ~72 B, so 128 keeps ~2x headroom; the cap sizes the
  `buf[MEMORY_BACKUP_CAP]` locals in `LoadAllBackups` / `HandleStaticSaveRecall` /
  `HandleSystemBlockWrite` / `SystemFieldPersistExplicit`, so it cuts the *peak stack* by 128 B
  against a 576 B reservation. DAS flash -32 B as a side effect (smaller buffers). Still to do
  if wanted: measure the real peak with `-fstack-usage` and then shrink the peservation.
- [x] **A4 core `-O3` measured, not enabled**: `-O3` on the project's own sources works but grows
  the image **667 952 -> 1 029 280 B (+54 %)**. Flash is free (1.9 MB spare), but the win is
  host-measured only and the C3's icache could make it a *regression*, so the committed setting
  stays **-O2** and the flip is one line (`-O3` in the Tamu env's build_flags). Note: a *global*
  `-O3` fails the build - it makes GCC's `-Wmaybe-uninitialized` fire in IDF's
  `tinycrypt/ecc.c`, which IDF compiles with `-Werror`.
- [x] **A5 dead defines**: `DAS_LED_PORT` / `DAS_LED_PIN` removed (superseded by `LEDR`/`LEDW`).
- [x] **Flag-scoping bug (found while doing A4)**: an earlier edit of mine inserted `-O2 -fwrapv`
  into the **DAS** env instead of the core env - both envs contain `VERSION_YEAR`, and the text
  replacement hit the first match. The DAS then built at `-O2` (not `-Os`) and the core fell back
  to the compiler default (~1 MB image, i.e. *slower* than the original `-Og`). Fixed and both
  re-measured; the numbers above are post-fix. Worth remembering when editing `platformio.ini`
  with a text replace.

- [x] **Active flags are source-side (user correction)**: the specification of a write is
  **declared by the writer** in the write's ValueInfo flags and the receiver just applies it -
  it never guesses from the sender's address. Replaced the `frame.id_src` heuristic with
  `OriginFromDeclaredFlags` (`ScriptUpdated` -> local script, `External` -> subscription or a
  foreign script, neither -> plain manual write, which clears both) and made
  `SCRIPT_OP_SERVICE_REG_WRITE_FOREIGN` **declare `External`** in the packet it sends, so the
  receiving device marks "a different device updated this via script" as the docs describe.
  The receiver is now purely "apply the write + manage Not Saved": Not Saved stays
  destination-side (only the register knows its backup), and the origin flags are stored exactly
  as declared. In-process writers (a local script, a subscription update) already declared their
  origin the same way.
  **App side**: the app *echoes* the meta it read on a write, and reads now carry the active
  bits, so it would have mis-declared every edit. `RegisterClient` now masks them at its two
  write choke points (`FieldFlags.activeMask`), leaving the passive flags for the firmware's
  type/size validation. DAS flash -20 B (13 992 -> 13 972, 85.3 %).

### Track B - needs the rig (batch into one session)
- [x] `-O2 -fwrapv` sanity: **boot + scripts verified** on the rig (boot log, enumerate, SNDB
  recovery), and the script VM's full HIL suite (`hil_script_test` + `hil_script_vm_test`,
  20 VM cases: arithmetic, rounding, Get time, While/EndBlock, Delay/Wait, local + foreign
  register write with the **ScriptUpdated flag set** (`dyn flags=0x4000`), compose/extract,
  error states, load-on-boot across a reset, expressions, vectors/matrices, BlockInfo targets,
  Limit). **render unverified - this rig has no display** (the render block exists but there is
  nothing to drive), so that half stays open for a display-equipped session.
- [ ] `Polygon`/`Star` look after the `atan2` fix.
- [ ] Rounded `Square`/`Rectangle` look, if a scene uses `Rounding` (the evaluation scene does
  not): the A8 part-3 fix stops the four straight sides being pulled in by the corner radius.
- [x] **DAS reflashed** (minichlink, current build 13 988 B / 85.4 %, RAM 1 636 B, and **no
  `__*di3`/`__muldi3`/`__clzsi2`/`__clz_tab`** in the image). Sensor sanity on hardware: the
  NTC reads **27.5 / 26.1 °C** (plausible room temperature, and it tracks the room), the
  auto-range picks the **330 kΩ** reference for the ~100 kΩ NTC and the reported value stays
  compensated, and `FilterCoeff` round-trips. The **LDR lux (measuring instance 1) now reads
  too** - it needed the CLI's instance addressing (see the CLI update below) - and reports
  **6.76 lux** for the room, i.e. the `log`-fix lux chain is verified end to end. A lux-meter
  cross-check is still the only way to confirm the calibration constants themselves.
- [x] DAS `LoadAllBackups` restore across a reboot - **verified** (see the persistence item
  below, same mechanism: the node restores its saved static values at boot).
- [x] **DAS static persistence - verified, after fixing two bugs that made it impossible.**
  Rebooted the node and read back: `FilterCoeff` **0.25** (the written value, not the 0.5
  default), `SamplingRate` 10.0, `SensorType` 5. A real power-cycle is still the strongest
  form; a reflash resets the CH32 and (checked) **preserves the 256 B storage region**, so it
  exercises the same restore path.
- [x] **Device Name shows Not Saved and clears on Save - verified on hardware** (A1's fix).
  Added to the verification suite: a persistent Name write reads back with `notSaved`, and
  `saveStatic` clears it. The System block has no static-registry entry, so this is exactly the
  case that used to be silently dropped.
- [ ] Node re-register -> provider re-push (deferred by request).
- [ ] DAS provider stale entries (the cancel path does not always reach the node).
- [ ] Backup view: the app's block order vs the firmware's static registry order.
- [ ] LED brightness brown-out -> firmware current cap/ramp decision (deferred by request).
- [ ] Mask-versioning correctness, if A11 is implemented.

### Cleanup pass: app per-tick work, wire bounds, flaky assertion, TODO hygiene

- [x] **Log page: adaptive poll cadence.** The device streams its *entire* log store for every
  read (`LogHandler CID 1`), so a quiet view pulled the whole buffer - several packets over the
  RS-Bus once the store is full - on every 0.5 s tick. An identical reply now backs the poll off
  (linearly, up to 10 ticks); any change, or a manual refresh, resets it to every tick, so a
  device that is actually logging stays live while a quiet one costs ~10x less. New widget test
  (`log_refresh_test.dart`) drives the hook directly and asserts all three properties: fetch on
  open, a handful of fetches over 21 quiet ticks instead of 21, and a change picked up within
  one skip period.
  The **devices page** was measured rather than changed: its tick marks every known device stale
  and probes each, which is two requests here and scales with the node count. Left as is - the
  A2 values/topology split is the pattern if the setup grows.
- [x] **Wire-bounds audit: clean.** Every `frame.payload[...]` site was checked for a payload
  length guard: the Script service's share one `PayloadBytes(frame)` local per case, the
  Register/Storage/Log/Subscriptions handlers all bound-check before reading, and the rest stay
  inside the struct regardless. One hardening applied: `PacketGetFrag` now returns an empty
  range for a frame that sets `FLAG_FRAG` with no payload, instead of reading whatever the
  struct's payload area holds (in-bounds but meaningless). +8 B on the DAS.
- [x] **The DAS clock-sync HIL assertion no longer fails good builds.** It converged to 19 ms in
  one run and 3 ms the next against a hard 10 ms bound - the residual is a function of the sync
  cadence (the DAS tracks the core's rate between syncs, RC drift ~1 %). The bound is now 25 ms
  (still catching a broken sync, which is orders of magnitude out) and the achieved offset is
  printed either way. Verified: the suite passes and reports the offset (-1 ms this run).
- [x] **TODO hygiene**: no action needed after all - the moot CLI entries (`file write`, the
  `subs` deadzone argument, the help sweep) went out with the CLI removal record, and the only
  remaining references to the deleted tools are inside that record, which is history.
- [ ] **BLE is a coverage gap** (noted, needs tooling): every HIL suite drives the core over USB,
  while the docs' Android path is BLE. A Linux host could exercise the core's BLE through BlueZ;
  until then the BLE link is only verified by using the app.

### Issue 3: node-reboot re-push (3a) and orphan cleanup (3b) - implemented

- [x] **3a - re-push on node registration.** `ReRegisterSubscriptionsForNode(addr)` re-pushes this
  device's requester entries that belong to `addr`, called from the core's discovery/assign path
  (`Core/Services/Device.h`, next to the "Registered new device" log). A node's provider table is
  RAM-only, and the existing retries stop once the first value has arrived - which is exactly the
  state a running subscription is in - so a node reboot used to end its subscriptions silently.
  The node's add is TRID-keyed, so a re-push replaces rather than duplicates.
- [x] **3b - the requester cancels orphans it is told about** (user's design, and it costs no
  traffic in the normal case). In `HandleRequesterValueUpdate`, an incoming value update whose
  TRID matches no active requester entry is by definition an orphan provider entry - the only
  notice we ever get, since the node's table is its own. It is cancelled immediately, for its
  TRID, back to the sender. A lost cancel simply means the orphan announces itself again on its
  next period, so the node's table converges without polling.
- [x] **3b verified on the rig, with a three-way control** (via `test/tamu_proto.py`):
  1. a **real** subscription (trid 4356) persists across every read - it is never cancelled;
  2. a **core-addressed orphan** is never observable: its first value update arrives at once and
     the core cancels it within the same read window;
  3. an orphan addressed **elsewhere** (requester 99) **persists** - no update reaches the core -
     which isolates the trigger as the update itself rather than a timer or a table scan;
  4. a stale entry left by earlier sessions (trid 4420) disappeared once it next reported.
- [x] **3a verified on the rig.** The first version called the re-push **inline** in the core's
  discover handler - a blocking verified send, issued before the assignment reply - and with a
  node that was not yet answering it stalled the assignment: the DAS went silent for minutes.
  **That was my regression**, and it is now deferred: the handler only sets a bitmask
  (`SubscriptionsRequestReRegister`), and `SubscriptionsTick` does the sending from the main
  loop. After the fix the DAS registered normally, and a freshly rebooted node (empty provider
  table) came back holding **exactly** the subscription the core wanted (requester 1, trid
  0xFA00) with no app involvement - and the orphan I had left behind was correctly *not*
  re-pushed. HIL suites all pass with the DAS present (10/10, 8/8, 4/4); see `Issues.md` for
  the general rule about not sending on the bus from inside packet dispatch.
- Cost: core flash **629 488 -> 629 658 B (+170 B)**; both changes are core-only, so the DAS is
  untouched at 13 988 B / 1 636 B.

### Rig follow-ups: vector deadzone cap fixed, direct harness working

- [x] **Vector delta deadzone cap fixed** (`SubscriptionsProvider.h`). Both sides of the
  comparison used to be squared in full Q16.16, where a practical change *or* deadzone
  overflows 32 bits - so both saturated at the same ceiling and every deadzone behaved like
  1.0: a 3-unit change was sent with a 100 deadzone. The comparison now runs in Q8.8 (±256
  units at 1/256 resolution; a deadzone below 1/256 still means "any change"). The scalar path
  was already exact. Cost: core flash -2 B plus the dead function below.
- [x] **New deterministic test** for exactly that bug: a *writable* dynamic vector source, so
  the check needs no physical motion (the accelerometer version depends on not touching the
  board). It asserts a change past the deadzone arrives, a ~3-unit change under a 100 deadzone
  does not, and a change past it still gets through.
- [x] **The "delta vector honours the deadzone" test's deadzone corrected** (100 -> 5). With the
  cap fixed, a deadzone above the source's whole initial state means nothing is sent - which is
  correct behaviour, but the test's "the first value should have arrived" assumed the old
  saturation made it slip through.
- [x] **The "scalar provider keeps its last value" test made deterministic.** It subscribed to
  the DAS's LDR, whose value drifts with the room light, and compared a value read from the
  core with a hash read from the DAS: a delta push is fire-and-forget, so one lost packet
  leaves the provider's last-sent hash legitimately ahead of what reached the target. It now
  drives a *writable* DAS source (the ResistiveMeasure Filter Coefficient), retries the nudge
  if a push goes missing, and asserts the mechanism it was written for - the provider's hash
  equals the raw value that arrived.
- [x] **Dead code**: `StaticDirtyGet()` removed (defined, never called; invisible on the DAS
  because that build passes `-Wno-unused-function`).
- [x] **`test/tamu_proto.py` now works end to end**: ping, block-type enumeration and a System
  read over the protocol. Three fixes were needed, all in the client: the packet CRC covers
  offsets 1..11 (flags..trid) plus the payload; the request's **source service must be a real
  tag** (`ServiceType::App << 8 | txid`) because the device addresses its reply to it and drops
  replies to an unknown service; and the **link frames must be unwrapped** before the packet
  parser sees the stream.
- [x] **Artifacts**: `tamu_backup_2026-09-22.zip` and `test_connect.dart` removed;
  `Tamu_current_setup.zip` kept as requested.

### The CLI was removed (it outlived its purpose)

The console/REPL and everything that existed to serve it are gone; the App Interface now owns
the USB port outright. Kept: the protocol harness (`test/tamu_proto.py`, now the rig entry
point - see `test/README.md`) and the Flutter HIL suites, which both speak the wire protocol.

- **Deleted**: `firmware/src/Devices/Tamu_v2.0A/CLI/` (4 files, ~96 KB: `Entry.h` with `StartCLI`
  and its 16 KB console task, `Handler.h`, `Block.h`, `SNDB.h`); the console-driven
  `test/testsuite.py` + `test/hwtest.py` (+ `__pycache__`); the CLI reply-tag dispatch and the
  eight `HandleCLI_*` declarations in `Dispatcher.h`; `ServiceType::CLI`;
  `Capabilities::Cli` (bit 2 stays reserved so no other bit moves); the CLI-only Discover reply
  branch in `Core/Services/Device.h`; `AppCLIConnected()` and the System field 8 key 1
  ("CLI Active"); the app's `Capability.cli` + its contract-test entry and the
  `system_schema` CLI labels.
- **Refactored**: `AppUSB.h` lost the two-mode state machine (`USB_MODE_*`, `EnterCliMode`,
  the line editor, the prompt, the CR/LF revert, the CLI shadow framer) and keeps the link
  task, framer, wire parser, RX queue and TX pump - `ConsoleTask` became `AppLinkTask` with a
  4 KB stack (from 16 KB). `Main.h` now calls `AppUSBInit()` + `AppUSBStartTask()` (it was
  `StartCLI` that initialised the port).
- **Effect**: **the wedge hazard I reported earlier is gone** - with a single mode there is no
  state to get stuck in. Docs (`Docs/Services/CLI.md`, the Capability bit, System field 8 key 1)
  are now ahead of the implementation; noted in `Issues.md`.
- **Measured**: core flash **679 390 -> 629 490 B (-49.9 KB, 21.6 % -> 20.0 %)**; the DAS is
  unchanged (it never had a CLI: 13 988 B / 85.4 %). RAM improves too - the console task's
  16 KB stack and the CLI's static buffers are gone.
- **Verified on the rig**: the HIL suites still pass on the rebuilt firmware
  (verification 10/10, subscriptions 7/7, dynamic persistence 4/4), and the host suite is green
  (11 native groups, 139 app tests, analyzer).

### Rig session results (Tamu + 1 DAS, default sensors, no display/fans)

Suites run and passing on hardware: `tamu_hardware_verification_test` (**10/10**),
`hil_script_test` + `hil_script_vm_test` (2/2), `hil_subscriptions_test` (**7/7**),
`hil_dynamic_persistence_test` (4/4), `hil_backup_test` (3/3), `hil_storage_files_test` (3/3).
Both targets were reflashed from the current tree first - the devices had **very old firmware**
(the CLI prompt had changed and the app could not connect at all).

- [x] **Fixed: delta-subscription change magnitude was computed from raw bit patterns.** Both
  delta paths (scalar and vector) took `a >= b ? a - b : b - a` on the *unsigned* raw values,
  which only matches the value ordering while both sit on the same side of zero. A source that
  **crosses zero** - the accelerometer's X axis idles at −0.38 with Y at 0.07 - then looked
  like a change of ~2^32, so it exceeded every deadzone and sent every minimum interval.
  Found by `hil_subscriptions_test`'s "delta vector honours the deadzone" (a *huge* deadzone
  must only send the first value). One shared `SubscriptionsAbsDelta` (a signed difference,
  negated through unsigned so `INT32_MIN` is safe) now serves both paths. The suite passes
  7/7 after the fix; the same bug would have spammed a temperature subscription crossing 0 °C.
- [x] **Fixed: the CLI's save/recall/read-backup hard-coded the Dynamic block type.** The
  BlockInfo it built was always `Type=0x3FF`, so a *static* block could not be saved from the
  CLI at all - the core logged `MEM: CID 3 failed block=255 field=255 key=197`. Static blocks
  are now addressed by their TYPE with instance 0 (what the app does); the "no block given"
  form keeps the Dynamic all-instances encoding.
- [x] **Fixed: no static save on the DAS could ever succeed** (two independent causes):
  1. `WriteBackupFile`'s atomic staging writes a temporary name and renames - but the reduced
     file system has one pre-allocated file per settings name and its `RenameFile` is a no-op,
     so `CreateFile("STATLO~")` failed outright. It now writes the live name in place (no
     atomic swap, acceptable for the deliberately reduced FS).
  2. Worse, `ReadBackupFile` returns the **file length**, and the DAS's fixed storage
     pre-allocates STATLOG at 256 bytes of erased 0xFF - so the append position looked like a
     full buffer and `LogEntryWrite` refused. `BackupLogUsed` now gives the save paths the
     log's *logical* end (up to the format's 0xFF terminator), which the recall paths already
     honoured. Symptom before the fix: every DAS save returned status 255 and STATLOG stayed
     untouched, so the node had nothing to restore after a reboot.
  **New regression check** in the verification suite ("DAS static save writes the backup log"):
  write a persistent field back unchanged, `saveStatic`, then read the node's STATLOG and
  require a non-erased entry - the save path's *result*, not just its status reply, which is
  what the suite was missing. Flash cost of both DAS fixes: 13 960 -> 13 988 B (+28 B).

### Track C - docs gaps to keep in `Issues.md`
Active-flag model (partly resolved by A1; the other three flags are unimplemented *and*
under-specified) · `Current setup v3` predates the emote interface · UI-info v2 format ·
`SCR_XX` vs `SCR_XXX` · script management CID 8 · pre-rotated translation convention · OS
notifications ("To OS") never delivered · no framebuffer readback (visuals are eye-only).
The Register Current/Backup view item is closed (implemented).

### Track D - decisions
- [ ] D1 Keep or drop the two remaining Android `Issues.md` items (`permission_handler` pinned to
  11.x, Flutter "Built-in Kotlin" migration)? On-device verification was called a non-issue.
- [x] **D2 answered** - the flags are implemented as write provenance (see the A1 record): the
  origin is declared by the writer in the write's ValueInfo and both sides apply it; the
  Subscription-Source flag has no wire bit and stays a docs gap in `Issues.md`.
- [ ] D3 Mask versioning now (flagged for the rig) or later?
- [ ] D4 LED brightness cap stays deferred?
- [ ] D5 Cross-script control: does the app get UI too?

### Track E - intentionally not doing (with reasons)
Merging the UI role-flag pairs (`_loadFieldsFor{Target,Source}`, `addDestination`/`addOperand`,
`_cycle{Net,Type}`) and the two `*EntrySerialize`s - the merge needs a role bool and reads worse.
Hoisting the `ScriptResolveOperandScalar`/`ScriptOperandBlockInfo` preamble - their `scratch`
buffer's lifetime is load-bearing, so it would return a dangling pointer. App APK size
(`--split-per-abi`) - out of scope by request. Extra `-Warray-bounds`/`-Wstringop-overflow`
suppressions beyond the one documented false positive.

Order: **A1 -> A2 -> A3 -> A4 -> A5 -> A6 -> A7 -> A8 -> A9 -> A10 -> A11**, then one rig session.
