# Issues

## Open

- **Data Formats.md line 30**: last row uses `SRC` but the real field is `SRV` (8-bit service + 8-bit custom ID). Fix: rename `SRC` to `SRV`.
- **App Interface.md line 20**: says `(MTU - 2) bytes max` but real overhead is 3 (ATT) + 2 (length prefix) = 5, leaving **MTU - 5** bytes per notification. Line 24 already says this correctly.
- **Devices.md line 16**: `TODO (Script)` is stale — scripts are fully implemented. Fix: replace with `Scripts`.
- **GammaTable has only 240 entries** (`Vysi1Display.h`): indices 240-255 are zero, snapping bright pixels to black. Needs full 256-entry gamma table.
- **Tamu serial number carries only 48 real bits**: bytes 6-13 of the 14-byte SN are always zero (MAC fills bytes 0-5). Confirm whether docs promise full 14-byte uniqueness; if so, pad from eFuse.

## Needs Hardware Verification

- **BLE HIL back-to-back flakiness**: rapid connect/disconnect cycles intermittently drop a session ("Disconnected"). Isolated user-facing flows pass reliably. USB HIL 8/8 every time.
- **Fast page erase restores 0xFF**: the fix (using `FLASH_ErasePage_Fast` per 64-B block) was verified on hardware but the pre-analysis "fast erase leaves 0x00" predates the correct unlock sequence. Retest recipe: create -> verify table -> dump region -> resize grow -> delete.
- **Measurement calibration**: NTC/LDR/auto-range thresholds, IMU scales need tuning against known references. Values are board-calibrated, not physical units.
- **Full App Interface E2E**: USB CLI -> attach app mid-session (priority switch), detach on unplug back to CLI; USB and BLE sessions each running memory/storage/log reads; BLE MTU negotiation chunk sizes.

## Resolved (code)

**Packet/protocol**: New packet type implemented end-to-end; header byte 2 = Priority (128 default); payload len in 4-byte units (max 69 = 276 B); frame max 288 B. FRAG: first 4 payload bytes = `u16 current + u16 total` fragments; non-last carry 256-B actual payload. `MAX_PAYLOAD_SIZE = 276` everywhere; DAS old `-D MAX_PAYLOAD_SIZE=128` removed.

**Service IDs aligned** to `Docs/Service ID table.md`: Bootloader=0, Device=1, LogHandler=2, Storage=4, SystemMemory=5, DynamicMemory=6, KeyedMemory=7, Script=8, ScriptInstructions=9, Router=16, App=17, CLI=18.

**FRAG fragment cap**: `MAX_FRAG_CONTENT_SIZE = 256` added; Storage/Script FRAG paths use this constant.

**DAS RAM/stack fixes**: PacketFrame on stack overflowed CH32V003 RAM/stack; `SendAndVerifyPacket` rewritten as byte-stream send (<100 B stack). `MEMORY_BACKUP_CAP` 256->64. DAS stack 1024->576. Stack-allocated PacketFrame instances replaced with single global `tx_frame` (288 B static). RX buffer 320->592 (2-packet capacity). RAM 98% (2008/2048 B), Flash 77.4% (12680/16384 B).

**RS-485 large frames**: Core WS2812 LED bit-bang masked UART RX ISR for ~6 ms/loop, overflowing the 128-B FIFO on large frames. Fixed: LED yields every 48 bits (15 µs gap, under WS2812 reset threshold); UART RX FIFO-full threshold lowered to 4 bytes. 192-B+ frames now arrive 100%.

**Keyed Memory CID 7** (batched dictionary read): removed from firmware, app client, app UI, and tests — never specified in `Docs/Services/Keyed Memory.md`. Entries loaded via per-key CID 2 reads.

**None-typed placeholders**: `DataType::None = 0x00` (tombstone/spacer), `Undefined = 0x0E` (valid unspecified). Deletion marks `FlagsAndType` type=None IN PLACE — no index shift until Purge-on-save. `EnsureCapacity` shrink bug fixed (signed check).

**Dynamic/Keyed entry delete**: CID 1 with field removes just that entry (not whole block). Keyed delete works at dict/key level via `MarkKey`. Deleted entries read back as None.

**SetKey tail-shift bug fixed**: moved whole block tail, not just current field's tail. Dicts beyond field 0 no longer corrupted.

**Storage**: File records 16 bytes (Offset 32b + Size 32b + Name 8 chars). Table is a file with flexible size; `MoveFiletable()` grows/shrinks by one page. In-place record model (append-only, no compaction). `FindSpace` no longer allocates wrapped runs. `ResizeFile` grow-in-place reserves pending pages. `BlocksForSize()` saturates. `ReadTablePointer`/`WriteTablePointer` scan one slot at a time. `FreeRegistry` frees descriptor array. SNDB migrated to storage file system (`SNREG`). `WriteTable` streams entries on table move.

**Static block fields 4-byte aligned**: `StaticBlockDescriptor::Get` now aligns to 4 B like dynamic/keyed descriptors. LEDButton `LEDState`@0, `ButtonState`@4 (padded).

**LED button**: active-LOW read (`!PinRead`); LED OFF configures pull-up input; persisted LEDState re-applies at boot; `CommLed()` removed (pin contention). Pin high-Z while off = pull-up idle = button readable.

**BLE**: advertisement carries name (scan response enabled before `setName`). Writes parsed with length-prefix stripped. Notifications filtered by correct UUID. Preferred ATT MTU 512; connection parameters 7.5-15 ms. Pacing removed (natural throttle). Boot advertising deferred to `AppBLETick`. Advertising watchdog: rebuild only on evidence (3 s prompt + 120 s last-resort). Post-upload hard reset required (NimBLE stale state). `BleTransport.connect()` retries on transient ATT errors.

**App Interface**: USB frames `START|CRC8|Length|Payload|STOP` (START no longer overwritten). TX ring 2048->8192 B. RX queue 6->12 depth. App session has USB priority; CR/LF outside frame reverts to CLI. `AppRxStream` stamps `s_usb_last_rx`; TX pump skips USB drain when BLE up + USB silent 500 ms. USB exclusive mode (CLI vs APP). `libserialport` xon_xoff init + config retry + port-handle leak fixed. Control lines held for session.

**Time sync**: device-initiated after ID assignment (DAS computes offset locally). NTP formula: `offset = ((t1-t0)+(t2-t3))/2` with t0/t3 on app clock. `TimeOffsetMs -= theta` (accumulates). Wrap-safe: signed `(int32_t)` deltas. `Now()` returns synchronized time; `TimeFromBoot()` returns raw ms. `TimeUpdate` primes on first call.

**SNDB**: `FindLowestAvailableID` bounds checked. Entries with ID 0 skipped in walk. Compact recovery from temp file on any crash window. AccGyr ODR verified against `cmd1[1]`/`cmd2[1]`.

**Storage writes**: `FindSpace` allocates non-wrapped runs. `ResizeFile` grow reserves pending pages. Write stream at EOF rejected. `FileExists` returns size or 0xFFFFFFFF. `DeleteFile` returns empty payload on success.

**Memory views**: Auto-refresh starts at launch (0.5 s). Keyed page preserves open dicts/entries across refresh. System/Dynamic/Keyed pages reload expanded blocks on refresh. Backup CID 4 reads implemented. Backup toggle switch works.

**Dynamic/Keyed memory**: Append fills first None placeholder. Arbitrary gap indexes padded with None. None rows clearly labelled (non-editable). Keyed dictionary type settable in app.

**Script**: Implemented end-to-end. Manager CIDs 0-17 + CIDs 64+. Instruction set/VM with 4-byte symbols. File name `SCR` + 3-digit id. Live preview in editor. Expandable rows with live input controls. Symbol-based instruction editor. Input dictionary with interaction styles. MacroCall, Pause/Resume/Terminate/Restart. `MemWrite` sets `ScriptUpdated` on dynamic/keyed targets. Fresh scripts start blank (file created on first save).

**CLI**: `ParseService` accepts hex. Reply parsers bounds-checked. Matrix print guarded. SNDB failures printed. Timeout flag on response handlers. `dev <addr> discover` works. Short string writes work. Field/meta replies validated against payload length.

**Log DB**: Heap-allocated, grows (initial 32, +16 per growth, cap 512). `ClearReadLogs` clears N most recent by sequence. GetLogs/ClearReadLogs reachable over bus (CLI `logget`/`logclear`). Dead slots reused.

**Device service**: Nine reply CIDs deduplicated into `SendDeviceReply`. Replies use `tx_frame`. CID 11 time sync uses `TimeFromBoot()` (not `UptimeMs`). `int64_t` eliminated from non-core CID 11. `Response`/`SendResponse` marked `noinline`.

**In-app notifications**: `notifyAppEvent` shows SnackBar when settings allow. Events: device discovered/lost, backup finished.

**Misc firmware**: `ProcessBus` runs continuously (non-blocking LED). Auto-range hysteresis thresholds. LDR/NTC conversions normalize to 10k reference. `LoadAllBackups` works on all devices (not just core). `OnVysi1FieldWrite` loads through temporary (revert on failure). `SerialNumber` ctor uses shared `PackNameBytes`. `BinarySearch` template added (SNDB). AccGyr filter clamped [0,1]. OOM rollback in `CopyBlockInto`/`DeserializeRegistry`. REQACK failures answered. `PercentToByte` scales before truncation. Vysi1 buffer cleared on render. `CalculateShapeAlpha` renders n-gon/star. Textures compose `Local*Base`. Single `StorageReply()` helper in Storage.h.

**Misc app**: Backup restore uses `file.bytes`. Keyed next-key clamped 0xFF. Storage table preview reads full size. `refreshNetwork()` coalesces concurrent calls. Serial-number editor regex-validated. Autoconnect help text matches behavior. Refresh while connected re-pulls network. `refresh()` paused during `_connecting`. SNDB ID 0 skipped. Vector editor follows value length. Signed values decoded properly. String limits from wire format. Memory clients consolidated (`MemoryClientBase`). Save/Recall success uses status byte. Time-offset estimate uses NTP formula. Autorefresh menu dismissal preserves setting. `ConnectionManager.isRefreshing` removed. `FormatUptimeMs`/`FormatOffsetMs` in widgets.dart.

**Doc alignment**: Device-initiated time sync matches `Device service.md`. `FieldFlags::RemoteOrigin` deprecated. Keyed Memory CID 7 removed. `script_editor_page.dart` output/variable delete guarded. `connection_page.dart` uses `byId(coreId)`. Device name persisted to standalone file (`DEVNAME`). BLE HIL `ble_switch_test.dart` deleted. Test tagging system (`hil`, `ble` tags in `dart_test.yaml`).
