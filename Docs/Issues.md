# Issues

Status of items from the docs-vs-implementation audit and follow-up work.

## Resolved (code)

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
