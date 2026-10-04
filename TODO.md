# TODO

Long-term plan (`Docs/Plan.md`): 1) Scripts, 2) blocks/modules + subscriptions, 3) app backup.

Host gate: `./test.sh` = the native numeric/geometry/CRC/stride/align tests (core, 32-bit and DAS
configs, both `OPTIMIZE_SPEED` states) + the app host suite + `flutter analyze`. HIL needs the rig
(core on `/dev/ttyACM1`, one DAS on `/dev/ttyACM0`); `hil_current_setup` needs two DAS nodes, and
`hil_bootloader` needs the DAS built with `-D BOOTLOADER_FORCE`.

## Open work

- [ ] **A11 / D3 - per-field geometry-mask versioning.** A write to an eye block bumps the block
      generation and the renderer recomputes all 9 masks; the panel is at its cap (~127-132 FPS),
      so not urgent. Needs a per-field invalidation token: a per-field version array, or comparing
      the cached geometry inputs each frame and recomputing only the changed fields (host-checkable).
      **Deferred to the display/rig batch.**
- [ ] **A10 part 2 / D5 - cross-script macro calls.** `Script.md` names "Macro call" but gives no
      opcode/boundary/argument rules. The VM runs one script per tick and wait state lives on the
      callee, so it needs a `(script, line)` call stack and a tick loop that resumes the waiting
      script. Proposal (in `Issues.md`): a blocking `Call script` op, values via registers.
      **Blocked on a docs decision.**
- [ ] **Rig looks.** `Polygon`/`Star` after the `atan2` fix; a rounded `Square`/`Rectangle`
      (eye-only; the evaluation scene sets no `Rounding`). Needs the display rig.
- [ ] **D4 - confirm the LED brightness-cap value** by eye (mechanism landed: layout brightness
      limit 178 = 70 %, enforced in the render).
- [ ] **DAS provider stale entries (low priority).** Effectively solved by the 120 s provider lease
      + orphan re-cancel; revisit only if a *confirmed* cancel is wanted.

## Code-cleanup backlog

No open items: the 2026-10-04 audit's correctness fixes and the follow-up cleanups (storage `Find*`
error signalling + offset-only invalidation; `RegisterResolveByBlockInfo` resolver sharing and the
`.SUBREQ` unchanged-write skip; the core bootloader host-helper dedup + `Bootloader.padPayload`; the
app `ScriptDraftValue.setType` / `ValueInfo` unification; the added coverage tests and fixture
fixes) are committed.

**Deliberately left** (a merge would read worse): the three flag-name decoders (`flagWords` = full
words for the backup format, `ValueFlags.describe` = RO/P/TR, `_flagsSuffix` = RO/P) and the two
flag renderings in `register_page_tiles` (chips vs small text).

## Decisions (locked)

- **Register**: `ValueInfo = Type(16)|Size(8)|Flags(8)` (internal and wire); `BlockInfo =
  Type(10)|Inst(6)|Field(8)|Key(8)`. Flags are passive: ReadOnly `0x01`, Persistent `0x02`,
  Trigger `0x04`. **Save All = 5 / Recall All = 4** (swapped vs `Command ID table.md`;
  `Register.md` wins - the user updates that file).
- **Static memory** is two flat compile-time spaces (volatile + persistent); `.SV` is a raw 1:1
  mirror of the persistent space (targeted saves/recalls are the app's job). The static block table
  is literal (`BlockEntry`: Field&Key + MemoryOffset + ValueInfo) with a literal trigger table.
- **Dynamic** types `0x3F0-0x3F3`, **Scripts** `0x3F4-0x3F7`, Reserved `0x3F8-0x3FF`, each 64
  instances, addressed by one **global** index `0..255`. Basic CIDs `0-5`, dynamic `0x10-0x13`.
- **System block** (type 0): a `StaticBlockDescriptor` (`System_Block` + `System_Entries`, one
  entry per field at key 0). Struct fields 0/3/4/5 are `Undefined` with Size = the member sum (the
  struct position is not on the wire); Name is a fixed space-padded `char[16]`; NetID is core-only.
- **Packet**: 12-byte header `CRC8|Flags|Reserved+Priority|PayloadLen(bytes)|SRC|TGT|CMD|TRID`,
  payload exactly `len` bytes, `12+len <= 128`. `SUCCESS`/`FAIL` flag bits exist.
- **TRID ranges** (`Packet.h`): System/Logs `0x0000-0x0FFF` (incrementing counter; the service type
  stays in the high byte so echoed replies route), Subscriptions `0x1000-0x1FFF` (table), Scripts
  `0x2000-0x2FFF` (slot), App `0xF000-0xFFFF` (slot). Replies echo the request's TRID.
- **Addresses** are 6-bit net + 10-bit device (`MakeId`). Net 0 means the local net and is resolved
  to the local NetID at match/forward time (`NetQualifyLocal`); the core is `NetID.1`.
- **Subscriptions**: shared 16-byte table (`sourceReg`, trigger, `minTime` uint24, period,
  deadzone); requester 28 B / provider 32 B; 120 s lease renewed by keepalive/updates; CIDs
  `0x0400/0x0401` inter-device, `0x0410-0x0413` requester, `0x0420/0x0421` provider; cancel is
  `trigger None`; `.SUBREQ` holds 24 B entries (TRID persisted, timeout regenerated).
- **Subscription capabilities** are split: `SubscriptionRequest = 1<<7`, `SubscriptionProvide =
  1<<9` (`Node = 1<<8`).
- **Scripts**: `SCR_XXX` (4096 file ids) but only 64 loaded slots (6-bit); the caller picks the
  slot. Boot-load prefers the identity slot.
- **BLE advertising** uses the persisted System Name.
- **Bootloader**: raw frame `0xCA | control(5 pad, 1 even parity, 2 cmd) | offset u32 LE |
  [32 B payload] | 0xBC`; parity is even over the command + offset + payload; cmds `01` write /
  `10` read-request / `11` read-response. The core's Device `0020/0021` passthrough relays the raw
  frame onto its RSBus (the frame carries no address). No capability bit. DAS: bootloader 2 KB at
  `0x0`, app at `0x800`; button-only entry (PC0). The DAS waits a widened 32-byte CSMA silence
  before replying so its frame cannot collide with the core's TX-enable release; the core relay
  polls the UART status instead of `uart_wait_tx_done`. The app paces writes and verifies by
  read-back + retry.

## Notes / gotchas

- **TimeSync is synchronized-device initiated**: the node sends Device CID 3 and applies the offset
  to its own clock; the core only answers. All four timestamps use the synchronized `Now()`; the
  node estimates its drift (Q16.16) and extrapolates between syncs; the re-sync interval is
  measured in RAW time. Nodes re-sync every **60-75 s** (30 s warm-up), holding the DAS's ~1 % RC
  drift within ~10 ms.
- **A script loop advances at most once per main-loop tick** (`ScriptRun` stamps every line and
  yields when a line is revisited). A `While` re-reads its operand each iteration.
- **Fixed-point `^`**: a non-integer exponent is a product of nested square roots (each truncates);
  keep chains short. Exponent/weight literals are Q8.8.
- **DAS sensors**: the NTC is a **100 kΩ** part (`MeasNTC100K`); the LDR uses `R(E)=R10*(E/10)^-gamma`
  with `LDR_R10_KOHM`/`LDR_GAMMA` knobs. The lux path wants a lux-meter calibration of R10.
- **LED strips can brown out the board**; the builder clamps the displays to 5 % and the
  brightness script caps at 70 %, and the layout file's brightness limit enforces it in the render.
- **The renderer samples the geometry mask forward** (`pp = Position * coord`), so both writers
  store `t' = L * t` to keep the centre at `-t` for any rotation.
- **Storage names are space-padded, not NUL-terminated** (`NameMatch` packs the plain name first).
  Files: `.SV` (static persistent mirror), `.TABLE`, `.DT_<xx>`/`.DV_<xx>` (dynamic), `SCR_XXX`,
  `.SNREG` (SNDB), `LAY_1` (display layout), `.SUBREQ` (requester table). The DAS runs the same
  multi-file filesystem (384 B region); it only ever holds `.SV`.
- **`.SV` presence is real on every target.** An absent/short mirror makes `StaticRecallAll` keep
  the compiled-in defaults and re-persist them, so the mirror exists for the app's read-modify-write
  partial saves; a present-but-erased mirror (torn write) is also re-persisted via the `0xFF` System
  Name guard. HIL: `HIL: DAS recalls over an erased .SV`.
- **The DAS's flash image does not cover the storage region** (code ends ~0x2B5C, storage at
  0x3F00), so reflashing preserves the `.SV`; the erased-mirror handling makes a reflash recover.
- **`.SUBREQ` is removed when the requester table empties** (`SaveRequesterTable` deletes it at
  count 0).
- **Backup zips** are semantic format 2, one JSON per device (no manifest); entries are built from
  UTF-8 bytes. Large files are skipped above 128 kB by default.

## Done (condensed)

- **Scripts** (`Docs/Services/Script.md`): `SCR_XXX` parser, loaded-script registry (banked types
  `0x3F4-0x3F7`), management CIDs `0x0500-0x0507`; the VM (preloaded instructions, line/block
  tables, math/logic/flow/time/services, infix expressions with vectors/matrices, per-instance
  TRIDs, loop guard); boot/load; in-script (un)load (ops 7/8); the app list page + editor.
- **Blocks/modules + subscriptions**: schemas aligned; `Deadzone` on both entries; all trigger
  types; per-trigger hashlike; high/low priorities; script I/O as source/target; auto-save on
  set/delete; two-device HIL.
- **App backup** (`Docs/App/Backup.md`): semantic format; capture/restore of the whole register,
  subscriptions, scripts, SNDB and every file; per-part sync UI with remap; HIL round-trip.
- **Register service** (revised `Register.md`): L0-L6 + P1-P8; the doc-vs-code passes.
- **Evaluation setup** (`Docs/Current setup v3.md`): the setup builder, the emote system, tuning
  rounds, the LED transfer curve + ring look, the LDR/brightness calibration, the setup HIL suite.
- **RSBus packet + TRID + subscriptions rework** (docs 2026-10-03): byte payload length, central
  TRID ranges + reply echo, the subscription rework, the `.SUBREQ` name, the System-block rework.
- **Cleanup / optimization passes**: split files, DAS flash reductions, core speed build, native
  host tests, the app↔firmware contract test, and the 2026-10-04 duplication passes.
- **Deterministic versioning** (2026-10-04): per-target content-hash versions
  (`scripts/version.py` + `version.json`); firmware envs stamp `VERSION_*`
  (`firmware/scripts/auto_version.py`, bootloaders unversioned); the System block packs
  `YY:MM:DD:II` (7/4/5/16); the app generates `app_version.g.dart` and shows it on Settings.
- **DAS erased-`.SV` recovery** (2026-10-04): `StaticRecallAll` detects a `0xFF` System Name and
  re-persists the live settings; `.SUBREQ` is deleted when the requester table empties.
- **Bootloader + App-service removal** (2026-10-04): the core factory/`ota_0` split, B1 entry,
  re-arm, raw-USB HIL, app `DirectUsbTransport`, `Tamu_v2_0A -t upload` -> `ota_0`; the dead
  `ServiceType::App` removed.
- **Audit fixes** (2026-10-04): the 15 per-area audits (register, storage, device/log,
  subscriptions, tamu, das, bootloader, app, tooling, tests) — correctness fixes, dedup,
  optimization, the subscription capability split, net-qualified addressing, `.DT_`/`.DV_` and
  `.SNREG` dotted names, the dynamic name commands, and the BLE service-UUID filter.
- **Cleanup follow-ups** (2026-10-04): storage `Find*` bool error signalling + offset-only
  invalidation; shared `RegisterResolveByBlockInfo` resolver + `.SUBREQ` unchanged-write skip; core
  bootloader `BootloaderHost.h` dedup + `Bootloader.padPayload`; `RegisterRead` version-packing
  cross-ref/`static_assert`s; app `ScriptDraftValue.setType` and `ValueInfo` unification; added
  storage-client/dynamic-trace/TRID tests.
- **DAS full filesystem** (2026-10-04): the DAS drops the reduced `StorageFixedFS` and runs the
  shared `StorageBlockFS` in a **512 B** region at `0x3E00` (pointer page + table + 384 B data);
  `USE_FIXED_STORAGE` and the fixed branches are removed, and the DAS now advertises
  `StorageFiles`. `StaticRecallAll` re-persists the defaults when `.SV` is absent/short so the full
  FS's real file presence keeps the mirror valid. The app's fixed-FS branches are gone too:
  `StorageClient` no longer probes/detects fixed storage, `parseFileTable` always skips offset-0,
  file writes always stage-and-rename, and the Storage page no longer hides create/rename/delete.
- **Storage size variants** (2026-10-04): following the `Crc8` `OPTIMIZE_SPEED` pattern, `FindSpace`
  picks the bitmap best-fit (speed) or a linear first-fit (size), and `MoveFiletable` grows/shrinks
  the table (speed) or keeps the page count (size). The DAS takes the size shapes: **−328 B**,
  leaving **376 B** app headroom at the 512 B region (24 B without them); the core keeps both speed
  shapes. File enlargement stays available on every target (gating it was rejected: the Storage docs
  require resize-to-larger).
- **Docs-conformance sweep** (2026-10-04): four read-only audits of the whole Docs set. Code fixes:
  the DAS bootloader `APP_LIMIT` `0x3F00 -> 0x3E00` (storage moved to `0x3E00`); the core
  `BOARD_DAS_v0_1` ifdef replaced by the `MAX_PROVIDER_SUBS` build flag; Device core/SNDB CIDs
  `10-13 -> 0x10-0x13` (firmware + app, matching `Command ID table.md`); log reports addressed to
  the local core `0.1` instead of broadcast. The remaining doc-wording and design items are parked
  in `Issues.md`.
- **Log source + packet priorities** (2026-10-04): the log struct now matches `Log Handler.md` -
  16-bit `source` = BlockType|Instance, 8-bit category, 8-bit specifics (a service log uses the
  reserved source type `0x3FF` with the ServiceType in the instance field), so instances are no
  longer dropped and the core dedups per instance; covered by a native `log_test` and an app decode
  test. Packet priorities implement the documented classes (`PRIORITY_ERROR`, `TIMESYNC`,
  `SUB_HIGH`, default, `SUB_LOW`, `STREAM`, `LOG`): error reports are highest, TimeSync next,
  fragmented replies are Streams, the log DB stream is Logs, subscription updates keep 4/12.
