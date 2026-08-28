# Issues

Status of items from the docs-vs-implementation audit and follow-up work.

## Resolved (code)

### Memory views flickered on the (new) auto-refresh (2026-08-28)

- After the memory views started auto-refreshing at the documented 0.5 s, the pages rebuilt
  aggressively and flickered: the Keyed page swapped in fresh block objects with EMPTY
  dicts/entries on every tick (blanking open dictionaries before reloading them), and the
  System/Dynamic pages did an extra rebuild per tick.
- **Fixed**: the Keyed page now preserves the previous blocks' open dicts/entries onto the
  fresh objects before the reload (values update in place, never blank); all three pages
  guard against overlapping refreshes (a tick in flight is skipped, so a slow BLE refresh
  never stacks) and rebuild once per tick after the reload instead of twice.
- Verified: `flutter analyze` clean, offline suite 50 passing, GUI rebuilt + relaunched.

### Memory views showed stale field values; autorefresh was off by default (2026-08-28)

- **Field values went stale**: the System/Dynamic Memory pages loaded a block's entries only
  once when it was expanded (`if (!autoRefreshActive) await _loadVisibleFields()`), and the
  auto-refresh tick re-read only the block LIST while preserving the old field values - so a
  live read-only value (the LEDButton's Button state) never updated after the first expand.
- **Autorefresh did not match the docs**: Docs/App/Service views/{System,Dynamic,Keyed}
  Memory.md all say "Automatically refreshes the visible view (0.5s)", but the pages started
  with the timer off until the user opened the refresh menu (only the Connection page
  started on by default). 
- **Fixed**: every refresh now reloads the visible (expanded) blocks' values (the Keyed page
  already did for its open dicts), and all three memory views start auto-refreshing at the
  documented 0.5 s on page open (started post-frame so the mixin's setState is legal). The
  LED/Button's Button field now tracks the physical button while the LED is off.
- Verified: `flutter analyze` clean, offline suite 50 passing, GUI rebuilt + relaunched.

### Tamu LED-Button: LED state never matched the real LED (2026-08-28)

- **Button read inverted (root cause)**: the button is active-LOW (line idles HIGH via the
  pull-up; a press pulls it LOW), but `ButtonUpdate` read `PinRead()` directly (HIGH =
  pressed). The internal pull-down the OFF path enabled could not beat the board's external
  pull-up, so the line idled HIGH -> a permanent false press -> the LED auto-triggered and
  could not be turned off ("given LED state does not correspond to the real state").
- **Fixed**: `ButtonUpdate` now reads active-LOW (`pressed = !PinRead(...)`), and the LED
  OFF path configures the pin as an input **with pull-up** (`PinModeInputPullUp`, new in
  Base.h) matching the external pull-up - idle HIGH = LED off + button readable.
- **The button only reports its state** (Button out field) - pressing it no longer drives
  the LED (a previous iteration of the fix auto-triggered the LED on a press, so the
  button's state was hidden once the LED lit). While the LED is on the shared line is
  driven and cannot be sampled, so ButtonState reads false.
- **Persisted LEDState now re-applies at boot**: LEDState is a writable static-block field
  saved in the SYSMEM backup; the restore wrote the RAM field but never re-ran its write
  trigger, so after a reboot the block said "on" while the pin stayed high-Z. The Tamu boot
  sequence re-applies the restored LED state (`OnLEDStateChange`) after the init blink;
  the identify blink also restores it on exit.
- **Removed the comm-LED contention**: `CommLed()` (AppUSB/AppBLE) pulsed the same pin as a
  link-activity indicator, fighting the LED-state writes whenever the app was connected.
  The white/notification LED is documented as missing hardware, so the activity indicator
  is gone; the LED-Button module owns the pin.
- Hardware topology documented in Docs/Modules/Generic system blocks.md (LED + button in
  series, pin in the middle, button to GND, LED to VCC via resistor, external pull-up).
- Verified live: boot matches the persisted state, write-on sticks, write-off sticks, idle
  button reads false. HIL 8/8 + script HIL 3/3 still green after the changes.

### Packet protocol upgraded to the new packet type (2026-08-28)

Implemented the redesigned packet format (Docs/Data Formats.md) end-to-end (firmware +
app) and reworked the file-transfer services around the new FRAG fragmentation:

- **Header**: byte 2 is now Priority (default 128); Payload Length is in 4-byte units
  (max 73 units = 292 B, frame max 304 B). Payloads are zero-padded to 4 on the wire;
  real lengths are derived from format fields (BlockMeta.Size etc.), never
  `payload_len - fixed`. `PayloadBytes()` converts the stored units to bytes.
- **FRAG** (flag bit 4): the first 4 payload bytes are `u16 current + u16 total
  fragments`; non-last fragments carry a full 256-B actual payload, the last is the
  (padded) remainder. Stream headers (file name / script ID) ride in the Information
  section of fragment 0.
- **Storage service** (per Storage.md): CID 0 Read File Table streams entries as FRAG;
  CID 6 Read File streams the whole file (`Name` -> `Name + FRAG + contents`); new CID 7
  Write File streams `Name + FRAG + contents` and answers each acknowledged fragment with
  the last sequential fragmentation index written (u16). The old Write Stream
  Open/Close/64+ CIDs and the `StorageStream*` helpers were removed.
- **Script service** (per Script.md): CID 15 Read script streams `ScriptID + FRAG +
  contents`; CID 16 Write script streams `ScriptID + FRAG + contents` (the device creates
  the file on fragment 0 and shrinks it to the real length on the last); CIDs 17/64+
  removed.
- **Device service renumbered** (per Device service.md): new CID 2 Identify (blinks the
  red LED fast); Type->3, SN->4, Version->5, Capability->6, Read Name->7, Set Name->8,
  Uptime->9, Loop->10, Time sync->11, Set time offset->12, SNDB Read All->13 (now FRAG),
  SNDB Read->14, SNDB Write->15. `Capabilities::Bootloader` bit added (not yet set by any
  device - the bootloader is a later session).
- **Other stream senders moved to FRAG**: SNDB Read All, LogHandler GetLogs, Storage file
  table/read, Script read.
- **DAS stack fixes**: `PacketFrame` grew to 304 B, overflowing the CH32V003's 256-B
  stack (service replies crashed the node into a watchdog reset on any SystemMemory read).
  The DAS now builds with `MAX_PAYLOAD_SIZE=128` (node frame = 140 B), hoisted
  per-handler response buffers, a 768-B stack and a 320-B RS485 echo ring. Verified:
  no crashes under repeated service traffic.
- **App**: `protocol.dart` (units, padding, FRAG), `connection.dart` FRAG reassembly
  (strips the 4-B info per fragment; clients trim the last fragment via the known file
  size), memory clients slice values by BlockMeta.Size, `storage_client`/`script_client`
  reworked to the new CIDs, `sendNoReply` retired. App write fragments set the FRAG flag.
- **CLI**: response handlers use `PayloadBytes()`; FRAG streams strip the info; Device
  commands renumbered; new `dev <addr> identify [on|off]`.
- **Fixed during bring-up**: a corrupt `payload_len` (up to 255 units = 1020 B) could
  overflow the RX payload buffer - the shared assembler now rejects frames larger than
  `MAX_PAYLOAD_SIZE`; the write handlers' `s_write_seq + 1` wrap comparison promoted to
  `int` (0xFFFF + 1 = 65536, not 0) and is now cast to uint16.
- **testsuite.py's FilterCoeff expectations were stale** (they assumed a [0,1] clamp;
  Devices/DAS_v0.1/Measuring.h bounds it to >= 0) - updated to match the firmware; the
  app HIL test was updated the same way.
- Verified: both boards build + flash, `flutter analyze` clean, offline suite 50 passing
  (2 new protocol tests), USB HIL 8/8 + script HIL 3/3 (incl. a 600-B writeFile->readFile
  round trip on the core), CLI identify + renumbered commands + sndb live.

### Input switch/slider no longer "returns" (2026-08-26)

- Root cause: the input controls were *controlled* purely by the live value, so after a
  toggle the poll (1 s) rebuild with a stale live value (or the default for stopped
  scripts) snapped the control back. `ScriptInputControl` is now stateful and keeps the
  value the user last set as a `_pending` until the device echoes it (or a differing live
  value arrives). Applies to the Switch, Picker, Slider and value-editor fallback.
- Verified with widget tests (`script_input_widget_test.dart`): switch commits + stays
  toggled across a stale rebuild then tracks the echo; slider commits on release, thumb
  follows the drag, and keeps its position while the live value lags. Offline suite 48/48;
  GUI rebuilt + relaunched.

### Script slider drag + input controls in the editor (2026-08-26)

- The slider thumb follows the finger during the drag (local drag state) and, on
  release, **keeps the committed value until the device echoes it back** - the 1 s poll's
  rebuild with a stale (or default-for-stopped-scripts) live value no longer resets the
  thumb. Verified by three widget tests (`script_input_widget_test.dart`): commit on
  release, thumb-follows-before-release, and keeps-committed-value-while-live-lags.
  Offline suite 45/45; GUI rebuilt + relaunched.

### Script slider drag + input controls in the editor (2026-08-26)

- **Slider now slides**: the slider thumb follows the finger while dragging (local drag
  state in a dedicated `_ScriptSlider`) and only commits the value to the script on
  release, instead of snapping back because the committed value never changed.
- **The same interaction controls are available in the editor**: each Input card in the
  editor now renders its live `ScriptInputControl` (Button/Switch/Picker/Slider/Text)
  below the definition row, wired to writeInput - so an input can be driven from the
  editor while the script runs, alongside the definition editing (key/type/default/style).
- Verified: `flutter analyze` clean, offline suite 42/42, GUI rebuilt + relaunched.

### Script list: expandable rows with live input controls (2026-08-26)

- **Script rows unfold** to a detailed view (chevron or tapping the row) that renders the
  inputs as their interaction controls - Button (momentary press writes true/1), Switch
  (toggles), Picker (dropdown), Slider, Text field - wired to writeInput, plus live
  output/variable values and an "Open editor" action. A script can be driven from the
  list without opening the editor.
- The expanded scripts are polled (1 s) for state + live input/output/variable values; the
  header state badge and Start/Pause/Stop controls update live.
- New `script_input_widget.dart` maps a ScriptInput's (resolved) style + type to a control.
- Verified: `flutter analyze` clean, offline suite 42/42, GUI rebuilt + relaunched.

### Script editor live preview + script list controls (2026-08-26)

- **Editor live preview**: while the editor is open it polls (1 s) the script's state,
  current instruction and the real input/variable/output values. Variables and outputs
  rows show the live value; the current instruction line is highlighted in the
  instruction editor (orange border + ▶ marker).
- **More editing options**: inputs and variables can be written live (type-aware value
  editor -> writeInput/writeVariable, which also wakes a Waiting script); instruction
  lines can be duplicated; outputs show live values read-only.
- **Script list page**: each row now shows the input/output definitions in the script's
  format (input types + output names, read from the file) and inline state controls
  (Start/Pause/Stop) alongside the state badge.
- Verified: `flutter analyze` clean, offline suite 42/42, GUI rebuilt + relaunched.

### Script editor pickers refined (2026-08-26)

- **Expected type dictates interaction styles**: the input dialog's style dropdown is
  filtered by the data type (`InputStyle.allowedFor`); changing the type clamps a now
  incompatible style back to the type's automatic default. Automatic maps Bool->Switch,
  Enum->Picker, Number/Index->Slider, Colour->Picker, String/other->Text.
- **Operand/output selection is name-based, not index-based**: the picker first chooses a
  category (Inputs/Outputs/Variables/Constants/Predefine), then lists the already-declared
  entries by name/type/value, with an "add new" entry that auto-declares. No raw index
  entry field. Symbol chips for named variables/outputs show the name.
- **Instruction picker is two-level**: recommended instructions are listed first, then
  per-category buttons (Math/Logic/Compare/Compose/Memory/Flow/Time/State/Macro) that open
  the smaller category lists; search still finds anything.
- Verified: `flutter analyze` clean, offline suite 42/42 (added input-style tests), GUI
  rebuilt + relaunched.

### Script editor rebuilt (symbol-based) + input dictionary (2026-08-26)

- **Input dictionary**: each input is edited as a dictionary entry with key, expected
  type, default value (type-aware editor) and an interaction style (Automatic / Button /
  Switch / Picker / Slider / Text). The style is persisted in the script file: the input
  meta now carries one style byte per input (5 B/input) after the legacy 4 B/input layout;
  both formats parse and the firmware reads either (RuntimeSeed, manager CID 5).
- **Symbol-based instruction editor** (at the bottom of the editor page): every line is a
  row of pickable chips `[Output] [Op] [Operands...]` - no text editing. The opcode picker
  is searchable, grouped by category (Math/Logic/Compare/Compose/Memory/Flow/Time/State/
  Macro) with per-op hints and a "Recommended" section (not limiting). Operand/output
  pickers select kind + index; predefine literals get a subtype + value picker.
- **Auto-compile**: referencing an undeclared `InN/OutN/VarN/ConstN` in an instruction
  auto-declares it (input with next free key, named variable/output, Number-0 constant) -
  both when picking a symbol and on save. Line templates (ADD/compare/IF/WHILE/delay/
  mem read-write/get time/END) plus reorder/delete per line.

### Script editor fixed for fresh scripts (2026-08-26)

- A freshly created script only **reserves** the Script ID - the file is not created
  until the first save (write-stream open). The editor previously showed "corrupt
  file"/"could not read" and was unusable. It now starts from a blank script (default
  `END` line) when the file does not exist yet, and a corrupt file is overwritten on
  save.
- `ScriptClient.readName`/`readScriptFile` now treat a 1-byte status reply (missing
  file) as null; the list page falls back to "Script N" for unnamed scripts.
- New HIL test covering the exact editor save path: create -> no file -> save a blank
  script -> read back -> run to Finished -> delete. HIL script suite now 3/3.

### Script service implemented end-to-end (2026-08-26)

- **Script manager service** (Docs/Services/Script.md CIDs 0-17, 64+): Get number of
  scripts, Read Name, Read I/O size, Read state, Set state, Read/Write input, Read
  output, Get info, Read/Write Variable, Get/Set current instruction, Create, Delete,
  Read script (streamed), Open/Close/Write script write-stream (the stream reuses the
  Storage write-stream machinery via new `StorageStreamOpen/Write/Close` helpers).
  File name scheme `SCR` + 3-digit id (`SCR001  `), id 0 = invalid, 0xFF = auto-assign.
- **Instruction set / VM** (Core/Functions/Script.h): 4-byte symbols
  `[Type][Subtype][Value u16 LE]`; line = `[Output][Instruction][Input...] EndLine`;
  math/logic (Number fixed-point), comparisons, Compose/Extract (Vector/Colour),
  MemRead/MemWrite, If/While with embedded M&L condition expressions (end-matching
  jump targets patched at load), Delay/GetTime, Pause/Resume/Terminate/Restart/
  InfoReport/ErrorHalt, MacroCall (cross-script, depth-limited 4). Predefines:
  Bool/Char/Index/State/Type/MathOp. States per the doc; scheduler runs in the main
  loop (ScriptTick) executing forward until loop-back, wait or finish.
- **MemWrite sets ScriptUpdated** on dynamic/keyed target fields (Data Formats.md);
  static-block targets keep their const schema flags (limitation documented). Dynamic
  MemWrite creates a missing contiguous field (like the Dynamic Memory Write service).
- **v1 interpretations** (the doc's example was illustrative, not a literal program):
  no top-level instruction chaining (line 51); IF/WHILE conditions embed ops;
  degenerate `[EndIf|EndWhile|End] EndLine` lines; one body iteration per main-loop
  tick; RAM preload always (fallback to run-from-file not implemented); input-write
  wakes a Waiting script; the "instruction service" is internal (VM memory ops), the
  manager is the only user-facing service.
- **App**: `script_client.dart` (all CIDs + write stream), `script_file.dart`
  (parse/build), `script_asm.dart` (compile/decompile/validate), Scripts page
  (list/create/delete) + script editor (name, instructions with token highlighting,
  inputs/outputs/variables/constants, save via stream). Script tile gated on
  `Capability.scripts`.
- **CLI**: `script list|read|create|delete|start|stop|state` + decompiler.
- **Guard aligned** to the doc: `USE_SCRIPTS` (was `USE_SCRIPT`); added to the Tamu
  env; `Capabilities::Scripts` in `kCapabilities`.
- **Verified**: builds clean (Tamu + DAS), `flutter analyze` clean, offline tests
  36/36 incl. new `script_test.dart` (16), USB HIL script tests 2/2 + full fast HIL
  8/8.

### In-app notifications + small UI consistency (2026-08-25)

- **Settings notifications were stored but never fired (doc gap closed)**: Docs/App/
  Settings.md documents "Allow notifications (When app open) - per event selection" with
  the events Device discovered / Device lost / Backup finished. The app persisted the
  toggles but nothing consumed them. New `core/notifications.dart` (`notifyAppEvent` +
  a global `appMessengerKey`) shows an in-app SnackBar for an event only when the settings
  allow it; wired into `MaterialApp`. Events fire from: `DeviceDatabase._doRefresh`
  (a device unknown this sweep -> "discovered"; a previously reachable device that goes
  stale -> "lost") and the Backup page on a successful save ("Backup finished"). OS
  notifications (the "To OS" settings) remain a platform feature (flutter_local_notifications
  is not a dependency) - noted in Improve.md.
- **Device view "Capabilities" row was floating outside the Device info card**; moved it
  inside the card with the other rows.
- **System Memory block-level Recall in backup view refreshed nothing**: after recalling a
  block the shown (backup) values stayed stale until the next refresh; it now reloads the
  block's fields.
- Verified the static-block backup serialization (`SerializeSystemBlocks`/
  `DeserializeSystemBlocks`): `writable_count` matches the non-ReadOnly field count exactly
  and all reads are bounds-checked (no change needed).

### Vysi v1.0 display layout file preloaded (2026-08-25)

- **The layout-file mechanism was already implemented** (`Vysi1Display::LoadLayoutFromStorage`,
  format per Docs/Modules/LED display.md: `u8 width | u8 height | w*h u16 LE 0-based LED
  indices`, 0xFFFF = unused; the `LayoutFile` field's write trigger validates the file loads
  before committing). What was missing was an actual FILE on the device - only a compiled-in
  default (`LayoutVysiv1_0`) existed.
- **New `layouts/` directory in the project root** with `Vysi v1.0.lay` (the 11x10 layout,
  222 bytes, matching the compiled-in default) and a README describing the format.
- **Preloaded to the Tamu v2.0A**: `PreloadVysiLayout()` (in Vysi1Display.h) creates the
  `VYSIV1  ` file in storage on first boot when absent (never overwrites a user layout);
  called from the Tamu boot sequence after storage init. Verified live: the file appears in
  the file table (222 B), reads back byte-identical to the project file, and setting the
  LEDDisplay block's LayoutFile to `VYSIV1` loads it successfully.
- **CLI now prints String fields**: `PrintValue` had no String case, so every string field
  (e.g. the layout name) read back as "Unknown (0x00C)". It now prints the value with
  trailing space-padding trimmed. Also confirmed the short-string write path works end to
  end (`write 1 s 4 3 0x0C VYSIV1` -> field reads back `"VYSIV1"`).
- Note: this session's first (buggy) preload wrote a malformed `VYSIV1 \0` file (name byte 7
  was a NUL instead of a space), which the app could not delete (its delete pads names with
  spaces, and both entries decoded to the same trimmed name). `PreloadVysiLayout()` now
  heals such devices by deleting the legacy NUL-named entry (unconditionally, so already-
  preloaded devices are cleaned too). Verified: the file table now shows only the single
  correct `VYSIV1  `. The app's Storage page does expose delete (row popup menu ->
  `StorageClient.deleteFile`, CID 3); it works for the space-padded file.

### Device name persisted to a standalone file (2026-08-25)

Per the updated Device service doc ("Device name is stored in standalone file to allow
persistence"):
- **The device name now lives in a dedicated storage file** (`DEVNAME `, 24 bytes) instead
  of being RAM-only. `LoadPersistedDeviceName()` (idempotent, safe pre-storage-init)
  restores it at boot on BOTH the Tamu and the DAS; `PersistDeviceName()` saves on every
  Set-Name (CID 7), written NOR-safely (stage in `DEVNAME~`, rename into place - the same
  pattern the memory backups use, so a power cut never leaves a torn name).
- **BLE advertising follows the persisted name**: the advertise name already used
  `DeviceName` (`adv->setName(DeviceName)`), so once the name is loaded at boot it is what
  scans see. Verified live: renamed to `MYTEST` -> BLE scan showed `MYTEST`; hard reset
  kept it; deleting `DEVNAME` and rebooting fell back to the built-in `Tamu v2.0A`.
- **USB name**: the ESP32-C3's USB Serial/JTAG string descriptors are fixed in the
  controller (no custom descriptor path in the IDF driver), so the OS always shows the
  device as "USB JTAG/serial debug unit". The app's connected banner already shows the
  device's reported (now persisted) name instead. Not feasible without a custom USB stack.
- **CLI note**: `dev <addr> name <newname>` splits on spaces, so a name containing spaces
  (e.g. "Tamu v2.0A") can only be set via the app (CID 7 carries the full string).

### CLI discover, string writes, device name (2026-08-25)

- **CLI `dev <addr> discover` never showed a result (fixed)**: nodes ignore Discover and
  the core dropped its own broadcast, so the subcommand always ended in "no response" even
  though registration happened. The core's Discover request handler now answers the CLI
  directly (srv_tgt = CLI CID 3, id_src = the assigned ID) when the request came from the
  CLI, so `dev 1 discover` prints the SN + assigned ID. Verified live.
- **Short strings to fixed-size String fields now work (fixed)**: `StaticBlockDescriptor::Set`
  rejected any length that was not exactly the field size, so the CLI (and app) could not
  write "SNAKE" to the 8-byte layout-name field. String fields now accept shorter input,
  space-padded to the field size (bounded pad buffer so the DAS's small stack is safe); the
  write then only fails if the referenced layout file actually does not exist.
- **Tamu's reported device name was "Tamu Node" (fixed)**: `Main.cpp` defaulted the non-DAS
  name to "Tamu Node"; per Docs/Devices.md the Tamu is "Tamu v2.0A". The banner also now
  shows the device's REPORTED name with the link as the subtitle, instead of the same text
  twice.
- **Whole-device backup no longer captures script-updated fields**: Docs/Data Formats.md
  says ScriptUpdated values are stored only when the user requests that specific entry;
  `captureDevice` now skips them.

### Debugging + doc-alignment round 2026-08-25 (measurement, renderer, backup views)

Firmware:
- **`LoadAllBackups` now works on ALL devices, not just the core**: the shared version in
  `Dispatcher.h` was `#ifdef TYPE_CORE`, forcing the DAS to carry a private duplicate that
  only restored System Memory. The guard is removed (the `USE_DYNAMIC_MEMORY`/
  `USE_KEYED_MEMORY` guards stay), so any node that compiles a memory service restores its
  backup at boot through one shared function; the DAS's duplicate definition was deleted
  (its forward declaration + call remain).
- **DAS auto-range had no hysteresis (fixed)**: `raw > 850 ? 2 : (raw < 200 ? 0 : 1)` was
  applied every sample regardless of the current reference, so an unknown resistor near a
  boundary oscillated between the 330R and 330k references every loop - re-seeding the EMA
  filter each iteration (zero smoothing) and flickering `CurrentRange`. Each range now only
  leaves via its own threshold (330R->10k >400, 10k->330k >850, 10k->330R <150, 330k->10k
  <600), which cannot oscillate for a stable input.
- **LDR/NTC conversions ignored the auto-range reference (fixed)**: `MeasLDR10K` and
  `MeasNTC10K` computed lux/degC from `(ADCRES-in)/in`, which equals R/10k only on the 10k
  reference - when auto-range selected 330R or 330k the reported values were wrong (e.g. a
  1.5k NTC on the 330R range reported ~-9 degC instead of ~+30). Both now normalize the
  ratio to the 10k reference by the actual Rref.
- **Vysi1Display Polygon/Star rendered as discs (fixed)**: `CalculateShapeAlpha` only
  computed `Radius - r`, ignoring `PointNumber`, so every polygon/star drew as a filled
  circle. It now renders a regular n-gon (Polygon) and an n-point star (alternating R/R/2
  vertices) via the signed distance from the angular position within each vertex sector
  (fixed-point atan2/cos; `Number::operator/` guards the divisor).
- **Keyed backup read (CID 4) walked from the wrong data offset (fixed)**: the keyed
  branches of `BuildBackupPayload` scanned `data + 0` bounded by `fd.Size`, but a
  dictionary's entries live at the sum of the PRECEDING fields' aligned sizes - so any
  dict beyond field 0 (and every per-key read) failed with a status byte. It now offsets by
  `AlignTo4(sum of map[i].Size)` like the dynamic branch. Verified live: after save, a
  keyed backup entry and a dynamic backup field read back correctly.
- **CLI `ParseService` misread hex selectors (fixed)**: `atoi("0x05")` returns 0, so the
  documented `0x05`/`0x06` service spellings silently fell through to System Memory; only
  decimal `5`/`6` worked. Now `strtol(str, 0)` accepts both forms.
- **NTP offset arithmetic wrapped on counter rollover (fixed)**: `(int64_t)(t1 - t0)` used
  unsigned uint32 differences, so ~49.7-day wrap made a small negative interval a huge
  positive one. Both Device service and CLI time-sync now widen signed `(int32_t)` deltas
  first (matching TimeSync's own wrap-safe convention).
- **`OnVysi1FieldWrite` mutated the layout name before validating (fixed)**: it copied the
  new `LayoutFile` into the block and only then loaded the layout; a failed load reported
  failure but left the stored name pointing at an unapplied layout. It now loads through a
  temporary and reverts the RAM field on failure (stored == applied).
- **CLI field/meta replies were printed without size validation (fixed)**: the handler
  passed `desc->Size` straight to the value printer, so a truncated reply printed garbage
  past the payload. The descriptor size is now clamped to the bytes actually present.

App:
- **Memory "Backup" views actually show the backup now**: the Current/Backup toggles
  existed but every read used CID 2 (live values) - the toggle was cosmetic. The memory
  clients re-gained CID-4 backup reads (`readBackupField`/`readBackupEntry`/
  `readBackupValue`), the Sys/Dyn/Keyed pages load backup values in Backup view, the Keyed
  page gained the documented Current/Backup toggle + per-entry recall, and per-entry Recall
  re-reads the backup value it just restored.
- **Autorefresh now starts at launch**: the doc says "automatically on" (1 s), but the
  periodic timer only started on the first tab switch. The Connection page starts it in
  `initState`.
- **SNDB entries with ID 0 no longer create a phantom device**: the SNDB walk added every
  entry's ID (including unassigned 0) to the refresh set, creating a bogus "device" and
  firing requests at an invalid target each sweep. ID 0 is skipped.
- **Vector editor no longer hardcodes 3 components**: `Docs/Data Formats.md` defines Vector
  as size-flexible; the editor now follows the current value's length (falling back to 3
  for a new entry).

Notes (calibration / known):
- **AccGyr scale factors**: the `/209` (accel) and `/939` (gyro) divisors match the
  previously-working driver but are not the datasheet sensitivities for the programmed
  +/-2 g / +/-2000 dps ranges; reported units are board-calibrated, not physical - worth a
  calibration pass against a reference.

### Dictionary types + key-addressed entries + service error logging (2026-08-25)

- **Dictionary type is now settable in the app**: keyed dictionaries previously had no
  type affordance (they were created as `undefined` and immutable). The client gained
  `writeDictMeta` (CID 3, key invalid - the firmware's "update the dictionary's type"
  branch, which also clears a deleted dict's stale entries when filling it) and
  `appendDict` now takes an optional `type`. The dict tile shows the type (tappable chip +
  a "tune" icon) and the add-dictionary flow asks for a type. Verified live: type write
  sticks (`undefined -> integer`), append-with-type sticks (`colour`).
- **Keyed entries are key-addressed (order irrelevant)**: the delete-entry confirmation
  and tooltip claimed "indexes stay" - wrong framing for keyed entries, which are
  identified by their key (0x00-0xFF), not position. Re-worded to "the key is marked None
  in place; re-add the same key to restore it". The entry list now renders keys in
  ascending order (via `KeyedDict.sortedKeys`) for a stable display, since storage order
  carries no meaning.
- **Every service/module logs its errors**: the memory services previously had zero
  logging. `RespondStatus` now logs every failure on the core (DeviceLog with service tag,
  CID, and the request's block/field/key) AND broadcasts a structured LogHandler report
  (source = service, code = CID) so failures from textless nodes (DAS) reach the core's
  log DB too. Short-payload (no-BlockIndex) requests log in each memory service. The
  Storage service logs create/resize/rename/read failures, stream opens rejected or on a
  full/inactive/completed stream, and flash-write failures. Device service logs SNDB
  lookup misses and write failures. LogHandler logs allocation failures. Dispatcher logs
  unhandled services. SNDB already logged its own failures. CLI suite still 98/98 (the
  `logs` test now sees device entries from the newly-logged failures).

### Deep bug-hunt round 2026-08-25 (storage, SNDB, transports)

Firmware:
- **FindSpace allocated wrapped runs past the end of the data area (fixed)**: a free run
  spanning the end of flash and wrapping back to the start was measured as contiguous
  (`probe = (idx + run) % num_blocks`), so the file's linear
  `[offset, offset + blocks*PAGE)` range extended past `DataEnd` while the wrapped pages
  physically live at the START of storage - invisible to `BlockUsed` (double-allocation)
  and unreadable through the file API. Runs are now measured linearly (no wrap).
- **ResizeFile grow-in-place skipped the pending reservation (fixed)**: `CreateFile` and
  the resize-copy path reserve the freshly-erased area (`pending_offset/pending_blocks`)
  before `WriteFilerecord`, but the grow-in-place path erased its new tail pages and wrote
  the record without reserving them - a table move triggered by a full table could relocate
  onto the erased tail. Now reserved like `CreateFile`.
- **Block-count math overflowed for huge sizes (fixed)**: `(size + PAGE_SIZE - 1) / PAGE_SIZE`
  wraps in uint32 for sizes near 0xFFFFFFFF (the Storage service accepts untrusted sizes),
  silently yielding a 1-block allocation for a giant file. All four call sites
  (`FindSpace`/`CreateFile`/`ResizeFile`/`BlockUsed`) now use `BlocksForSize()`, which
  saturates and clamps.
- **SNDB compaction recovery only triggered when SNREG was missing (fixed)**: `Available()`
  rebuilt from the temp file only when `FileExists(SNREG)` failed, so a power loss during
  the mid-swap entry rewrite (SNREG freshly created/partially written, temp still present)
  permanently lost the staged entries. Recovery now restores from the temp whenever it
  exists (it is written before SNREG is touched, so it is authoritative at every crash
  window), and `Compact()` deletes the temp only after every entry is confirmed written.
- **AccGyr ODR write was never verified (fixed)**: `OnAccGyrFrequencyChange` compared the
  read-back config register against `cmd1[0]`/`cmd2[0]` - the *register addresses*
  (0x10/0x11), never the written values - so it always failed and `SamplingRate` never
  updated even though the ODR write succeeded. Now compares against `cmd1[1]`/`cmd2[1]`.
- **Comm LED stuck on after BLE activity (fixed)**: the BLE RX (`onWrite`) and TX notify
  paths turned the activity LED on but nothing ever turned it off. `AppBLETick` now turns
  it off at the start of every tick, making it a per-burst pulse like the USB path.
- **Dead `RemoveKey` removed** (the keyed None-in-place deletion replaced it); `Remove`
  stays - the keyed dict-fill path uses it to clear a deleted dictionary's stale entries.

App:
- **Backup restore wrote nothing (fixed)**: `restoreDevice`'s compatibility check built a
  `BlockMeta` from `flagsAndType` only, so its `size` defaulted to 0 and `liveField.meta.size
  != 0` skipped every non-empty field. It now compares against the captured `field.size`.
- **Dynamic Memory page reloaded fields into a detached block (fixed)**: after `_refresh()`
  swapped in fresh `DynBlock` objects, `_addEntry`/`_deleteEntry` called `_loadBlockFields`
  on the captured pre-refresh block, so new entries showed a permanent spinner and deleted
  entries kept their old value. Both now re-look-up the fresh block from `_blocks`.
- **Remote link loss now tears the session down (fixed)**: BLE never surfaced a remote
  disconnect (the `characteristicValueStream` has no completion), so the app stayed
  "connected" to a dead link. The BLE transport now listens to
  `UniversalBle.connectionStream(deviceId)` and raises a "BLE link lost" error on a remote
  drop; the USB transport forwards the reader stream's `onDone` (clean port close/unplug).
  Both transports guard against double-closing their stream controllers.
- **Autorefresh menu dismissal no longer disables autorefresh (fixed)**: "Off" popped `null`
  - indistinguishable from a barrier dismissal - so cancelling the dialog turned the
  setting off. "Off" now pops `Duration.zero`; a dismissal (`null`) leaves the setting
  untouched.
- **Backup restore used the picker path instead of in-memory bytes (fixed)**: with
  `withData: true` the content is in `file.bytes`; the code re-read `file.path`, which can
  be ephemeral/absent. Uses `bytes` with a path fallback.
- **Keyed next-key suggestion could overflow the 2-char field (fixed)**: a max key of 0xFF
  suggested 0x100, truncated by `maxLength: 2` to "10" (a potential collision). The
  suggestion is now clamped to 0xFF.
- **Storage table preview no longer capped at 4096 bytes (fixed)**: `_showTable` read only
  4 KB, truncating >256-record tables so the decoded view disagreed with the CID-0 list;
  it now reads the table's full size.

Investigated, no change needed:
- **DAS `Storage_FlashWrite` unaligned read-modify-write**: the preserve-the-surrounding-
  bytes read uses `maddr[byte_addr - faddr]`, which for the leading partial word indexes
  negative and wraps. In practice offsets are always >= one page in the real flows, so the
  wraparound reads the correct preceding flash byte; it is technically UB but works on the
  current hardware and was verified on it. Documented here rather than risk touching the
  verified flash path.
- **Vysi1Display shape divisions by unconfigured dimensions**: `Number::operator/` returns
  0 on a zero divisor, so an unconfigured geometry renders as a dot/black rather than
  NaN/crash - not a fault.
- **`FileCount()`/`ReadFileEntry` 8-bit width**: bounded by real storage geometry (Tamu
  ~237 pages, DAS 32), so >255 files cannot occur; no wire change needed.
- **SNDB `FindLowestAvailableID` vs explicit IDs >= 512**: unreachable (max 128 entries can
  never fill the 512-ID bitmap; explicit large IDs don't collide with the <512 scan).
- **Backup-view toggle shows current (not backup) values** on the memory pages: the backup
  values were only reachable via the (removed) CID-4 readers; the toggle switches the
  save/recall actions. Worth clarifying in the page, not a data-loss bug.

### Streamlining round 2026-08-25 (dead code removed, autorefresh unified)

Firmware (behavior-neutral; verified by the fast USB HIL 8/8 after reflash):
- **DAS `Log.h` dead branch removed**: every DAS build defines `DEVICE_LOG_TEXTLESS`
  (Main.cpp), so the `#ifndef` DeviceLog/DeviceLogHex definitions were never compiled
  and only duplicated the no-op macros. The file is now just the explanatory comment.
- **`GetRAM()` removed** (Tamu Base.h + SysFunctions.h declaration): no caller existed
  anywhere, and the DAS never implemented it (any future call would fail to link).
- **`DeviceType::Valu_v2_0` removed** (firmware Enums.h + app `DeviceType.valuV20` +
  its icon): no Valu firmware/env exists; undocumented in Docs/Devices.md. 0x02 is now
  an unused value.
- **Unused fixed-point helpers removed**: `RandomPercent`, `PercentToByte`,
  `MultiplyBytePercentByte`, `LimitPi` (Number.h), `CreateRotation2D`,
  `InverseTransform2D` (Matrix.h), `ColourClass::FromHSV` (Colour.h). Zero callers;
  `CreateTransform2D` (used by the LED display + CLI) was kept.
- **RSBus TX buffer guarded**: `static_assert(MAX_PAYLOAD_SIZE <= 256, ...)` in the
  Tamu `SendAndVerifyPacket` so the 269-byte staging buffer can never silently
  overflow if the payload cap ever grows.

App:
- **Autorefresh unified into `AutoRefreshMixin`** (widgets.dart): the Devices,
  Device-view, System/Dynamic/Keyed Memory, Storage, Log and SNDB pages each carried
  their own `Timer` + `_autoInterval` + `_applyAuto` + (for the Devices tab) ShellTabs
  listener. The mixin owns the timer, remembers the interval, pauses tab pages while
  hidden, and re-runs an immediate refresh on enable; pages only override
  `onAutoRefresh`, `onAutoRefreshStarted` and `shellTabIndex`. The Connection page
  keeps its distinct ConnectionManager-driven timer.
- **Dead client code removed**: the CID-4 backup-value readers
  (`readBackupField`/`readBackupEntry`/`readBackupValue`) had no caller (backup uses
  live `readField` + `save`), and `StorageClient.resizeFile` had no UI (the App Storage
  doc only promises upload/download).
- **Transaction-ID reuse guarded**: `_takeTxId()` skips IDs still in `_pending`, so a
  slow request can never have its 8-bit CID slot silently re-used by a later one
  (worst case with all 255 IDs in flight it throws instead of colliding).

### Consolidation + audit round 2026-08-25 (bugs fixed, code streamlined)

Firmware:
- **DAS Raw-Voltage scale bug (fixed)**: `MeasRawVoltage` multiplied the raw ADC sample by the
  `double` literal `VOLTAGE` (`3.3`); `Number` has no `double` overload, so the implicit
  double->int32 conversion truncated 3.3 to 3 and every voltage reading was ~9 % low. Now
  `N(VOLTAGE)`.
- **Storage write stream at EOF silently dropped data (fixed)**: Write Stream Open (CID 7)
  accepted `offset == file_size`, but the write path only programs while
  `current_offset < file_size`, so every chunk was discarded and the stream never closed.
  Open now rejects `offset >= file_size`.
- **RSBus receive path consolidated (refactor)**: the byte-identical
  SYNC/HEADER/PAYLOAD assembler + CRC validation lived twice (`Tamu_v2.0A/RSBus.h` and
  `DAS_v0.1/RSBus.h`). It now lives once in `Core/Functions/Bus.h`
  (`RxValidateFrame` + `ReceivePacketFrame<ReadByte>`); each device keeps a tiny byte-source
  lambda. Verified live (CLI 98/98, both buses).
- **Log DB never reused cleared holes (fixed)**: the free-slot scan only picked slots
  `>= LogCount`, so `ClearReadLogs` holes below it were never reused and the heap database
  grew to `LOG_MAX_CAPACITY` even with free slots. Any unused slot is now eligible.
- **AccGyr field renamed to match the doc**: `GyroFilter` -> `AngFilter` (Docs/Modules/
  Generic system blocks.md names the field AngFilter; schema is index-addressed so no wire
  impact).
- Small hygiene: `BLE_CHUNK` comment corrected (480 is a buffer cap, not the MTU-185
  payload); duplicate `extern LogSeq/LogCapacity` removed from Functions/Log.h; unused
  `<string>`/`<sstream>` and `esp_vfs_dev`/`driver/uart`/`linenoise` includes removed from
  the Tamu CLI Block.h.

App:
- **Memory clients consolidated (refactor)**: System/Dynamic/Keyed clients duplicated the
  same request/read-block/read-value/write-value/backup-read/save-recall plumbing. A shared
  `MemoryClientBase` (`lib/core/memory_client.dart`) now carries it; each client keeps only
  its model types and type-specific ops.
- **Save/Recall success semantics unified (bug fixed)**: the firmware answers Save/Recall
  with a single status byte (0 = ok, 0xFF = failure). sysmem checked `reply[0] == 0`, but
  dynmem/keyedmem treated ANY reply (including 0xFF) as success - a failed save/recall was
  reported as OK. All three now use the status byte.
- **Refresh no longer races a connect in flight (bug fixed)**: `refresh()` only paused while
  `_transport != null`, but `_transport` is assigned only AFTER BLE connect + discovery +
  MTU (seconds); the 1 s autorefresh kept re-enumerating mid-connect, which can starve the
  GATT session. Now paused while `_connecting` too.
- **Time-offset estimate was not the NTP formula (bug fixed)**: the CID 10 probe averaged
  `(t1 + t2)/2` (t2 is a single-sample reply's second stamp) and never captured t3. Now
  `offset = ((t1 - t0) + (t2 - t3)) / 2` with t0/t3 stamped on the app clock.
- **Serial-number editor could crash (bug fixed)**: the OK handler validated only the first
  8 hex chars then `int.parse`'d every later pair, throwing `FormatException` on a bad tail;
  the whole 28-char string is now regex-validated (a whole-string `int.tryParse` would
  overflow int64 on 112 bits).
- **Autoconnect help text now matches behaviour**: the Settings page documents
  "Long-press a device on the Connection page to set it" - long-press on a connection tile
  now sets that device as the autoconnect target (previously only the connected banner's
  autorenew icon did).
- **Refresh button while connected now does something**: the manager pauses scanning while
  connected, so the connection-page refresh now re-pulls the network
  (`DeviceDatabase.refreshNetwork()`) instead of spinning up and doing nothing.
- **`refreshNetwork()` coalesces concurrent calls (fixed)**: `connectTo` kicks one off
  unawaited, and page code can trigger another while it is still running; the second call
  used to no-op on the `_refreshing` guard and callers then read half-populated entries
  (the HIL "network discovery" test saw a type/capability-less core). Concurrent callers
  now await the in-flight refresh. USB HIL 8/8 after this fix.
- **Shared helpers**: `promptBlockNameAndType` (Dynamic/Keyed create/edit dialogs were
  byte-identical), `formatUptimeMs` (Device view + Log page), single `_hex32` in the storage
  page. Dead code removed: `ConnectionManager.isRefreshing`, `addrInvalid`/`addrBroadcast`
  constants. Unawaited futures in the connection page / `_attach` teardown are now
  `unawaited(...)`.

### BLE session dead after switching from USB (fixed, 2026-08-25)

- **Root cause**: after the app closed its USB port (link switch / manual
  disconnect) the cable stayed plugged in, so the SOF-based
  `usb_serial_jtag_is_connected()` kept reporting USB present and the firmware
  drained every response into the dead USB port - the BLE session connected but
  never received anything ("not responding/loading anything").
  `AppRxStream` now stamps `s_usb_last_rx`, and the AppInterface TX pump skips
  the USB drain when a BLE session is up and USB has been silent for 500 ms, so
  responses route to BLE. Verified: connect USB -> disconnect -> connect BLE ->
  pings all succeed (previously all timed out).
- **BLE connect retry**: a fresh connection right after a disconnect can fail
  transiently (ATT error 0x0e while the device restarts advertising);
  `BleTransport.connect()` now retries a few times.

### Memory backup files now human-readable; connection switching + autoconnect (fixed, 2026-08-25)

- **SYSMEM / DYNMEM / KEYMEM decode as registry views**: `MemoryBackupView`
  parses the serialised registries (SerializeSystemBlocks for SYSMEM; the
  common SerializeRegistry format for DYNMEM/KEYMEM: u16 block count, then per
  block name/type/map/data with aligned field data). SYSMEM shows the six Tamu
  blocks (LEDButton, Fan1/2, AccGyr, LEDDisplay, LEDDisplay2) with named
  writable fields; DYNMEM shows blocks with their entries and types; KEYMEM
  shows blocks with dictionaries and keyed entries. Verified against live
  backups and synthetic widget tests (incl. empty/corrupt files not crashing).
- **Switch connections by tapping another device while connected**: previously
  the device list was inert while a session was up; now tapping any listed
  (different) device disconnects the current session and connects to the
  tapped one.
- **Manual disconnect no longer instantly reconnects**: a manual disconnect
  (disconnect button or a device switch) suppresses autoconnect for 30 s;
  autoconnect resumes once the window expires. Link-loss disconnects (stream
  error/close) are NOT suppressed, so autoconnect still recovers dropped
  sessions.

### Storage reads truncated at ~1.8 KB; file views now match reality (fixed, 2026-08-25)

- **Root cause**: the app-interface TX ring was 2048 B with a drop-newest
  policy, but `Storage Read File` streams a whole file synchronously in one
  handler. A 4 KiB read bursts ~17 packets (~4.5 KB of wire frames), so packets
  8+ (including the stream's STOP) were silently dropped and the app received
  only ~1801 B. `APP_TX_RING_SIZE` is now 8192, so a full 4 KiB read fits.
  Verified: `readFile` returns the full 4096 B for `.TABLE` and `SNREG`.
- **Table view now agrees with the file list**: the decoded `.TABLE` records
  match the CID-0 file list exactly (live records: `.TABLE`, `SNREG`, `SYSMEM`,
  `DYNMEM`, `KEYMEM`). The table accumulates hundreds of invalidated (offset 0)
  records from old generations/backups, so the view now shows live records
  first with a "N invalidated (old) records" collapsible section instead of a
  wall of deleted entries that looked like a disagreement.
- **SNREG view decoded the wrong record layout**: `RegistryEntry` is 32 bytes
  (u16 valid marker 0x55AA/0x0000/0xFFFF, u16 short ID, 12 reserved, 14-byte
  serial) but the app read 16-byte records (14-byte SN + u16 ID), showing
  garbage. `_snregView` now steps 32 bytes, skips unwritten slots and labels
  removed entries. Verified against the live SNREG (core ID 1 / DAS ID 2).

### "Add" now fills None placeholders - no more uneditable None rows (fixed, 2026-08-25)

- **Root cause of "new blocks prefilled with none values, nothing editable"**: a
  plain append used `fieldCount` (which includes None placeholders left by
  deletes), so after deleting an entry the next append landed PAST the
  placeholder - the block accumulated an uneditable None row per delete and the
  value appeared out of sync. Dynamic "Add entry" (and keyed "Add dictionary")
  now pass the first None placeholder index when the user picks Append, so the
  freed slot is filled in place (indexes stable, no clutter). Verified: delete
  entry 0 -> re-add fills index 0 with the new value and type.
- **Filling a deleted keyed dictionary clears its stale entries**: the keyed
  Write "dictionary type update" branch now `Remove`s a None-marked dict field
  and re-inserts it empty before setting the type, so a re-added dictionary
  starts clean instead of exposing the old (deleted) keys.
- **None rows are clearly labelled**: dynamic entries render as "Entry N
  (deleted) - Add entry fills this slot"; keyed dicts as "Dictionary N
  (deleted)". They are deliberately non-editable (there is no value to edit).
- A full crash-hunt probe (create/add/edit/delete/fill/pad/refresh cycle with a
  device-alive check after every step) passes with no reboot: the earlier
  "keyed page stuck / device crashed" report matches the pre-fix behaviour of
  accumulated None blocks (readBlocks iterated hundreds of indexes) plus the
  `SetKey` tail-shift corruption, both now fixed.

### Keyed page refresh wiped open dicts; arbitrary indexes now padded with None (fixed, 2026-08-25)

- **Refresh wiped open state**: `_refresh()` swapped in fresh `KeyedBlock`
  objects whose `dicts`/`entries` maps were empty, so every open block/dict
  instantly fell back to its infinite "..." spinner until manually
  collapsed+reopened - the page looked unrefreshable. The refresh now
  re-reads the dicts of open blocks and reloads entries for open dicts after
  swapping in the new block list, keeping the view live.
- **Arbitrary (gap) indexes now work - padded with None**: creating a block
  at index N past the end pads block_count..N-1 with `BlockType::None`
  tombstones (both memory services, CID 0); writing a dyn entry or keyed dict
  at a gap field index pads the missing fields with `DataType::None` metas
  (CID 3). The padded slots read back as None and stay addressable until a
  save compacts them. The app's create/add dialogs already offer the Index
  field (empty = append).

### Keyed dictionary corruption + slow loading + index-aware creation (fixed, 2026-08-25)

- **Keyed `SetKey` tail-shift bug (root cause of "dictionary loading broken")**:
  `SetKey` shifted only the *current field's* tail when an entry grew/shrunk
  (`map[field_idx].Size - tail_start`), but later fields' data lives after the
  current field too. Adding a key to dict N after dict N+1 existed overwrote
  dict N+1's data with the new entry (and `map_count` sizes then pointed at the
  shifted garbage). Now the whole block tail is moved
  (`data_end - (cursor + tail_start)`, and the shift runs for newly-added
  entries too, not just replaced ones). Dynamic `Set` already used the whole-tail
  length; only keyed had the bug.
- **Keyed Delete (CID 1) now honours dict and key levels**: it used to be
  block-only, so the app's "delete dictionary" silently marked the WHOLE block
  None. It now marks the dictionary field's meta None (dict level) or the
  keyed entry's meta None in place via `MarkKey` (key level); block level marks
  the block None. Deleted dicts read back as empty (type None, 0 keys) and render
  as "(deleted)" placeholder rows in the app.
- **Batched dictionary read (CID 7)**: reading a dict's entries was N sequential
  round trips (very slow over BLE). New CID 7 returns the dict BlockMeta followed
  by aligned `BlockMeta + value` pairs for all visible keys in one reply; the app
  loads a whole dict in a single request (falls back to per-key reads on older
  firmware).
- **Index-aware creation**: an explicit index in a create/write request repurposes
  a None tombstone slot IN PLACE (both memory services, CID 0), so a block can be
  recreated at its old index after deletion; the app's create dialogs gained an
  optional Index field and entry/dictionary add flows let the user fill a
  placeholder slot instead of always appending.
- **File table row was not tappable**: the storage ListTile was `enabled: !isTable`
  (read-only), which also suppresses `onTap` - so the file table could never be
  opened. The row is now enabled (read-only-ness shown by the lock icon) and
  tapping opens the decoded table view. Verified end-to-end via
  `StorageClient.readFile('.TABLE')`.

### None-typed placeholders keep memory indexes stable (fixed, 2026-08-25)

Per the updated `Docs/Data Formats.md` "Basic types", deletion no longer shifts indexes:
- **DataType::None = 0x00** (renamed from Unknown; legacy alias kept) is the placeholder/spacer
  marker: "no value there, a deleted entry". A new distinct **DataType::Undefined = 0x0E** means
  a *valid* entry whose type is not specified yet (blocks created from the app default to it,
  never to None). Same for **BlockType**: None = 0x00 (tombstone), Undefined = 0x01 (valid
  unspecified), Deleted = 0x07 kept for wire compat.
- **Dynamic/Keyed entry delete (CID 1 with a field)** marks `map[field].FlagsAndType` type=None
  IN PLACE - the old `Remove()`/`RemoveKey()` compact-and-shift paths are gone. A keyed key is
  deleted the same way by writing a None-typed meta (empty value); `SetKey` already writes metas
  in place.
- **Read/summary are index-stable**: summaries (invalid block) now return the TOTAL registered
  block count (including tombstones) so every index stays addressable; tombstoned (None/Deleted)
  blocks answer their META read (as type None) while their entries are unreachable; `ListKeys`
  and `GetKey` skip None-marked keys so deleted keys are invisible but never shift others.
- **Save compacts**: `PurgeDeleted` (shared template) now also drops None blocks, so a Save
  physically frees tombstoned slots and the registry renumbers - matching "deallocated only if
  saved" in the docs. Between deletes and a save, indexes never move.
- **Firmware `EnsureCapacity` shrink bug**: `(uint16_t)(-delta) > length` overflowed for any
  small negative delta (e.g. -4 -> 65532 > length), so every shrink - including the None-mark
  delete - failed. Now `(int32_t)length + delta < 0`.
- **App**: `DataType.none` renders as "∅" placeholder (not editable), `undefined` offered for
  creation, `dataTypeLabel` covers both; block/field filters exclude `BlockType.none`; keyed
  entry tiles gained a delete action (writes a None-typed meta); `writeKeyValue`/`writeField`
  accept the 8-byte echo of an empty-value (None) write (BlockIndex is 4 bytes, not 8).
- **Autoconnect** now refreshes the network after a successful connect and keeps scanning on a
  3 s retry timer while the target is not yet discovered.
- **Storage file table view**: offset-0 records decode as "(invalidated)" (the old predicate
  `valid = off != 0xFFFFFFFF || size != 0xFFFFFFFF` showed offset-0 records as live). Verified
  `.TABLE` reads back through the app (`StorageClient.readFile`) and decodes all records.

Verified live: multi-block delete keeps survivors readable and writable; dyn entry delete leaves
fieldCount intact and the deleted slot reads back as None; keyed key delete hides the key while
others survive and new keys append after it; full CLI suite 98/98 and mem probe green.

### DAS stack overflow in the reply path (fixed)
`SendAndVerifyPacket` used 844 B of stack (a `PacketFrame` copy + two 269 B tx/rx staging
buffers) against the CH32V003's 256 B stack. Every service reply nested it under a handler
(300-570 B) and `SendResponse`'s own 270 B reply frame, so the discovery handshake and any
request-response exchange overflowed the stack into BSS. Rewritten as a byte-stream send +
incremental echo verify: <100 B stack, no RAM change. `MEMORY_BACKUP_CAP` also reduced
256 -> 64 (the DAS System Memory backup is ~52 B), keeping the memory-service handlers
(572 -> 380 B) within budget. Worst measured chains are now ~750-810 B vs ~1028 B of
contiguous headroom. DAS build switched to `-Os`.

### Static block fields are 4-byte aligned (fixed)
`StaticBlockDescriptor::Get` summed raw `Map[i].Size` without the 4-byte alignment the
dynamic/keyed descriptors use. The DAS `ResistiveMeasure` block has a 1-byte Enum field
between 4-byte Numbers, so `MeasuredValue`/`CurrentRange` (fields 4/5) were addressed at
offsets 9/13 instead of the real struct offsets 12/16. `Get()` now aligns like
`align_to_4`, fixing every static-block read/write/save/recall.

### MAX_PAYLOAD_SIZE (256) vs uint8_t payload_len (max 255)
`payload_len` is a single wire byte, so 256 payload bytes truncate to 0. Storage Read File
(CID 5) chunked at 256, corrupting any file read of >=256 B. Chunk cap is now 255 and
`Packet_Append` refuses to exceed 255.

### Storage table could overlap freshly-created file data
`AllocateContiguousBlocks` only excluded a run whose *start* fell inside the excluded
range, and the just-allocated file data was not yet in the table (so `BlockUsed` couldn't
see it). A run starting before the excluded range but overlapping it could be picked for
the new file table. The exclusion now tests the whole run.

### Storage pointer block read on the stack
`ReadTablePointer`/`WriteTablePointer` allocated `uint32_t slots[PTR_SLOTS]` on the stack
(4096 B on the Tamu). They now scan/write one 4-byte slot at a time.

### FreeRegistry leaked the registry array
Per-block Dynamic/Keyed Save/Recall freed each block's map/data but never the descriptor
array (`reg.blocks`), leaking ~224 B per call on the Tamu. Now freed.

### Storage write stream had no bounds check
A write stream could program flash past the end of its file (into the table or another
file). Writes are now clamped to the file size.

### Measuring.h still pulled in __muldi3
Despite `NUMBER_ONLY_32BIT`, the Raw Voltage and resistance conversions used explicit
`int64_t` multiplies, linking the 64-bit multiply helper. Converted to `FixedMul32`; the
DAS now links no `__muldi3`/`__divdi3` at all.

### PercentToByte scaled after truncation
`Value.ToInt() * 255` made `PercentToByte(0.5)` return 0 instead of ~127. Now scales
before truncating.

### Device service hardening
Discover handling validates `payload_len` before casting to `AssignPayload`/`SerialNumber`
(both the core's request path and the node's response path); the CID 10 time-sync reply
samples `t2` after constructing the frame (patched + CRC recomputed) instead of reusing
`t1`; SNDB Read All (CID 12) sets STOP from the actual iterated count (two passes) so the
stream always terminates; SNDB Write (CID 14) now acknowledges every request (empty frame
on failure).

### REQACK failures now answered
Memory-service request paths that previously dropped the frame silently
(`break`/`return`) on an invalid block/field or a short payload now send
`RespondStatus(frame, false)`: SystemMemory CID 2/3, DynamicMemory CID 2/3, KeyedMemory
CID 2/3. `DynamicMemory::Set`/`insert_field` and `KeyedMemory::SetKey` failures report
failure instead of a false-success echo.

### OOM rollback in registry copy/deserialize
`CopyBlockInto` and `DeserializeRegistry` now remove the partially-built block if a
mid-copy allocation fails, leaving the registry unchanged instead of holding a broken
entry.

### CLI fixes
`parse_cli_value` now parses DevType, Index, Enum, SN (hex), String, Colour and Vector
(and the Matrix path no longer leaves its scratch buffer unterminated); a 1-byte status
response is printed as an operation failure instead of being misparsed as a `BlockIndex`;
the `time` offset display uses the local receive stamp (t3); `dev` hint gained `loop` and
the `log` hint matches its parser.

### LED display block wired to the hardware
The Tamu main loop now calls `Display1.Render()` (the Vysi1Display block renderer, driven
by the LEDDisplay block's Brightness/Offset/RenderBlock fields) and sends its 86-LED
buffer to both strips (pins 0,3). The hardcoded test-pattern path
(`LEDDisplay::Display1`/`testBuffer`) is removed.

### Misc header hygiene
`sqrt`/`sin`/`atan2`/`log`/`RandomPercent` (Number.h), `TimeUpdate` (SysFunctions.h) and
`isKeyedType` (Enums.h) are `inline`; `RandomPercent` now uses the top 16 bits of the LCG
(matching its comment); `ColourClass::Layer` clamps the blended R/G/B channels against
out-of-range `Overlap`.

### Backup-file rewrite never erased flash (fixed)
`WriteBackupFile` wrote straight over an existing same-size backup file. NOR flash can only
program 1s to 0s, so on the second Save of the same file any bit that had to return to 1
stayed 0, silently corrupting the stored registry (System/Dynamic/Keyed memory backups).
It now erases the file's data blocks before rewriting whenever the file already existed.
This also covers the ResizeFile-shrink path (which keeps the same blocks un-erased).

### LEDButton static block layout mismatched Get() (fixed)
`LEDButtonStruct` held two adjacent 1-byte bools (`LEDState`@0, `ButtonState`@1), but
`StaticBlockDescriptor::Get` addresses fields at 4-byte-aligned offsets, so field 1 resolved
to offset 4 instead of 1 (reading/writing out of bounds). Added 3 bytes of padding so
`ButtonState` sits at offset 4, matching the alignment used by every other static block.

### TimeUpdate() first-tick loop-time spike (fixed)
On the first call `DeviceStatus.UptimeMs`/`LastTime` are both 0, so `DeltaTime` equalled the
whole boot uptime, spiking `AvgLoopTimeMs`/`MaxLoopTimeMs`. `TimeUpdate` now primes the
clock (no `DeltaTime`) on its first invocation.

### Keyed Memory dictionary read could overrun `keys[64]` (fixed)
`ListKeys` fills up to 64 entries but returns the *total* key count; the response then copied
`key_count` bytes out of the 64-byte stack buffer. The copy length is now clamped to what
actually landed in the buffer.

### Vysi1Display renderer kept stale pixels (fixed)
`Render()` never cleared the LED `Buffer`, so LEDs not covered by the current frame's geometry
kept their previous colour (the doc says a texture clears the buffer before applying). The
buffer is now cleared at the start of every `Render()` call.

### Device service replies deduplicated (refactor)
The nine near-identical `Packet_Construct`+`Dispatcher_Dispatch` reply blocks (Ping, Type, SN,
Version, Capability, Read/Set Name, Uptime, Loop Time) now share a `SendDeviceReply` helper.

### Tamu device renamed to v2.0A (refactor)
The device folder is now `Devices/Tamu_v2.0A`, the PlatformIO env `Tamu_v2_0A` and the board
define `BOARD_Tamu_v2_0A` (was `BOARD_Tamu_v2_0`). The `DeviceType` enum value was renamed to
`Tamu_v2_0A` and the reported software version to "Tamu v2.0A". The env's `sdkconfig` was
carried over so the USB Serial/JTAG console and NimBLE configuration match the old build.

### Tamu now has a second LED display (fixed)
The Tamu drives two WS2812 strips (pins 0, 3) but only had a single `Vysi1Display` instance,
so both strips always showed the same image despite the "LED Display x2" module. A second
`Vysi1Display Display2` instance was added, registered in the static block registry as
"LEDDisplay2", and the main loop renders and sends each strip from its own display block.

### Tamu always boots as core (fixed)
Becoming core (ShortAddress 1) was a side effect of pressing the LED button during the startup
discovery loop, which is undocumented and easy to trigger accidentally (see Improve). The Tamu
now always assigns itself ID 1 at boot; the button check and discovery loop were removed.

### SNDB is now a single shared implementation (refactor)
The serial-number database lived in `Devices/Tamu/SNDB.h` and talked to flash through
`esp_partition_*` directly, making it device-specific. It is now a single implementation in
`Core/Functions/SNDB.h` that stores the registry in the reserved tail of the storage region
(`Storage_FlashReserve` bytes) and accesses it through the shared `Storage_FlashRead/Write/
Erase` functions. The device-specific file was removed.

### SNDB migrated to the storage file system (refactor)
The serial-number registry no longer uses a reserved tail of the storage region
(`Storage_FlashReserve` was removed from both device Storage.h files and from
`Core/Functions/Storage.h`; the file data area is now the full flash region). The registry
is stored as a log-structured file named `SNREG` in the filesystem (`Core/Functions/SNDB.h`),
using only the public file API (`FileExists`/`CreateFile`/`DeleteFile`/`ReadFromFile`/
`WriteToFile`): new entries are appended, removed entries are tombstoned (valid 0x55AA ->
0x0000), and when the file fills up `Compact()` rewrites it densely (delete + recreate +
write valid entries). Appends are crash-safe (body written with an EMPTY marker, then the
VALID marker last), and `RecoverState()` treats only fully-erased (0xFF) slots as the append
head. Capacity is `SNDB_MAX_ENTRIES` (128 by default, override via build flag); the DAS
never compiles SNDB.h (no TYPE_CORE), so it is unaffected.

### Storage WriteTable no longer snapshots on every write (refactor)
`WriteTable` always buffered the whole file table into a `FileEntry[MAX_FILES]` stack array
(112 B on the DAS's 256 B stack, 896 B on the Tamu) even when writing to a fresh table
location. It now streams entries one at a time when the table moves, and only snapshots for
in-place delete/rename/resize operations. `Format()` also writes only the self-describing
entry 0 (the erase already leaves every other slot free), dropping its `MAX_FILES` array.

### NUMBER_ONLY_32BIT define for the Number class
`Core/Types/Number.h` now supports `NUMBER_ONLY_32BIT`: when defined, `Number::operator*`
and `operator/` use 32-bit-only math (`FixedMul32`/`FixedDiv32`), so no libgcc 64-bit
helpers (`__muldi3`, `__divdi3`) can be pulled in. Verified bit-exact against the 64-bit
versions over 1M random inputs. Enabled for the DAS build in `platformio.ini`.

### Storage reworked to the updated Storage.md spec (refactor)
The storage layer was rewritten to match the updated `Docs/Services/Storage.md`:
- **File records are 16 bytes** (Offset 32bit + Filesize 32bit + Name 8 plain-text chars)
  instead of the packed 14-byte compressed-name record; `FileEntry` is naturally 4-aligned
  and no longer carries 6-bit compressed names.
- **The file table is now a file itself** with a flexible size: entry 0 self-describes
  (points to its own location and byte length), and `MoveFiletable()` grows it by one page
  when it is >75% full, relocating via `FindSpace` and copying only the valid entries.
- **In-place record model**: records are appended (`WriteFilerecord`), invalidated holes are
  never rewritten (NOR 1->0), and the pointer page (first page) is only touched when the
  table moves - giving even wear across the data area (rotating `FindSpace` cursor) with a
  reserved low-wear first page.
- **Per-device main functions** now match the doc: `Storage_FlashRead` returns bytes read,
  `Storage_FlashFormat()` wipes the entire storage region, and `Erase` is byte-length based.
- **Service CIDs 2/4 respond Success (bool)** instead of a start offset; names in service
  payloads are now 8 plain-text bytes. `CreateFile`/`DeleteFile`/`ResizeFile` return `bool`,
  `FileExists` returns the filesize or `0xFFFFFFFF`, and the new `ReadFromFile`/`WriteToFile`
  wrappers exist. Consumers (memory-service backup names, Script, CLI) were updated to the
  plain 8-char form. `STORAGE_MAX_FILES` was removed from the build flags.
- This also supersedes the earlier `WriteTable` snapshot refactor and resolves the Improve.md
  "orphaned tables" concern (the table now relocates and the old pages are reused).

### Real-hardware session 2026-08-22 (see `Docs/RealHW.md`)
- **CLI REPL stack overflow**: every `read`/`write`/`save`/`recall`/`rmem` crashed
  `console_repl` with a HW stack-guard fault because local dispatch runs the whole
  request+response chain recursively on the REPL task stack (default 4096 B). `StartCLI`
  now sets `task_stack_size = 16384` (`Devices/Tamu_v2.0A/CLI/Entry.h`).
- **LSM6DS3 I2C bus had no pull-ups**: `Devices/Tamu_v2.0A/AccGyr.h` created the master bus
  without `enable_internal_pullup`; this board has no external pull-ups on SDA4/SCL5, so the
  lines floated low, every read returned `ESP_OK` with zeros and config writes were dropped
  (sensor looked dead, WHO_AM_I=0x00). Fixed by matching the previously-working driver
  (`I2C.h`/`I2CDevice.h` reference): `gpio_reset_pin`, `I2C_CLK_SRC_RC_FAST`,
  `enable_internal_pullup=true`, finite 1000 ms timeouts. The IMU now reads live accel/gyro
  (WHO_AM_I=0x6A). `trans_queue_depth` raised 1 -> 8 (the 10 ms loop overflowed the op pool:
  "ops list is full"), and the init gained a forced BOOT+SW_RESET fallback + config
  read-back verification for the part's occasional stuck state.
- **Core SN registered as ID 1 in the SNDB**: previously the core's own SN was absent, so a
  Discover of itself (CLI self-test) allocated a fresh ID (2) and left a bogus entry.
  `Devices/Tamu_v2.0A/Main.h` now registers `SN -> 1` at boot (replacing any wrong ID).
- **CLI `dev time` now stamps t0**: the time-sync request previously carried a zeroed
  payload, making the offset estimate off by half the uptime.
- **DAS `Now()`/`Sleep()` froze in tight loops (fixed)**: `Devices/DAS_v0.1/Base.h::Now()`
  added `(SysTick delta) / (SystemCoreClock/1000)` to `ms_accum`. The WCH QingKe SysTick
  free-runs *up* at HCLK (CMP=0, 48 MHz), so in a tight `Sleep(ms)` loop the delta between
  consecutive `Now()` calls is a few hundred cycles - far below 48000 - and integer division
  truncates every contribution to 0. `ms_accum` never advanced, so the first `Sleep(500)` in
  the discovery loop spun forever: the DAS broadcast one Discover, never processed the core's
  ID-assignment reply (ShortAddress stayed 0), never sampled, and never answered. Fixed by
  carrying the fractional cycles in a remainder accumulator so the millisecond count advances
  regardless of poll frequency. Diagnosed on hardware via the Tamu's LogHandler records and
  WCH-Link RAM/SysTick register dumps (the DAS has no console): frozen `ms_accum`/`last_cnt`,
  the 29-byte assign reply stuck unread in the RX ring buffer (head=56, tail=27), and
  SysTick CNT having advanced 897 M cycles while `ms_accum` stood still.
- **Core->node time-sync offset had the wrong sign (fixed)**: `TimeSyncService` pushes the NTP
  offset theta = node_time - core_time (positive = node ahead). The node applied it as
  `UptimeMs = Now() + TimeOffsetMs`, i.e. *added* theta, doubling the error every 5-minute sync
  round (theta, 2*theta, 4*theta...). On hardware the DAS uptime diverged to ~4.29 G ms within a
  session. `HandleDeviceService` CID 11 now stores `-theta`, and `dev 2 time` converges to a
  sub-second offset. The DAS uptime tracks the core's within ~1 s after a sync round.
- **DAS default device name was "Tamu Node" (fixed)**: `src/Main.cpp` hard-coded the shared
  `DeviceNameBuffer = "Tamu Node"` for every board, so the DAS reported the core's name. It is
  now board-specific (`"DAS v0.1"` under `BOARD_DAS_v0_1`).
- **DAS `Now()` baseline could include a stale SysTick count (fixed)**: the free-running SysTick
  counter is not guaranteed to start at 0 after a debugger reboot-into-halt/resume, and the
  `ms_rem`-based `Now()` started from `last_cnt = 0`, adding the stale counter value as a bogus
  uptime chunk. The first `Now()` call now primes `last_cnt` from the live counter.
- **DAS storage erase op selection (fixed)**: early debugging mis-attributed the storage
  corruption to "fast erase leaving cells at 0x00"; the real root cause (see the next section)
  was using the 1 KB `FLASH_ErasePage` sector erase per 64 B allocation block, which wiped the
  pointer page and file table. The current code uses the 64 B `FLASH_ErasePage_Fast` page erase
  for file operations and the 1 KB sector erase only in `Format()`, with `STORAGE_BLOCK_SIZE`
  asserted equal to the 64 B erase page.
- **DAS now verified working on hardware**: discovery (ID 2), ping, type/sn/version, System
  Memory block reads/writes, and the resistive measurement loop (ADC sample + auto-range +
  kOhm conversion) all respond over RSBus through the Tamu. `Meas1/Meas2` report ~1022 raw
  (open input -> 330 kOhm auto-range, ~13750 kOhm computed) as expected with nothing wired.

## Fixed (RealHW 2026-08-23 bug report - storage corruption)

**Root cause of "every create/save leaves only the newest file": the wrong erase
operation.** The CH32V003 flash controller has two distinct erase ops (`ch32v00x_flash.c`,
`ROM_ERASE`):
- `CR_PAGE_ER` / `FLASH_ErasePage_Fast` = **64-byte page erase** (the application-note
  mechanism for regular operation),
- `CR_PER` / `FLASH_ErasePage` = **1 KB sector erase** (its docstring "page(1KB)" was
  literally correct).

All previous storage code used `FLASH_ErasePage` (the 1 KB op) per allocation block - first
at 256 B steps, then (after the block-size change) at 64 B steps. Every file-data erase
therefore wiped an entire kilobyte around it, taking the pointer page and file table with it
whenever the data area shared that sector. The reported flash dump is reproduced exactly:
BBB's erase at [192,256) destroyed offsets 0..256 (pointer + `.TABLE` + AAA), after which
`WriteFilerecord`'s re-scan saw an "empty" table and appended BBB's record cleanly at slot 0.
`resize` failing and `delete` misbehaving are downstream casualties: resize's append-then-
invalidate sequence programs records over non-erased cells (NOR 1->0 violation ->
`FLASH_ProgramWord` error -> "File resize failed"), and delete operated on already-wiped
records.

Fixes:
- `Storage_FlashErase` now uses `FLASH_ErasePage_Fast` (64 B `CR_PAGE_ER`) with both standard
  and fast-mode unlocks, one page per allocation block. `STORAGE_BLOCK_SIZE` stays 64 B,
  enforced equal to `FLASH_ERASE_PAGE_SIZE` by static_assert.
- `Storage_FlashFormat` deliberately uses the coarse `CR_PER` 1 KB sector erase (two calls
  cover the whole region) - appropriate for format only, per the application note.
- **DAS `tree 2` empty dump**: `CmdTree` sent its three summary reads back-to-back; on the
  half-duplex bus the node's answer to request 1 collided with the core's transmission of
  request 2 (the node's CSMA gives up after its bounded wait and transmits into a busy
  window), so the System summary reply was lost while lone `read` commands (clean turnaround)
  kept working. The CLI now spaces the three requests **400 ms** apart (100 ms was still too
  short for the DAS's slower reply; verified on hardware that 400 ms works). Dynamic/Keyed
  summaries legitimately get no answer from a node (services not compiled in).

Needs hardware verification: that the fast page erase (with the full KEYR+MODEKEYR unlock
sequence) restores cells to 0xFF - the earlier session's "fast erase leaves 0x00" observation
predates this analysis and may itself have been caused by the missing/mismatched unlock
sequence or by reading back through the same confusion. Retest recipe unchanged (create ->
verify table -> dump region -> resize grow -> delete), plus confirm `tree 2` prints the
System summary.

## Fixed (App Interface implementation + device/app interaction audit, 2026-08-23)

Firmware (`USE_APP_INTERFACE`, core only):
- **App Interface service implemented**: identity = `ServiceType::App (0x08)` in SRV SRC
  (CID byte = app transaction ID); no app network address - the core rewrites
  `id_src` on ingress (proxy) so responses route back by service type alone
  (`Dispatcher.h` case App -> TX stream). Wire helpers `PacketWireSize/PacketToWire`
  added to Packet.h.
- **Core routing module** `Core/Functions/AppInterface.h`: global `AppConnected`,
  heap TX ring (2048 B, drop-newest when full), inbound frame queue dispatched from a
  single task, session-scoped TX flush on attach/detach.
- **USB exclusive mode machine** (`AppUSB.h` + reworked CLI): own USJ driver + VFS
  stdio; CLI mode (line editor + shadow sniffer, candidate bytes withheld from the
  line editor so embedded \n cannot execute garbage commands) vs APP mode (all bytes
  to the app, CLI fully ignored); app has priority (first valid link frame attaches);
  detach on physical USB loss (`usb_serial_jtag_is_connected`, SOF-based); comm LED
  pulses on RX/TX bursts only.
- **BLE link** (`AppBLE.h`): Nordic UART UUIDs, uint16-LE length-prefixed transfers,
  negotiated-MTU-aware notification chunking, 20 ms pacing with notify backpressure
  (no data loss), deferred advertising restart retried until it succeeds.

App:
- Requests now carry REQACK (**critical fix** - System/Dynamic/Keyed Memory services
  respond only when it is set; without it every memory request timed out).
- `srvSource` switched to the App service type (0x08); BLE UUIDs replaced with the
  final Nordic UART values; MTU properly negotiated via `UniversalBle.requestMtu`;
  BLE outgoing chunking fixed to respect ATT MTU - 3 (test updated accordingly).
- New service clients and pages: Dynamic Memory, Keyed Memory, Storage (file table +
  file preview), Log viewer, SNDB viewer; Device view gained the documented time
  offset row (CID 10 probe) and capability-gated service links.
- **Backup restore bug**: BlockMeta.Size was never serialized, so the restore
  compatibility check compared against 0 and silently skipped every field; size now
  stored in the archive (older archives fall back to payload length).

Router table viewer and Script editor remain open until their firmware services exist.

## Fixed (RemoteOrigin deprecation + Log DB rework, 2026-08-23)

- **`FieldFlags::RemoteOrigin` deprecated and removed** (per updated docs): dropped from
  Enums.h and the CLI flag printer. Bit 15 is now free for future use.
- **Log DB reworked per the updated Log Handler doc**:
  - Lives on the heap and GROWS when full (initial 32 records, +16 per growth, hard cap
    512); only when the heap cannot provide more room is the OLDEST record dropped -
    never the new one. Each record carries a RAM-only monotonic sequence number so
    "oldest" is well defined even with dedup-in-place (the seq array is not transmitted;
    GetLogs still streams plain documented LogRecords).
  - ClearReadLogs now clears the N most recently RECEIVED records (by sequence), since
    slot order stopped tracking age once evictions became possible.
  - GetLogs/ClearReadLogs are now reachable over the bus: the CLI gained `logget [addr]`
    and `logclear [addr] [count]` (responses routed via srv_src = CLI CID 6; timeout
    errors like every other command). The App can consume the same CIDs.
  - Local `logs` command prints chronologically (sequence order) instead of slot order.

## Open issues (service audit, 2026-08-21)

Services verified against `Docs/Services/*.md`, `Docs/Data Formats.md`, `Docs/RSBus.md`,
`Docs/Modules/*.md` and `Docs/Devices.md`. Excluded (not fully documented / not implemented):
Script, App Interface, Router.

### Doc mismatches — FIXED

- **System Memory Read backup (CID 4)** now returns `BlockIndex + BlockMeta + Value` per the
  docs: `SystemBackupPayload()` parses the backup file directly (no heap) and serves a summary
  (invalid block), a block's meta + stored field count (invalid field), or a single stored field
  value.
- **System Memory Save (CID 5) / Recall (CID 6)** are now per-entry: Save writes one block into
  the backup file keeping the others untouched (`SaveSystemBlockToFile` merges); Recall restores
  one block (`DeserializeSystemBlocks` gained a `target_block` filter). Invalid block = all.
- **Dynamic/Keyed Recall (CID 6)** now restores a single block from the backup file
  (`RecallRegistryBlock`), matching per-entry Save. Temporary registries are released afterwards
  (`FreeRegistry`), fixing a heap leak in per-block Save.
- **DAS `Measuring_Update` honors `SensorType`** (Raw Measurement = ADC sample, Raw Voltage =
  volts, Raw Resistance / LDR 10K / NTC 10K = kOhm).
- **Dynamic/Keyed enable guards** now use `USE_DYNAMIC_MEMORY`/`USE_KEYED_MEMORY` (as the docs
  state) instead of `TYPE_CORE`, in the service files, the Dispatcher routing and
  `LoadAllBackups`. A core build without `USE_*` no longer fails to link.
- **`SysFunctions.h` comment** corrected: the time offset is set via Device service CID 11, not 9.
- **Node time-sync reply (CID 10)** reports distinct "local time received" (t1) and "local time
  reply sent" (t2) stamps.
- **Log database is on the heap** on core devices (`LogBuffer`/`LogUsed` are lazily allocated by
  `EnsureLogStorage()`), per the Log Handler doc. Frees ~530 B of static RAM on the core.

### Remaining doc mismatches (intentionally left open)

- ~~Capability bitfield is empty~~ **RESOLVED**: `Capabilities::Core = bit 0` implemented;
  Tamu reports it, DAS reports none, Discover gating uses the bit (2026-08-22).

### Cleanup / dead code

- `src/Blocks/Vysi1Display.h` includes `esp_log.h` and uses `keyed_block_registry` (TYPE_CORE) —
  the shared Blocks folder therefore contains ESP32/core-only code usable only on the Tamu.
  (RSTest.h, VysiTest.h, `_todo/*`, `LEDDisplay.h` and the `[env:Valu_v2_0]` build were removed.)

## Needs hardware verification

- BLE HIL back-to-back flakiness (known, pre-existing): running the whole
  `hil_live_test.dart` suite over BLE stresses the link with rapid connect ->
  work -> disconnect -> reconnect cycles (the device defers its advertising
  restart). Tests intermittently drop mid-session ("Disconnected"), and the
  failing test rotates between runs. The isolated user-facing flows
  (`ble_ping_test`, `ble_switch_test`, `ble_scan_probe_test`) pass reliably
  every time, and the USB HIL suite is 8/8.
- App Interface end-to-end: USB CLI -> attach app mid-session (priority switch), detach
  on unplug back to CLI; USB and BLE sessions each running memory/storage/log reads;
  BLE MTU negotiation chunk sizes; comm LED activity pulses.
- Measurement constants (NTC/LDR/auto-range thresholds) · IMU scales · **fast-erase restores 0xFF**
  (underpins the storage fix) · full storage flow incl. rename + backup power-cut behavior ·
  app_task/console task stack HWM.- **Measurement calibration**: `Measuring_Update` reports resistance in kOhm (so 330 kOhm fits
  Q16.16) and uses a simple auto-range heuristic; divider constants and thresholds need tuning
  against the real sensor.
- **IMU scale/units calibration**: the accel/gyro work on hardware now; a stationary unit reads
  ~1 g as Z ~= 10.28 with the `/209` divisor. The config bytes `{0x44, 0x4C}` and the
  `/209`/`/939` scale factors are identical to the previously-working reference driver, but the
  resulting physical units vs the LSM6DS3-family datasheet sensitivities deserve a calibration
  pass against a known reference.

## On hold
- ~~LED display block Layout File Name + Refresh Rate fields~~ **IMPLEMENTED** (2026-08-23,
  see the docs-vs-code audit implementation round). Nothing on hold.

## Fixed (firmware code review, 2026-08-22)

Full pass over `tamu/src` (Core, Blocks, Devices); findings were verified against source,
then fixed in the same session. Both environments build clean after every fix
(DAS_v0_1: RAM ~51 % / flash ~78 %; Tamu_v2_0A: clean).

### LED / display

- **DoubleParabola read uninitialized geometry data**: `ResolveGeometryDefinition()` gained a
  `DoubleParabola` case (Width/Height/EdgeFade), and both `GeometryData`/`TextureData` union
  constructors now zero-initialise their active member so no rasterizer path can read
  indeterminate memory (`Vysi1Display.h`, `Render.h`).
- **Negative Brightness wrapped**: Brightness is clamped to [0, 100] before the uint32_t
  brightness scale is computed.

### Packet / bus

- **Crc8 length truncation**: `Crc8` now takes a `uint16_t` length; all call sites
  (`PacketConstruct`, `PacketAppend`, both RSBus send/verify paths) use it, so frames with
  payload >= 245 B no longer lose CRC coverage.
- **DAS RX ISR tested the wrong constant**: `USART_IT_RXNE` (interrupt-config encoding) was
  replaced with `USART_FLAG_RXNE`; the no-op `STATR &= ~(...)` write was removed (the flags
  are read-to-clear via DATAR).
- **Storage table read dropped FLAG_STOP** (CID 0): entries are counted first, then emitted,
  so the final packet always carries FLAG_STOP even if a late entry read fails.
- **Storage file read with num_bytes == 0 sent nothing** (CID 5): an empty START|STOP frame
  is answered so the client never hangs.
- **Blocking loops without timeout bounded**: `RS485_WaitForSilence` on both devices gives up
  after RS485_SILENCE_TIMEOUT_MS (100 ms), and the DAS ADC EOC wait is bounded (~10 ms).

### Storage / SNDB

- **SNDB erased-slot pattern was wrong**: the `{0xFF}` array initializer only set byte 0;
  `RecoverState()` now builds a fully erased comparison buffer with `memset`.
- **SNDB Compact was non-atomic**: compaction stages valid entries in a temp file
  (`SNRTMP`) while SNREG stays intact; SNREG is swapped last. A power failure mid-compaction
  is repaired by `Available()`, which rebuilds SNREG from the temp file.
- **Table move could land on pending file data**: `CreateFile`/`ResizeFile` copy path reserve
  the freshly-erased area via new `pending_offset/pending_blocks` members; `FindSpace`,
  `BlockUsed` treat reserved pages as used until the committing record is written.
- **Field/key delete always reported success**: Dynamic Memory field delete (Deleted type)
  and Keyed Memory `RemoveKey` now report the actual result.
- **Keyed Size uint8_t wrap**: `SetKey` refuses to grow a dictionary field past 255 bytes.
- **GetKey walked dictionary entries without bounds checks**: a shared `KeyedEntryFits()`
  helper is now used by `GetKey`/`SetKey`/`ListKeys`/`RemoveKey`; corrupt/truncated entries
  end the walk instead of stepping into foreign memory.

### Dynamic memory registry

- **CopyBlockInto left stale pointers for empty blocks**: the destination slot's
  map/data_ptr/lengths are zeroed unconditionally before the allocation branches - no more
  aliased/dangling descriptors that would double-free later.
- **First per-block Save failed while the backup file didn't exist**: with no backup file,
  the whole registry is stored once so stored block indices stay aligned with live ones.

### DAS node

- **ProcessBus ran only ~once per second**: the LED blink is non-blocking (millis-based
  toggle), so the main loop services the bus continuously.
- **Auto-range switch corrupted the measurement filter**: filter state is re-seeded with the
  raw sample on every range transition, range thresholds use the *filtered* value (no
  chatter near thresholds), and FilterCoeff is clamped to [0,1].
- **Meas2.SamplingRate was dead configuration**: each channel samples at its own configured
  rate (`SampleIntervalMs()`).

### CLI / misc

- **CLI matrix 'T' mode crashed on short input**: strtok results are NULL-checked like the
  'R' branch.
- **AccGyr filter divide-by-zero**: AccFilter/GyroFilter are clamped to [0,1] before the
  filter weights are computed.
- **No allocator-failure handling in log DB init**: `EnsureLogStorage` frees and nulls both
  allocations if either fails; `HandleLogHandler` checks `LogUsed` too.
- **CLI keyed-field reply length underflow**: `HandleCLIService` requires BlockIndex +
  BlockMeta bytes before doing pointer arithmetic on field replies.
- **LED-off left the button line floating**: turning the LED off now switches the shared
  pin to pull-down input (`PinModeInputPullDown`), matching the active-high button read in
  `ButtonUpdate()`.

### Investigated, no change needed

- **Blocks metadata offsets all 0x00**: harmless by design - `StaticBlockDescriptor::Get()`
  derives field offsets from aligned schema sizes and ignores the metadata offset byte
  (it is the keyed-entry Key field elsewhere). Documented here so nobody "fixes" it blindly.

## Fixed (firmware code review round 2, 2026-08-22)

Second pass focused on node-reachable service code and DAS flash size
(12,744 -> 11,120 bytes, 77.8 % -> 67.9 %; RAM 1,056 -> 1,036 B).

### Follow-up fixes (same day, third pass)

- **DAS `ReceivePacket` no longer discards partial frames**: reworked as a persistent
  SYNC/HEADER/PAYLOAD assembly state machine that writes directly into the caller's static
  frame and survives across `ProcessBus()` polls - a frame split mid-transfer now completes
  instead of being consumed and lost. The unused `UART_ReadBuffer` helper was removed with
  it. Residual limitation (documented in code): a sender aborting mid-frame desyncs until
  length/CRC checks fail, same recovery as before.
- **Capability bitfield implemented** (`Core/Types/Enums.h`): `Capabilities::Core = bit 0`
  per the Device service doc ("if device has core capability, it can assign IDs and store
  them in the registry"). The Tamu reports it (`kCapabilities = Capabilities::Core`), the
  DAS reports none, and the Discover handler now gates ID assignment on the reported CORE
  bit in addition to the short address, so behaviour matches the advertised capability.
- **Tamu app task stack raised 8192 -> 16384 bytes**: `LoadAllBackups` places a 2 KB buffer
  on this stack plus ProcessBus dispatch recursion (matching the REPL task sizing rationale).
- DAS flash after these changes: 11,128 B (67.9 %), RAM 1,040 B.

### Correctness

- **Time-sync offset assignment never converged** (`Core/Services/Device.h` CID 11): theta is
  measured against the node's *displayed* time (which already contains the old offset), so
  the correction must accumulate - `TimeOffsetMs -= theta`. The previous `= -theta` left the
  previous round's residual alive forever (error oscillated between +/- one sync-interval of
  drift instead of settling at 0).
- **File-table pointer recovery picked stale tables** (`Core/Functions/Storage.h`):
  `WriteTablePointer` writes the new pointer *before* invalidating older slots (the safer
  order - invalidating first would risk booting into a Format() wipe), but `FindFiletable`
  returned the FIRST valid slot. After an interrupted update both old and new slots are
  valid, so a crash could silently boot the stale table while `FindSpace` considers the new
  table's pages free and reallocates over them. `FindFiletable` now returns the LAST valid
  slot (slots fill sequentially within a page generation, so highest = newest).
- **TimeSync scheduling broke on UptimeMs wraparound** (`Core/Functions/TimeSync.h`): bare
  `now_ms >= due_ms` comparisons stop firing for ~49 days after wrap; switched to signed
  differences `(int32_t)(now_ms - due_ms) >= 0`.
- **Max-loop-time reset could be missed entirely** (`SysFunctions.h::TimeUpdate`): the old
  `UptimeMs % 20000 < 20` test only fires if a tick lands inside a 20 ms window; ticks slower
  than 20 ms never reset the max. Replaced with a deterministic 20 s window counter. The
  first-call prime condition also uses a bool flag now (an uptime that ever passes through
  exactly 0 would have re-triggered it).
- **CLI reply parsers read past truncated payloads** (`Devices/Tamu_v2.0A/CLI/Handler.h`): the
  registry-summary branch indexed `data_ptr[0]` with only BlockIndex-length guaranteed, and
  the block-meta branch dereferenced a full BlockMeta under the same guard. Both now verify
  the payload actually carries those bytes.
- **CLI Matrix print ignored data_len** (`Devices/Tamu_v2.0A/CLI/Block.h`): a truncated reply
  would print garbage matrix dimensions/values; guarded now. `FloatToNumber` also saturates
  instead of invoking UB through an out-of-range int32 cast.
- **CLI SNDB responses vanished silently** (`Devices/Tamu_v2.0A/CLI/SNDB.h`): 1-byte failure
  statuses printed nothing; they now report like the main CLI handler. The command parser
  also dropped its per-command `std::string` heap allocation for plain `strcmp`.

### Robustness guards

- **Payload alignment invariant pinned** (`Core/Functions/Packet.h`): direct word reads out
  of `frame.payload` are only legal because the packed header is exactly 12 bytes
  (`static_assert(offsetof(PacketFrame, payload) % 4 == 0)`) - RV32EC faults on misaligned
  loads. If the header ever changes, those call sites must switch to memcpy.
- **Main.cpp global definitions moved below their headers**: `DeltaTime`/`LastTime`/
  `TimeOffsetMs` were defined before the headers declaring them (include-order fragility).
- **StoredName ctor duplicated EncodeName's bit packing** (`Core/Types/Name.h`): factored into
  a shared constexpr `PackNameBytes`.

### DAS flash size (-1,624 B)

- **`-flto` enabled** for `env:DAS_v0_1` (-1,212 B): the single-TU header build cross-inlines
  heavily; verified after enabling that `USART1_IRQHandler` stays a global symbol and
  `.fixed_data` remains pinned at 0x3800 (~3.2 KB headroom before the storage region).
- **DeviceLog call sites compiled out** (-376 B incl. the next item): the DAS log format
  carries no free text, so every formatted diagnostic was string-literal rodata plus vararg
  setup feeding a function that discards its arguments. `DEVICE_LOG_TEXTLESS` (Main.cpp,
  board-guarded) maps `DeviceLog`/`DeviceLogHex` to no-ops before any Core include;
  `ReportLog(MakeLog(...))` calls remain for real error reporting.
- **No-op FLASH unlock dance removed** from `Storage_FlashInit` (write/erase paths lock/unlock
  themselves).
- **`Meas_SelectRange` table-driven** (was 210 B of duplicated PinHigh/PinLow sequences; now
  one loop over a `{port, pin}` table, shared by `Measuring_Init`).
- **Memory-service write echoes simplified**: System/Dynamic/Keyed write success replies echo
  the request payload verbatim (it already IS BlockIndex + BlockMeta + value) instead of
  re-assembling a copy - less flash and fewer large stack buffers in the handlers.
- **SendDeviceReply reuses the caller's reply frame**: removes a second full PacketFrame
  (~270 B) from the Device-service stack chain on the node, where these handlers run too.

## Fixed/Closed (RealHW 2026-08-23 edge-case session)

- **BUG 2 - Meas Number fields stored out-of-range values verbatim**: the DAS
  `ResistiveMeas_Schema` gained write triggers that clamp at write time, so the STORED value
  now always equals the APPLIED value: Sampling Rate clamped to [1, 1000] Hz (the loop
  divides by it), Filter Coefficient clamped to [0, 1]. The runtime clamps in
  `Measuring_Update`/`SampleIntervalMs` remain as defense in depth (recall bypasses
  triggers by design - direct field memcpy - but the loop still sanitizes).
- **BUG 3 - Fan duty**: not actually a bug - duty is specified in **percent**, so the
  tester's `2.0` was correctly stored and applied as 2 % (the report assumed a 0-1
  fraction). Verified the whole pipeline: `OnPWMDutyChange` clamps to [0, 100] and stores
  the clamped value, so >100 writes already read back as 100. Hardened while there: the
  block field is only committed after `ledc_update_duty` succeeds (previously a failed HW
  update still updated the field).
- **BUG 4 - deleted blocks visible until next save**: reads and topology summaries of the
  Dynamic/Keyed Memory services now skip `Deleted`-flagged blocks (read returns status
  failure, summary counts only visible blocks), so delete-then-read no longer shows stale
  data. Purge-on-save semantics unchanged per docs; writes to deleted blocks are rejected.
- **BUG 5 - silent timeouts**: every CLI response handler now sets a shared flag, and the
  command functions (`dev`, `save`, `recall`, `delete`, `rmem`, `create`, `sndb`) wait 500 ms
  for it before returning - printing "no response from device N (timeout)" when the target
  is dead or the service is missing on the node. (`tree` stays asynchronous by design.)
- **BUG 6 - garbage Number strings parsed as 0**: CLI Number parsing now uses strict
  `strtod` with full-consumption check and rejects hex strings ("0x10" previously became
  16.0, "abc" became 0.0); malformed values produce "Failed to parse value" instead of
  writing silently wrong data.
- **BUG 1 - deleted files reappeared in the DAS file table (CLOSED, not reproducible)**:
  at the start of a session the table contained `ZERO`(size 0) and `ALPHA`(size 100) that had
  been deleted - and verified clean - at the end of the previous session, with no writes in
  between. Could never be reproduced afterwards: create/delete/reboot cycles, a multi-file
  fill/stress state, pointer-page exhaustion (13/16 slots used, several stale table
  generations left in flash) and storage-full states all persisted correctly, and WCH-Link
  dumps of the pointer page + table matched the display byte-for-byte. The pointer-page
  last-valid-wins recovery is verified sound. **Closed as irreproducible.** If it ever
  recurs, dump flash 0x3800-0x383F plus all candidate table regions before any filesystem
  operation.
- **Size-0 files did not occupy their reserved block (fixed)**: `CreateFile(name, 0)` and
  `ResizeFile(name, 0)` reserve (and erase) one block, but `BlockUsed` computed a size-0
  file's coverage as `offset + 0`, so the block looked free. `FindSpace` then handed it to
  another file -> two live records aliased the same flash (reproduced on hardware:
  `ZERO`@512 + `BETA`@512; after resizing `ZERO` up to 200 B it silently shadowed `BETA`'s
  data region). Fixed by clamping the block count to >= 1 in `BlockUsed`
  (`Core/Functions/Storage.h`), matching `CreateFile`/`ResizeFile`/`FindSpace`. Verified on
  hardware: create-to-0 reserves the block (the next file gets a distinct block), resize-to-0
  keeps the block reserved, resize-from-0 is blocked when the extension would overlap a live
  file and succeeds when space is free; table moves and reboot persistence are unaffected.

## Docs-vs-code audit (2026-08-23)

Full comparison of `Docs/{Data Formats,General architecture,RSBus}.md`, `Services/*.md`,
`Modules/*.md` against `src/`. Fixed immediately:

- **Stream packets with FragID >= 1 failed CRC on receivers**: `PacketConstruct` computes
  the checksum while frag_id is still 0 and four stream senders patched frag_id afterwards
  without refreshing it (SNDB Read All, LogHandler GetLogs, Storage table/file streams).
  Invisible locally (DispatchPacket calls handlers directly, no RX validation) but any
  remote receiver of a core-originated multi-packet stream silently dropped packet 2+.
  Fixed via `PacketSetFragId()` which patches and recomputes.
- **Storage table never shrank**: Storage.md specifies "<25% full -> decrease by one page
  (minimum one page)"; implemented in `MoveFiletable`.
- **DAS `Storage_FlashErase` reported success unconditionally**: `FLASH_ErasePage_Fast`
  returns no status; each page is now verified by reading back 0xFFFFFFFF.
- **Button polarity inverted**: pull-down line idles LOW, so `!PinRead` reported "pressed"
  at idle. Now active-high (`PinRead`). Implemented the documented "pushing the button
  triggers the LED": rising edge while the LED is off lights it (the shared line cannot be
  read while the LED is driven - turn off remotely/by field write).
- **Texture transform order mismatched geometry pass**: textures composed
  `Base*Local`, geometries `Local*Base`; aligned to `Local*Base`.
- **CLI comment claimed wrong Storage CIDs** ("1 Read File"); corrected to the real table
  (0 table, 1 format, 2 create, 3 delete, 4 resize, 5 read, 6/7 stream open/close).

### Doc updates needed ([doc-bug]s; Docs are off-limits to code sessions)

- Data Formats.md: broadcast written `0xFFFFFFFF` but the ID field is 16-bit (code uses
  0xFFFF); Payload Len is 1 byte so payload maxes at 255, not "...256"; BlockMeta flag list
  lacks the implemented `RemoteOrigin` (bit 15); BlockIndex "Padding unspecified = 0xFF"
  vs code default 0x00.
- Device service.md: "provides time synchronization to core" - direction is core->node;
  sample gap is 1.5 s vs doc's "few seconds"; discover response is sent as broadcast
  (nodes filter by SN); software-version reply has no documented encoding; SNDB Read
  not-found = empty response (undocumented); core address hard-coded to 1 (undocumented
  constant alongside the capability bit).
- Wire constants worth pinning in docs: service-type numbers (Device 0x01 ... CLI 0x09),
  RSBus start/sync byte 0xAA, log notifications are TYPE=0 frames without REQACK, CLI
  responses ride `srv_src = CLI` with CIDs 0-5 (undocumented wire persona of the console).
- Dynamic/Keyed/System Memory docs: block-name width self-contradiction ("16char/12byte");
  the backup-record table does not match the actual serialized backup layout (count-prefixed
  TLV-style, not `BlockIndex|BlockMeta|Values` rows) - clarify whether that table was meant
  as wire payload or record format; "Create ... the part it's in must exist" vs Create only
  handling whole blocks; field/key deletion takes effect immediately while the docs say
  deletion is deferred to Save (only whole-block delete defers); keyed dictionary meta Size
  stores BYTES, doc reads like key count; "separate value and metadata arrays" is actually
  interleaved `[meta][value]` entries; block indices RENUMBER when purged on save (no
  documented contract).
- Storage.md: pointer recovery uses the LAST valid first-page slot (crash-safe ordering);
  `Erase` has no default argument.

### Design decisions / documented-but-unimplemented (triage list)

Status after the implementation round: collision-abort, Acc&Gyr Sampling Rate callback,
LDR/NTC units, key-cap question, Uptime semantics and the LED display Layout/Refresh fields
are DONE (see the section above). Still open:

- **Atomic backup updates**: DONE via `Storage::RenameFile` copy-and-swap (see above).
- RSBus net/device ID split (4-bit net + 12-bit device) unimplemented - flat 16-bit
  addresses; presumably deferred until Router.
- Resistive sensor constants (LDR curve C/exponent, NTC B value) still need hardware
  calibration; conversions themselves are implemented.
- Star geometry ignores PointNumber (identical to Polygon/circle).
- Texture rendering blends over existing buffer content; doc says "texture always clears
  the buffer and applies the texture in the given areas" - decide intended layering.
- **Log DB (B14)**: capacity capped at 32 records with silent drop on overflow;
  ClearReadLogs "from end" is ill-defined given dedup-in-place; GetLogs/ClearReadLogs have
  no bus consumer yet (CLI reads the core-local DB directly).
- App "Service views": value-entry names/units metadata will be hardcoded APP-side per
  BlockType (decision recorded above); no firmware work.
- Doc updates from this audit remain to be applied by a docs pass (broadcast value,
  payload 255-vs-256, RemoteOrigin flag, time-sync direction, backup-record table,
  name width, pointer-selection wording, wire constants).

## Fixed (docs-vs-code audit implementation round, 2026-08-23)

Implements the accepted audit items; the rest remain in the triage list below.

- **A3 - Tamu `ReceivePacket`**: ported the DAS persistent SYNC/HEADER/PAYLOAD assembly
  state machine (bytes pulled one at a time from the UART driver buffer, frame assembled
  across ProcessBus polls, CRC validated on completion).
- **Storage CIDs renumbered per the updated Storage.md** (Rename=5 inserted): Read File 5->6,
  Write Stream Open 6->7, Close 7->8. New CID 5 Rename handler (OldName+NewName -> bool).
  CLI gained `file <addr> rename <old> <new>`; read uses CID 6.
- **B5 - atomic backup updates**: new `Storage::RenameFile` per Storage.md (append record
  under the new name for the same data area, invalidate superseded records - each step an
  append-only NOR write or 1->0 invalidation). `WriteBackupFile` now stages the new
  generation in a temp file (`NAME...~`), then renames it onto the live backup name.
  Readers always resolve a complete generation; power-cut windows leave the previous
  backup intact. Replaces the erase-then-rewrite that could destroy the sole copy.
- **B6 - collision abort during transmit** (RSBus.md "verify while sending"): Tamu sends in
  16-byte chunks and compares each chunk's echo as it returns, aborting mid-frame on
  mismatch; DAS drains and verifies echoed bytes between byte transmissions with the same
  early-abort. Both keep full-frame verification and random backoff retries.
- **B8 - Acc&Gyr Sampling Rate callback is real**: snaps the requested rate to the nearest
  LSM6DS3 ODR (12.5..1660 Hz), rewrites CTRL1_XL/CTRL2_G preserving full-scale bits,
  verifies by read-back, stores the applied ODR in the block field.
- **B9 - LDR/NTC conversions implemented per the `Sensor.h` reference**: transformations
  operate on the RAW ADC sample and the CONVERTED value is EMA-filtered afterwards
  (weight = 1/(1 + FilterCoeff), matching `SensorClass::Run`):
  TempNTC10K degC = 1/(0.0034 + ln(raw/(1023-raw))/3950) - 273.15;
  Light10K lux = 18 * ((1023-raw)/raw). The Filter Coefficient write trigger now clamps
  only f >= 0 (any f is a valid averaging weight). The resistive/voltage modes keep their
  existing math. Constants still flagged for hardware calibration.
- **B12 corrected**: dictionaries hold up to 256 keys (key ids 0..255) - the previous
  255-byte field-size cap was removed from SetKey; the key-list response buffer grew to
  256 with payload-space clamping (a single packet carries at most ~247 keys).
- **B13 reworked**: `Now()` returns the SYNCHRONIZED time (raw timer + core-pushed
  offset); new `TimeFromBoot()` returns raw ms since boot on both devices. TimeUpdate,
  scheduling and timestamps all use `Now()`; the Device service Uptime (CID 8) reports
  `TimeFromBoot()`.
- **E - LED display Layout File Name + Refresh Rate fields implemented**:
  Layout File Name = plain 8-char storage file name (`DataType::String`; the Name data
  type was deprecated and removed along with Core/Types/Name.h); write trigger loads the
  layout file immediately (Docs/Modules/LED display.md format: u8 width, u8 height,
  W x H uint16 indexes, FFFF=missing, 0-based, row-first) and rejects the write if the
  file cannot be loaded; blank name reverts to the built-in layout. Each display carries
  a runtime index table (256-entry cap) used by both rasterization passes.
  Refresh Rate = Read-Only Number reporting the achieved FPS (exponentially averaged),
  measured per display around Render+Send in the main loop.
- **B15 decision**: value-entry names/units metadata will live in the APP (hardcoded per
  BlockType), not on devices - no schema changes to save flash. Recorded here and in the
  triage list below.

## Fixed (app ↔ hardware integration debugging, 2026-08-23)

First end-to-end bring-up of the app's real core stack against live hardware (new HIL test
harness `app/test/hil_live_test.dart`, run with `TAMU_HIL=/dev/ttyACM0` and
`LIBSERIALPORT_PATH` pointing at the bundled `libserialport.so`). Eight root causes found
and fixed; the suite passes 8/8 repeatedly and the CLI battery 98/98.

- **Firmware: USB replies had no START byte** (`Devices/Tamu_v2.0A/AppUSB.h::AppUSBSend`):
  the builder wrote START to `frame[0]`, then overwrote it with the CRC - every device→app
  frame went out as `CRC|LEN|payload|STOP` and the host parser (which scans for 0xFA) could
  never sync. The app has never received a single reply because of this. Fixed to the
  documented layout `START|CRC8|Length|Payload|STOP` (buffer grown accordingly). Verified:
  replies now arrive well-formed with the transaction ID echoed.
- **App: libserialport `sp_new_config()` leaves `xon_xoff` uninitialized** - every other
  field is set to -1 ("unchanged"), so an unset config randomly fails `sp_set_config` with
  SP_ERR_ARG depending on heap garbage; connect failures were intermittent by construction.
  Fixed by always setting `SerialPortXonXoff.disabled`. Also added a bounded config retry
  (opening pulses DTR/RTS which resets the ESP32-C3; its USB CDC rejects line-coding until
  re-enumerated) and closed the port-handle leak on config failure (device stayed busy).
- **Control-line ownership**: the app now asserts and holds both DTR/RTS for the whole
  session (the state the board runs stably in). Note the ESP32-C3 USJ decodes DTR/RTS
  transitions into reset/boot actions: every port open/close pulses them, so a flaky
  connector manifests as an apparent reboot storm (observed and misdiagnosed as firmware).
- **Firmware: app RX queue too shallow for concurrent transactions**
  (`APP_RX_QUEUE_DEPTH` 6 → 12): firing 8 parallel requests dropped exactly the frames
  beyond depth 6 (verified per-txId forensics via the new AppDiagnostics ring).
- **App: Storage delete misread success as failure** - delete replies carry an EMPTY
  payload on success, unlike create/resize status bytes; the client required
  `reply[0] != 0`. Also raised storage request timeouts to 6 s (flash erases on slow nodes
  exceed the 2 s default).
- **CLI storage responses aligned to Docs/Services/Storage.md CIDs** (`CLI/Handler.h`):
  Read File moved to CID 6 (file-data stream), Rename File added at CID 5 (status byte);
  previously a `file read` printed "Unknown Storage response CID 6".
- **Firmware/app: custom console never initialized esp_console** (`CLI/Entry.h
  ::StartCLI`): replacing the stock REPL removed the implicit `esp_console_init`; every
  command lookup took the "not found" exit which does not write `cmd_ret`, so the console
  printed uninitialized stack values ("Command returned 1107297998") for ALL commands
  including built-in help. Fixed with explicit `esp_console_init` (+ max_cmdline_args 16;
  the initial value of 8 truncated longer command lines mid-arguments, silently shifting
  write parameters).
- **Firmware: APP mode is now escapable from the terminal side** - a CR/LF outside any app
  frame cannot be app traffic, so it reverts the port to CLI mode. A software-only host
  close does not drop the USJ connection state, so without this the console stayed dead
  until the cable was replugged.
- **Permanent diagnostics added** (replacing throwaway debug prints): app-side
  `core/diagnostics.dart` ring (timeouts, parse errors, link state changes - dumped in
  test failure output), firmware-side `DeviceLog` hooks for app session start/end,
  malformed-frame rejects and queue/ring overflow drops (visible via the existing Log
  Handler service).

## Fixed (BLE transport bring-up, 2026-08-23)

First over-the-air validation of the app's BLE transport against the core's Nordic-UART-style
GATT service. Three defects fixed; the full HIL battery now passes over BOTH transports.

- **Firmware: advertisement carried no device name** (`Devices/Tamu_v2.0A/AppBLE.h`): hosts
  saw only the raw MAC ("E4-B0-63-C8-20-72") and could not identify the device. Root cause:
  ordering - `setName()` routes the name into the scan-response data ONLY when scan response
  is already enabled; called before `enableScanResponse(true)` it lands in the main ADV
  payload where FLAGS(3)+UUID128(18)+NAME(12) exceeds the 31-byte legacy limit and is
  silently dropped. Fixed by enabling scan response first.
- **Firmware: BLE writes were parsed with their length prefix attached**
  (`AppBLE.h::onWrite`): each characteristic write carries a uint16 LE length prefix, but
  every byte was fed straight into the wire-stream parser - shifting all bytes so no frame
  ever validated. Fixed with a small reassembler state machine that also tolerates BlueZ
  delivering writes fragmented or coalesced.
- **App: notification stream filtered by the wrong UUID** (`core/ble_transport.dart`):
  `characteristicValueStream(deviceId, characteristicId)` filters per CHARACTERISTIC, but the
  transport subscribed with the SERVICE uuid - every notify was silently dropped. Fixed to
  pass the notify-characteristic uuid.
- **Robustness**: the RX ring round-trip between the NimBLE host task and the application
  task was replaced by direct parser feed + enqueue from onWrite (the pump still routes),
  removing a cross-task buffer whose drain raced the producer. NimBLE host task stack raised
  4096 -> 8192 B and event/ACL pool counts increased for bursty app sessions.
- Verified end-to-end over the air: scan by name, connect, PONG x4, version string, plus the
  complete HIL battery (discovery, sysmem walk, storage CRUD, concurrent transactions,
  timeouts, SNDB) passing 8/8 over BLE and 8/8 over USB.

## Fixed (BLE advertising watchdog wedge, 2026-08-24)

After ~20-40 min of runtime the core stopped advertising entirely: no advertisement over the
air, silent failure on direct connects, and the application task stopped logging (USB console
still served commands - only that task was wedged).

- **Root cause** (`Devices/Tamu_v2.0A/AppBLE.h::AppBLETick`): the self-healing watchdog
  forced a FULL advertising teardown/rebuild (`clearData` + `removeServices` + service re-add)
  every 15 s of idle, unconditionally. After roughly a hundred cycles the repeated GATT
  service de/registration wedged the NimBLE host stack and blocked the application task.
- **Fix**: rebuild only on evidence - promptly (3 s) when `isAdvertising()` reports down with
  no session, or once per 120 s of session-less idle as a last resort against a stale
  advertising instance that claims to run. Healthy operation now performs zero teardowns.
- Operational note: reset attempts via `esptool.py` failed silently ("Operation not permitted"
  - lost exec bit); invoke it as `~/.platformio/penv/bin/python .../esptool.py ...`. A board
  that "ignores" resets may simply never have rebooted.
- Verified: two consecutive full HIL battery passes (8/8) over BLE after the fix, plus soak.

## Fixed (BLE latency + MTU, 2026-08-24)

User-reported high BLE latency. Measured ping round trip (app transaction layer, flutter
test probe): USB median 6 ms vs BLE median ~60 ms. Improvements landed on both sides.

- **Firmware** (`Devices/Tamu_v2.0A/AppBLE.h`, `sdkconfig.Tamu_v2_0A`): preferred ATT MTU
  256 -> 512 (negotiated dynamically; BlueZ confirms mtu=512 in the new conn-params
  breadcrumb); connection parameters requested on connect (7.5..15 ms interval, latency 0,
  4 s supervision) replacing the BlueZ default of ~30-60 ms; notification pacing
  BLE_PACE_MS 20 -> 4 ms with BLE_CHUNK 180 -> 480 B so large responses stream in few
  notifications (chunk/pkt buffers made static - too large for the task stack).
- **App** (`core/ble_transport.dart`): unchanged - already negotiates MTU dynamically and
  chunks to the negotiated size.
- **Boot advertising race** (`AppBLEInit`): the initial advertisement was started before the
  controller finished syncing, so start() succeeded while nothing reached the air; hosts saw
  no device until the 120 s watchdog rebuilt it. Initial start is now deferred to AppBLETick.
- **Diagnostics**: permanent breadcrumbs - conn-params update log (interval+MTU), first-write
  per session, notify-backpressure counter (escalating log), session start/end.
- **Follow-up squeeze**: notification pacing removed entirely (the ~2 ms app-task loop
  throttles naturally) and a 2M PHY preference requested on connect - this host adapter
  declined it (breadcrumb logs tx=1 rx=1), but capable hosts will pick it up automatically.
  Verified stable at median ~58 ms; the earlier 80 ms readings were RF noise.
- **Result**: BLE RTT median ~60 ms (min ~36 ms) at 11.25 ms measured connection interval;
  meets the <100 ms requirement. The remaining budget is air time (2 x conn event) plus
  per-message D-Bus cost in BlueZ (~15-25 ms each way); going materially below ~30 ms would
  require fd-passing (AcquireWrite/AcquireNotify), which universal_ble does not expose.
- **Operational note**: a test process that dies without disconnecting leaves a zombie BlueZ
  connection that survives even `bluetoothctl remove`; the device then won't advertise or
  answer. Recovery: power-cycle the adapter (`bluetoothctl power off/on`). HIL tests now
  register disconnect teardowns immediately after connecting.

## Fixed (app UI round 2, 2026-08-24)

- **Refresh on connect**: the Devices list now populates as soon as a session
  comes up (Connection page triggers a network refresh after a successful tap-to-connect).
- **Signed values**: Number (16.16) and Index/Int32 decoding sign-extended properly -
  Dart ints are 64 bit so the old `| 0` trick never wrapped; negative numbers displayed
  as huge positives. Vector and matrix editors inherit the fix.
- **String length limits**: editors honor wire-format lengths from the block registry
  (Vysi1Display Layout File Name = 8 chars per firmware char[8]).
- **Storage interactions**: per-file menu - view, download to ~/Downloads (chunked read),
  rename, delete (with confirm); create file in the app bar. Known file types render
  decoded: SNREG as an ID/SN registry table, LAY files as an LED-index grid (0xFFFF =
  missing), textual data as text, else hex. File type icons + labels in the list.
- **SNDB is a page** with refresh, autorefresh and assign/update-ID via SNDB Write (CID 14),
  showing known display names and device icons.
- **Logs**: only listed for CORE devices (the log database lives on cores). The viewer
  decodes entries into readable text (source service/block name, AccGyr error strings,
  failed-CID reports, uptime timestamps), offers a per-device filter and clear-database.
- **Firmware**: Tamu kCapabilities now reports Core | CLI | DynamicMemory | KeyedMemory
  (Capabilities namespace extended to the documented bit positions), so the app shows the
  memory service views that were previously hidden.

## Fixed (BLE "wedge" root cause: post-upload state, 2026-08-24)

The twice-recurring "NimBLE host wedge" (advertisement gone, GATT writes undelivered,
app-task breadcrumbs silent while USB keeps working) is NOT runtime accumulation:

- **Root cause**: `pio run -t upload` leaves the BLE controller/host in a broken state
  (the board either does not fully reset or the BT controller keeps stale state across
  the flash cycle). Every occurrence started right after an upload; every recovery was a
  hard reset via `esptool.py --after hard_reset`.
- **Rule**: after ANY firmware upload, force one hard reset before BLE testing:
  `~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py
  --port /dev/ttyACM0 --chip esp32c3 --before default_reset --after hard_reset chip_id`
- Verified: battery failing immediately after upload passes 8/8 twice in a row after
  a single hard reset. The advertising watchdog refinements remain valuable but were
  never the culprit for this signature.

## Fixed (app UI round 3, 2026-08-24)

- **Dynamic Memory**: create dialog now picks block type + name; existing blocks can be
  renamed AND retyped (firmware Write handler extended: field-invalid write sets the type
  per the docs' "type is user editable"); entries can be appended to any block (data type
  picker -> value editor -> write at map_count).
- **Keyed Memory**: blocks creatable with type; dictionaries appendable; keyed entries
  addable with a hex key id + data type + value.
- **SNDB Delete**: new Device service CID 15 tombstoning by short ID (extension beyond the
  documented CID set - proposed doc addition in Docs/Improve.md); SNDB page gets a remove
  action per entry (core device protected).
- **Autoconnect implemented end-to-end**: long-press a device on the Connection page to set
  it as target (persisted); ConnectionManager connects automatically whenever discovery
  sees the target while disconnected; Settings page shows/clears the real target.
- **Memory views formatting**: card-per-block layout with index badges, flag/type chips,
  fixed-width value columns and unit subtitles on System/Dynamic/Keyed pages.

## Fixed (app UI round 4 + memory service bugs, 2026-08-24)

User-reported issues from hands-on testing:

- **"ListTile background color or ink splashes may be invisible"**: the memory pages
  nested ListTiles inside colored Containers; Flutter requires a Material ancestor for
  ink. Replaced with `Material(color: ...)`.
- **Dynamic entry types not changeable**: the firmware Set() already updates
  FlagsAndType, but the app always re-sent the old meta. The entry menu now has
  "Change type" (data type picker -> fresh value editor -> write with new type);
  verified live that the change persists.
- **Deleting one dynamic value deleted the whole block**: firmware Delete (CID 1)
  ignored the field index entirely. Fixed to remove just that entry when a valid field
  index is given; the block survives (verified live).
- **Keyed dictionaries never load**: reading a freshly created EMPTY dictionary failed -
  `FieldResult.Data` is null when the dictionary has no storage yet, and the read
  handler rejected on that before reaching the dictionary branch. Validity is now
  checked via map_count instead.
- **File table visible but read-only** in Storage: it now appears as a locked,
  disabled row ("internal directory").
- Suite timing hardened further: mutating verbs wait out NOR-flash stalls (4 s idle);
  three consecutive green core runs.

## Open (firmware code review follow-ups)

- **GammaTable has only 240 entries** (`Blocks/Vysi1Display.h:9-24`): the initializer list
  ends at index 239, so entries 240-255 are zero-initialized and any channel value ≥ 240
  snaps to black - the brightest pixels go dark. The table also caps at 200 instead of ~255,
  so it is not a valid gamma-1.8 curve even for indices it covers. Regenerate the full
  256-entry table. (Intentionally not yet fixed.)
- **Tamu serial number carries only 48 real bits** (`Devices/Tamu_v2.0A/Main.h`): the factory
  MAC fills bytes 0-5; bytes 6-13 stay zero, so all Tamu SNs share an 8-zero suffix. Confirm
  whether the docs promise a full 14-byte unique SN; if so, pad from additional eFuse fields.

## Open (app rewrite, 2026-08-22)

The Flutter app (`/tamuapp`) was fully rewritten against `Docs/App/*.md`. The old
object/message-manager code did not match the documented layout (Connection /
Devices / Backup / Settings) or the packet protocol. The new app builds and
passes analysis for Linux desktop (BLE + USB). Doc gaps discovered during the
rewrite:

- **App Interface: USB CRC8 coverage is undefined.** The doc gives the frame
  layout (`0xFA | CRC8 | Length | Payload | 0xBF`) but not which bytes the CRC8
  covers. RESOLVED by implementation (2026-08-23): both sides use CRC8 over
  Length + Payload (firmware `AppUSB.h` / app `transport.dart`). Doc still worth
  updating with this detail.
- **App Interface: BLE GATT UUIDs are undocumented.** RESOLVED (2026-08-23):
  firmware and app agreed on the Nordic UART style UUIDs (service
  `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`, write char `…0002`, notify char
  `…0003`), implemented in `AppBLE.h` / `ble_transport.dart`. Docs should record
  these as final.
- **App source ID is undefined.** RESOLVED differently than assumed (2026-08-23):
  the app has NO network address at all. Its identity is the App Interface service
  type (`ServiceType::App = 0x08`) in SRV SRC, with the CID byte used as an
  app-managed transaction ID. The core rewrites `id_src` on app-originated frames
  to its own short address (proxy), so responses - local or relayed from the bus -
  return addressed to the core and are forwarded to the app by service type. The
  app's old `appSourceId = 0xFFFE` is inert. Also note: USB serves the CLI or the
  app EXCLUSIVELY (app priority; entering app mode happens on first valid link
  frame, leaving it on physical unplug).
- **Data Formats.md broadcast value inconsistency**: "ID - 16bit" but broadcast
  is written as `0xFFFFFFFF`; firmware uses `0xFFFF`. App follows the firmware.
- **System Memory block-meta read returns the block name appended after
  BlockMeta** (firmware behaviour, used by the app) - this extra name string is
  not mentioned in `Docs/Services/System Memory.md`.
