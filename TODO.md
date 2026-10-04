# TODO

Long-term plan (`Docs/Plan.md`): 1) Scripts, 2) blocks/modules + subscriptions, 3) app backup.

**Baseline** (2026-10-04): core **623 824 B** (~20 % of the 3 MB partition), DAS **11 092 /
16 384 B** (67.7 %), DAS RAM 2 048 B (statics 1 048 + stack 1 000, no heap). Host gate:
`./test.sh` = the native numeric/geometry/CRC/stride/align tests (core, 32-bit and DAS
configs, both `OPTIMIZE_SPEED` states) + the app host suite + `flutter analyze`. The 8 HIL
suites need the rig (core on `/dev/ttyACM1`, one DAS on `/dev/ttyACM0` via WCH-Link).

## Open work

- [ ] **A11 / D3 - per-field geometry-mask versioning.** Any write to an eye block bumps the
      block generation and the renderer recomputes all 9 masks. Headroom says it is not urgent
      (the panel sits at its cap, ~127-132 FPS). Sketch: the per-frame pass still has to run
      (`ApplyGeometryField` combines the cached mask), so the win is only skipping the per-LED
      `RenderGeometryField`; that needs a *per-field* invalidation token - either a per-field
      version array (RAM per field) or comparing the cached geometry inputs each frame and
      recomputing only the fields whose inputs moved. The latter needs no block-model change and
      is host-checkable. **Deferred to the display/rig batch.**
- [ ] **A10 part 2 / D5 - cross-script macro calls.** `Docs/Services/Script.md` names "Macro
      call" but gives no opcode, no boundary-crossing rule and no argument passing. The VM runs
      **one script per tick** and every wait state (`waitUntil`, `pendingForeign`, the foreign
      deadline) lives on the callee, so this needs a call stack of `(script, line)` pairs and a
      tick loop that resumes whichever script is waiting. Proposal (in `Issues.md`): a `Call
      script` flow op taking `(loaded id, entry line)`, blocking by construction (the caller
      resumes on the callee's `Return`/`Halt`), values exchanged through registers.
      **Blocked on a docs decision.**
- [ ] **Rig looks.** `Polygon`/`Star` after the `atan2` accuracy fix; a rounded
      `Square`/`Rectangle` (the evaluation scene sets no `Rounding`, so no shipped look changed).
      Both are eye-only, so they need a display-equipped rig.
- [ ] **D4 - confirm the LED brightness-cap value** on a display. The mechanism landed (the
      layout file's brightness limit, 178 = 70 %, enforced in the render); only the value is
      unconfirmed by eye.
- [x] **Bootloader** (`Docs/Services/Bootloader.md`). A per-device raw packet bootloader that
      replaces the main binary, entered by holding the button at boot. Complete:
  - [x] **A. Update page.** Done: a top-level left-sidebar tab that picks a `.bin`, probes for a
        node in bootloader mode, flashes with write/verify progress + errors, and carries the
        user guide. The client's channel is behind `BootloaderTransport` (`PassthroughTransport`
        now; direct-USB drops in for Phase C). Tests: `bootloader_client_test`,
        `update_page_test`; HIL still 1 pass / 0 corrections.
  - [x] **B. DAS bootloader size/tuning.** Done: **2004 → 1792 B (87.5%, 256 B free)**. The SPL
        flash driver is now direct-register with shared `noinline` helpers (LTO had inlined the
        unlock/lock into both the erase and program paths), and the framework debug code + C++
        init/fini are dropped (`board_build.use_builtin_debug_code/cpp_support = false`). HIL
        still 1 pass / 0 corrections. The remaining big block is the framework startup +
        `SystemInit` (~650 B); replacing it needs a custom `board_build.startup` and a minimal
        48 MHz `SystemInit` (clock-critical).
  - [x] **C. Tamu (core) bootloader.** Done: partition split (`factory` bootloader + `ota_0`
        main app), the factory app (B1), the `otadata = factory` re-arm, raw-USB HIL
        (`test/core_bootloader_flash.py`, 0/20228), the app `DirectUsbTransport` + Update-page
        toggle, and `Tamu_v2_0A -t upload` now targets `ota_0` (`scripts/core_app_offset.py`)
        without touching the factory bootloader.
  - [x] **Manual**: the DAS enters bootloader on a button-held reboot (confirmed).
      Locked: passthrough targets the connected core; no capability bit; button-only entry.
- [ ] **DAS provider stale entries (low priority).** Effectively solved: a dropped cancel leaves
      the provider for at most the 120 s lease, and if the requester still exists a later value
      update re-cancels it (the orphan path). Revisit only if a *confirmed* cancel is wanted.

## Code-cleanup backlog

Non-urgent, no functional gaps (from the 2026-10-04 duplication pass; the reply/stream,
subscription, register, script and requester-persistence dedups are done).

**Firmware**
- SNDB: five `for i < num_entries { ReadEntry; ... }` scans share a prologue (a visitor would
  add indirection; the loops are short and clear as-is).
- `RegisterGetByBlockInfo`/`SubscriptionsGetField` share one resolver - awkward because
  `RegisterDispatch.h` precedes `SubscriptionsDefs.h` in the include order.
- Unused enum members (`DataType::NetAddr/UnknownKeyed/BlockInfo/Deleted`,
  `BlockType::Deleted/Undefined`, `AccGyrError::ErrTimeout`, Render `Mesh`/`Colour3`/
  `PointCoordinates`) - cross-cutting with `firmware_contract_test.dart`.
- The one-shot healing paths (`RemoveObsoleteFiles`, `DeduplicateFiletable`, `DeleteFileExact`,
  the `LAY5X5`/`VYSIV1` migration) run every boot; consider a one-shot migration marker.

**App**
- `backup_capture`/`backup_restore` share one field walker.
- `script_value_dialog._changeType` -> `ScriptDraftValue.setType` (the dialog's local state
  shape differs, so this needs a small state refactor).
- `ScriptValueInfo` vs `ValueInfo` (the raw codec helpers now live only in `types.dart`).

**Deliberately left** (a merge would read worse): the three flag-name decoders (`flagWords` =
full words for the backup format, `ValueFlags.describe` = RO/P/TR, `_flagsSuffix` = RO/P) and
the two flag renderings in `register_page_tiles` (chips vs small text) are different
vocabularies/shapes.

## Decisions (locked)

- **Register**: `ValueInfo = Type(16)|Size(8)|Flags(8)` (internal and wire); `BlockInfo =
  Type(10)|Inst(6)|Field(8)|Key(8)`. Flags are passive: ReadOnly `0x01`, Persistent `0x02`,
  Trigger `0x04` (the four active flags are gone). **Save All = 5 / Recall All = 4** (the IDs
  are swapped vs `Command ID table.md`; `Register.md` wins - the user updates that file).
- **Static memory** is two flat compile-time spaces (volatile + persistent); `.SV` is a raw 1:1
  mirror of the persistent space (targeted saves/recalls are the app's job). The static block
  table is literal (`BlockEntry`: Field&Key + MemoryOffset + ValueInfo) with a literal trigger
  table holding only fields that have a trigger.
- **Dynamic** types `0x3F0-0x3F3`, **Scripts** `0x3F4-0x3F7`, Reserved `0x3F8-0x3FF`, each 64
  instances, addressed by one **global** index `0..255`. Basic CIDs `0-5`, dynamic `0x10-0x13`.
- **System block** (type 0): a `StaticBlockDescriptor` (`System_Block` + `System_Entries`, one
  entry per field at key 0). The struct fields 0/3/4/5 are `Undefined` with Size = the member
  sum (the struct position is not on the wire); Name is a fixed space-padded `char[16]`; NetID
  is core-only and applies on reboot.
- **Packet**: 12-byte header `CRC8|Flags|Reserved+Priority|PayloadLen(bytes)|SRC|TGT|CMD|TRID`,
  payload exactly `len` bytes, `12+len <= 128`. `SUCCESS`/`FAIL` flag bits exist.
- **TRID ranges** (`Packet.h`): System/Logs `0x0000-0x0FFF` (incrementing), Subscriptions
  `0x1000-0x1FFF` (table), Scripts `0x2000-0x2FFF` (slot), App `0xF000-0xFFFF` (slot). Replies
  echo the request's TRID.
- **Subscriptions**: shared 16-byte table (`sourceReg`, trigger, `minTime` uint24, period,
  deadzone); requester 28 B / provider 32 B; 120 s lease renewed by keepalive/updates; CIDs
  `0x0400/0x0401` inter-device, `0x0410-0x0413` requester, `0x0420/0x0421` provider; cancel is
  `trigger None`; `.SUBREQ` holds 24 B entries (TRID persisted, timeout regenerated).
- **Scripts**: `SCR_XXX` (4096 file ids) but only 64 loaded slots (6-bit); the caller picks the
  slot. The editor/app keep file==slot in practice; boot-load prefers the identity slot.
- **Bootloader**: raw frame `0xCA | control(5 pad, 1 even parity, 2 cmd) | offset u32 LE |
  [32 B payload] | 0xBC`; parity is even over the rest of the packet (command + offset +
  payload); cmds `01` write / `10` read-request / `11` read-response. The core's Device `0020/0021`
  passthrough targets the connected core and relays the raw frame onto its RSBus (broadcast,
  the frame carries no address). No capability bit (every device gets a bootloader, every
  core the passthrough). DAS: bootloader 2 KB at `0x0`, app relocated to `0x800` via
  `board_upload.offset_address`; button-only entry (PC0), white LED PD0. Because the DAS
  answers immediately (a running node's reply follows a full service-handler pass), it waits a
  widened 32-byte CSMA silence before replying so its frame cannot collide with the core's
  TX-enable release; the core relay releases TX-enable by polling the UART status instead of
  `uart_wait_tx_done` (whose FreeRTOS wakeup adds a tick). The app paces writes ~50 ms (a page
  erase+program cycle outlasts that and the next frame is lost), then verifies by read-back +
  retry until one clean pass; dropped writes/reads are acceptable.

## Notes / gotchas

- **TimeSync is synchronized-device initiated**: the node sends Device CID 3 and applies the
  offset to its own clock; the core only answers. All four timestamps use the synchronized
  `Now()`; the node estimates its drift (Q16.16) and extrapolates between syncs; the re-sync
  interval is measured in RAW time so a correction cannot trigger the next sync. Nodes re-sync
  every **60-75 s** (30 s warm-up), which holds the DAS's ~1 % RC drift within ~10 ms.
- **A script loop advances at most once per main-loop tick** (`ScriptRun` stamps every line and
  yields when a line is revisited in the same tick). A `While` re-reads its operand each
  iteration, so a condition computed once before the loop never updates.
- **Fixed-point `^`**: a non-integer exponent is a product of nested square roots (each
  truncates); keep chains short where precision matters. Exponent/weight literals are Q8.8.
- **DAS sensors**: the NTC is a **100 kΩ** part (`MeasNTC100K`); the LDR uses the datasheet
  relation `R(E)=R10*(E/10)^-gamma` with `LDR_R10_KOHM`/`LDR_GAMMA` knobs. The lux path wants a
  lux-meter calibration of R10.
- **LED strips can brown out the board**; the builder clamps the displays to 5 % first and the
  brightness script caps at 70 %, and the layout file's brightness limit enforces it in the
  render.
- **The renderer samples the geometry mask forward** (`pp = Position * coord`), so a shape's
  centre would land at `-L^-1 * t` for a rotated Position. Both writers store `t' = L * t` so
  the centre stays at `-t` for any rotation.
- **Storage names are space-padded, not NUL-terminated** (`NameMatch` packs the plain name
  first). Files: `.SV` (static persistent mirror), `.TABLE`, `DT_<xx>`/`DV_<xx>` (dynamic),
  `SCR_XXX`, `SNREG` (SNDB), `LAY_1` (display layout). The DAS reduced filesystem has one
  settings file and no rename.
- **The reduced `.SV` has no file-presence bit.** Its fixed-size file always reports a full
  size, so a never-written (or Formatted) mirror reads back erased instead of "absent".
  `StaticRecallAll` treats a `0xFF` System Name as "not a valid mirror" (the Name is always
  space-padded text) and re-persists the live values instead of copying erased bytes over
  them - without this, a freshly flashed DAS came up with a `0xFF` Name and `0xFF` Meas
  values, and the app showed "broken" fields. `Recall All` heals the same way; the HIL check
  is `HIL: DAS recalls over an erased .SV`.
- **The DAS's flash image does not cover the storage region** (code ends ~0x2B5C, storage at
  0x3F00), so reflashing preserves whatever `.SV` was there - the erased-mirror handling above
  is what makes a reflash recover cleanly.
- **`.SUBREQ` is removed when the requester table empties** (`SaveRequesterTable` deletes it at
  count 0); it used to leave a 0-entry file behind after the last cancel.
- **Backup zips** are semantic format 2, one JSON per device (no manifest); entries must be
  built from UTF-8 bytes (archive sizes by UTF-16 code units otherwise). Large files are
  skipped above 128 kB by default.
- **`Issues.md`** tracks the open docs decisions (OS notifications, script UI-info enum labels,
  `Current setup v3` predating the emote interface, the pre-rotated translation convention,
  cross-script macro semantics, the trigger-table function pointer, script CID 0 file-ids vs
  slots, the dynamic request shape) and the Android on-device gaps.

## Done (condensed)

- **Scripts** (`Docs/Services/Script.md`): `SCR_XXX` parser, loaded-script registry exposed as
  banked types `0x3F4-0x3F7`, management CIDs `0x0500-0x0507`; the VM (preloaded instructions,
  line/block tables, math/logic/flow/time/services, infix expressions with vectors/matrices,
  per-instance TRIDs, loop guard); boot/load (`Load-on-boot`, `Run-on-load`); in-script
  (un)load (ops 7/8); the app list page + editor (function/IO/variables/constants/instructions,
  validation, upload, apply-live). Cross-script macros remain open (above).
- **Blocks/modules + subscriptions** (docs-driven): the block schemas aligned; `Deadzone` on
  both subscription entries; the trigger types (Periodic, OnChange+period, OnChangeConfirm,
  edges, DeltaPeriodic); per-trigger hashlike (FNV-1a / counter / last value / subresolution
  vector); high/low priorities; script I/O (`0x3FE` → the banked range) as source/target;
  auto-save on set/delete; two-device HIL.
- **App backup** (`Docs/App/Backup.md`): semantic format (type/enum/flag/colour/matrix/
  dictionary words); capture/restore of the whole register, subscriptions, scripts, SNDB and
  every file; per-part sync UI with remap; HIL round-trip.
- **Register service** (revised `Register.md`): L0-L6 (identifiers, descriptor unification,
  32-bit memory model, protocol, banks, the static split into flat spaces, persistence to
  `.SV`); P1-P8 of the docs revision (active flags removed, script CIDs, LED layout brightness
  byte, Save/Recall All one-pass, enumerate rewrite, ValueInfo reconciliation); the doc-vs-code
  passes (dynamic Read Only enforced, fixed 16-char block names, App Active enum, `.DT_XX`
  MemoryOffset, literal block/trigger tables, app-derived static layout).
- **Evaluation setup** (`Docs/Current setup v3.md`): the setup builder (dynamic blocks, 8/9-part
  eye render dictionaries, the display wiring, four subscriptions, the scripts), the emote
  system (custom enum inputs, script 5, the forced blink), device tuning rounds, the LED
  transfer curve + ring look, the LDR/brightness calibration, and the setup HIL suite.
- **RSBus packet + TRID + subscriptions rework** (docs 2026-10-03): P1 byte payload length, P2
  central TRID ranges + reply echo, P3 the subscription rework, the `.SUBREQ` name, and the
  System-block rework. All sub-items done.
- **Cleanup / optimization passes**: the split files (Register/Subscriptions/Storage/Script/
  Memory/Vysi1Display, the app pages), the DAS flash reductions (`pow10` polynomial,
  `LoadAllBackups` one pass, `MEMORY_BACKUP_CAP` 128, the flag-array removal, the enumerate
  rewrite), the core speed build (`-O2 -fwrapv`, the CRC8 table under `OPTIMIZE_SPEED`), the
  native numeric/geometry/CRC/stride/align host tests, the app↔firmware contract test, and the
  2026-10-04 duplication passes (`abe921e`, `e56676e`, `dfc8c56`, `4f433aa`).
- **Deterministic versioning** (2026-10-04). Per-target content-hash versions
  (`scripts/version.py` + `version.json`): a build mints only when that target's sources change,
  and a date change resets the iteration (same day grows it). Firmware envs stamp
  `-D VERSION_*` via `firmware/scripts/auto_version.py` (bootloaders unversioned), and the System
  block now packs the documented `YY:MM:DD:II` (7 year + 4 month + 5 day + 16 iteration) u32;
  the app generates `app/lib/core/app_version.g.dart` via `scripts/gen_app_version.py` and shows
  it on Settings.
- **DAS erased-`.SV` recovery** (2026-10-04): `StaticRecallAll` detects a `0xFF` System Name in
  the reduced filesystem's mirror and re-persists the live settings instead of clobbering them
  with erased bytes; the DAS no longer comes up with a `0xFF` Name after a reflash. `.SUBREQ`
  is deleted when the requester table empties. HIL: `tamu_hardware_verification_test`
  (erased-`.SV`), `hil_subscriptions_test` (empty-`.SUBREQ` removal).
- **Core bootloader + App-service removal** (2026-10-04): the factory/`ota_0` split, B1 entry,
  re-arm, raw-USB HIL, app `DirectUsbTransport`, and `Tamu_v2_0A -t upload` targeting `ota_0`;
  the dead `ServiceType::App` (0x11) tag removed from firmware + app; `script_file` codecs
  deduped onto `types.dart`; the missing test tags declared.
