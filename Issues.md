# Issues

## Code audit findings (2026-10-04)
From the per-area audits; fixes pending unless noted.

**App / storage**
- **`StorageClient.unpadName` strips interior spaces** (`app/lib/core/storage_client.dart:89-91`).
  `text.replaceAll(' ', '').trim()` removes spaces *inside* a name, not just the 8-byte right
  padding, so a stored file such as `A B` reads back as `AB` and no longer matches the device's
  record. Latent today (no shipped firmware file has an interior space). Strip only trailing
  spaces/NULs, like `decodePaddedString` (`types.dart:32`).

**Bootloader**
- **Core bootloader: offset overflow bypasses the bounds check** (`firmware/src/Bootloader/CoreBootloader.cpp:97,112`).
  `offset + PAYLOAD_SIZE > s_app->size` wraps for `offset >= 2^32-32`, so an out-of-range offset
  passes, then `offset / SECTOR_SIZE` indexes `s_erased[]` out of bounds (into an 84-byte array)
  and `esp_partition_read` leaves `data` uninitialised, leaking stack bytes in the read reply. The
  DAS checks `addr < APP_BASE` first; the core has no equivalent. Use
  `if (offset > s_app->size || s_app->size - offset < PAYLOAD_SIZE) return;`.
- **Core bootloader: mid-session retry/reflash is unsafe** (`CoreBootloader.cpp:100-105`, bitmap `:38-48`).
  Sectors are erased once per bootloader *session*; a second full write (retry, or flashing a
  different image) skips the erase, so bits that must go 0->1 cannot be cleared and verification
  never converges - contradicting `update_page.dart:202` ("safe to retry"). The DAS re-erases each
  page per write so it is retry-safe. Add a session/pass reset or erase per write pass.
- **Direct-USB replies are not matched to the request** (`app/lib/core/bootloader_client.dart:149-160`).
  `DirectUsbTransport.readChunk` completes on the first 39-byte `0x11` frame and checks only
  cmd/end bytes, not the offset or parity; a late reply from a previous read can be returned for
  the wrong offset. Check `Bootloader.offset(frame) == offset` (and decode/parity).
- **DAS bootloader reply silence-wait can hang forever** (`firmware/src/Devices/DAS_v0.1/Bootloader.cpp:206-225`).
  `SendRaw`'s `for(;;)` silence loop has no deadline, so a continuously noisy/stuck bus blocks the
  bootloader permanently; `ReceiveFrame` also applies the full timeout per byte. Bound the loop
  and use a single per-frame deadline.
- **`update_page` accepts non-raw firmware files** (`app/lib/ui/update_page.dart:62`).
  `allowedExtensions: ['bin','img','uf2','hex']`, but the bootloader writes raw bytes, so a
  `.hex`/`.uf2` is guaranteed to fail verification. Restrict to `['bin','img']` (or decode them).
- **LED-entry doc inconsistency** (`Docs/Services/Bootloader.md:5` vs `Docs/Devices.md:12` /
  `update_page.dart:195-196`): white LED vs "missing hardware" vs red LED. Code drives no LED in
  the core bootloader. Needs a docs ruling.

**Subscriptions**
- **`DeltaPeriodic` with deadzone <= 0 always resends** (`SubscriptionsProvider.h:244-247` scalar,
  `:264-266` vector). `dz`/`dz2` become 0 so `delta >= 0` / `dist2 >= 0` is always true, even for
  an unchanged value - contradicts the doc's "distance / 0 = any change". Require a non-zero delta
  when the deadzone is 0.
- **120 s requester timeout removes the entry but never saves `.SUBREQ`** (`SubscriptionsControl.h:257-262`),
  so the dead entry is restored on the next boot ("re-activates after boot"). Call
  `SaveRequesterTable()` after the removal loop.
- **The provider's CID 1 initial-value reply is never consumed by the requester**
  (`SubscriptionsControl.h:126-139`): the reply CMD carries the request TRID (service byte 0x10),
  so the dispatcher routes it to the app range / drops it. Periodic and Edge subscriptions get no
  initial value. Route/consume it on the requester, or send an explicit CID 0 value after install.
- **`ProviderInstall` on an existing entry keeps `hash`/`lastSentMs`/`lastVec`**
  (`SubscriptionsProvider.h:81-87`), so changing source/deadzone can suppress the first send or
  emit a stale delta. Reset trigger state when the target differs / on an explicit Set.
- **`ApplyRequesterValue` doesn't renew the lease before the unresolvable-target return**
  (`SubscriptionsRequester.h:88-102`): a subscription whose target register is temporarily
  unresolvable still expires at 120 s and cancels its provider.
- **OnChangeConfirm confirmation uses the default priority** (`SubscriptionsRequester.h:108-111`)
  though it is a high-priority trigger. Pass `SUB_PRIORITY_HIGH`.
- **Deadzone scaling for non-`Number` scalar types** (`SubscriptionsProvider.h:243-245` vs
  `:160-164`): the deadzone is Q16.16 but `Uint32`/`Index` raw values are integers, so their
  deadzone is scaled by 65536. Gate the deadzone by type, or restrict scalar delta to `Number`.
- **Value updates set `FLAG_REQACK` for triggers that never ack** (`SubscriptionsProvider.h:302-309`);
  only OnChangeConfirm answers. Set it only for that trigger.
- **`kReRegisterMax = 4` silently drops further re-registration requests**
  (`SubscriptionsPersist.h:128-129`). Loop/batch or grow the queue.
- **Table-full / short-payload `break`s with no FAIL reply** (`SubscriptionsControl.h:125,178,219`),
  so the app times out. Reply `FLAG_FAIL` consistently.
- **App setters treat `reply != null` as success** (`app/lib/core/subscription_client.dart:44-53,91-95,138-143`);
  `FLAG_SUCCESS`/`FLAG_FAIL` both carry a payload. Surface/check the response flags.
- **Only one global subscription listener** (`app/lib/core/connection.dart:473-483`), but
  `backup_restore.dart:324,381` makes a `SubscriptionClient` per device, so clients clobber each
  other's `startListening`. Key listeners per device/TRID.

**Device + Log**
- **Net-ID collision detection is dead** (`firmware/src/Core/Functions/Device.h:42-46`,
  `TimeSync.h:44-60`): `CoreCollisionFlag()` is only read (`Tamu_v2.0A/Main.h:182,200`), never
  set; `HandleDiscoverResponse` never compares net bits. The docs require the check. Compare
  `MakeId` net of the responder's `id_src` against `DeviceStatus.NetId` and set the flag.
- **Net bits are never applied to addresses** (`Device.h` / `Packet.h:43`): `MakeId` is used only
  for `ADDR_ALL_CORES`; `ShortAddress`/`assign->new_addr` store the bare shortID, so the documented
  "core's NetID is automatically added" / foreign-net addressing never happens. Confirm intent.
- **SNDB interrupted-compaction recovery can destroy the only good copy** (`SNDB.h:119-136`):
  if `CreateFile(SNREG)` fails it still deletes the staged temp registry. Only delete the temp
  after a successful restore.
- **GetLogs is not "oldest first"** (`LogHandler.h:104-118`): it scans physical slot order, but
  `LogSeq` diverges after a dedup refresh or an overwrite. Emit/sort by `LogSeq`.
- **Device replies are disambiguated only by payload length** (`Device.h:215-256`): the 16-byte
  SNDB Read reply has the same length as an Assign reply; it avoids the assign branch only because
  SNDB replies echo an app TRID and are routed to the app first. Any future 12-17-byte Device
  reply would be misparsed. Tag the reply kind explicitly.
- **Assign reply discards the request TRID** (`Device.h:310-316`): hardcodes
  `MakeService(Device,0)`/TRID 0 and broadcasts, violating "responses echo the request's TRID".
  Echo `frame.trid` (high byte Device) or document the exception.
- **`NextSystemTrid` is an 8-bit counter shared across service types** (`Packet.h:105-109`): the
  docs give System/Logs a 12-bit range; this only cycles the low byte and never skips in-flight
  IDs (could reuse within 256 outstanding). Low risk today; widen to 12-bit and/or per-service.
- **Fragment reassembly ignores fragment sequence** (`app/lib/core/connection.dart:499-508`):
  concatenates payloads regardless of `fragInfo.current`/`total`, so a lost/duplicated fragment
  silently corrupts the stream. Track the expected `current` and abort on mismatch.
- **`device_db` SNDB Read All parsing duplicated and inconsistent** (`device_db.dart:258-270` vs
  `321-332`): `_doRefresh` skips `id == 0`, `sndbEntries` emits it. Share one parser. Also a stale
  comment at `:219-220` says CID 12 where the read is CID 13.

**Register**
- **System "Used RAM" reports FREE RAM** (`firmware/src/Core/Services/RegisterRead.h:51`): the
  field is labelled Used|Total and shown as "Used RAM" (`system_schema.dart:39`), but `used` is
  `GetFreeRAM()`. Compute `GetTotalRAM() - GetFreeRAM()` (clamp >=0) or rename.
- **Read/write responses > 108 bytes are silently truncated** (`RegisterDefs.h:99-116`):
  `FIELD_RESPONSE_BUF_SIZE` is 268, but `SendResponse` clamps to `MAX_PAYLOAD_SIZE` (116) and
  dynamic reads are not fragmented, so any dynamic entry with `Size > 108` loses bytes; writes
  symmetric. Cap `ValueInfo.Size` to `MAX_PAYLOAD_SIZE-8` or fragment.
- **Create Dynamic accepts an unbounded index** (`RegisterPersist.h:87-93` + `MemoryBlocks.h:336-357`):
  `AddBlockAt` pads tombstones to any index, so `block_count` can exceed `MAX_DYNAMIC_BLOCKS` (256);
  `EnumTypeWord` then truncates the index to 8 bits and save/cleanup only iterate 256, so such
  blocks are unreachable/never saved. Reject `gi >= MAX_DYNAMIC_BLOCKS`.
- **Recall All doesn't clear live blocks with no DT file** (`RegisterPersist.h:69-77`):
  `RestoreDynamicBlock` returns early when no file is found, leaving an in-session live block
  untouched (CID 4 is reachable at runtime). Tombstone the slot before the early return.
- **Create Dynamic diverges from the docs** (`RegisterPersist.h:87-93` vs `Register.md:119-123`):
  docs say request `Index (uint16)`, response `Success`; code consumes `Index + name` and replies
  with a 5-byte `BlockIndexAck`. Reconcile the docs or the wire.
- **Basic Write with field 0xFF returns `BlockIndexAck`, not the documented echo**
  (`RegisterWrite.h:17-24` vs `Register.md:69`). Document the special case or echo.
- **Short Read/Write payload is silently dropped** (`RegisterDispatch.h:114`): `if (PayloadBytes < 4) return;`
  sends no reply, unlike every other guard. `RespondStatus(frame,false); return;`.
- **Get/Set Name operate on tombstoned slots** (`RegisterPersist.h:103-117`): only guard
  `block_idx >= block_count`, not `!present`.
- **Static instance count truncates at 64** (`RegisterEnumerate.h:64`): `& 0x3F` wraps silently.
- **Doc gap:** the dynamic range's 8.8 enumerate encoding is only in code comments
  (`RegisterEnumerate.h:53-55`), not in `Register.md:66`.

**App (general)**
- **Script editor treats a read failure as an empty script** (`app/lib/ui/script_editor_page.dart:97-100`):
  `readFile` returns null for both "missing" and a transport timeout (`storage_client.dart:164`),
  so a transient bus error yields an editable blank draft whose upload overwrites the real file.
  Distinguish not-found from failure and show `_loadErrorBody()`.
- **"Update (reload live)" discards unsaved edits** (`script_editor_page.dart:212-219,265-270`):
  it re-reads the stored file instead of applying the edited draft, contrary to
  `Docs/App/Service views/Script.md:32`; there is also no unsaved-changes guard (`_dirty` only
  drives the FAB). Write-then-reload, and add a `PopScope` confirm.
- **`storage_client.writeFile` deletes before creating** (`storage_client.dart:178-179`): if
  `createFile` fails, the original is already gone (this is the script save/upload path). Stage to
  a temp name and rename, or restore on failure.
- **Update tab has no drawer affordance** (`update_page.dart:168`): `AppBar` lacks
  `leading: ShellDrawerButton()`, unlike every other tab; on compact it is only reachable by edge
  swipe.
- **Settings save race** (`connection_page.dart:69,93`): `settings.load()` reloads immediately
  before `update()`, and `AppSettings.save()` is fire-and-forget (`settings.dart:90-94`), so a
  pending save can be clobbered. Drop the redundant `load()` (main loads once) or await/queue saves.
- **Refresh error dot never shows** (`scripts_page.dart:263`, `script_editor_page.dart:281`):
  `RefreshButton(error: false)` is hard-coded, so failures render as "No scripts loaded".
- **`supportsUsb` is true on Windows/macOS** (`platform_caps.dart:22-26`) contrary to
  `Docs/App/General info.md` ("Do not implement yet").
- **`appBuildDate` is always `unknown`** (`settings.dart:20-21`): no build path passes
  `--dart-define=APP_BUILD_DATE`; diverges from `Docs/App/Settings.md:9`.
- **Script enum options are written for non-enum types** (`script_value_dialog.dart:84-87`): stale
  `options` survive a type change into `SCR_XXX`.
- **`SCR_` file match is case-sensitive** (`scripts_page.dart:157`, `script_file.dart:27-31`)
  while other call sites uppercasing; a lowercase script file is hidden.
- **Stale shell comment/doc** (`main.dart:46-47`): lists only 4 tabs but Update is inserted at
  index 2.

**App (connection / device)**
- **App TimeSync uses raw Uptime instead of Current time** (`app/lib/core/device_db.dart:172,180`):
  `_registerRead(coreId, 3, 0)` is System field 3 key 0 = `Uptime` (`TimeFromBoot`), but the
  firmware requires the synchronized `Now()` (field 3 key 1, `Device.h:238-247`). `t1/t2` are
  `Now()`, so the offset mixes two clocks. Read key 1 for `t0`/`t3`.
- **Cached devices are never pinged, so "Device lost" never fires** (`device_db.dart:271-278`):
  a known device with a serial is marked fresh without a ping, so an offline device still in the
  SNDB stays listed. Ping every id before applying the stale/lost logic.
- **BLE plain (non-permanent) denial is unrecoverable** (`connection.dart:248-262,349-354`):
  on `denied` the blocked flag stays false, so no banner shows and the resume-retry is gated on it.
- **`linkName`/`seedLinkName` is a dead identity path** (`device_db.dart:28,93-99`): `name` is
  always defaulted non-empty, so `displayName` never consults `linkName`; the picked connection
  name is not preserved. Make the default empty, or drop the field.
- **`_rxBuffers` is not cleared on disconnect** (`connection.dart:447`): `_detach()` clears
  `_pending` only, leaving stale per-txId buffers.
- **`BleTransport.packetStream` maps a shared parser per access** (`transport.dart:320-321`): a
  second subscriber would feed the same parser twice. Cache one stream.
- **BLE scan entries are never cleared or expired** (`connection.dart:62,267-296`): `_bleEntries`
  only grows, so out-of-range devices are listed forever. Add per-entry expiry/prune.
- **`notifyAppEvent` wipes unrelated snackbars** (`notifications.dart:22-24`): `clearSnackBars()`
  before every event can dismiss an in-flight autoconnect/backup confirmation.
- **Docs:** Device view (`Docs/App/Device view.md:6-7`) says it "Interacts with the device service
  only", but the app reads identity via the Register System block (matches Device Commands.md).
  Stale doc; needs a ruling.

**App (subscriptions)**
- **FAIL replies are reported as success** (`app/lib/core/subscription_client.dart:94,101,133,136,142`):
  `ConnectionManager.request()` ignores `isFail` and returns an empty non-null payload, so
  `reply != null` is true; adding a subscription on a provider-only DAS silently "succeeds".
  Surface success/fail and check it.
- **Editing a self/offline-provider subscription asserts** (`subscriptions_dialog.dart:166,174`):
  the ProviderPicker excludes the local device but `_selectedProviderAddr` is forced to the
  existing value, so `DropdownButtonFormField` asserts. Include the local/synthetic entry when it
  is the current value.
- **`fetchFieldSelections` treats a field *count* as a max *index*** (`subscriptions_dialog.dart:69-78`):
  dynamic blocks accept arbitrary sparse field indexes, so a block with fields `{5,6}` reads 0/1
  and the picker is empty; key 0 is always used for keyed fields. Iterate
  `enumerateFieldIndexes`/`enumerateKeys`.
- **`KeyPicker.keys = [0..7]` cannot address documented keys** (`subscriptions_pickers.dart:104`):
  dictionary keys go to 11 (Point coordinates=10, Noise seed=11) and key 0 is a marker with no
  value. Enumerate actual keys (or free entry); drop key 0 for dictionary markers.
- **Requester UI is not capability-gated** (`subscriptions_page.dart:116` vs `:169-203`): the docs
  model separate "Request"/"Provide" capabilities but app+firmware have one bit
  (`types.dart:383`, `Enums.h:17`). Gate the requester tab once a request bit exists; for now at
  least detect FAIL replies.
- **`"${sub.lastSentMs} ms ago"` mislabels an absolute uptime** (`subscriptions_page.dart:157`):
  the field is "ms in local uptime" (`Subscriptions.md:43`), not a delta.
- **Trigger/field-type compatibility is never validated** (`subscriptions_dialog.dart:431-440`):
  edges are bool-only and delta scalar/vector-only (`Subscriptions.md:72-82`), but `_canSave`
  accepts any source type.
- **Deadzone is never displayed** (`subscriptions_page.dart:154-161,221-224`) though it is an
  entry field (getters exist at `types.dart:555,635`).
- **Docs gap:** `Docs/App/Service views/Subscriptions.md` does not exist (only Register/Script/
  Storage/Device view docs are present).

**App (storage / backup)**
- **`writeFile` always delete+create, unsupported on reduced storage** (`storage_client.dart:177-197`):
  CIDs 1/2/3/4 are compiled out under `USE_FIXED_STORAGE`, so on a DAS `deleteFile`/`createFile`
  get no reply and `writeFile` returns false after ~12 s of timeouts - breaking app-side per-field
  `.SV` save (`register_page.dart:341`). When `fixedStorage`, skip delete/create and use the CID-6
  fragment loop directly.
- **File restore is offered on targets without create/delete** (`backup_restore.dart:237-238,414-423`):
  `RestoreKind.file` never checks `Capability.storageFiles` (unlike script/subscription/sndb), so
  reduced-FS targets show files "ready" and apply fails. Set "No file support".
- **`SCR_` files are restored twice** (`backup_capture.dart:24-35`, `backup_restore.dart:287-289,297-299`):
  capture keeps both the semantic script and the raw file; the plan emits both and apply writes
  both, so the raw file can overwrite the semantically rebuilt image. Exclude `SCR_` files from
  `RestoreKind.file`.
- **Backup drop condition ignores providers and SNDB** (`backup_capture.dart:79`): a core whose
  only content is `sndb`/provider subscriptions is dropped from the backup.
- **String decode does not clamp to the target size** (`backup_value.dart:172-179`): a longer
  semantic string resolves to more bytes than `target.meta.size`. Truncate to `size` (mirrors the
  firmware) and validate the final length.
- **Static-type stride is not 4-byte aligned** (`device_backup.dart:144,149`): per-field alignment
  is applied, not the struct stride, contradicting the 32-bit alignment rule; latent for the
  current 4-aligned structs. Use `stride: align4(inner)`.
- **`unpadName` also disagrees with the storage page display** (`storage_page.dart:362` uses
  `.trim()`), so a name with embedded spaces shows differently in the two views.
- **`storage_client.dart:2` doc claims resize, but there is no `resizeFile`** (CID 3). Add it or
  drop the mention.

**DAS_v0.1**
- **Auto-range applies the new reference before the transform uses it** (`firmware/src/Devices/DAS_v0.1/MeasuringRun.h:38-45`
  with `:79,104`, `Main.h:189-197`): the ADC sample is taken with the previous reference, but the
  code increments the range and computes `R` with the *new* Rref, so every range transition
  reports a grossly wrong value and mis-seeds the EMA. Compute the transform with the range active
  for the read, then apply the new range.
- **`FilterCoeff` polarity is inverted; 0 freezes the channel** (`MeasuringRun.h:25,56`,
  `Measuring.h:26,70`): the code is `raw*w + prev*(1-w)` (1 = no filtering, same as AccGyr) but the
  comment/clamp say "0 = no filtering"; `0` outputs `prev` forever. Fix the comment (or invert the
  weights and the AccGyr convention).
- **The storage-overlap guard never runs on the DAS** (`Devices/DAS_v0.1/Storage.h:46-66` +
  `Core/Functions/StorageFixedFS.h:56-59`): `Storage_FlashInit()` (reservation + `_etext` check,
  the only reference to `storage_flash_reservation`) is called only from `StorageBlockFS.h:18`,
  excluded under `USE_FIXED_STORAGE`; `StorageFixedFS::Init()` skips it, and `--gc-sections` may
  discard the reservation. Call it from `StorageFixedFS::Init()`.
- **`UsedFlashBytes()` reports the fixed `.SV` size** (`StorageFixedFS.h:145-150`,
  `RegisterRead.h:57-62`): always `STORAGE_FLASH_SIZE` (256) though only `sizeof(staticPer)` (40) is
  meaningful. Feed the real persistent size in.
- **`DeviceVersion` is documented mandatory but only the Tamu defines it** (`DAS_v0.1/Main.h:14`,
  `Core/Functions/Device.h:38`). Define it for the DAS or relax the comment.

**Storage (firmware)**
- **`ResizeFile` deletes the file it just resized** (`firmware/src/Core/Functions/StorageBlockFS.h:412-414,438-442`):
  both paths `WriteFilerecord(new_record)` then `DeleteFilerecord(name)`, and `DeleteMatching`
  zeroes *every* same-named record including the just-appended one, orphaning the data region
  (`FindInFiletable` then returns none). Invalidate the old generation before appending, or
  invalidate by index like `RenameFile` (`:206-220`).
- **`UsedFlashBytes()` counts the file table itself** (`StorageBlockFS.h:519-533`): the loop starts
  at `i = 0` (the `.TABLE` record) despite the comment; start at `i = 1`.
- **`ResizeFile` enlargement has no data-area bound and loops up to ~1M times** (`StorageBlockFS.h:405-430`):
  `new_blocks` is never checked against `DataEnd()`, and `BlockUsed()` (a full-table scan) runs per
  block - a single-packet flash/CPU DoS via CID 3. Reject past `DataEnd()` and bound the loop.
- **`ValidateTable` bounds check can overflow** (`StorageBlockFS.h:630`): `file_table_offset + entry0.size > DataEnd()`
  wraps for a corrupt size. Use `entry0.size > DataEnd() - file_table_offset`.
- **Invalidation is inconsistent** (`StorageBlockFS.h:154` vs `:208/:216`): `DeleteMatching` zeroes
  offset+size (8 B), `RenameFile` only offset (4 B); the app comment says the full FS leaves the
  size. Choose one policy and align the comments.
- **DT/DV save is not crash-atomic across the two files** (`MemoryDynamic.h:115-136` vs `:177-178`):
  DT is renamed before DV, so a power cut yields a new DT with an old/absent DV and
  `RestoreDynamicBlock` silently tombstones the block (values lost). Stage both and rename DV
  first (or version them).
- **`StaticBlockDescriptor::Set` can over-read a String/Filename source** (`MemoryTypes.h:151-174`):
  when `Length < Size` and `Size > sizeof(pad_buf)` (32) it falls through and `memcpy`s `Size`
  bytes from a `Length`-byte source. Reject/clamp.
- **Reduced-FS clamp is not overflow-safe** (`StorageFixedFS.h:118,126`): `offset + length > sz`
  wraps for a huge `length`; use `length > sz - offset` like the full FS.
- **`Find*` return partial results on flash read failure** (`StorageBlockFS.h:42-43,96-97,112-113`):
  a transient read error can resolve a stale/wrong record. Return an error sentinel.
- **App client has no CID 3 `resizeFile`** (`app/lib/core/storage_client.dart`) although the
  service implements it and Storage.md:41 lists it.

**App (register UI)**
- **System NetID is 1 byte but `_editNetAddr` reads/writes 2** (`app/lib/ui/value_editor_visual.dart:387-402`):
  editing field 7 writes the wrong size/value (likely rejected). Make it size-aware (emit 1 byte
  when the value is 1 byte).
- **Dialog state uses the page `_rebuild` instead of the `StatefulBuilder` setState**
  (`register_page_edit.dart:78,303,309,379`): the type dropdown and flag switches never repaint
  (values change, visuals stay stale) because the dialog route is outside the page subtree.
- **`...?.value.first` throws on an empty value** (`register_page_edit.dart:425,489`): a size-0
  entry's `value` is empty, so `.first` raises `StateError`; `?? 0` doesn't guard it.
- **`_editValue` always targets the primary key of a System struct field** (`register_page.dart:196`):
  the struct-member `ExpansionTile` passes no member key, so editing a member would write key 0
  (latent - struct fields are RO today).
- **GBlockInfo picker offers app-only marker types** (`value_editor_containers.dart:239-247`):
  `BlockType.script` (0x3FE) and `BlockType.dynamic` (0x3FF) are not wire types; picking one builds
  an unaddressable BlockInfo.
- **Backup recall evicts the wrong dynamic cache key** (`register_page.dart:282`): dynamic entries
  are cached at `field * 256 + key`, but it removes `field`.
- **Backup view doesn't hide dynamic tombstone slots** (`register_backup_view.dart:60-74`):
  unlike the live view, it never calls `isHiddenRegisterSlot`, so tombs appear as "not backed up".
- **`_systemRows` ignores `hasNetId`** (`register_backup_view.dart:104-119`): always emits a NetID
  row, so a node shows a spurious "not backed up" row.
- **Partial `.SV` save zero-pads string/filename** (`register_page.dart:335-337`): pads with NULs
  where the docs require spaces (latent).
- **Stale script-editability comment** (`register_page_tiles.dart:10-11` vs `:28`): says inputs and
  variables are editable, but only `ScriptField.input` is.
- **`_changeDynamicKey` reports success even if the old-key delete fails**
  (`register_page_edit.dart:505-509`), and a partial write can leave duplicates
  (`register_client.dart:597-615`).

**Tamu_v2.0A**
- **CRITICAL: `Vysi1Persistent` field offsets overlap the 28-byte matrix - `RenderBlock` and
  `LayoutFile` are addressed 4 bytes early** (`firmware/src/Blocks/Vysi1Layout.h:85-86`, comments
  `:74,:79`). `Offset` is a `Matrix<2,3>` = 4-byte header + 6 Numbers = 28 B, but field 2 is
  declared at offset 24 and field 3 at 28 (real struct offets are 28 and 32). Reads/writes of
  field 2 hit the matrix's last cell; field 3 reads RenderBlock + half of LayoutFile. The app
  derives the layout from reported sizes, so it computes 28/32 and disagrees. Set offsets to
  28/32, update the comments and `app/test/device_backup_test.dart:22,63`, and add `static_assert`s.
- **AccGyr write triggers apply the old config, then commit the new index**
  (`Devices/Tamu_v2.0A/AccGyr.h:265,270-272`): `ApplyAccGyrConfig()` builds CTRL1/CTRL2 from the
  not-yet-updated `staticPer.accgyr.*`, so the part keeps the old ODR/range while the field reports
  the new one (read-back verifies the old value). Store the clamped index first, then apply (revert
  on failure), or build the CTRL bytes from `index`.
- **USB/BLE packet parsers are reset from a different task than the one feeding them (race)**
  (`AppUSB.h:178,205` fed in `AppLinkTask` vs `:264-265` reset in ApplicationTask; `AppBLE.h:142-166`
  fed from the NimBLE host task vs `:248` reset in ApplicationTask). `got`/`full`/`frame` mutate
  concurrently with no lock. Feed and reset each parser on one task; fix the `AppBLE.h:38` comment.
- **Device logs share the USB Serial/JTAG stream with the app link** (`Devices/Tamu_v2.0A/Log.h:15`
  + `AppUSB.h:109-119`): `ESP_LOGI`/`DeviceLog` from `Button.h`, `AccGyr.h`, `AppBLE.h`, `Main.h`
  can interleave into app frames on the shared TX FIFO, even though `RSBus.h:138-139` avoids
  `ESP_LOG` for exactly that reason. Gate on `!AppConnected`.
- **`LoadLayoutFromStorage` ignores flash-read failures and mutates state before validating**
  (`Blocks/Vysi1Layout.h:221-235`): an unchecked/failed read leaves state half-applied; check both
  reads and return false before mutating.
- **Geometry `Alpha` is not clamped** (`Blocks/Vysi1Render.h:83`): a >1 value wraps the mask
  modulo 256 though it is documented 0-1. Clamp with `LimitZeroToOne`.

**Tests**
- **Tests exercising retired opcodes** - `app/test/hil_script_vm_test.dart:318-334` ("type error
  halts" uses `catMath,1` "Add", retired per `ScriptDefs.h:63`), `backup_script_test.dart:26,30`
  (`catMath,1`, `catLogic,6`), `script_instructions_test.dart:12` (`catMath,1` while `:298-305`
  asserts it is gone). They pass but no longer test the intended path.
- **`render_dict_test.dart:21-28`** uses pre-renumbering geometry keys (Operation=0, Shape=1, ...)
  vs the documented 0=Dictionary, 1=Shape, 2=Operation ... - key-agnostic so it tests nothing real.
- **Stale Save/Recall CID comments** (`current_setup.dart:276`, `current_setup_apply.dart:98`,
  `hil_backup_test.dart:172`): say Save=4/Recall=3, actual Recall=4/Save=5. A host test could pin
  `RegisterCid` against the firmware enum.
- **`file_viewers_test.dart:9` / `membackup_view_test.dart:8`** cite renamed firmware files;
  `firmware/test/native/run.sh:8` cites the old `Crc8` location.
- **`test/README.md:32-36`** invocation is wrong (`run_hil_tests.sh` resolves paths relative to
  `app/`), and `run_hil_tests.sh:9` references a non-existent `test/hil_live_test.dart`.
- **DAS-discovery race** (`hil_subscriptions_test.dart:16-22`, `tamu_hardware_verification_test.dart:165-180`):
  rely on `connectTo`'s unawaited `refreshNetwork`, so tests silently `return` ("DAS not found")
  and lose coverage. Await `refreshNetwork()` like `hil_backup_test.dart:17-24`.
- **`connectHil` capabilities race** (`hil_helpers.dart:117-171`): waits only for a ping, so
  `StorageClient.fixedStorage` can cache `false` from `capabilities == 0`. Force a network refresh.
- **Hard-coded esptool paths + fixed sleeps, no BLE guard** (`hil_script_vm_test.dart:338-355,383-398`,
  `hil_current_setup_test.dart:514-535`): the advertised `TAMU_HIL=ble` path will fail. Reuse the
  `resetReason` guard, resolve the tool via env/`pio`, poll uptime.
- **Subscription test TRIDs use the App range** (`0xFA00` etc.) though the docs reserve
  `0x1000-0x1FFF`; use `0x1xxx` to exercise the reserved-range behaviour.

**Tooling**
- **`firmware/scripts/tamu_bootloader_env.py:9` leaks `IDF_COMPONENT_MANAGER=0` process-wide**:
  it mutates `os.environ` (PlatformIO's `ENV` is a direct reference), so
  `pio run -e Tamu_bootloader -e Tamu_v2_0A` disables the component manager for the main app too,
  which needs `h2zero/esp-nimble-cpp` - a clean multi-env build fails. Scope it to the bootloader
  env (e.g. per-env `cmake_extra_args`).
- **Firmware version hash is not target-independent** (`scripts/version.py:27,32`): both targets
  list the shared `firmware/platformio.ini`, so editing any unrelated env (or bootloader env)
  mints a new version for both. Hash only the target's `[env]` section / resolved flags.
- **The version hash omits inputs that change the image** (`version.py:26-33`):
  `firmware/src/CMakeLists.txt`, `firmware/CMakeLists.txt`, `partitions.csv`, `idf_component.yml`,
  `sdkconfig.Tamu_v2_0A`. Add them.
- **The app version stamp is never generated by the build** (`scripts/gen_app_version.py`,
  `app.sh:1-6`): run manually only, so `app_version.g.dart` goes stale whenever `app/lib` changes.
  Call it from `app.sh` and/or fail CI on drift.
- **`APP_BUILD_DATE` is never defined** (`app.sh:5`) - duplicate of the app-general finding; pass
  `--dart-define` or drop the row.
- **Date-reset only happens on content change** (`version.py:79-92`): the docstring/TODO say a date
  change resets the iteration, but `_mint` returns the stored version unchanged when the hash
  matches. Reword or implement a rollover.
- **Dead arch guard** (`firmware/src/CMakeLists.txt:15`): `MATCHES "xtensa"` is false for the
  RISC-V C3, so the "exclude DAS_v0.1 files" filter never runs (harmless while DAS is header-only).
  Key on `CONFIG_IDF_TARGET_ARCH`/an option.
- **Build mutates the tracked `version.json`** (`version.py:95-107`): every build dirties the tree
  (and parallel envs could race the read-modify-write). Treat it as a build output or lock it.

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

- **Script CID 0 lists "Script File IDs", not slots.** CID 1 takes a separate loaded id, so the
  two may differ - and CID 0's list is then not addressable: the caller cannot recover which slot
  holds which file. The docs were updated to **uint16 file ids** (SCR_XXX, 4096 files) with the
  slot still 6-bit (64 loaded); the app picks a free slot and tracks file->slot itself, and a
  script's block meta carries its function name (not its file id), so an untracked slot can only
  fall back to the file==slot convention. Reporting the loaded **slots** in CID 0 (or a slot in
  each list entry) would remove the guesswork - a docs decision.

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

Resolved in the TRID-range pass (2026-10-03, later):
- The reserved ranges are defined centrally (`TRID_*` in `Core/Functions/Packet.h`): System/Logs
  `0x0000-0x0FFF`, Subscriptions `0x1000-0x1FFF`, Scripts `0x2000-0x2FFF` (the docs leave the
  script range as "..."), App `0xF000-0xFFFF`. Replies echo the request's TRID
  (`FinalizeReply`), and the app allocates/matches on the full 16-bit App TRID.
- **System/Logs now uses an incrementing counter** (`NextSystemTrid`): the service type stays in
  the TRID's high byte so an echoed reply still routes to the service, the low byte is the
  counter, and the Device handler identifies its replies by payload. Discover/TimeSync verified
  on the rig (DAS synced to the core within 10 ms).
## Storage / DAS reduced filesystem
- **The reduced `.SV`'s fixed size is the whole storage region, not the persistent space.**
  `StorageFixedFS` declares `.SV` as `STORAGE_FLASH_SIZE` (256 B on the DAS), while
  `sizeof(staticPer)` is only 40 B, so the app's raw `.SV` view shows ~216 trailing `0xFF`
  bytes (the decoded backup view uses the layout and is correct). `Docs/Devices.md` says the
  DAS memory is **128 B** while the build uses `STORAGE_FLASH_SIZE=256` - decide the region
  size, and whether the fixed `.SV` should report only the persistent size (which would need
  the device to feed `sizeof(staticPer)` into the const file table, e.g. via a build flag
  guarded by a `static_assert`).
- **`.SUBREQ` on a node with no subscriptions** was reported by the user; the provider-only
  DAS cannot create it, and neither the core nor the app currently shows one. It is written
  only by requester set/cancel (`0x11`) or Save All (`0x13`); the empty-table file is now
  deleted (above). Re-check if it reappears.

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
- **DAS provider subscriptions could accumulate stale entries.** Effectively solved by the 120 s
  provider lease (renewed by the requester's keepalive) plus the orphan-cancel path (a value
  update whose TRID matches no requester entry is cancelled back); a missed cancel holds a slot
  for at most 120 s. Low priority.
