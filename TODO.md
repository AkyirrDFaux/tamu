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

## Bootloader protocol v2 (2026-10-10) - implemented and verified on hardware

The bootloader wire changed (3-bit command field, write acknowledgement, identify request/response) and
the core passthrough became asynchronous with remapped IDs (0x20 identification / 0x21 write / 0x22 read).
Spec: `Docs/Services/Bootloader.md`; partition sizes are in `Docs/Devices.md`. Landed and committed - see
`Issues.md` "Completed 2026-10-09/10" for the commit list.

**Outstanding:** both bootloader paths are now exercised on hardware (2026-10-10). The *core's own* bootloader takes the app image end to end: `test/core_bootloader_flash.py` identifies it (app space 2752512, name "Tamu v2.0A"), writes `ota_0` through the adaptive ack-driven pipeline (20496 chunks, 38.2 KiB/s, 0 resends) and reads the whole image back with 0/20496 mismatches. The *node* bootloader is reachable over the core passthrough: with the DAS in update mode, identify, whole-image read and rewrite+verify all pass (`TAMU_HIL_BOOTLOADER=1 bash test/run_hil_tests.sh --bootloader`, 3/3, 6-8 ms per request). Still unrun: the app's update flow end to end on Android, which the beta criteria below require.

## Script system improvements (2026-10-11) - planning

Goal: round out the script system - the registry/block representation, reusable functions (macros), loading
and unloading, error reporting, and the instruction set with its documentation. A read-only review of the
firmware VM, the app's editor and their tests is the basis below.

Stages in order: **0** fixes needing no design decision (below; settled, not started); **1** error reporting -
load and run failures carry state, code, the position of the failing instruction and a log entry, which is the
critical path for everything after it; **2** the script block and the load contract (below); **3** reusable
functions - local (defined in the file) and global (a stored library),
expanded at assemble time in the app, so the VM needs no calling convention until the line limit
actually bites; **4** loading/unloading - atomic activation, explicit slot with a lowest-free convenience,
Unload that reports failure, and settling the 256-slot vs the Valu's 16; **5** instruction set - implement or
strike the doc's promised restart/report-info/halt-on-error state ops, implement or remove the shift ops the
app already offers, and give `Pow` failures and the `Get time` integer-only rule a defined outcome; **6**
documentation - make `Script.md` normative for the opcode/category/predefine and error-code enumerations,
the 4-byte value packing it leaves unstated, and the CID 3/4/6/7 "script id = loaded slot" clarification.

### Stage 0 - fixes needing no design decision (settled, not started)

- App: validate block end matching - an `If`/`While` with no `End` passes the editor today, and the doc already
  requires the check.
- Firmware: honour run-on-load for a script loaded at any time, not only during the boot scan.
- Firmware: **load-on-boot is a flag bit plus an index.** The enable bit says whether to load at boot and the
  index says which slot (uint8, 0-255), so a boot load is deterministic instead of guessing the identity slot
  or the first free one, and all 256 slots stay addressable. This is also what makes the file-to-slot mapping
  knowable without asking the device.
- Firmware: the in-script `Load script` / `Unload script` ops return the code as their result instead of
  silently advancing.
- No change to CID 0: it reports file ids for positioning only.
- Firmware: log both load-time and run-time errors - today a VM error exists only in RAM until CID 3 is polled.
- Native tests for the script VM, of which there are none today, covering the above.

Open detail: none. Broken existing `SCR_XXX` files are accepted as the cost of the header change.

### Stage 1 - error reporting (in progress)

Decided:

- **Being not fully loaded is a state, not an error.** The state set expresses partial loading directly, so an
  incomplete load is never reported as a fault.
- **The report record:** `state (8b) | reserved (8b) | line (16b)` = **4 bytes**, with `instruction (32b) |
  error (32b)` appended **only for an error** = **12 bytes** total. The reserved byte pads it so `line` sits on
  a 2-byte boundary and the error's two 32-bit fields land on 4-byte boundaries - line at offset 2,
  instruction at 4, error at 8. There is no separate symbol field: the error's instruction-specific detail
  carries it. `line` is the instruction counter, **moved from CID 5 to CID 3**; CID 5 keeps the rest of the
  internal picture (the variable RAM) and is still how a full dump is taken.
- **On an error, `instruction (32b)` is the offending instruction's own encoding** - the context the failure
  happened in, so the app can show which instruction failed.
- **The error is structured, not a flat list:** `class (8b) | instruction-specific detail (24b)`. The class set
  stays the eight that exist today; the detail's meaning is defined by the instruction that faulted, so
  "no such block" versus "read refused" are the same class and different details.
- **Error names are app-side only.** The app keeps the human-readable text; nothing goes on the device and
  there is no mirroring contract. (The class numbers are still wire values, so the spec still enumerates them.)
- **Failures surface two ways, both wanted:** an instruction carries its success flag out so a script can
  check it further down, and a failure that is not checked becomes an error. The mechanism by which a script
  reads an instruction's success belongs with the instruction work (Stage 5).

Open:

- The per-instruction detail catalogue: the detail values for each instruction that can fail (a register
  access, `Pow`, `Get time`, the in-script `Load`/`Unload`).
- Logging both load and run errors is Stage 0, not here; the `Load` code is shared by the CID 1 reply and the
  in-script `Load script` op result (Stage 0).

### Stage 4 - loading and unloading

Decided:

- **CID 0 streams the whole slot map, in slot order** - the file id in each slot, with `0xFFFF` for an
  unoccupied slot. That is the file-to-slot mapping the app needs, and the stream length is what tells it the
  device's slot count, so the app stops assuming 256 everywhere.
- **Loading into an occupied slot fails.** Only empty slots accept a load.
- **"Not fully loaded" means reserved but not ready**: the slot's space exists but the script cannot run yet.
  It leaves that state by becoming ready (`Stopped`) or by being released when the load fails.
- **Unload reports success or failure only**, nothing more.
- **The state enum gains reserved-but-not-ready**, so an incomplete load is a state rather than a fault.
- **The app learns which slots are empty from CID 0's map and keeps its own bookkeeping.** There is no
  separate slot-request command; a load that names no slot takes the lowest free one.
- No addition to the synthesised block metadata is needed.

Remaining:

- **CID 0's 512-byte slot map (256 slots x uint16) and CID 5's internal-state dump (the variable RAM) both far
  exceed one frame** and must stream through the app interface's fragmentation. The spec already calls for it
  on CID 0 - the `Fragmentation` field in that command's response row - but the firmware does not emit it yet,
  and CID 5 must do the same. So this is implementation plus test coverage: reassembly is currently untested,
  and the slot map is the first thing that will exercise it. The Valu's 16 slots fit one frame.

### Stage 2 - the script block and the load contract

**Register representation: settled.** A loaded script is *addressed*, not registered. The script block range
(0x3F4-0x3F7, four types x 64 instances = 256 slots) is a fixed set of block-type numbers at which a
`BlockInfo` can name a script's I/O, and the Register dispatcher forwards an access in that range to the Script
service. Nothing is stored, allocated or registered in the registry for a script, and it takes no part in the
dynamic-block lifecycle - no create, delete or resize. The type numbers are unconditional constants in
`BlockTypeRange`; only the routing on top of them is gated by `USE_SCRIPTS`. Only I/O is exposed (inputs and
outputs), and the block's metadata is synthesised by the Script service from the script's own. This is what
the code already does; no change is planned here.

Decisions taken:

- The script register block is an addressing window, not a registry entity: nothing is stored, allocated or
  registered for a script. A fixed range of block-type numbers (0x3F4-0x3F7, four types x 64 instances) lets
  a `BlockInfo` name a script's I/O, and the Register dispatcher routes an access in that range straight to
  the Script service. The block's metadata (name, fields, types) is synthesised there from the script's own.
- Only I/O appears in the register (input and output fields). Variables and constants stay off the register
  path, reached through the management CIDs as today.
- Already true in the code and to be kept, not a change: the type numbers are plain constants in
  `BlockTypeRange`, defined in every build, and only the routing behind them is gated by `USE_SCRIPTS` (four
  `#ifdef` sites in the Register dispatch). The script range shares the four-types-by-64-instances banking
  scheme with the dynamic range only because `BlockInstance` is 6 bits - arithmetic, not a dependency.
- Up to 256 loaded scripts, as today. That count comes from the space being a family of four types x 64
  instances, which is what the 6-bit `BlockInfo` instance field allows; a single type would cap at 64, and
  widening the field would renumber existing types.
- The Valu v2.0 carries a script service too, on its own smaller slot count. The device-block space is shared,
  so the reserved type numbers and the `USE_SCRIPT` gating have to work for a script build on a different
  target; the app must stop assuming 256 slots everywhere.
- Script files are portable in the weak sense only: a file loads and runs on another device, but positional
  references mean it need not behave correctly there. References stay positional `BlockInfo`
  (Type.Instance.Field.Key). No name binding and no file versioning - the device's software version is the
  compatibility signal.
- Scripts are independent. A reference to a block that is not present is a run-time error on the read/write
  instruction (target not found), not a load failure and not a tracked dependency.

The load contract:

- **Validation at load is memory bounds only.** The requirement is computed from the file header - the
  declared input/output/variable/constant sizes, the constants and input-defaults lengths, and the instruction
  length - and checked against available memory. Every other fault - unknown opcode, out-of-range index or
  operand, bad flow target, unresolvable reference - is a run-time error, raised when execution reaches it.
  A malformed script can therefore run part-way and then stop; that is accepted, and it is why the error must
  carry the position of the failing instruction.
- **Two allocations per script, both made once at load**: the variable spaces (I/O, variables, constants) and
  the instruction (bytecode) space, kept separate. Nothing allocates later.
- Everything the script runs on is copied into that allocation before the slot goes active: the VM never
  aliases storage, so rewriting the stored file under a running script cannot corrupt it. Optimization
  deferred.
- Load may hold the main loop briefly while it reads, parses, allocates and copies. Accepted for now;
  an incremental load is deferred.
- A memory-bound failure leaves the slot inactive and the caller sees the failure, as today.

Remaining in this stage:

- No block-space change is needed. The script range is already out of the registry and out of the
  dynamic-block lifecycle; the numbers are unconditional and the routing is gated.
- The load contract: two allocations at load (variable spaces, bytecode), the header-derived memory check as
  the only load-time rejection, and the copy-not-alias rule.
- The Valu's script service (a new build, not a change): enable `USE_SCRIPTS` there and size its slot table
  against the RAM budget - the slot table is static, so the count is a per-target decision, and the app must
  query the slot count rather than assume 256.
- Docs (proposal): the load contract, and that the script range is an addressing window into the script
  memory space rather than a registry entity.
- Tests: native for a load that fits versus one over the memory, and for nothing allocating after load; HIL
  for the copy-not-alias rule - rewrite the stored file under a running script and assert it is unaffected.

Open in this stage: whether the editor pre-checks a script against the device's free memory and slot count
before upload. A memory-bound failure's reporting is deferred to the error-reporting stage. Note that a slot
count is not a memory promise: with 256 slots on the core, scripts averaging more than a fraction of a
kilobyte each will run the RAM out long before the slots do.

Knock-on: with rejection only at run time, Stage 1 (error reporting - state, code, and the position of the
failing instruction, plus a log entry) stops being polish and becomes what makes Stage 2 usable.

## Beta window plan (2026-10-07) - Tamu core/DAS + app; Valu v2 parked

Reason: a tester is being handed updated builds. Targets are **Windows 11 + Android**. No hard date.
Doc rule changed: docs (`Docs/`, `AGENTS.md`, READMEs) are approval-gated - Amp edits them directly,
in batched change-sets per area, only after Akyirr approves. `TODO.md`/`Issues.md` stay working files.

**Phase 0 - baseline gate.** `./test.sh`, `pio run -e Tamu_v2_0A` / `-e DAS_v0_1`, HIL small rig;
record a numbers table (flash/RAM per env, suite counts, timings) and define the beta acceptance
criteria. Everything after is measured against it.
  **DONE 2026-10-08 - host and builds (HIL not yet run).**
  - `./test.sh` **exit 0**: native suite clean, **168 app tests pass** ("All tests passed!") - **178** after
    the 2026-10-10 fact-check wave,
    `flutter analyze` clean ("No issues found!").
  - `Tamu_v2_0A` **SUCCESS** - RAM 21.0% (68892/327680), flash 23.5% (646846/2752512), bin 647520 B.
  - `DAS_v0_1` **SUCCESS** - RAM **99.0%** (2028/2048), flash 93.8% (13440/14336), bin 14336 B.
  - Android debug APK builds here (see Phase 1a); Linux desktop release builds here too.
  - **HIL: 11 passed, 0 failed, 0 skipped, exit 0** (5 s) - core ping, System block fields, SystemMemory
    blocks 6 + flags, storage 112-fragment churn, SNDB + TimeSync, Register Enumerate/BlockInfo, NetID
    write, DAS static blocks, DAS static save, DAS `.SV` recall over an erased name, System Name clamp,
    DAS clock within 10 ms of the core. Destructive suites deliberately not run (one DAS vs a two-DAS
    evaluation setup - nothing could restore it).
  **Found and fixed a regression (caused by the Valu work).** The Valu device folder's custom startup
  `.S` files were compiled into both sibling envs: duplicate `_start` - on the DAS against the
  framework's `startup_ch32v00x`, on the core against each other. Fixed by excluding
  `Devices/Valu_v2.0/` in the DAS `build_src_filter` and in `src/CMakeLists.txt`'s GLOB exclusion.
  Both images came back **byte-identical to the recorded baselines**, so the fix restored the prior
  state rather than changing it. Pitfall recorded in the `tamu-project` skill's firmware reference.
  **Proposed beta acceptance criteria:** host gate exit 0 with zero failures; both firmware envs build
  with no size regression (DAS <= 2028 B RAM, <= 13440 B flash); the Android APK installs and connects
  to the rig; the update flow completes end to end on Android.

**Phase 1 - deliverability (Android is the only target delivered this window, and it builds here).**
  a) Android: the SDK **was** present at `/home/akyirr/Programs/Android` (SDK 37, build-tools
     35/36.1/37, ndk, platform-tools with `adb`) - Flutter simply had no path to it, and
     `/home/akyirr/Programs` is mode 750 so filesystem searches cannot see it. Fixed with
     `flutter config --android-sdk /home/akyirr/Programs/Android`. Doctor's "license status unknown"
     is its own staleness - `--android-licenses` reports "no longer needed" in this toolchain and the
     licenses dir holds accepted hashes; Gradle is the honest test. APKs from 2026-09-25 already sit
     in `app/build/` (gitignored, `.gitignore:39`). Java in use is Studio's JBR 25; AGP 8.11
     nominally wants 17-21, so watch the first real build.
     **DONE 2026-10-08:** a debug APK built by Amp on this box - 156,017,696 B, sha256 `4c0a01db...`,
     package `tamu.app.beta`, compileSdk 36, signer CN=Android Debug SHA-256 `76b2caa5...` vs Akyirr's
     `f5bca9c7...`. The cross-host signing warning is now *measured*, not theorised. Build takes
     ~8 min cold, ~7 s warm (just needs `JAVA_HOME` on JDK 21).
     **Correction:** the move-aside preserved the 2026-09-25 *release* APK (56,123,794 B, also still in
     `outputs/apk/release/`), but the 178,716,229 B Sep-25 *debug* APK did not survive it - the scratch
     copy of `app-debug.apk` is the new build. Regenerable in one command, no source touched, but the
     earlier "preserved, not deleted" claim was wrong for that one file. A first *release* build will
     hit the same permissions wall on `outputs/apk/release/app-release.apk` (still his).
  a2) **Signing trap - RESOLVED 2026-10-08 by Akyirr's call: one build host.** All distributable builds
     are made on his machine, so the debug key stays consistent and the tester's installs update
     normally. No release keystore is introduced. **Consequence: an APK built by the agent must never
     be installed over an existing install or handed to anyone** - a different key breaks that update
     path (`INSTALL_FAILED_UPDATE_INCOMPATIBLE`). Agent builds are verification only (does it build,
     does it run), not artefacts for distribution. Background: `build.gradle.kts` signs release with
     `signingConfigs.debug`, and each host auto-generates its own `~/.android/debug.keystore` - his is
     CN=Android Debug SHA-256 `f5bca9c7...`, the agent's is `76b2caa5...`.
  a3) APK size: release 56.1 MB, debug 178.7 MB - ABI-fat. Phase 5 target: `--split-per-abi` or a
     single-ABI build. `applicationIdSuffix = ".beta"` is deliberate (installs alongside the release).
  a4) **Build-host friction.** `app/build/` holds artifacts owned by Akyirr (the 2026-09-25 APKs), so a
     build run by Amp fails at the copy step - `Could not set file mode 664 on .../flutter-apk/...`.
     Moved aside (preserved) to `~/.hermes/cache/scratch/stale-apk-2026-09-25/`; the durable fix is an
     ACL/chown on `app/build/` (the mirror image of agent edits stripping Akyirr's write bit).
     Also: the Java in use must be 21 - AGP 8.11 rejects Studio's JBR 25 with a bare `25.0.3`.
  a5) **Forward-compat warning.** Flutter 3.44 warns that `build.gradle.kts` applies the Kotlin Gradle
     Plugin and that `file_picker`, `flutter_libserialport` and `universal_ble` do too - a future
     Flutter will *fail* on this. Track those plugin upgrades; do not take them mid-beta without a
     full gate run.
  b) Windows - **dropped by Akyirr's call (2026-10-08)**: no Windows build is needed this window, so no
     steps are written and no Windows audit is done. Kept only as record: not buildable on Linux (needs
     MSVC), `flutter_libserialport` 0.6.0 does ship `windows/`, and the platform-specific Dart surface
     is `lib/core/{connection,transport,settings,platform_caps,host_files}.dart` (still relevant to
     Android).
  c) The update flow end to end (connect -> detect -> update app/bootloader) on Android - this is the
     sequence the tester actually performs.

**Phase 2 - app interface (audit done 2026-10-08, report-only - nothing changed).**
  `flutter analyze` is clean, so this is design-level inconsistency, not lint-level. Full report lives at
  `/var/lib/hermes-mgr/.hermes/notes/tamu-ui-audit-2026-10-08.md` (durable; kept out of the repo because
  new docs are approval-gated).
  *Root cause:* `theme.dart` exports 3 colours + 3 text styles, and `lib/ui/` bypasses them -
  `TextStyle(` inline 149x, `fontSize:` 108x (10 distinct sizes), `Colors.<x>` 130x (four greys for one
  secondary-text role), `EdgeInsets.` 131x, 17 distinct `Card(margin:)`, two card backgrounds for the same
  role (~13 explicit `kSurfaceAlt` vs ~16 plain `Card(`).
  *Duplication:* label/value row 3x; 7 ad-hoc prompt dialogs; ~30 bespoke dialog action rows; ad-hoc
  empty/loading/error (20 `Center(Text)`; spinner vs the literal `'Loading...'` on the same class of page);
  Subscriptions alone uses a bare refresh `IconButton` and lacks `AutoRefreshMixin`.
  *Missing confirmations:* subscriptions delete, and register block/field/entry deletes, skip `confirmDialog`.
  *Clunky flows:* Devices is a dead end when disconnected (no button at all - connecting lives only in the
  Connection tab); the refresh button silently changes meaning by connection state; switching device
  disconnects with no confirmation; autoconnect is long-press-only and shows a raw MAC in Settings;
  register add-entry is 3-4 dialogs deep; the script editor has two "Upload" affordances told apart only by
  tooltip.
  *Spec mismatches:* Device view omits the documented Router-table row; Settings puts the target in the
  title instead of the subtitle+hint.
  *Ranked fixes (benefit/risk first):* 1) theme tokens + `AppCard`; 2) `Empty/Loading/ErrorState`;
  3) `PromptDialog` + `DialogActions` + mandatory `confirmDialog`; 4) `RefreshButton` + `AutoRefreshMixin`
  on Subscriptions; 5) autoconnect to a friendly name and surface it on Connection; 6) one labelled Upload
  in the script editor; 7) `LabelValueRow` unification; 8) title/spacing normalisation.
  *Not determinable from code:* realised contrast and tap targets, touch behaviour, and anything needing a
  running app plus hardware; the ordering above is code-path evidence, not user data.
  **Waiting on Akyirr** to pick or amend the order before any UI code changes.

**Phase 3 - scripts.** Gap list (`Docs/Services/Script.md` vs the 8 `Script*.h`, ~2.2 kloc), macros,
completion, tests.

**Phase 4 - documentation consistency/completeness.** Collect proposals from day 1; deliver as batched
change-sets per area - the one serial step in the plan.
  **Style guide is in place:** `Docs/Style Guide.md` (written 2026-10-08, with Akyirr's approval), built
  from his decisions on all 25 open items. **Carve-outs:** `Docs/Plan.md` is Akyirr's personal notes and
  is out of scope for the style pass; `Docs/Services/Router.md` is `TBD`.
  **Rewrite order:** Services docs first (most table-driven, they set the patterns), then the protocol
  and structure docs (`RSBus and Packets`, `Data Formats`, `General architecture`), then
  `Modules and blocks/`, then `App/`. Skip `Plan.md`. Commit before starting, so the rewrite can be
  reverted.
  **Two decisions the guide applies that already touch content:** command IDs are hex with the range
  heading supplying the prefix (Akyirr has already applied this to the SNDB commands - verified
  consistent with `Core/Services/Device.h:42-52`), and index/allocation tables are left unrelated.
  **Rewrite progress (2026-10-09).** Done: `Services/Storage.md` (approved as the template),
  `Services/Register.md`, `Services/Subscriptions.md`, `Services/System Block and Device Commands.md`,
  `Services/Router.md` (marked `TBD`), `Services/Bootloader.md`, `Services/Log Handler.md`,
  `Services/App Interface.md`. Remaining: `Services/Script.md`, `RSBus and Packets.md`,
  `Data Formats.md`, `Command ID table.md`, `General architecture.md`, `Devices.md`,
  `Modules and blocks/*` (4 files), and `Example Setup.md` (borderline - firmware, or rig notes?).
  **ALL DONE (2026-10-09):** 19 firmware docs rewritten (Storage approved as the template first).
  Verified clean: no `#`/`##` headings, no colon-ending headings, no old typos, no `TRID`, no old
  command-table headers, no old range-heading form. 1000 insertions / 857 deletions across 20 files
  (`Docs/Style Guide.md` included, for the `Name` type in its vocabulary list). `Docs/App/**` is a
  separate pass, `Plan.md` stays out of scope. **Committed 2026-10-09** (the `docs:` series that day).

**Phase 5 - optimization + final verification pass.** DAS headroom (~94% flash / ~99% RAM), gate
green, release checklist.

## Open work

- Router capability bit: reserve one for the router service so the documented capability can be set.
- App Active enum: add `WiFi` (`Enums.h:34-38`) when that work starts. E10 dropped `LegacyBt` from
  the documents, so it is no longer listed as a planned state.
- [x] **A55 (Example Setup) - the brightness-cap column. DONE 2026-10-10 (E15).** The curve was
      rescaled to 7-100 % with the cap at 8 000 lux; the table now reads the four anchors.
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
- **Evaluation setup** (`Docs/Example Setup.md`): the setup builder, the emote system, tuning
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
