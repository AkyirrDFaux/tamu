# Improvements

Suggestions that are not bugs, but would bring the code closer to the docs or improve
robustness. From the service audit (2026-08-21), the follow-up pass (2026-08-22) and the
firmware code review (2026-08-22). Items already fixed are tracked in `Issues.md`
(Resolved) and removed here.

## Firmware code review (2026-08-22)

Findings from this pass were fixed in the same session (both environments build clean;
details in `Issues.md` "Fixed (firmware code review)"). Implemented: FindSpace usage bitmap,
one-pass FindLowestAvailableID, header-only PacketConstruct init, const-ref renderer
params + sq double-evaluation fix, blend fast-path skip, SaveSystemBlockToFile buffers
(static above cap 128, stack below), CountWritableFields helper, write-stream file-info
caching + EOF auto-close, 32-bit-only sin/log math, SNDB EnsureRecovered + shared
KeyedEntryFits walker, MakeBlockMetaPayload reply helper, Script not-implemented statuses,
Colour/Vysi1Display inline, function-local RNG state + GetPI, include guards for
PWM/AccGyr/Button blocks, exact PWM duty math, parameter-shadow renames, SerialNumber
copy-ctor removal, Vector insert/remove index clamps, SerialNumberToString size param,
ReportLog const&, dead-code removal (vTaskDelete, ADCRES, stale DAS comment), real-SP
GetFreeRAM on the DAS, LEDButton static_assert, InverseTransform2D singular guard.

Deferred on purpose:

- **Tamu/DAS RSBus near-duplicate transport**: extracting a shared transport layer is an
  architectural refactor across both boards; deferred until the next bus-layer change.
- **`Matrix<R,C>` runtime dims header**: removing the `{height,width}` header would change
  the serialised Matrix layout stored in keyed/dynamic block data (and every saved backup),
  so it needs a migration decision first.
- **Blocks metadata offsets all 0x00**: verified harmless - `StaticBlockDescriptor::Get()`
  derives offsets from aligned schema sizes; the BlockMeta offset byte is unused there.

### Performance / footprint

- **`FindSpace` was O(blocks x capacity) flash reads** - FIXED: builds a per-call usage
  bitmap (`Core/Functions/Storage.h`).
- **`FindLowestAvailableID` rescanned per candidate** - FIXED: one pass marks used IDs in a
  bitset (`SNDB.h`).
- **`PacketConstruct` memset the full ~264 B frame** - FIXED: only header fields are written.
- **Per-pixel pass-by-value in the renderer** - FIXED: `CalculateShapeAlpha` takes the
  geometry by const ref; `RenderGeometry`/`RenderTexture` take transforms by const ref; the
  `sq(P[i])` double evaluation uses local copies.
- **Blend path ignored the cheap fast path** - FIXED: `Layer()` is skipped when overlap <= 0.
- **`SaveSystemBlockToFile` stacked two backup buffers** - FIXED: static above cap 128,
  stack locals on small-cap node builds.
- **Redundant writable-count walks** - FIXED: shared `CountWritableFields()` helper.
- **`GetFileInfo` table walk per write-stream packet** - FIXED: offset/size cached at stream
  open; streams also deactivate when they reach EOF.
- **64-bit math not gated by NUMBER_ONLY_32BIT** - FIXED for `sin()`/`log()`
  (FixedMul32 replacements); `sqrt()` needs uint64_t intermediates and carries a comment -
  keep it out of node images unless 64-bit helpers are already linked.
- **`Matrix<R,C>` stores runtime dims**: DEFERRED (see above).

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
- **Node packet desync**: the DAS `ReceivePacket` now keeps assembly state across polls
  (fixed); the Tamu side still consumes-and-discards on an incomplete header/payload - a
  frame split across two `ProcessBus()` polls loses its bytes there too. A persistent
  assembly buffer would fix the core as well.
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
- Cleanup candidates from review round 2: `CmdSave`/`CmdRecall` (Entry.h) are identical except
  for the CID - collapse into one helper taking the CID; the thirteen `esp_console_cmd_t`
  registrations could become a static table + loop; SN hex formatting/parsing is duplicated in
  CLI/Block.h and CLI/SNDB.h despite `SerialNumberToString`; WS2812 bit timing in `LED.h`
  matches spec only at 160 MHz and its comments' ns arithmetic doesn't match the NOP counts -
  derive cycles from `ets_get_cpu_frequency` or pin the assumption before anyone changes the
  clock.

## Device specifics

- **DAS flash budget**: at ~67.9 % flash (11,120 B of 16 KB) after the round-2 review
  (-flto, textless DeviceLog, table-driven range select, payload-verbatim echoes). Watch this
  headroom if more DAS features are added; prefer 32-bit math (a single 64-bit division pulls
  ~2.5 KB of `__divdi3` helpers back in).
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
