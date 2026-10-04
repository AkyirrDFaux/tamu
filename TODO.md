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
subscription, register, script, requester-persistence and backup-walk dedups are done).

**Firmware**
- SNDB: five `for i < num_entries { ReadEntry; ... }` scans share a prologue (a visitor would
  add indirection; the loops are short and clear as-is).
- `RegisterGetByBlockInfo`/`SubscriptionsGetField` share one resolver - awkward because
  `RegisterDispatch.h` precedes `SubscriptionsDefs.h` in the include order.
  (The enum-alias cleanup is done: `DataType::NetAddr/Unknown` and `AccGyrError::ErrTimeout`
  removed; the remaining `BlockInfo`/`Deleted`/`Undefined` and the render `Mesh`/`Colour3`/
  `PointCoordinates` entries are documented vocabulary or used by the app's tombstone paths.)

**App**
- `script_value_dialog._changeType` -> `ScriptDraftValue.setType` (the dialog's local state
  shape differs, so this needs a small state refactor).
- `ScriptValueInfo` vs `ValueInfo` (the raw codec helpers now live only in `types.dart`).

**Audit findings (2026-10-04)** (legacy/dead-code sweep; more audits in flight)

Firmware:
- Dead build flag `-D USE_REGISTER_SERVICE` (`platformio.ini:44`) - referenced nowhere.
- Unused defines `TRID_SYS_BASE`/`TRID_SYS_MAX`/`TRID_APP_MAX` (`Packet.h`), `Capabilities::None`
  (`Enums.h:10`).
- The `#ifndef BOOTLOADER_FORCE` / `TAMU_BOOTLOADER_FORCE` test hooks (`DAS_v0.1/Bootloader.cpp`,
  `Bootloader/CoreBootloader.cpp`) are only selectable with an ad-hoc `-D`; no env defines them.
  Add documented `*_force` envs, or drop the branches.
- `AppBLE.h` `BLE_PACE_MS == 0`, so the paced-TX `#if BLE_PACE_MS > 0` branch never compiles.
- Removed app-identity model remnants: the `id_tgt/id_src == 0xFFFE` branches and comment in
  `Dispatcher.h`, the `id_src == 0xFFFE` branch in `SubscriptionsControl.h:16`, and the stale
  `AppInterface.h` header comment (claims `ServiceType::App` identity + CID-as-TRID).
- Dedup: `RS485_SendRaw` repeats the CSMA/echo loop of `SendAndVerifyPacket`; `SendResponse` and
  `SendDeviceReply` repeat the same `PacketConstruct`.
- Stale comments: `MemoryDynamic.h:36` says `DT_XXX`/`DV_XXX` (actual `DT_<hex2>`).

App:
- Unused `tridSysBase/Max`, `tridScriptBase/Max` (`protocol.dart`) and `ServiceType.router`
  (0x10, no firmware Router service).
- `render_dict.dart:45` `if (aligned == 0) break;` is unreachable.
- `log_page.dart:330,361` duplicate `ServiceType.fromValue(sourceId & 0xFF)` (the `& 0xFF` is
  vestigial - the wire already carries an 8-bit type).
- Stale comments: `file_viewers.dart:340` (old `DT_` layout) and `file_viewers_test.dart:152-163`
  (removed CLI capability in the sample cap word, wrong `DT_` layout).

Bootloader:
- Stale/misleading comments in `DAS_v0.1/Bootloader.cpp` (`:17-19` SPL claim, `:109-110` FCR
  claim); `FlashWrite` (`:130-141`) returns a bool that is always true and ignored; blank lines
  at `:310-311`.
- `CoreBootloader.cpp:38` magic `0x2A0000u` duplicates `partitions.csv:6` (bitmap size silently
  wrong if the partition changes).
- Dedup: `ReceiveFrame`, `HandleRead` and the dispatch loop are duplicated between
  `DAS_v0.1/Bootloader.cpp` and `Bootloader/CoreBootloader.cpp`; `_chunkBytes`
  (`bootloader_client.dart:355-360`) duplicates `Bootloader._pad`.
- Optimization: direct-USB write pacing is applied twice (~6 ms/chunk, ~2 min over the core
  image) - zero one of the two pacings.

Subscriptions:
- Cleanup: `Periodic` computes an unused `hash` (`SubscriptionsProvider.h:205`); unused
  `confirm = true` default (`SubscriptionsRequester.h:88`); stale staging comment
  (`SubscriptionsPersist.h:14-16`); "Reduced subscription service" label
  (`SubscriptionsDefs.h:13`) is stale; no firmware subscription tests exist.
- Dedup: CID 0x01 `ProviderFindByTrid` + `ProviderInstall` re-searches; CID 0x11
  `RequesterFindByTrid` + `RequesterUpsert` re-walks; OnChangeConfirm computes `Fnv1a` twice
  (`SubscriptionsProvider.h:217` vs `:299`); `SaveRequesterTable` walks the table twice.
- Optimization: OnChangePeriodic/Confirm hash before the `SubMinTime` gate
  (`SubscriptionsProvider.h:209,217`); `SubscriptionsGetField` runs per entry per tick even for
  not-due Periodic (`:195-199`); `.SUBREQ` rewritten on every set/cancel (`SubscriptionsPersist.h:44`);
  `RequesterRemove` `i++` skips the entry shifted into the freed slot (`SubscriptionsControl.h:257-263`);
  leftover `.SUBREQ~` is not deleted when the table empties.

Device + Log:
- Cleanup: `Dispatcher.h:36` Router TODO should point at `Router.md:9` ("not to be implemented
  yet"); `0xFFFE` branches (also `SubscriptionsControl.h:16`); `ServiceType.router` /
  `Capability.router`; `appSourceId`; stale `Device.h:360` comment.
- Dedup: `SendFragFragment` (`MemoryBackup.h:46-58`) is reimplemented by SNDB Read All
  (`Device.h:60-86`) and GetLogs (`LogHandler.h:97-126`); reply construction duplicated ~15x; the
  streaming loop body is duplicated between those two.
- Optimization: SNDB Read All counts then streams (two registry scans); `ClearReadLogs` is O(n*m);
  log append/read rescan `LogCapacity` each time; `AppInterfacePump` shifts the whole RX queue per
  frame (O(n^2)) and reads `AppRxCount` outside the lock (`AppInterface.h:174-183`); a 128-byte
  stack copy per outgoing reply (`:75-77`); `APP_TX_RING_SIZE` comment claims ~4.5 KB but is 8192;
  `GrowLogStorage` partial-failure path commits a grown buffer but returns false (`Log.h:89-106`).

Register:
- Cleanup: stale CID comments (off-by-one/wrong) in `Register.h:10-12`, `RegisterRead.h:3,9`,
  `RegisterWrite.h:3,9`, `RegisterPersist.h:3,9,121`, `RegisterEnumerate.h:3` (actual Read=2,
  Write=3, Recall=4, Save=5); dead `appendDynamicEntry` (`register_client.dart:439-450`);
  `BlockType.render(0x100)` unused and collides with the 0x100 dictionary marker
  (`types.dart:264,289`); `writeDynamicBlockMeta`'s `type` param is ignored by the firmware
  (`register_client.dart:422-435`); `INVALID_INDEX` vs literal `0xFF` (RegisterWrite.h:17).
- Dedup: `RegisterGetByBlockInfo`/`RegisterSetByBlockInfo` duplicate the script/dynamic/static
  handler routing; the dynamic name is reachable via both 0x12/0x13 and field 0xFF; redundant thin
  wrappers in the app client (`getFieldCount`/`getBlockKeys`/`getDynamic*`).
- Optimization: block-type enumeration is O(types x blocks) (`RegisterEnumerate.h:56-86`);
  `FieldCount()` allocates 256 B and scans on every meta read (`MemoryBlocks.h:283-287`);
  `CleanupDynamicFiles` stats/deletes 256x4 files on every Save All (`MemoryDynamic.h:74-83`);
  the app re-requests `enumerateBlockTypes` instead of caching (`register_client.dart:94`); the
  app reads one item per round-trip in `readBlocks`/`readStaticFieldLayout`/`createDynamicBlock`.

App (general):
- Cleanup: stale `settings.dart:3-5` header (claims Android not implemented); misplaced comment
  `script_editor_view.dart:332-334`; `theme.dart:9-10` effectively-private `kOrangeDark`/`kWhite`;
  `debugPrint` in `connection.dart:302,324` bypasses `AppDiagnostics`; pointless `_revision++`
  (`scripts_page.dart:159`); unreachable non-enum branch in `script_widgets.dart:324-327`;
  test-only core API (`ScriptDraftValue.setType/setValue`, `mathTransformOp`, `ScriptField.count`,
  `script_client.writeVariable`).
- Dedup: Start/Stop control rows (`scripts_page.dart:326-357` vs `script_editor_view.dart:29-51`);
  `_card`/`_row` (`update_page.dart:342,363` vs `device_view_page.dart:178,198`); `SectionHeader`;
  raw `ScaffoldMessenger` vs `showSnack`; `dataTypeLabel` vs `dataTypeWord`.
- Optimization: `IndexedStack` eagerly builds all shell tabs (`main.dart:100`); `_probeLoaded`
  scans slots 0..255 sequentially per refresh (`scripts_page.dart:91-150`); `_refresh` per-key
  reads (`script_editor_page.dart:137-184`); unawaited whole-file `save()` per toggle
  (`settings.dart:90-94`); redundant `settings.load()` (`settings_page.dart:21`); sync
  `readAsBytesSync` (`host_files.dart:19`); `_events.removeAt(0)` O(n) (`diagnostics.dart:34`);
  unbounded `_log` (`update_page.dart:36`).

App (connection / device):
- Cleanup: `takeTxId()` public but unused and doesn't record in `_pending`
  (`connection.dart:512-525`); unused `pingCore()` (`device_db.dart:101-105`); inert
  `appSourceId` (`protocol.dart:38,181`).
- Dedup: five copies of the "request + try/catch -> null" wrapper (`device_db.dart:74-89`,
  `script_client.dart:28-37`, `storage_client.dart:58-70`, `register_client.dart:46-54`,
  `subscription_client.dart:45-53`); the 4-byte fragment strip (`connection.dart:477-479` vs
  `:499-501`); a fresh `RegisterClient` per System read (`device_db.dart:108-111`).
- Optimization: `DeviceDatabase.all` sorts/allocates per access and pages re-sort
  (`device_db.dart:63`, `devices_page.dart:52,156-157`); per-device `notifyListeners` in a sweep
  (`device_db.dart:123,151,190,203,245,299`); `discoveredLinks` rebuilds+sorts per call
  (`connection.dart:81-113`); BLE scan event path O(n) + notify per event (`connection.dart:283-293`);
  `StreamBuilder` resubscribes every rebuild (`connection_page.dart:162-169`); `_refreshUsb` opens a
  `SerialPort` per port per refresh (`connection.dart:312-320`); log-page rebuild allocations
  (`log_page.dart:127-128,169,228-232`).

App (subscriptions):
- Cleanup: the value-update listener path is unconsumed (`subscription_client.dart:11-13,21,31-42`);
  `recallAll`/`saveAll`/`setProviderSubscription`/`_providerPayload` unused (`:132-156`);
  `regClient` param unused (`subscriptions_dialog.dart:86,94`, `subscriptions_page.dart:29,41,259`);
  unused `isEdit` getter; `_deadzoneController.text = '0'` hardcoded (`:136`); stale comments
  (`subscription_client.dart:44`, `connection.dart:541-543`); magic `16` duplicates
  `MAX_REQUESTER_SUBS`.
- Dedup: `_fetchBlocks`/`_fetchFields` pure pass-throughs (`subscriptions_dialog.dart:206-212`);
  `setRequesterSubscription` re-fetches the requester table and rebuilds the entry by hand
  (`subscription_client.dart:107-130`) - use the passed list / a `copyWith`.
- Optimization: `getProviderSubscriptions`/`getRequesterSubscriptions` run sequentially
  (`subscriptions_page.dart:60-61`); nested `setState` in `_load*Fields*`
  (`subscriptions_dialog.dart:169-190`); N+1 register round-trips (`subscriptions_dialog.dart:74`,
  `subscription_client.dart:81`); redundant `toDevice` (`subscription_client.dart:141`).

App (storage / backup):
- Cleanup: pre-release `backup.json` branch (`backup_capture.dart:169`); legacy numeric-format
  rejection (`backup_format.dart:337-340`); three near-duplicate name normalizers
  (`unpadName`/`normalizeFileName`/`decodePaddedString`); stale `.TABLE` "self-describing" comment
  (`storage_client.dart:98-99`); `storage_page.dart:213-216` dead null/empty split + copy.
- Dedup: file-table record parsing (`storage_client.dart:113-129` vs `storage_page.dart:354-389`);
  file-kind classification (`backup_value.dart:47-56` vs `file_viewers.dart:36-70,504-510`);
  file-table read + per-file loop (`backup_capture.dart:99-128`, `register_page.dart:240-251,294-304`).
- Optimization: dynamic per-field save reads the table 3x (`register_page.dart:309-342`);
  `_loadStaticFields` runs for every file type (`file_viewers.dart:111-121`); `.SV` read +
  `readBlocks` then another layout read (`storage_page.dart:284-299`); `_hexView` materializes a Row
  per line (`file_viewers.dart:147-183`); sequential `readLiveDevice` in `buildRestorePlan`
  (`backup_restore.dart:255-261`); `StaticSpaceLayout.fromRegistry` recomputed per build
  (`file_viewers.dart:429-432`).

DAS_v0.1:
- Cleanup: unused `AppConnected` (`Main.h:14`) and `g_rs485_ready` (`RSBus.h:12,94`); stale
  comments (`Storage.h:39` says 0x3000, actual 0x3F00; `MeasuringRun.h:8,61`; `Measuring.h:8`;
  `MeasuringRun.h:39-40` thresholds; `Main.h:26-31` drift).
- Dedup: per-channel sample blocks copy-pasted (`Main.h:187-198`); the `[1,1022]` clamp repeated
  (`MeasuringRun.h:89-90,101-102`); `log10` reimplemented (`MeasuringRun.h:92`); `VOLTAGE`
  duplicated from the Tamu Base.h.
- Optimization (RAM at 99.6%, 8 B headroom): `rx_buffer[300]` -> 258/260 (~40 B,
  `RSBus.h:3,7`); `providerTable[4]` -> 2 if tolerable (~104 B, `SubscriptionsProvider.h:16,39`);
  `s_sysValueBuf[24]` -> 20 (`RegisterRead.h:90`). Flash: the bootloader's per-byte timeout ->
  single frame deadline (`Bootloader.cpp:182-204,315`). Per-tick: `SampleIntervalMs` software
  divide twice per loop (`Main.h:89-95,189,194`) - cache it on `SamplingRate` write;
  `Meas_SelectRange` drives all 6 range pins every sample (`MeasuringRun.h:42`) - only on change.

Storage (firmware):
- Cleanup: orphaned "Dynamic memory block" section header at the end of `MemoryBackup.h:155-165`;
  duplicate "all matches removed" comment (`StorageBlockFS.h:136-142`); `wear_cursor` relies on
  zero-init (`:637`); redundant `extern` before the definition (`MemoryDynamic.h:13`); dead no-op
  `DeleteFile`/`RenameFile`/`ResizeFile` in fixed builds (`StorageFixedFS.h:130-135`); stale
  `SubscriptionsPersist.h:14-17` staging comment; legacy NUL-strip in `normalizeFileName`.
- Dedup: read/write clamp + `GetFileInfo` between the two file systems; `ReadBackupFile` should use
  `Storage.ReadFromFile`; `DeleteDynamicBlockFiles`' redundant `FileExists`; `DeleteMatching` vs
  `RenameFile` invalidation.
- Optimization: the `used_bitmap` still does O(num_blocks x capacity) reads because `BlockUsed`
  rescans the table per block (`StorageBlockFS.h:305-310,590-614`); `CleanupDynamicFiles` probes all
  256 slots x 4 names on every save (`MemoryDynamic.h:74-83`); `DynamicRecallAll` probes 256 slots
  (`RegisterPersist.h:81-85`); `ReadTableEntry` reads 16 B at a time (bulk-read the page);
  `device_backup.dart:135-149` stride lacks the trailing `align4`.

App (register UI):
- Cleanup: block-actions popup always rendered though empty for non-dynamic blocks
  (`register_page_tiles.dart:130-153`); System struct-member popup always empty (`:523-542`); dead
  `_changeType`/`_deleteEntry` path (`:600-603`, `register_page.dart:587-591`); unreachable
  per-field dynamic-save branch + unused `block` param (`register_page.dart:316-326,373-377`);
  redundant ternary (`:352`); two imports on one line (`:17`); dead `hasValue` state
  (`value_editor_containers.dart:30`).
- Dedup: `field*256+key` / `256+f*256+key` magic repeated (`register_page.dart:312,464,480`,
  `register_page_tiles.dart:33,218,480`, `register_page_edit.dart:215,421,425,462,482`); the
  show-editor -> write -> reload flow across three editors; `_saveField`/`_recallField` &
  friends; dynamic field read via two client paths.
- Optimization: `readBlocks` re-issues the block-type enumeration per static type
  (`register_client.dart:267-297`); simple value edits trigger a full topology refresh
  (`register_page_edit.dart:229,327,454,473,511`); Backup auto-refresh re-reads every file every
  0.5 s (`register_page.dart:228-273`); repeated file-table walks (`:295-342`); per-instance caches
  never pruned on topology change (`:394,408,429`); repeated full enumerations in
  `createDynamicBlock`/`moveDynamicBlockTo` (`register_client.dart:400-412,513-553`).

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
