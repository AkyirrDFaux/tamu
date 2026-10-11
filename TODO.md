# TODO

Long-term plan (`Docs/Plan.md`): 1) Scripts, 2) blocks/modules + subscriptions, 3) app backup.

Host gate: `./test.sh` = the native numeric/geometry/CRC/stride/align tests (core, 32-bit and DAS
configs, both `OPTIMIZE_SPEED` states) + the app host suite + `flutter analyze`. Builds: export
`PLATFORMIO_CORE_DIR=~/.platformio`, build one env per `pio run` invocation, and delete
`.pio/build/<env>` after a platform change (the stale CMake cache reports `Source ... not found`).

HIL: one entry point, `bash app/test/run_hil_tests.sh`. It scans the bus, builds a rig inventory and
runs every test whose prerequisite the rig satisfies; an unmet prerequisite is a skip, never a
failure (spec: `Docs/HIL Tests.md`). Categories: generic, service, module and partner - plus
bootloader, which runs on its own (`--bootloader`) because it takes its device off the bus. `--scan`
lists the rig and stops; `--category <name>` and `--device <id>` narrow a run. Every run writes
`app/test/hil/logs/hil_<stamp>.log` (gitignored). Tests are destructive - they change blocks, files,
subscriptions and addresses - but clear their own temporaries. Source lives under `app/test/hil/`.

Serial ports renumber between sessions - resolve them from `/dev/serial/by-id` (Espressif USB-JTAG =
core, WCH-Link = DAS) rather than remembering a `/dev/ttyACM<N>` number. `run_hil_tests.sh` does
this itself; `TAMU_HIL` overrides it, or set it to `ble` for the BLE path. `TAMU_HIL_BOOTLOADER=1`
declares that a device is in bootloader mode for the bootloader category; the value is ignored, since
bootloader frames carry no address.

## Script system (2026-10-11) - implemented; three items open

The staged plan landed on both sides: the 24-byte `SCR_XXX` header with the Function reservations
block; the Function-library flag and the load-on-boot slot in Properties bits 8-15; the Service
instruction set (`Start`/`Stop`/`Pause`/`Resume`/`Error`/`Report info`, opcode 3 retired); the CID 3
report record; CID 5's fixed-size call stack plus variable RAM; the whole streamed CID 0 slot map
with a lowest-free load; and local/macro/global functions with link-at-load. The extended Log struct
- 32-bit source carrying the whole block index, 32-bit source-specific code - landed with it. Gate
green; HIL 48/48 at the time, 55/55 after the later waves.

Still open:

- **The per-instruction error-code catalogue** - the engine-level codes that report with instruction
  0, the codes for each instruction that can fault, and the result codes the external-facing
  instructions return. `Pow`'s failure detail is the only one settled.
- **The Valu's script flash headroom** - its 16-slot registry costs ~9 KB flash to restore the tick
  path (32 788 -> 42 176, 93.6 %), parked. See `Issues.md` E29.
- **Whether the editor pre-checks a script** against the device's free memory and slot count before
  upload, and how a memory-bound load failure surfaces (reporting belongs with the catalogue above).

## Beta window plan (2026-10-07) - Tamu core/DAS + app; Valu v2 parked

Targets Windows 11 + Android; **Windows is dropped for this window**, so Android is the only
delivered target. No hard date.

**Proposed acceptance criteria:** host gate exit 0 with zero failures; both firmware envs build with
no size regression (DAS <= 2028 B RAM, <= 13440 B flash); the Android APK installs and connects to
the rig; the update flow completes end to end on Android.

Open:

- **The update flow end to end on Android** (connect -> detect -> update app/bootloader) - the
  sequence the tester performs, and the last acceptance criterion. Tracked with `Issues.md` E20.
- **The app interface pass** (`Docs/App/**`), waiting on Akyirr to pick or amend the ranked fix
  order. The audit report lives at `~/.hermes/notes/tamu-ui-audit-2026-10-08.md`.
- **Phase 5 - optimization + final verification**: DAS headroom (~94 % flash / ~99 % RAM), a final
  gate run, and the release checklist.

## Open work

- [ ] **The DAS firmware still lacks the subscription-stream stride fix** (`SubscriptionsControl.h`).
      The core is flashed; the DAS is not, so the app's *provider*-table reads on a DAS are still
      wrong. Reflash the DAS app with it in update mode (`./upload.sh app das`).

- Router capability bit: reserve one for the router service so the documented capability can be set.
- App Active enum: add `WiFi` (`Enums.h:34-38`) when that work starts. E10 dropped `LegacyBt` from
  the documents, so it is no longer listed as a planned state.
- [ ] **Valu app works standalone but NOT when launched by the bootloader (open).** Verified
      2026-10-07: the identical app image, ISP-flashed at 0x0 (no bootloader), runs and enumerates
      as `1a86:6001 "Valu v2.0"` and reaches its main loop; launched from the bootloader
      (`((void(*)())0x3000)()` from `JumpToApp`) it never reaches `main` - the LED stays dark and no
      USB device appears. Bisected with raw-register LED blinks injected into the custom startup:
      the app's `.init` (its very first instruction) **does** run, so the jump lands, but the CRT
      does not finish. App-side attempts that did NOT fix it: (a) skipping the framework's
      `SystemInit` when the PLL is already up (`ValuClockInit` via the custom
      `startup_ch32v20x_D6.S`), (b) putting SYSCLK back on HSI before the jump (this one makes the
      *bootloader* hang on SWS). Leading suspects now: the CPU state the bootloader leaves (its
      `mtvec` points into the bootloader, so an early app fault vectors into bootloader code; CSRs
      `0xbc0`/`0x804`; a stale flash prefetch buffer). Next: try a hand-off that starts the app from
      a reset-equivalent state, or debug the app's CRT on hardware (WCH-Link/SWD).
      **This defect is what blocks on-board verification of the
      Valu app** (below): the app cannot be flashed until the bootloader can store it.
- [ ] **Valu v2.0 application built (`[env:Valu_v2_0]`, `Devices/Valu_v2.0/`) - on-board
      verification blocked.** The app links at `0x3000` in a 44 KiB window (`[0x3000,0xE000)`),
      brings up the USB CDC App Interface (same `0xFA..0xBF` link protocol as the core), the
      LED-Button (PA2), three pull-down buttons (PB13-15), one fan PWM (PA8/TIM1) and three
      resistive inputs (PA6/PA1/PA0), and runs the mandatory + Dynamic-memory + Script services
      over an 8 kB storage region at `0xE000`. It BUILDS (29 KB flash / 16.5 KB RAM); nothing is
      flashed on the rig (no hardware attached, and it still does not reach `main` when launched by
      the bootloader - above). Two unverifiable facts to
      settle on hardware: (a) the app's flash writes run at the framework's 144 MHz HCLK, where
      `CH32VRM` RM 32.1 note 2 recommends <= 120 MHz / a <= 60 MHz flash access clock - the
      bootloader's flash path has the same open question and no mitigation; (b) the USB bring-up
      recipe is copied from the bootloader (proven there), but the app re-enumerates as its own
      CDC device and that has not been confirmed. The three resistive channels use a placeholder
      reference resistor (Docs says "not defined") - see `Issues.md`.
- [ ] **Valu v2.0 layout and USB facts (recorded so they need not be rediscovered).** Bootloader
      region is **12 KB at flash 0x0** - the 8 KB the design started from overflows by 2028 B once
      a working USB stack is linked - app at **0x3000**, storage reserved from **0xE000**. The
      board's USB is the **FSDEV/USBD controller on roothub port 0**; the USBFS/OTG path (port 1)
      is a dead end here, `USBPRE` must be /3 for the 144 MHz clock, and the D+ pull-up has to be
      asserted explicitly (`EXTEN->EXTEN_CTR |= EXTEN_USBD_PU_EN`) - without it nothing appears on
      the bus even with everything else right. First-time flashing is over the CH32V203 **ROM
      bootloader** with `wchisp-nightly` (the v0.3.0 release has no `CH32V203G8R6` in its chip
      table) and **one command per USB reset** (upstream ch32-rs/wchisp#22; a USB port reset
      substitutes for a replug). The bootloader cannot rewrite itself - `HandleWrite` refuses
      anything below `APP_BASE` by design - so every bootloader change costs an ISP session, and
      the board has no SWD header to fall back on.
- [ ] **D3 - per-field geometry-mask versioning.** A write to an eye block bumps the block
      generation and the renderer recomputes all 9 masks; the panel is at its cap (~127-132 FPS),
      so not urgent. Needs a per-field invalidation token: a per-field version array, or comparing
      the cached geometry inputs each frame and recomputing only the changed fields (host-checkable).
      **Deferred to the display/rig batch.**
- [ ] **D5 - cross-script macro calls** (= `Issues.md` E4). `Script.md` names "Macro call" but gives no
      opcode/boundary/argument rules. The VM runs one script per tick and wait state lives on the
      callee, so it needs a `(script, line)` call stack and a tick loop that resumes the waiting
      script. Proposal: a blocking `Call script` op, values via registers.
      **Blocked on a docs decision.**
- [ ] **Rig looks.** `Polygon`/`Star` after the `atan2` fix; a rounded `Square`/`Rectangle`
      (eye-only; the evaluation scene sets no `Rounding`). Needs the display rig.
- [ ] **D4 - confirm the LED brightness-cap value** by eye. The layout brightness limit (178 = 70 %)
      maps the register's 0-100 % linearly onto the physical ceiling (`Vysi1Render.h`); confirm the
      top of the E15 curve looks right at the ceiling.
- [ ] **DAS provider stale entries** (= `Issues.md` E23; low priority). Effectively solved by the 120 s
      provider lease + orphan re-cancel; revisit only if a *confirmed* cancel is wanted.

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
  `0x2000-0xEFFF` (slot), App `0xF000-0xFFFF` (slot). Replies echo the request's TRID.
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
- **LED strips can brown out the board**; the builder clamps the displays to 7 % and the brightness
  script caps at 100 %, but the register's 0-100 % maps **linearly** onto the layout file's
  brightness limit (178 ≈ 70 %, `Vysi1Render.h`), which is the physical ceiling.
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
  **0x3E00**), so reflashing preserves the `.SV`; the erased-mirror handling makes a reflash recover.
- **`.SUBREQ` is removed when the requester table empties** (`SaveRequesterTable` deletes it at
  count 0).
- **Backup zips** are semantic format 2, one JSON per device (no manifest); entries are built from
  UTF-8 bytes. Large files are skipped above 128 kB by default.

- **Android builds.** All distributable APKs are built on Akyirr's machine; a build made here signs
  with a different debug key, so one must never be installed over an existing install or handed to
  anyone (agent builds are verification only). Java must be 21 - AGP 8.11 rejects Studio's JBR 25.
  `app/build/` holds Akyirr's artifacts, so an agent build fails at the copy step
  (`Could not set file mode 664`). Flutter 3.44 warns that `build.gradle.kts` and three plugins
  apply the Kotlin Gradle Plugin; a future Flutter will fail on that.
