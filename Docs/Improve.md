# Improvements

Suggestions that are not bugs, but would bring the code closer to the docs or improve
robustness. From the service audit (2026-08-21), the follow-up pass (2026-08-22) and the
firmware code review (2026-08-22). Items already fixed are tracked in `Issues.md`
(Resolved) and removed here.

## Firmware code review (2026-08-22)

### Performance / footprint

- **`FindSpace` is O(blocks x capacity) flash reads** (`Core/Functions/Storage.h:283-329`,
  `576-591`): each candidate block calls `BlockUsed()`, which rescans and re-reads the whole
  file table from flash. Worst case (table move on a fragmented volume) is thousands of
  single-entry reads. Build a usage bitmap once per call (STORAGE_FLASH_SIZE / PAGE_SIZE bits
  = 16 bytes at 64 KB / 4 KB) and scan that.
- **`FindLowestAvailableID` rescans per candidate** (`Core/Functions/SNDB.h:287-313`): O(n^2)
  flash reads (~126 x 128 x 32 B worst case). One pass building a used-ID bitmask (64 B)
  fixes it.
- **`PacketConstruct` memsets the full ~264 B frame** (`Core/Functions/Packet.h:98`): only
  header + `payload_len` bytes are ever read (CRC covers 11 + len); clearing just the header
  fields removes ~250 B of memset per packet - relevant on the CH32V003.
- **Per-pixel pass-by-value in the renderer**: `CalculateShapeAlpha(GeometryDefinition def, ...)`
  copies a ~28-byte struct for every pixel of every geometry every frame
  (`Blocks/Vysi1Display.h:153`, `211`); `RenderGeometry`/`RenderTexture` copy `Matrix<3,3>`
  by value too. Pass by const ref. The `sq(P[0])` macro also evaluates `P[0]` (an
  `operator[]` call) twice.
- **Blend path ignores the cheap fast path** (`Blocks/Vysi1Display.h:350-377`):
  `Buffer[PIdx].Layer(blendCol, Overlay[PIdx])` runs even when `Overlay[PIdx] <= 0`
  (the `Full` branch at line 342 checks). Skip Layer when overlap is zero.
- **`SaveSystemBlockToFile` stacks two backup buffers**
  (`Core/Services/SystemMemory.h:216-270`): unlike `SaveRegistryBlock`, it holds
  `file_buf` + `out` (2 x MEMORY_BACKUP_CAP) simultaneously, and `HandleSystemMemory` is
  dispatched unconditionally (`Dispatcher.h:69-70`) - fatal on a 256-byte-stack node build if
  this service is ever targeted there. Reuse one buffer or guard the dispatch.
- **Redundant writable-count walks** (`SystemMemory.h:27-67` vs `107-135`):
  `SerializeSystemBlocks` re-walks each schema to count writable fields, duplicating
  `SerializeSystemBlock`'s body. Factor a `CountWritableFields(block)` helper used by all
  three sites.
- **`GetFileInfo` table walk per write-stream packet** (`Core/Services/Storage.h:19-31`):
  every stream write re-scans the whole file table just to clamp the offset. Cache
  offset/size at stream open (invalidate on resize/delete). Also: writes silently dropped at
  EOF leave the stream active forever - consider closing it or flagging overflow.
- **64-bit math not gated by NUMBER_ONLY_32BIT** (`Core/Types/Number.h`): `sin()`/`log()`
  use `(int64_t)a * b >> 16` and `sqrt()` works in uint64_t; if these ever link into the DAS
  image they pull libgcc 64-bit helpers despite the gate's purpose (~2.5 KB). Convert to
  MulHigh32/FixedMul32.
- **`Matrix<R,C>` stores runtime dims** (`Core/Types/Matrix.h:7-12`, `30-38`): a 4-byte
  `{height,width}` header per instance plus runtime multiplies in `operator()`, although the
  dims are template parameters. Pure overhead on the 2 KB-RAM node - and a corrupted embedded
  header enables OOB writes via `operator()` if matrices are ever deserialized with dims.

### Duplication / streamlining

- **Tamu and DAS RSBus are near-duplicates**: `ReceivePacket`, CSMA constants and echo-verify
  logic exist twice, differing only in transport primitives - and drift already produced the
  identical Crc8-length bug in both copies (see Issues). Extract a core transport layer
  parameterised by the send/recv primitives.
- **SNDB recovery prologue copy-pasted 7x**: `Available(); if (!recovered) { RecoverState();
  recovered = true; }` opens every public method; extract `EnsureRecovered()`. The same
  file's keyed-entry walkers (`GetKey/SetKey/ListKeys/RemoveKey` in `Functions/Memory.h`)
  should share one cursor-advance helper (also fixes the GetKey bounds gap uniformly).
- **Block-meta + name reply payload built three times**: the identical BlockIndex + BlockMeta
  + name construction appears in `DynamicMemory.h:497-508`, `KeyedMemory.h:74-85` and
  `SystemMemory.h:295-306`; the "field read with size clamp" tail repeats as often. Two shared
  helpers next to RespondCreate/RespondEcho would cover all of them.
- **Script service stubs ACK success** (`Core/Services/Script.h`): cases 1/4/5 respond success
  without doing anything and `default:` ACKs unknown CIDs - clients can't distinguish "done"
  from "not implemented". Return an explicit failure/not-implemented status until implemented.
- **ColourClass / Vysi1Display out-of-class definitions lack `inline`**
  (`Core/Types/Colour.h:25-92`, six `Vysi1Display::` methods): harmless in today's single-TU
  build, but an ODR / multiple-definition trap the moment anything splits into a second TU.
- **Header-local statics duplicated per TU**: `_next_rand` (`Number.h:329`) gives every
  translation unit its own RNG state generating the same sequence; `PI` (`Number.h:222`) is
  likewise duplicated. Use function-local statics.
- **Include-order-dependent Blocks**: `PWM.h`, `AccGyr.h`, `Button.h` have no include guards
  and don't include their dependencies (`BlockSchema`, `StaticBlockDescriptor`, `Number`);
  double inclusion or a different include order breaks the build. Add guards/includes like
  `DeviceInfo.h`/`Render.h`.
- **`ProcessBus` dispatches one frame per call** (`Dispatcher.h:127-136`) despite its comment
  claiming "every received packet" - loop or fix the comment.

### Minor / hygiene

- **PWM duty truncation order** (`Devices/Tamu_v2.0A/PWM.h:98`):
  `(duty / 100 * 1023) >> 16` truncates Q16.16 before scaling, losing up to ~1.5 LSB and
  rounding tiny duties to 0. Compute `((int32_t)duty * 1023) / (100 << 16)` instead.
- **Parameter named `Number` shadows the class** (`Number.h:353-356`, `373`): `LimitByte(int
  Number)` only compiles via implicit int->Number conversion through min/max macros; rename.
- **DeviceInfo hand-written copy ctor/assignment** (`Core/Types/DeviceInfo.h:15-37`) exactly
  replicate implicit behaviour and suppress trivial-copyability; delete them.
- **`Vector::insert` doesn't validate pos** (`Core/Types/Vector.h:57-69`): a bad index is a
  silent OOB stack write; a debug assert costs nothing.
- **`SerialNumberToString` has no buffer-size parameter** (`Functions/Device.h:13`) and the
  serial length 14 is hardcoded in SNDB's memcmp instead of a constant.
- **`ReportLog(LogMessage log)` takes its argument by value** (`Functions/Log.h:16`), copying
  the message before a second copy into the packet; take const&.
- **Dead code / stale comments**: unreachable `vTaskDelete(NULL)` after `while(1)`
  (`Tamu_v2.0A/Main.h:121`); unused `ADCRES` macro in both Base.h files; stale
  "Fix crashes / Implement measuring..." comment at the end of `DAS_v0.1/Main.h`;
  `GetFreeRAM`'s comment claims live stack pointer but returns the boot-time linker symbol
  (`DAS_v0.1/Base.h:66-73`).
- **Lock in the LEDButton padding**: add `static_assert(offsetof(LEDButtonStruct,
  ButtonState) == 4)` so the alignment fix can't silently regress.
- **`InverseTransform2D` divides without a degenerate check** (`Core/Types/Matrix.h:129-144`):
  a singular matrix silently yields an all-zero inverse; return bool or assert.

## Stack discipline (DAS, 256 B stack / 2 KB RAM)

- **Service handlers still allocate `PacketFrame`-sized locals** (270 B): `HandleStorageService`
  has both a `reply` frame and a 256 B `temp_buf`, `HandleDeviceService` a `reply` frame,
  `ReportLog` a `log_pkt`, `SendResponse` a `reply` frame, and `main` the discovery frame.
  Worst chains are now ~750-810 B vs ~1028 B of contiguous headroom (256 B stack + the
  never-used heap gap). No change is required today, but any new handler should reuse the
  received frame or a small scratch buffer rather than stack a second `PacketFrame`. If a
  heap is ever enabled on the DAS, move these to a shared static frame or raise `__stack_size`.
- **The heap gap is the only headroom**: `__stack_size = 256` (Link.ld) plus ~772 B of unused
  RAM between BSS and the stack absorbs overflow. Nothing mallocs on the DAS today (Dynamic/
  Keyed memory are not enabled), so this is safe; revisit if that changes.

## Documentation alignment

- **Capability bitfield**: define a CORE capability bit (and any future bits) in `Enums.h` and
  have the Tamu set it, so Device service `cap` reports something meaningful instead of 0.
- **System Memory block names**: doc says the block name is the `Name` type (16 chars / 12 bytes,
  `Data Formats.md`); the implementation uses 24-byte C strings (`BLOCK_NAME_LEN`). Either encode
  names as the Name type or update the System Memory doc to "plain string".
- **LED display block fields**: still missing the documented `Layout File Name` (Name type with
  callback) and `Refresh Rate` (Number, Out) fields (see Issues "On hold").

## Robustness / correctness

- **I2C bus needs internal pull-ups (Tamu v2.0A)**: the board has no external pull-ups on
  SDA4/SCL5; without `flags.enable_internal_pullup` the bus floats low and every transaction
  returns `ESP_OK` with all-zero data (looks exactly like a dead sensor). The IMU driver now
  enables them (plus `gpio_reset_pin` and `I2C_CLK_SRC_RC_FAST`, matching the previously
  working `I2C.h` reference). If other I2C peripherals are ever added, they must also enable
  the internal pull-ups (or the board needs real pull-up resistors).
- **IMU scale/units calibration**: `ReadIMUData` uses `/209` (accel) and `/939` (gyro) and the
  config bytes `{0x44, 0x4C}`, which reproduce the previously-working driver's output (~1 g
  reads ~10 on Z). The physical units vs the LSM6DS3-family datasheet sensitivities are not yet
  verified; calibrate against a known orientation/rotation.
- **`Number(int)` overload**: `int32_t` is `long` on the embedded toolchains, so a separate
  `Number(int)` ctor is required for unambiguous `int`/`uint16_t` calls, but the file does
  not compile on hosts where `int32_t == int`. Consider gating it with a feature check.
- **Storage**: after the table-rework, add a boot-time sanity check or a small self-test so a
  corrupt table is never reported as many valid files.
- **Per-stream FragID**: `NextFragmentId()` keeps one shared static counter on the core; two
  interleaved multi-packet streams would produce colliding/incorrect fragment numbers. Give each
  stream (or sender CID) its own counter.
- **Node packet desync**: `RecievePacket` consumes the 0xAA sync byte and then returns 0 on a
  partial/invalid header, so the frame is lost and the stream must re-sync on the next 0xAA.
  Consider buffering until a full frame is present instead of discarding.
- **DAS RX ring can be flooded by the node's own echoes**: `SendAndVerifyPacket`'s echo verify
  reads `total_tx_size` bytes straight out of the shared 256-byte RX ring, and
  `RS485_WaitForSilence` drains it. If a node transmits frequently (observed while debugging:
  5 log broadcasts/s), its own echoes pile up, the ISR's `next_head != tail` full-check drops
  real incoming frames, and `ReceivePacket` syncs on the node's own 0xAA echoes first. The bare
  DAS only transmits on request so this is latent, but a chatty node (telemetry logs, etc.)
  would deaden the bus. Options: flush the ring before the echo verify, restrict the echo
  verify to a short window, or give RX packets priority over echo bytes.
- **DAS `Now()` pattern (free-running SysTick)**: the CH32V003 SysTick free-runs *up* at HCLK
  with `CMP=0` (only `CTLR|=0x05` is set at boot - the SDK `Delay_Init` only computes
  constants). Time helpers must therefore accumulate deltas and carry the fractional remainder,
  never do a per-call `delta / (SystemCoreClock/1000)` (integer truncation freezes them in tight
  loops). `Base.h::Now()`/`Sleep()`/`SleepMicro()`/`RS485_Micros()` are consistent with this now;
  keep it in mind for any new timing code on the DAS.

## CLI (Tamu)

- Log Handler network commands (GetLogs CID 1 / ClearReadLogs CID 2) are implemented in the
  service but not reachable from the CLI. The local `logs` command already dumps the core's
  own RAM DB (fed by inbound CID 0 reports), so these network commands are only needed to
  query a *remote* core's DB; add `logs <addr>` if that use case appears.

## Device specifics

- **DAS flash budget**: at ~75.5 % flash with the SensorType switch, System Memory backup
  helpers and NUMBER_ONLY_32BIT compiled in. Watch this headroom if more DAS features are
  added; prefer 32-bit math (a single 64-bit division pulls ~2.5 KB of `__divdi3` helpers
  back in).
- **DAS sensor math**: the Raw Voltage / resistance conversions and auto-range thresholds still
  need calibration against the real sensor (see Issues "Needs hardware verification").
- **LED transmission disables interrupts**: `LEDDriver::Send` bit-bangs 86 WS2812 pixels with
  interrupts masked (~2.6 ms); a full RS485 frame arriving in that window overruns the UART
  ring. Consider DMA/SPI-based LED output if bus drops become a problem.

## Cleanup

- `src/Blocks/Vysi1Display.h` is the only "shared" block with an `esp_log.h` include and a
  `keyed_block_registry` dependency; either move it under `Devices/Tamu_v2.0A/` or gate it so
  the Blocks folder stays device-agnostic.
- Naming pass (2026-08-22): core helpers were renamed to PascalCase (`PacketConstruct`,
  `DispatchPacket`, `ReceivePacket`, `Crc8`, `MakeService`, `GetServiceType/GetServiceCID`,
  `AlignTo4`, `NumberToFloat`/`FloatToNumber`, descriptor methods `GetOffset`/`InsertField`/
  `AddBlock`/`GetBlock`/`RemoveBlock`/`Release`, block callbacks `OnLEDStateChange`/
  `OnPWMFrequencyChange`/`OnPWMDutyChange`/`OnAccGyrFrequencyChange`, CLI `Cmd*` handlers and
  device `SetupRS485`/`SetupFanPWM`/`InitLSM6DS3`/`ReadIMUData`). Shortened variable names in
  the memory/storage services were replaced with descriptive words (e.g. `reg`->`registry`,
  `buf`->`buffer`, `n`->`name_len`/`count`, `p`->`cursor`, `b`->`block`, `off`->`offset`).
  The `esp_console`/driver entry points (`app_main`, `ledc_*`, `i2c_*`) were left as-is.
## App (2026-08-22 rewrite)

- **Connection**: source selector (BLE/USB/All), refresh with autorefresh
  (1 s period, on by default, green/red status dot), signal/alphabetical
  sorting, connected-device row with disconnect, colour-coded RSSI.
- **Devices**: list + pannable graph views, net/type filters and ID/name/type
  sorting at the bottom, per-device icons; graph shows cores on top and nodes
  stacked below (router tree pending the Router service).
- **Device view**: rename in the app bar, device info card (type, SN, version,
  uptime, loop times), services list linking to the System Memory viewer.
- **System Memory view**: current/backup toggle, nested block/field lists with
  flags, tap-to-edit values (number/bool/int/string/hex fallback), per-entry and
  whole-memory Save/Recall, refresh dialog with selectable autorefresh interval.
- **Backup**: per-device selection, zip of per-device JSON files built from the
  System Memory services, live restore straight to the services with
  structure-compatibility checks (skips read-only fields).
- **Settings**: autoconnect, in-app notifications with per-event selection,
  OS-notification suppression flag and per-event selection, app version/build
  date. Persisted to `~/.config/tamuapp/settings.json` on Linux.

Suggested doc improvements:
- Specify USB CRC8 coverage and BLE GATT UUIDs in `Services/App Interface.md`
  once the firmware side is designed (tracked in Issues.md).
- Define the app's own network identity (ID SRC) in `App/General info.md`.
- Document the block-name string appended to System Memory block-meta reads.
