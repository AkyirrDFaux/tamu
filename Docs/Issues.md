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
- **DAS storage flash fast-erase left cells at 0x00 (fixed)**: `Storage_FlashErase` used
  `FLASH_ErasePage_Fast` (which leaves cells at 0x00 on the CH32V003) then "restored" 0xFF by
  programming 0xFFFFFFFF - which does nothing, since flash can only program 1->0. Every erased
  block ended up 0x00 (observed in the flash dump), corrupting the filesystem's erased-state
  expectations. Replaced with the bounded normal `FLASH_ErasePage` (256 B pages, 0xFF result).
- **DAS now verified working on hardware**: discovery (ID 2), ping, type/sn/version, System
  Memory block reads/writes, and the resistive measurement loop (ADC sample + auto-range +
  kOhm conversion) all respond over RSBus through the Tamu. `Meas1/Meas2` report ~1022 raw
  (open input -> 330 kOhm auto-range, ~13750 kOhm computed) as expected with nothing wired.

## Open (needs root cause, real hardware 2026-08-22)

- **DAS storage `create`/`resize`/`delete` hang the DAS.** `file 2 create <name> <size>` (and
  `resize`/`delete`) make the CH32V003 stop responding to anything on the bus until re-flashed.
  `save 2 s -` (System Memory backup) and `file 2 table` work, so the flash controller and table
  reads are fine; the failure is in `CreateFile`/`WriteTable`. Bisection on hardware: the request
  is received and dispatched (Storage service, CID 2), then the DAS hangs inside
  `AllocateContiguousBlocks`/the write path; a stack overflow is unlikely (the measured ~850 B
  chain fits the 256 B stack + ~1 KB RAM headroom, and enlarging the stack did not help). The DAS
  fast-erase was also replaced with the bounded normal erase because the fast erase leaves cells
  at 0x00 (flash can only program 1->0, so "re-programming 0xFFFFFFFF" cannot restore the erase
  state) - this is correct but did not resolve the hang.

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

- **Capability bitfield is empty**: both devices define `kCapabilities = 0` while the Tamu is a
  core (TYPE_CORE). The Capability function (CID 5) reports no capability at all. Either define
  capability bits (e.g. a CORE bit) or document that TYPE_CORE is the sole core marker.
  (Left open per decision to defer the capability bitfield.)

### Cleanup / dead code

- `src/Blocks/Vysi1Display.h` includes `esp_log.h` and uses `keyed_block_registry` (TYPE_CORE) —
  the shared Blocks folder therefore contains ESP32/core-only code usable only on the Tamu.
  (RSTest.h, VysiTest.h, `_todo/*`, `LEDDisplay.h` and the `[env:Valu_v2_0]` build were removed.)

## Needs hardware verification
- **Measurement calibration**: `Measuring_Update` reports resistance in kOhm (so 330 kOhm fits
  Q16.16) and uses a simple auto-range heuristic; divider constants and thresholds need tuning
  against the real sensor.
- **IMU scale/units calibration**: the accel/gyro work on hardware now; a stationary unit reads
  ~1 g as Z ~= 10.28 with the `/209` divisor. The config bytes `{0x44, 0x4C}` and the
  `/209`/`/939` scale factors are identical to the previously-working reference driver, but the
  resulting physical units vs the LSM6DS3-family datasheet sensitivities deserve a calibration
  pass against a known reference.

## On hold
- **LED display block**: the renderer is wired to the hardware, but the documented Layout File
  Name + Refresh Rate fields are still missing.

## Open (firmware code review, 2026-08-22)

Full pass over `tamu/src` (Core, Blocks, Devices). Findings verified against source;
prior audit results are not repeated here.

### Bugs — LED / display

- **GammaTable has only 240 entries** (`Blocks/Vysi1Display.h:9-24`): the initializer list
  ends at index 239 (`..., 198, 199, 200`), so entries 240-255 are zero-initialized and any
  channel value ≥ 240 snaps to black - the brightest pixels go dark. The table also caps at
  200 instead of ~255, so it is not a valid gamma-1.8 curve even for indices it covers.
  Regenerate the full 256-entry table.
- **DoubleParabola reads uninitialized geometry data**: `ResolveGeometryDefinition()`
  (`Vysi1Display.h:143-149`) has no case for `Geometries::DoubleParabola`, but
  `CalculateShapeAlpha()` reads `def.Data.Basic.Width/Height/EdgeFade` for it
  (`Render.h:193-196`). Those union members are never initialized, so the rasterizer reads
  indeterminate memory. Either add the resolution case or reject the geometry.
- **Negative Brightness wraps** (`Vysi1Display.h:106`): `(Brightness * 255 / 100).ToInt()`
  converted to `uint32_t brightness_scale` turns a negative field value into a huge scale,
  producing garbage colours through `(scale * alpha) >> 8`. Clamp Brightness to [0, 100].

### Bugs — packet / bus

- **Crc8 length truncates mod 256**: `Crc8(const uint8_t*, uint8_t len)`
  (`Core/Functions/Packet.h:46`) is called with `11 + payload_len` (up to 266) at
  `Packet.h:114`, `Packet.h:151`, `Tamu_v2.0A/RSBus.h:86`, `Tamu_v2.0A/RSBus.h:159`,
  `DAS_v0.1/RSBus.h:218`. For any frame with `payload_len >= 245` the length wraps and the
  CRC silently covers only a few payload bytes - integrity protection vanishes exactly for
  the largest (file-transfer) frames. Widen `Crc8`'s length parameter to `uint16_t`.
- **DAS RX ISR tests the wrong constant**: `USART1->STATR & USART_IT_RXNE`
  (`DAS_v0.1/RSBus.h:19`) uses the interrupt-*config* encoding (0x0525) as a status mask; it
  matches PE/NE/LBD/RXNE simultaneously, so error bytes are pushed into the RX ring instead
  of discarded. Use `USART_FLAG_RXNE` (0x0020). Related no-op: `STATR &= ~(ORE|FE|NE)` at
  lines 34-36 cannot clear rc_w0 flags (harmless only because the DATAR read clears them).
- **Storage table read drops FLAG_STOP** (`Core/Services/Storage.h:47-61`, CID 0): when a
  `ReadFileEntry` fails mid-table the loop `continue`s without incrementing `sent`, so no
  packet ever carries FLAG_STOP and the requester's stream reassembly stalls. Count the
  entry as sent (or track "last emitted" separately) so the stream terminates.
- **Storage file read with num_bytes == 0 sends nothing** (`Core/Services/Storage.h:113-153`,
  CID 5): a valid name/offset but zero length skips the chunk loop entirely - neither data
  nor an empty START|STOP ack is sent, hanging the client.

### Bugs — storage / SNDB

- **SNDB erased-slot pattern is wrong** (`Core/Functions/SNDB.h:115`):
  `static const uint8_t ff[sizeof(RegistryEntry)] = {0xFF};` sets only `ff[0]`; the rest is
  zero-filled, so the memcmp looks for `FF 00 00...` and never matches a real erased slot.
  `RecoverState()` therefore never finds the append head, `write_head` stays at its default
  end-of-file value, and the first `AddDevice` after every boot triggers a pointless
  `Compact()`. Fix the initializer.
- **SNDB Compact is non-atomic** (`Core/Functions/SNDB.h:265-275`): the registry file is
  deleted and recreated before any valid entry is rewritten; power loss mid-compaction loses
  the whole registry. Write a fresh temp file completely, then swap.
- **Table move can land on pending file data** (`Core/Functions/Storage.h`, CreateFile /
  ResizeFile copy path): the data area is chosen via `FindSpace()` and erased, but the
  committing record isn't written until `WriteFilerecord()`; if that call finds the table
  full, `MoveFiletable()` → `FindSpace(new_table_size)` does not see the uncommitted area
  and can place the new file table exactly over the freshly erased file data. Reserve the
  pending allocation during the move (edge case, but real corruption).
- **Keyed/dynamic field delete always reports success**: if the field index is out of range
  (`DynamicMemory.h:548-553`) or `RemoveKey` fails (`KeyedMemory.h:165-170`), the handler
  still answers status OK. Reflect the actual result, matching the Set paths.
- **GetKey walks dictionary entries without bounds checks** (`Core/Functions/Memory.h:308-322`):
  unlike `SetKey` and `ListKeys`, one corrupt/truncated entry makes the cursor advance past
  the field and return a pointer into foreign memory. Share one bounds-checked cursor helper.

### Bugs — dynamic memory registry

- **CopyBlockInto leaves stale pointers for empty blocks**
  (`Core/Services/DynamicMemory.h:206-225`): when `source.map_count == 0` or
  `source.length == 0`, the malloc branches are skipped and the destination slot keeps
  whatever descriptors the shift loop / `AddBlock` left there - aliased with the shifted
  copy at `index+1` or dangling from a previously released block. A later Remove/Release
  double-frees them. Zero `map/data_ptr/map_count/length` unconditionally before the
  allocation branches.
- **First per-block Save fails while the backup file doesn't exist**
  (`Core/Services/DynamicMemory.h:240-258`): with `ReadBackupFile == 0`, `temp_registry` is
  empty and `CopyBlockInto(temp_registry, block > 0, ...)` rejects `index > block_count`,
  so saving block N fails until block 0 was saved once. Append unconditionally when the
  file is absent.

### Bugs — DAS node

- **ProcessBus runs only ~once per second** (`Devices/DAS_v0.1/Main.h:118-122`): the LED
  blink blocks with two `Sleep(500)` per loop iteration, so incoming requests sit in (or
  overflow) the 256-byte ring at 115200 baud and replies lag up to a second. Make the blink
  non-blocking (millis-based toggle).
- **Auto-range switch corrupts the measurement filter** (`Devices/DAS_v0.1/Measuring.h:145-153`):
  the low-pass filter keeps integrating across range switches even though the excitation
  circuit changed scale, so MeasuredValue/CurrentRange stay wrong for many samples after
  each switch; range thresholds also use the *unfiltered* sample, so noisy inputs chatter
  between ranges. Reset the filter state on transition and threshold on the filtered value.
- **Meas2.SamplingRate is dead configuration** (`Devices/DAS_v0.1/Main.h:107-116`): the
  sample interval derives solely from Meas1; both channels are sampled at Meas1's rate.

### Bugs — CLI / misc

- **CLI matrix 'T' mode crashes on short input** (`Devices/Tamu_v2.0A/CLI/Block.h:254-262`):
  five unchecked `atof(strtok(nullptr, ","))` calls; input like `matrix T,1` makes strtok
  return NULL and `atof(NULL)` dereferences it. The 'R' branch already checks - do the same.
- **AccGyr filter divide-by-zero**: `1 / (1 + AccGyr.AccFilter)`
  (`Devices/Tamu_v2.0A/AccGyr.h:156-159`) divides by zero if the remotely-writable field is
  set to -1. Clamp on write or before use.
- **No allocator-failure handling in log DB init**: `EnsureLogStorage`
  (`Core/Functions/Log.h:46-53`) ignores malloc/calloc failure; `LogHandler.h:20` checks
  `LogBuffer` but not `LogUsed` (a failed calloc would null-deref at line 32).

### Uncertain / needs confirmation

- **LED-off leaves button line floating**: turning the LED off does `PinHigh` +
  `PinModeInput` (`Devices/Tamu_v2.0A/Button.h:14-17`); `gpio_reset_pin` inside strips pull
  config. If the board has no external bias, `PinModeInputPullDown` (exists, unused) seems
  intended.
- **Blocking loops without timeout**: `RS485_WaitForSilence` (both RSBus.h files) and
  `Meas_AdcRead`'s EOC wait spin forever on a wedged/shorted bus or stuck ADC.
- **Blocks metadata offsets all 0x00**: `Blocks/PWM.h`, `AccGyr.h`, `Vysi1Display.h`
  BlockMeta tables list offset 0x00 for every field, contradicting the actual struct layout
  (`PWMStruct.Duty` is at offset 4). Harmless today only if nothing addresses fields by these
  offsets - verify and fix or document.
- **Keyed field Size is uint8_t**: accumulating dictionary entries past 255 total bytes
  wraps silently (`Memory.h:383-384`). Add an explicit cap.

## Open (app rewrite, 2026-08-22)

The Flutter app (`/tamuapp`) was fully rewritten against `Docs/App/*.md`. The old
object/message-manager code did not match the documented layout (Connection /
Devices / Backup / Settings) or the packet protocol. The new app builds and
passes analysis for Linux desktop (BLE + USB). Doc gaps discovered during the
rewrite:

- **App Interface: USB CRC8 coverage is undefined.** The doc gives the frame
  layout (`0xFA | CRC8 | Length | Payload | 0xBF`) but not which bytes the CRC8
  covers. The app assumes CRC8 over Length + Payload. The firmware side of the
  App Interface is not implemented yet; when it is, both sides must agree here.
- **App Interface: BLE GATT UUIDs are undocumented.** No service/characteristic
  UUIDs are specified for the BLE packet pipe. The app uses placeholders in
  `tamuapp/lib/core/ble_transport.dart` (`appServiceUuid`/`appWriteCharUuid`/
  `appNotifyCharUuid`) that the firmware must match.
- **App source ID is undefined.** Docs define device IDs but not what ID the app
  itself should use as ID SRC. The app uses a fixed net-15 address `0xFFFE`
  (`protocol.dart appSourceId`), chosen to never collide with assigned devices.
- **Data Formats.md broadcast value inconsistency**: "ID - 16bit" but broadcast
  is written as `0xFFFFFFFF`; firmware uses `0xFFFF`. App follows the firmware.
- **System Memory block-meta read returns the block name appended after
  BlockMeta** (firmware behaviour, used by the app) - this extra name string is
  not mentioned in `Docs/Services/System Memory.md`.
