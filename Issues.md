# Issues

Open items only. Resolved findings, the 2026-10-04 per-area audits and the doc-fact-check rulings
that were applied are in the git history.

Item codes: **B** = code-fix items (the 2026-10-09 fact check); **E** = the remaining open backlog items, numbered for later handling. Closed on 2026-10-15 and removed: **B11** (BLE large-reply drain fixed), **E12** (storage hooks converted to a class), **E19** (structurally requester-only), **E24** (Register string clamp documented), **E28** (no capability bit needed), **E4** (obsolete, replaced), **E22** (not needed), **E23** (fixed). Closed on 2026-10-27 and removed: **E54** (Raw Resistance multiplied before it divided, so every 330 kΩ reading from raw 100 up wrapped; the transform now divides first and clamps at the Q16.16 ceiling, and `firmware/test/native/meas_transform_test.cpp` pins the whole window). Closed on 2026-10-28 and removed: **B5** (the 8.1 row is gone - `03a585b` replaced System field 8 with an interface bitfield - and the WiFi code is `firmware/src/Blocks/WifiSettings.h`, block `0x0C`; the missing radio is stated by the specification itself, `Docs/Services/App Interface.md:47`).

## B. Code fixes still open (the document is the specification)

**B27 Script-file length validation wraps in uint32.** *(WITHDRAWN 2026-10-14 by the owner: a script file cannot approach the 32-bit limit, so the wrap is not reachable. The `474489e` change was reverted. Kept for the record - the sweep's other arithmetic findings were checked and stand.)* `ScriptProgram.h:209-217` summed four header-supplied uint32 lengths, so a crafted `SCR_XXX` header passed `Need > Size` with the running offsets ~4 GB past the buffer.

**B1 Router and branch broadcast are documented and unimplemented.** - **scope settled**: after the beta. The rig has no multi-bus device to test a tree topology on. `RSBus and Packets.md:3-7` and
`Data Formats.md:36` (`0x3FE`) describe a router tree; `Dispatcher.h:36-40` says the multi-bus topology
is not implemented and `Docs/Services/Router.md` is a stub.

**B6 The UDP framing has no code.** `App Interface.md:25-27` documents a UDP packet; the firmware
implements USB and BLE only.

## D. Unverifiable from code

`RSBus and Packets.md:1` supply voltage and USB PD (hardware), `Data Formats.md:7` "14 byte UUID"
(14 bytes confirmed, UUID loose), `Data Formats.md:52` the generic dictionary marker encoding,
`Register.md:33` the Internal/External Access columns, `Register.md:50` "sorting by the whole uint32",
`Register.md:86-98` the dynamic block descriptor ordering (runtime only), and the Valu feature rows that
are hardware-only.

## Open (2026-10-06)

- **E25** **The Valu bootloader can only ever be replaced over the ROM bootloader.** Our own bootloader
  refuses writes below `APP_BASE` by design, and this board has no SWD header, so every bootloader
  iteration needs an ISP session (`wchisp-nightly`, one command per USB reset). Worth keeping in
  mind when deciding how much logic belongs in the bootloader rather than the app.
- **E26** **`valu_upload.py` drives `wlink` over SWD, which this board cannot do.** The ISP recipe that
  actually works (USB port reset, then a single `wchisp-nightly` invocation) needs folding into it
  as an `--isp` mode so `./upload.sh boot valu` matches reality.
- **E30** **The DAS_v0_1 size gate was a measurement, not a budget — and the documented headroom is
  stale.** (Headroom figure superseded twice on 2026-10-27: **E53** found the image ending at `0x3df8`,
  8 B spare; the `t_1e134fcc` diet then moved the image end to `0x3bf4`, **524 B** spare, and E53 is
  closed with it.) Settled 2026-10-17 (owner decision, Akyirr, via kanban `t_2143e21b`, audit `t_e1d8c148`):
  the ceiling is the physical code space, **13824 B at `0x800`-`0x3E00`**, plus **2048 B RAM** — not
  the `<= 13440 B flash / <= 2028 B RAM` pair the criterion carried from 2026-10-11 until
  2026-10-17 (`TODO.md:165` as of `e0b5df3`).
  `13440 = 16384 - 2048 - 512 - 384` subtracts the storage's 384 B data payload a second time (it is
  already inside the 512 B), and its arithmetic merely coincides with the 2026-10-08 Phase-0 build
  (`6c3d210:TODO.md:34`, `13440/14336 = 93.75 %`) that the criterion was copied from. **The image is
  not over budget and nothing needs to be trimmed.** Clean rebuild (2026-10-25, this tree, the
  counter-only edge payload `t_378cc55c` on top of the rail flags `t_1381f097`): `RAM 100.0%
  (2048/2048)` / `Flash 95.6% (13708/14336)`; `.text` ends at `0x3D1C`, the 4 B `.init_array` takes
  it to `0x3D20`, and the 112 B `.data` initialiser image is flash-resident too, so the image ends at
  `0x3D90`: **13712 B of the 13824 B code space (99.19 %), 112 B spare** before `.fixed_data` at
  `0x3E00`. Platformio's 14336 B denominator is the storage-inclusive App size, so 95.6 % is not a
  fullness figure; its 13708 B counts the `.data` image and skips the `.init_array`.
  Trend: 13600 B at `0c55523` (2026-10-24), 13776 B at `9e1378b` (reset-cause report + edge counter),
  then 13676 B at `6a3a953` (two build flags bought 100 B back), 13748 B at `t_1381f097` (the two rail
  flags cost 72 B of image and 8 B of statics), 13712 B now (`t_378cc55c` gave 36 B back: an edge
  update's payload is the counter alone, with no value bytes behind it). Earlier headroom figures
  counted code only and ignored the 112 B `.data` image, so the real spare was always 112 B less than
  stated - 112 B at that measurement, **524 B since the 2026-10-27 `t_1e134fcc` diet** (image
  13816 -> 13300 B: the three CH32 prescaler tables plus the two boot-time consumers that needed
  them).
  Carrying the edge counter on the 0401 answer too was measured at a further ~120 B and does not
  fit; that is why the answer stays value-only (`SubscriptionsEdge.h`).
  The RAM figure is by design: statics (984 B = 48 B `.data` + 936 B `.bss`) + stack (1064 B) fill
  the 2 KB exactly, the state `firmware/platformio.ini` documents, and the linker errors if statics
  grow into the stack - which is exactly how the rail flags forced the stack down from 1000 B to
  992 B (4 B per measurement instance: two Bools plus the struct's tail padding). `t_378cc55c`
  removed a 4 B per-entry field from the provider entry (the send gate became a bool inside the
  existing padding), so the stack went back up to 1008 B - the total stays 2048 B, the stack is the
  part that grew.
  The 2028 B RAM gate could never have been met.
  **Still a real warning:** measured image history 13600 -> 13776 -> 13676 -> 13748 -> 13712 B (see the
  trend above); with 112 B left, further code growth fails the link rather than warning - measure
  before adding a DAS feature. Size lines are only trustworthy after a clean build; a stale
  `.pio/build/DAS_v0_1` can still report "up to date". Figures corrected in `TODO.md` (acceptance
  criteria and Phase 5) and `firmware/platformio.ini`; `AGENTS.md:54` still needs the owner
  (protected file, see E31).
- **E31** **The DAS flash layout is not fully documented.** `Docs/Devices.md:43-70` gives the DAS
  storage (512 B) and bootloader (2 kB) sizes but never states the 16 KB chip total or the
  `0x800`/`0x3E00` bases, so the code space cannot be derived from the docs alone — it has to be
  reconstructed from `firmware/platformio.ini` and the built ELF.
  `Docs/Services/Bootloader.md:41` defines "App size" as the uploadable region and does say it covers
  storage "on a device without a partition table", but it does not say that a linker-pinned storage
  region (`--section-start=.fixed_data=0x3E00`) *shrinks the code space* — the DAS reports the full
  storage-inclusive 14336 B there. Proposed change for the owner (Docs are Akyirr's to edit): add the
  chip total and the app/storage bases to `Devices.md`'s DAS section, and one sentence to
  `Bootloader.md`'s App size note that storage reserved by the linker is not code space.

- **E32** **A capture whose requester-table read fails archives an empty table, and a restore now
  prunes to it.** `_captureDevice` (`app/lib/core/backup_capture.dart:114-132`) logs a failed read of
  the requester table and stores `requesterSubscriptions: []`; `SubscriptionClient`'s table read also
  reports a timeout as an empty list. Since 2026-10-22 a restore cancels every target entry its
  archive does not name (`pruneRequesterTable`, `app/lib/core/backup_restore.dart`), and "the archive
  names none" is indistinguishable from "the capture could not read them" — restoring such an archive
  cancels every entry on the target. The prune does report the count and TRIDs of what it cancelled,
  so it is visible, but it is still a delete. Fix options: carry a `requesterRead` marker in the
  per-device JSON and skip the prune for a device whose archive does not claim the table was read, or
  fail the device capture rather than storing an empty table. Not done here: the archive's JSON
  contents are a Docs-governed decision (`Docs/App/Backup.md`).

- **E33** **`Docs/App/Backup.md`'s restore-plan section describes the page as it was, not the diff
  viewer it now is.** Lines 13-19 still say "per block/script/subscription/SNDB/file entries as
  checkboxes" with a footer of "selected / unavailable counts"; since card `t_6ffa42c6`
  (owner-directed, 2026-10-23) the page classifies every row against the device
  (Add / Replace / Same / Delete, compared per kind), lists the deletions a restore would apply as
  rows of their own, filters by class with Diff as the default, and counts the deletions apart in
  the footer. The code is the deliberate side of the divergence; the wording in
  `Docs/App/Backup.md` is the owner's to change. Proposed replacement text is in that card's
  handoff.

- **E34** **A requester prune with two archives on one target cancels each archive's entries.**
  `pruneRequesterTable` (`app/lib/core/backup_restore.dart:2689-2700`) loops per SOURCE device and
  cancels every entry of that source's target that source's own archive does not name, so when two
  archive devices are restored onto one live node - which the plan page's per-device target dropdown
  allows - the first pass cancels the second archive's entries and the second pass cancels the
  first's. Reproduced 2026-10-23 with a throwaway probe (two archives with one requester each, a live
  table holding both entries, both sources pointed at that one node), throwaway file deleted after
  the run:

      prune 57.1: before=[0x1000, 0x1001] cancelled=[0x1001] kept=[] remaining=[0x1000]
      prune 57.1: before=[0x1000] cancelled=[0x1000] kept=[] remaining=[]
      device table after: []   cancel commands: [0x1001, 0x1000]

  `pruneScriptSlots` had the same shape and was fixed to union the archive per TARGET (`c046e21`);
  this one was not. The plan's diff agrees with the run here (it lists each live entry the other
  archive does not name, so the page shows both deletions), which is why it was not a finding
  against card `t_6ffa42c6` - the page is truthful about a wrong action. Fix: group the sources by
  target and union `archiveRequesterKeys` over that group, as the script sweep unions slots; then
  the plan's requester removals must be derived per target too.

- **E35** **Two capture/restore rules now differ from `Docs/App/Backup.md`; the owner has to approve
  the wording (code is the deliberate side, per card `t_df26eda4`, 2026-10-23).** Both concern state
  the device DERIVES rather than owns:
  (1) *The archive no longer carries the core's own SNDB row.* `Docs/App/Backup.md:34` lists "the
  SNDB of a core" as archived content without an exclusion, and the format is unchanged, but the
  capture now drops the row whose address is the device's own bus address (or whose serial is its
  own serial) - a core does not address itself, and `bambi/bambi_setup.zip`'s first row was `0.1`,
  the core describing itself. The child rows are archived exactly as before, and an existing
  archive is not rewritten: the plan drops a self row on read (`_dropSelfSndb`). Proposed addition
  to `Backup.md:34`: "... the SNDB of a core (never the core's own record: a core does not address
  itself) and the device's files."
  (2) *The restore prunes stale script FILES, not only loaded slots.* `Backup.md:38-40` describes
  how a `SCR_XXX` travels but not what a restore does with one the archive does not carry. Since
  `t_df26eda4` the sweep runs a second phase over the file table: when a device's scripts are being
  restored as a whole, every `SCR_XXX` file no script of the archive needs is deleted and reported
  (name, size, the slot it backed), a file behind an unticked script is kept and named, a file a
  slot THIS RUN LEAVES LOADED still holds or that a restored script references as a function
  library is never deleted, and the plan shows each as a DELETE row. A slot the run unloads is no
  protection - the slot phase runs first, so the file behind it is stale and is planned too, which
  is why the plan's rows and the sweep's deletes are equal in that state as well (pinned by
  `backup_restore_script_files_test.dart`: plan rows == sweep deletes, both with the stale slot
  unloaded and with it still loaded at plan time).
  Without it a stale script stayed loadable after being unloaded - `SCR_000` survived bambi's
  restore. Proposed addition after `Backup.md:40`: "A script the archive does not carry is removed
  from the device in full: its slot is unloaded and its stored file deleted, both reported, so it
  cannot be loaded again."

- **E37** **A scratch destination for the external-facing instructions exists in the spec only; the
  firmware half now carries the code, the app half does not.** `Docs/Services/Script.md:198` says the
  register instructions never fault and carry their code into a result destination, "a scratch
  destination when the script ignores it". The firmware does that now, the foreign variants
  included: a failed local read/write stores `SCRIPT_ERR_REGISTER` and advances
  (`CarryResultCode`), and a foreign request that times out or whose reply is malformed stores
  `SCRIPT_ERR_TIMEOUT` / `SCRIPT_ERR_REGISTER` and resumes (`CarryPendingResultCode`), both in
  `firmware/src/Core/Services/ScriptExec.h`; only malformed operands still fault. What is left is
  app-side: no scratch destination exists in `app/lib` (the editor requires a real variable or
  output, `app/lib/core/script_instructions.dart:757-758`), and `Register write` / `Register write
  (foreign)` declare no destination at all, so a failed write's code has nowhere to land.

- **E43** **A foreign register reply that arrives after the 500 ms deadline can be read as the next
  request's.** *(Closed 2026-10-24 - **not a defect**. Owner ruling (Akyirr): "500 ms is long, that is
  not a defect." The window needs a remote that answers later than the deadline, and by that ruling
  such a peer is the problem, not the deadline.)* A script's TRID is fixed per slot
  (`TRID_SCRIPT_BASE + Slot`, `firmware/src/Core/Services/ScriptProgram.h:190`), so every foreign
  request it sends carries the same one; `HandleResponse` matches a reply on that TRID alone
  (`firmware/src/Core/Services/ScriptRuntime.h:96`). A timed-out request used to stop the script in
  `Error`, which made the window unreachable. It is reachable now: the script carries
  `SCRIPT_ERR_TIMEOUT` into its destination and runs on (`CarryPendingResultCode`, `t_2e856b86`),
  so the next foreign op is in flight on the same TRID when the abandoned reply arrives - the stale
  value lands in the new line's destination, or confirms a write that was never confirmed. Needs a
  remote answering slower than 500 ms plus a second foreign op in the same script.
  **Footnote, for anyone who ever shortens the deadline:** the fix is one TRID per request inside the
  slot's range (e.g. `TRID_SCRIPT_BASE + Slot * 64 + Seq`, which keeps the DISPATCHER's routing by
  range, `firmware/src/Core/Functions/Dispatcher.h:91`), or dropping a reply whose TRID was abandoned.

- **E45** **A `Register write` carrying `ValueInfo.Size = 0` stores a size-0 dynamic entry, and the
  field then never resolves again.** Measured on the rig (`iter=2`, pre-fix, 2026-10-24 21:12): one
  Register Write to `Dynamic.0` f6 with `ValueInfo` `04 00 00 00` took its meta from `04000100` to
  `04000000`, and the reply carried no FAIL; a real 1-byte value pushed afterwards on the
  subscription row's TRID renewed the row's lease (+12051 ms, so `ApplyRequesterValue` ran) while the
  field stayed size 0 - the "value arrives, never lands" signature. Only an explicit write carrying
  Size 1 restored it (meta back to `04000100`). **Fixed in `t_b579fa77`**: `SetEntry` refuses a
  0-length value (`ValueUpdateApplicable`, `MemoryTypes.h`) and the write now answers FAIL, and
  `ApplyRequesterValue` refuses one before touching its target. *Correction to the bambi
  investigation (`t_2c6631a5`): the cancel ping-pong it describes does not exist.* The dispatcher
  routes a typed packet in the Subscriptions TRID range to the service only when it is **not** a
  `FLAG_SUCCESS`/`FLAG_FAIL` reply (`firmware/src/Core/Functions/Dispatcher.h:69-76`), so the cancel
  ack (`SubscriptionsControl.h:141`) and the provider table-full FAIL (`:150`) go to the app link and
  never reach `Subscriptions.Handle`, let alone the orphan branch - measured: four cancel acks, zero
  row-lease renewals, node 0.3's provider entry gone each time and nothing re-sent. Remaining
  wart in that routing rule: the "source does not resolve yet" reply is a **1-byte** payload
  (`SubscriptionsControl.h:161-164`, `Resp = 1`), which the requester applies as the target's value -
  a Bool target reads that as a press.

- **E46** **A keepalive the provider never answered was still assumed to have renewed the entry, so
  one lost `Change` killed a subscription for a whole keepalive period.** The trigger runs on the
  **provider** (`EvaluateProviderTriggers`, node 0.3), whose entry is leased for 120 s and renewed
  only when the core's `Change` arrives; the core re-sent it every 60 s once any value had flowed,
  without ever asking whether it was answered. One lost frame (half-duplex, silence-gated transmit)
  and the node's entry expired 120 s after its last renewal: every switch press was then invisible
  until the next keepalive re-installed it - up to 60 s later. Shape as reported: random death,
  everything else healthy, recovery "after a while" bounded by the keepalive. **Fixed in
  `t_6dcaad54`**: `SubRequesterLease` (`SubscriptionsLease.h`) counts the installs sent since the
  provider last answered and drops the entry to `SUB_RETRY_MS` until it does (bounded to
  `SUB_FAST_ATTEMPTS` so an absent node is not hammered at 100 ms); an answered entry keeps the 60 s
  keepalive. The answer is the CID 1 reply itself, matched by echoed TRID + `id_src` == the entry's
  provider - a `FLAG_SUCCESS`/`FLAG_FAIL` ack cannot be it, the dispatcher routes those to the app
  link (`Dispatcher.h:69-76`). Per the owner's ruling the requester's own 120 s deadline is now
  renewed **only** by that answer, never by the send: an entry nothing ever answers expires 120 s
  after its last answer, is dropped from the live table (`.SUBREQ` keeps it) and stops retrying.
  *Not covered*: presses made while the node's entry is already gone - the trigger state is
  recreated, not replayed, so the fix bounds the dead window rather than closing it.
  **Superseded in part (`t_eb35360c`, owner ruling 2026-10-24)**: keying the requester's lifetime on
  the answer meant a row nothing answers is destroyed 120 s after its last answer, so a node off for
  more than two minutes needed a core reboot or a setup re-apply - the pre-fix firmware recovered by
  itself. The requester now **never** drops a row on time (only an explicit cancel, an orphan
  announcement from the node, or a new setup); the **provider** keeps its 120 s timeout and drops
  its own entry, an asymmetry that is deliberate. What bounds an absent provider is the retry
  cadence instead: fast (`SUB_RETRY_MS`) while installs go unanswered, capped at
  `SUB_FAST_ATTEMPTS = 5`, then the 60 s keepalive until an install is answered; a confirmation or a
  value update resets that counter (it is RAM-only, not persisted). Dropping the requester expiry is
  also what made the cap load-bearing: without it a powered-off node would be retried at 100 ms for
  ever.

## Valu v2.0 app (2026-10-06) - parked (owner: not now)

- **E27** **Resistive measurement reference resistor is undefined.** `Docs/Devices.md` gives three ADC
  inputs (PA6/PA1/PA0) but says "Reference resistor not defined." With no reference and no range
  selectors, the resistance/LDR/NTC transforms in `Devices/Valu_v2.0/MeasuringRun.h` use a
  placeholder `VALU_MEAS_REF_KOHM` (10 kOhm); raw and voltage measurements do not depend on it.
  Needs a hardware value before a resistance-reading module is meaningful.
- **Valu LED displays are registered; OLED still TODO.** `Docs/Devices.md` lists "LED Display x2"
  and "OLED Display (TODO)" for the Valu. The two LED-display instances are now registered in
  `Devices/Valu_v2.0/Main.h` (the generic Vysi1 block; the strip geometry comes from the layout
  file at runtime). The OLED module is still not built.
- **E29** **`MAX_SCRIPTS` is reduced to 16 on the Valu.** The Script service's loaded-script registry is
  `LoadedScript scriptRegistry[256]` (~38 KB) in `ScriptDefs.h`; the CH32V203's 20 KB cannot hold
  it, so the registry is now a `#ifndef`-guarded build knob and the Valu sets `-D MAX_SCRIPTS=16`.
  Higher Scripts-range indices report as unloaded. **Update 2026-10-11:** the mask word count is now
  `(MAX_SCRIPTS + 63) / 64`, so the Valu's 16 slots get one mask word (it was 0 - an out-of-bounds write
  that also left the script tick path uncompiled); restoring that path costs the Valu ~9 KB flash
  (32 788 -> 42 176, 93.6%), parked.

## Docs decisions needed (parked unless noted)

- **E1** **OS notifications.** `Docs/App/Settings.md` lists "Allow notifications (To OS)" with per-event
  selection; the app persists `notifyOs`/`osEvents`/`suppressOsWhenOpen` but only delivers in-app
  notifications (`app/lib/core/notifications.dart`). **Parked (owner: not now)** - implement OS
  delivery or mark the setting pending.
- **E6** **Dynamic descriptor doc omits `Name`/`generation`/`present`.** The `DynamicBlockDescriptor`
  (`MemoryBlocks.h:35-49`) has `char Name[`BLOCK_NAME_LEN`]`, `uint32_t generation` and `bool present`
  beyond the documented pointers and sizes (`Register.md:88-98`). **Under review** (owner wants a
  closer look).
- **E47** **`Subscriptions.md` does not say which side owns the 120 s timeout, and only the provider
  owns it now.** The opening sentence reads "A subscription times out after 120 s and is renewed by a
  new request"; since `t_eb35360c` (owner ruling 2026-10-24) the **provider** drops its entry on that
  deadline while the **requester** keeps its row until an explicit cancel (a node off for minutes
  must not need a core reboot). As written, a reader would take the sentence to apply to both sides.
  Proposed wording, for the parent to apply with Akyirr's approval (Docs are off limits to workers):
  "A subscription times out after 120 s and is renewed by a new request. The provider expires its
  entry on that deadline; the requester keeps its row until the subscription is cancelled." The
  requester-table `Timeout` row ("Local uptime; ignored by Set Subscription and not persistent")
  stays true either way.

## Android (on-device behaviour untested)

- **E20** No Android device/emulator: BLE runtime permission prompt + denied/permanently-denied paths, BLE
  scan/connect/MTU, Storage Access Framework backup save + restore and file download, and the
  compact drawer shell on a phone form factor. The app's **update flow end to end** is the other
  Android item, and it gates the beta criteria in `TODO.md`. **Owner directed: look for improvements.**

## App build warnings (Android)

- **E44** **`flutter_libserialport` 0.6.0 applies the Kotlin Gradle Plugin (KGP); the warning stays until
  upstream migrates.** `flutter build apk --debug` prints
  `WARNING: Your app uses the following plugins that apply Kotlin Gradle Plugin (KGP): flutter_libserialport`
  followed by "Future versions of Flutter will fail to build if your app uses plugins that apply KGP" and
  the report/migration links. The plugin's own Gradle script is the source: `apply plugin: 'kotlin-android'`
  (`flutter_libserialport-0.6.0/android/build.gradle:25`, with `ext.kotlin_version = '2.0.0'` at :5 and a
  `kotlinOptions` block at :50). There is no newer release to move to - `flutter pub outdated --show-all`
  reports `flutter_libserialport  0.6.0  0.6.0  0.6.0  0.6.0` (current, upgradable, resolvable and latest
  are all 0.6.0). **This one is upstream's to fix: do not fork or vendor the plugin here.** The app side of
  the same class of warning is cleared (2026-10-24, `t_ce1af8e3`): `android/app/build.gradle.kts` sets
  `compileSdk = 37` and no longer applies `id("kotlin-android")` or carries a `kotlinOptions` block, so the
  "compile against Android SDK 36 but permission_handler_android compiles against 37" warning and the
  "Your Android app project ... applies the Kotlin Gradle Plugin" warning are gone. This KGP warning
  remains until `flutter_libserialport` ships a Built-in-Kotlin-compatible release; it needs no action from
  us beyond re-checking on plugin upgrades. Filing guide:
  https://docs.flutter.dev/release/breaking-changes/migrate-to-built-in-kotlin/for-app-developers#report-incompatible-kotlin-gradle-plugin-usage-to-plugin-authors
  **Side effect of `compileSdk = 37`:** AGP 8.11.1 now also prints "We recommend using a newer Android
  Gradle plugin to use compile SDK version 37.0 ... tested up to compile SDK version 36.0" (a version-range
  notice, not a failure); it disappears with an AGP upgrade, which is a separate decision.

- **E48** **`Docs/Services/Subscriptions.md` leaves two edge-trigger details unstated: which changes
  the counter counts, and that the 040x Value-update row carries two different payload shapes.** The
  edge trigger's bullet (`Subscriptions.md:74-83`, owner-approved in `c0a0ac6`) already states the
  counter-as-payload rule, parity-as-level, a jump of `n` = `n` edges, and that the requester stores
  the derived `Bool` and never the counter; `t_378cc55c` implements exactly that (the whole payload is
  the counter, `uint16` little-endian, two bytes, nothing appended; odd = true/pressed, even =
  false/lifted). What the section does not say:
  (a) the provider's counter counts **every** level change, in both directions, while the trigger only
  selects which of those changes are sent - without that a reader cannot see why parity may be read as
  the level, since a rising-only counter would stay odd after the release;
  (b) the "Inter-device Commands (040x)" Value-update row (`Subscriptions.md:93`) gives the response as
  "Current value" only, which does not distinguish the edge push (exactly 2 bytes, the counter) from
  the `Change subscription` (0401) answer (1 byte, the current value, i.e. the `Bool` for an edge) -
  the two shapes stay apart precisely because they differ in length.
  Proposed additions, for the parent to apply with Akyirr's approval (Docs are off limits to workers),
  leaving the existing bullet text as committed: to the edge trigger bullet, "The counter counts every
  level change, in both directions; the trigger only selects which of those changes are sent."; and to
  the Value-update row's response, "`Current value`; for an edge trigger, the edge counter (`uint16`
  little-endian) alone - its parity is the level: odd = pressed/true, even = lifted/false."

- **E49** **`Docs/App/Backup.md` still describes the restore's report as "written/failed counts", while
  the app now reports a reason per failed item.** Since `t_7cfa3a99` (owner report, 2026-10-25: a run
  showed 17 failures with no reason) a restore records, per failed item, the device and target, the
  item kind (block field / script / script file / requester row / layout / file), the address as the
  plan names it, the code the device answered with - `FAIL (FLAG_FAIL 0x40, no reason payload)` for a
  refusal, or an explicit "no code from the device" for a client-side one - and one sentence saying
  what that means; the result screen groups them by device and copies the whole report as text, and
  the run's reasons survive in `RestoreSession.last` and the diagnostics ring. Proposed wording, for
  the parent to apply with Akyirr's approval (Docs are off limits to workers): in
  `Docs/App/Backup.md`, replace "**Restore** applies the checked items and reports written/failed
  counts." with "**Restore** applies the checked items and opens a result screen: the written/failed
  counts, one section per device for every failed item - its kind, the address the plan names it by,
  the code the device answered with (or that the app refused it and sent nothing) and what that means
  - and a **Copy as text** action for the whole report, so a failure can be quoted in a report
  instead of counted."

- **E50** **A register read with a genuine type mismatch is not REPORTED - it is silently padded.**
  Found while fixing `t_e57a9667` (a size-0 row read as `SCRIPT_ERR_TYPE`). `AssignResolved`
  (`firmware/src/Core/Services/ScriptVm.h:191-204`) has two branches: numeric source and numeric
  destination convert through `ToScalar`, everything else raw-copies `min(Ssize, Dsize)` bytes and
  zero-fills the remainder, returning 0. So a Matrix destination taking a 1-byte Bool row writes that
  byte plus zeros and reports success; a Matrix row into a Number destination is likewise a padded
  copy, not a type error. The only genuine `SCRIPT_ERR_TYPE` a read could produce came from
  `DecodeScalarInt`'s `Number` size check (fewer than 4 bytes) - which in practice meant the size-0
  row the card above removed. Two footnotes: that `Bool` case reads `Data[0]` without consulting
  `Size` at all, so a 0-length Bool row used to copy an uninitialized stack byte into a numeric
  destination (unreachable through a register read now that `RegisterGetByBlockInfo` refuses an empty
  row, but reachable from any other 0-length source); and if a mismatch really should be reported,
  the decision belongs beside `ValueReadable`/`ValueUpdateApplicable` in `MemoryTypes.h`, not in a
  branch of a 200-line assignment.

- **E51** **`Docs/App/Backup.md` does not describe the created / re-created rows a restore reports.** Since
  `t_ef2b1461` (owner defect, measured on the rig 2026-10-25: `Dynamic[0]` f4 k0 was a `Number` at 1
  byte where the archive declares `Bool` at 1, and the emote selector's register read of it landed as
  `SCRIPT_ERR_TYPE`) a restore makes a dynamic block's structure in the archive's DECLARED type and
  size, re-creates a row the device holds in another `(type, size)`, leaves zero-length dictionary
  markers alone, and reports each structure row as created or re-created - per row in the result
  screen and the copied text, and in `RestoreReport.metadata` (`created`, `recreated`, `structure`).
  The counts line is now `N written · A created · B re-created · C skipped (structural) · D failed`
  (the two middle parts appear only when non-zero, and a created row whose value is then written is
  counted in both). Proposed wording, for the parent to apply with Akyirr's approval - Docs are off
  limits to workers: in `Docs/App/Backup.md`'s restore paragraph, after the report sentence E49
  already covers, add: "A dynamic block's structure is made in the type and size the archive
  declares; a row the device holds in another type or size is re-created rather than written into,
  and the run names every row it created or re-created. A dictionary field's key-0 marker has no
  value to write and is left as structure, never written."

- **E52** **`Docs/Services/Subscriptions.md:80` still says the requester writes the derived `Bool`
  into the target, which the owner's pulse ruling supersedes.** The doc's edge bullet reads "The
  requester writes the derived `Bool` into the target; the counter is never stored as a value" - the
  counter half is exactly right and unchanged, but the value half now describes the old behaviour.
  Since `t_431b1125` (owner ruling, 2026-10-27: "an edge-mode requester clears its output if another
  edge was not detected") the target is a PULSE: an edge update is worth one `true` main-loop pass
  per new edge of the trigger's direction and one `false` pass after them, consecutive, and a target
  with no edge stays `false` instead of holding a level. Parity still describes the COUNTER (odd =
  pressed, even = lifted) and is what lets the requester count the edges of a direction; it is not
  written anywhere. Proposed wording, for the parent to apply with Akyirr's approval (Docs are off
  limits to workers): replace that sentence with "The requester writes a pulse into the target: one
  `true` main-loop pass for each new edge of the subscribed direction (a jump of `n` = `n` passes,
  consecutive) and `false` on the following pass, so a target with no edge reads `false`. The counter
  itself is never stored as a value."

- **E55** **The ADC is clocked at 24 MHz, above the 14 MHz the CH32 ADC allows - and the `t_1e134fcc`
  diet removed the only code that could have reported it.** `Devices/DAS_v0.1/Measuring.h:69-108`
  enables the ADC peripheral clock (`RCC_APB2Periph_ADC1`, line 71) but never programs the ADC clock
  divider: nothing in `firmware/src` calls `RCC_ADCCLKConfig`, and the built image confirms it (no
  `RCC_ADCCLKConfig` symbol; the only writers of `RCC->CFGR0` are `SystemInit`'s `RCC_DeInit`
  (`CFGR0 &= 0xF8FF0000`, which zeroes bits 0-15 - ADCPRE included) and the SW/PLLSRC writes, so no
  write in the image ever puts a non-zero value in ADCPRE). `ADCPRE[4:0]` (`CFGR0[15:11]`, mask
  `0x0000F800`) therefore ends at 0, which the SPL's own decode maps to `/2`
  (`ch32v00x_rcc.c:462-476`, `ADCPrescTable[0] = 2`) -> ADCCLK = PCLK2/2 = **24 MHz**. The in-repo reference manual states the limit twice
  (`datasheet/CH32VRM.pdf` p41 "ADC clock shall not exceed 14MHz at most", ADC chapter "Max=14MHz");
  that chapter is the V20x/V30x family, the V003's own sheet is not in the tree, so confirm the V003
  number before acting. Not a regression from the diet - the field was never written, before or
  after - but the diet did delete the reading that would have shown it: the removed
  `RCC_GetClocksFreq` computed exactly `ADCCLK_Frequency = 24 MHz` for this build. Proposed fix
  (needs an owner decision + the rig): one `RCC_ADCCLKConfig(RCC_PCLK2_Div4)` in `MeasuringInit()`
  before `ADC_Init` -> 12 MHz, inside the limit, and it doubles the sample window (73 ADCCLK = 6.1 us
  instead of 3.0 us), which is the right direction for the 330 kΩ range's source impedance. Cost:
  ~10 B of flash for the read-modify-write (456 B spare). Nothing here can be measured without the
  two nodes, which cannot be attached to the programmer.
  **FIXED in code (`t_26e15feb`, owner-approved 2026-10-27):** `MeasuringInit()` now calls
  `RCC_ADCCLKConfig(RCC_PCLK2_Div4)` before `ADC_Init`. The built image shows the read-modify-write
  (`2f08 lw a5,4(s0)` / mask `0xFFFF07FF` / `or a5,a5,s1` with `s1 = 0x4000` / `sw a5,4(s0)`), i.e.
  bit 14 set = ADCPRE `01` = PCLK2/4 = **12 MHz**. Measured cost: **+40 B** of flash (`0x3C60` ->
  `0x3C88`), leaving 376 B spare; the image is now `0x3C70` / 13424 B of 13824 after E56, **400 B
  spare** (not 456 - read the ELF, not an earlier note). Conversion window is now 73/12 MHz = 6.08 us
  (was 3.04) and TCONV = (73 + 11)/12 MHz = 7.0 us (was 3.5). Still unproven on hardware: an
  over-clocked ADC and a settling window both need a real divider on a measuring node.

- **E56** **The rail flags (fields 5/6) are computed from the EMA output, so a heavy Filter
  Coefficient hides a rail for seconds.** `Devices/DAS_v0.1/MeasuringRun.h:117-118` passes
  `FilteredRaw` to `MeasOpenCircuit`/`MeasShortCircuit`, while `Docs/Modules and Blocks/Measurement.md`
  describes the flags as "the input ... at the upper/lower limit of the ADC window", and the
  auto-range in the same function already judges the RAW sample. On a channel that is already on the
  330 kΩ range an open circuit does not switch ranges, so nothing re-seeds the filter: with
  FilterCoeff 0.5 the flag appears after ~9 samples (~0.9 s at 10 Hz), and with 0.02 after ~300
  samples (~30 s) - long after the value has railed, and symmetric on recovery. Fix (not applied):
  pass `Number(Raw)`; `test/native/run.sh:600-606` pins the literal `FilteredRaw` argument, so that
  guard has to move with it. Behaviour change at FilterCoeff < 1, so it is the owner's call.
  **FIXED in code (`t_26e15feb`, owner-approved 2026-10-27):** both call sites pass `Number(Raw)`;
  the `run.sh` guard now pins that literal AND fails if `FilteredRaw` reaches either predicate
  (negative control: reverting the two lines fails all three assertions). It also **shrank** the
  image by 16 B (`0x3C60` -> `0x3C50`, measured alone), so with E55 the net is +16 B / 400 B spare.
  Still unproven on hardware: a real open and a real short on the two measuring nodes.

- **E57** **The Valu ADC was clocked at 72 MHz - 5.1x the 14 MHz `fADC` maximum - because neither
  prescaler on its measuring path was ever programmed.** `Devices/Valu_v2.0/Measuring.h`'s
  `MeasuringInit()` enabled `ADC1` (`RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA |
  RCC_APB2Periph_ADC1, ENABLE)`, line 44) and never wrote the ADC clock: no `RCC_ADCCLKConfig` and
  no `RCC_PCLK2Config` call existed in the Valu env at any point before `a131de9` (both symbols first
  appear in that commit, `git log -S` on `Devices/Valu_v2.0/`). Both fields therefore sat at their
  reset values - the framework's `SetSysClock` paths all OR in
  `RCC_PPRE2_DIV1` (`System/ch32v20x/system_ch32v20x.c`, every branch), so `PPRE2` = `000` = HCLK/1
  -> **PCLK2 = 144 MHz**, and `ADCPRE` = `00` = PCLK2/2 (`ADCPrescTable[4] = {2,4,6,8}`,
  `Peripheral/ch32v20x/src/ch32v20x_rcc.c:72`) -> **ADCCLK = 72 MHz** against the part's 14 MHz
  ceiling (`datasheet/CH32V203.pdf` Table 4-27 ADC characteristics, `fADC` max 14 MHz; `CH32VRM.pdf`
  "ADC clock shall not exceed 14MHz at most"). Unlike the DAS case (E55) the `/8` tap alone cannot
  fix it: the v20x divider only offers /2,/4,/6,/8, so from a 144 MHz PCLK2 the slowest tap is still
  **18 MHz**, 4 MHz over the rating. The APB2 prescaler has to move as well, so every conversion the
  Valu measuring path has made so far ran 5x over spec.
  **FIXED in code (`a131de9`, reviewed on `t_b4d0623d` 2026-10-27):** `MeasuringInit()` now calls
  `RCC_PCLK2Config(RCC_HCLK_Div2)` then `RCC_ADCCLKConfig(RCC_PCLK2_Div8)` immediately before
  `ADC_Init`/`ADC_Cmd` (`Measuring.h:80-81`), i.e. `PPRE2` = `100` -> PCLK2 **72 MHz** and `ADCPRE` =
  `11` -> ADCCLK **9 MHz**. The image confirms both writes and their order: the `CFGR0`
  read-modify-write sits at `0x9634`-`0x9650` (mask `0xFFFFC7FF` then `or 0x2000` for PPRE2; mask
  `0xFFFF3FFF` then `or 0xC000` for ADCPRE, matching `CFGR0_PPRE2_Reset_Mask` /
  `CFGR0_ADCPRE_Reset_Mask` in the SPL), and the first `ADC1` access (`0x40012400`) follows at
  `0x9652`. What the PPRE2 halving does not cost: `TIM1CLK` stays `2 x PCLK2` = **144 MHz** whenever
  the APB2 prescaler is not 1 (clock tree, "if(APB2 prescaler=1) *1 else *2"), so the period `PWM.h`
  derives from `SystemCoreClock` (= HCLK = 144 MHz) still matches the fan timer; HCLK, SysTick and
  `USBCLK` (from the PLL) are untouched. It does cost APB2 register accesses 2 HCLK cycles instead
  of 1 - APB2 users here are only GPIOA/GPIOB/TIM1/ADC1, none of them bandwidth-bound. Timing at
  9 MHz: the 239.5-cycle sample window is 26.6 us, TCONV = 250.5/9 = 27.8 us, all three channels in
  one pass ~83 us (8.3 % CPU at the 1000 Hz Sampling Rate clamp), and `ADC_EOC_TIMEOUT_CYCLES`
  counts HCLK cycles, so the timeout is unaffected. Cost **+36 B** of flash: `43124` -> `43160` of
  `45056` (`.text` `0xa5dc` -> `0xa600`, bin `43128` -> `43164`, image end `0xD878` -> `0xD89C`,
  1892 B spare), RAM unchanged at `18116`/`20480`. Those figures come from the reviewed build on
  `t_b4d0623d` (fixed image against a control build of `a131de9^`), not from a build made for this
  entry. **No hardware proof exists:** nothing has been flashed on the Valu board and HIL has not
  been run, so neither the 9 MHz conversion accuracy nor the unchanged fan PWM has been observed on
  the rig - that needs a real divider on a measuring node, and the fan PWM compared against the
  pre-fix build.

## BLE Settings block (2026-10-28)

**2026-10-29, owner ruling "simplify the BLE block" - the block is SIX fields, and this note is the
authority for their numbers.** Card `t_41038b88`, image `26.10.29.3`, read back over USB on the rig
(`firmware/.pio/probe/bleset6.py`): block `BLE Settings`, type `0x0B`, `fields=6`, `EnumerateFields`
answers exactly `00 01 02 03 04 05` - fields 6..12 get no reply at all.

| Field | Index | Type | Declared size | Flags |
| --- | --- | --- | --- | --- |
| Enabled | 0 | `Bool` | 1 | Persistent, Trigger |
| Passkey | 1 | `Text` | 6 | Trigger |
| Mode | 2 | `Enum` | 1 | RO |
| Display name | 3 | `Text` | 16 | Persistent, Trigger |
| Bonds | 4 | `Undefined` | 66 | Trigger |
| Window | 5 | `Uint32` | 4 | Trigger |

- **`Bonds` is one value, not a table.** `BLE_SETTINGS_BOND_SLOTS` = 3 entries of
  `BLE_BOND_ENTRY_LEN` = 22 B (6-byte identity address + 16-byte peer name), so the declared size is
  `BLE_BOND_ARRAY_LEN` = 66 (`static_assert`ed against slots × entry in
  `firmware/src/Blocks/BleSettings.h`). The address is in the order it is printed
  (`AA:BB:CC:DD:EE:FF` = bytes 0..5), an **all-zero address is an empty slot**, and the name is
  16 bytes NUL-padded with no terminator guaranteed. The 16-byte name width is the device's name
  width, shared with `Display name`.
- **What a write can and cannot do:** an entry's name is writable; zeroing an address deletes that
  bond (the peer must pair again inside a window); a non-zero address the store does not hold in
  that slot is **refused as a whole write** - `FLAG_FAIL` with an empty payload, nothing applied,
  names included - because the keys (LTK/IRK) live in NVS and never in the register file, so a
  fabricated entry could never authenticate. A `Bonds` write shorter than 66 B never reaches the
  trigger: a non-Text field must match its declared size (`Core/Functions/MemoryTypes.h:210`). A
  read pads the value to 4 bytes, so 66 arrives as 68 - use `ValueInfo.Size`.
- **`Display name` IS the advertised name, live**, not "at the next advertising start": a write
  rebuilds and restarts the advertisement in the same loop pass, so the air and the field cannot
  disagree; an empty value means the System `Name`. Measured on the air with a timestamped scan
  (first advertisement after the write carried the new name, uptime continuous, no reboot).
  `Advertised name`, `Action` and `Last peer` / `Last air` / `Last link` no longer exist.
- **`Window` is a read-out and a command**: it reads the milliseconds left in the connectable
  window and a write sets the deadline (write the ms you want, 0 closes). Forgetting every bond is
  zeroing the `Bonds` array. Every write to the block, `Window` included, is still refused unless
  the frame arrived over USB (`BleBlockWritable`).
- **Every entry below that measures a build older than `26.10.29.3`** (E58–E66, E68–E71) quotes the
  twelve-field block's field numbers, `Bond count`, keyed `Bond` texts, `Action` and the `Last *`
  read-outs. That is the record of how the block got here - history, not the current block. Where an
  entry's field numbers still matter (E58), the correction is in place.

**E58 The `BLE Settings` block's field numbers are not in the docs.** `Docs/Services/Register.md:24`
keeps the `Static block types | ...` placeholder, so the block-type list numbers neither `0x0B` nor
`0x0C`. `03a585b` has since added a `BLE Settings` section of type `0x0B`
(`Docs/Services/App Interface.md:29`, table at `:31-40`), but its rows carried only key `0` (Bond
`0..N`) and no field index, and the twelve-field block the firmware shipped carried an `Advertised
name` field at index 8 for which the doc had no row at all, so the numbers a client was to address the
block with were only the firmware's - and in a different order from the section's rows. The
block was specified by the owner as a card (t_a9cda0b9) and implemented from that table, with field
numbers fixed in `firmware/src/Blocks/BleSettings.h`.
*(2026-10-29, card `t_41038b88`: the numbering this entry used to quote - `0 Passkey … 8 Advertised
name`, then `5 Action`, `6 Enabled`, `7 Display name` - is the twelve-field block and is history. The
six fields a client addresses today, and the only numbers that matter, are the table in the note at
the head of this section: **0 Enabled, 1 Passkey, 2 Mode, 3 Display name, 4 Bonds, 5 Window**, with no
`Advertised name` row to add because `Display name` is the advertised name. The doc change still owed
is spelled out under "Doc change owed" below.)*
Every write to the block is refused unless the frame arrived over USB.
Proposed doc change (parent only): the field indexes on that section's rows (stated the way the
System block table states its field numbers) - see "Doc change owed" below - and both static types
added to the block-type list. Not a code defect - the app's registry entry reads the same numbers
from the header (card `t_c432c8ac`).

**E59 `NimBLEDevice::setScanFilterMode()` does not restrict who may connect.** Card t_367f86d0
named it as the connect filter to apply while the pairing window is closed. In esp-nimble-cpp 2.5.0
it sets the *scan duplicate cache* filter type (`NimBLEDevice::setScanFilterMode` -> `m_scanFilterMode`,
`NimBLEDevice::init` rejects a call after init) and has nothing to do with connection admission. The
connect filter is `NimBLEAdvertising::setScanFilter(scanWhitelistOnly, connectWhitelistOnly)`, which
`AppBLE.h` applies as CONNECT-only: whitelist-only while no window is open and a bond is stored, off
while a window is open or the store is empty (see E64 for the advertising rule itself). No doc impact.

**E60 The NimBLE bond store keeps no peer name, so a Bond entry is an address only.** Card
t_a9cda0b9's example entry is `C4:7F:51:xx:xx:xx  Akyirr-phone`. `NimBLEDevice` exposes
`getNumBonds()`, `getBondedAddress(i)` and `deleteAllBonds()`; no name is stored (NimBLE keeps
none), so the field carries `AA:BB:CC:DD:EE:FF` and the app must not promise a name.
*(2026-10-29, the six-field block: the entry is still `address + name`, but the NAME half is now ours
and writable - `Devices/Tamu_v2.0A/BleBondLabelStore.h` keeps a label per identity address in NVS
(`ble`/`bondnames`), keyed by the address so eviction or a delete cannot hand a label to another
phone, and pruned to the bonds that exist. A second phone has no name until the host writes one, and
an empty name is the honest reading of "never labelled".)*

**E64 A phone-only setup cannot open a pairing window, so a bonded device had to be reachable
without one (owner ruling C, 2026-10-28).** Every write to the `BLE Settings` block is refused
unless the frame arrived over USB (`Devices/Tamu_v2.0A/BleSettingsGlue.h`, `BleBlockWritable`),
`Action = 0` included - an earlier comment on card t_1d304c9c said the open window would be accepted
from any link, and the final firmware says otherwise. The core's button is deliberately not a
trigger either, so a device that held a bond, had its window closed and had no USB host available
was simply off the air: it advertised only inside a window, and opened one by itself only while its
bond store was empty. Ruling C (2026-10-28), in the owner's simplified form - "it advertises
whenever no device is connected via BLE" - splits the question in two: advertising is ONE condition,
`BleAdvWanted()` = radio up and enabled (no bond term, no window term); who may CONNECT is a
separate rule, `ConnectFilterWhitelistOnly()` = no window open AND a bond stored.
- **no session, no window**: advertising runs, with bonds or without. With a bond stored the CONNECT
  filter is the controller's whitelist, so the bonded phone reconnects and a stranger's connect
  request is ignored in the link layer - the advertisement and its scan response stay unfiltered, so
  the name is still visible to a scan. (**Superseded by E66, 2026-10-28:** the accept list cannot
  match a peer under LE privacy on this controller, so it is not used at all - the bonded peer is
  admitted by the session gate and a stranger's link is refused after the CONNECT_REQ is answered.
  See E66's fix note.) With the store EMPTY there is nothing to protect: the filter
  is relaxed and the connect gate (`onConnect`) admits an unknown peer, which is how the first bond
  is made with no USB host - and exactly what `Action = 1` returns the device to. The five-minute
  recovery window still opens by itself on an empty store, but pairing no longer depends on it; it
  makes the device obviously discoverable and keeps the `Window` field meaningful in that state.
- **window open**: the filter is relaxed (`BLE_HCI_ADV_FILT_NONE`), so a new phone may connect and
  pair up to the three bonds the store holds, and bonded peers are still admitted.
- **window closes**: the advertisement is rebuilt inside the same loop pass with the whitelist
  filter (the whitelist is re-synced from the store first) - it is never taken off the air, so there
  is no gap to recover from.
- **connection up**: the controller stops advertising when the link comes up, and the deferred
  restart puts it back on the first loop pass after the link drops, carrying the filter in force at
  that moment (bonded-only if a window has since closed).
NimBLE mechanism: `NimBLEAdvertising::setScanFilter(false, true)` sets
`m_advParams.filter_policy = BLE_HCI_ADV_FILT_CONN`, which `NimBLEAdvertising::start()` passes to
`ble_gap_adv_start()` - the policy is read at start only (`start()` returns early on an already
active advertiser), which is why every (re)start in `StartAppAdvertising` applies it and why a
change of the filter answer rebuilds the advertisement. The list it matches is the controller
whitelist programmed by `NimBLEDevice::whiteListAdd/Remove` (`ble_gap_wl_set`), and its entries are
IDENTITY addresses: the phone connects from a rotating private address, and this build resolves it in
the controller from the peer IRK it stored - `ble_hs_pvcy_add_entry` and
`ble_hs_pvcy_ensure_started` are linked and `ble_hs_resolv` is not (checked with `nm` on
`firmware.elf`), i.e. controller-based privacy, which is what lets a whitelist match survive RPA
rotation. `AppBLETick` rebuilds the whitelist from the store whenever `BleBondGeneration` moves - and
does it before re-aiming the filter - so a bond the store evicted on overflow does not stay admitted
and a filter that tightens to whitelist-only never finds an empty list.
Revocation on a passkey change, the policy-shaped pairing deadline, `Action = 1` clearing the bonds
and the USB-only write rule are untouched. The block's `Window` field keeps its meaning - milliseconds
left in the PAIRING window, 0 when closed - so a client must not read 0 as "the device is off the
air": 0 means no pairing window is open, and with a bond stored that is "a bonded phone may connect,
a new one may not", while with an empty store a new phone may still pair. Consequences for the app
(card t_1d304c9c, item 5): its "the window opens only over USB" statement stays true and stops
mattering for a bonded device; what remains unrecoverable is a phone whose bond is stale (forget it
in the phone's settings) - there is no longer a device that needs USB to be reachable.
**Rig check, 2026-10-28 (build 26.10.28.27, card t_4fc81fc7, host hci0 as the central):** the
advertising rule and the empty-store path behave as described. The device is visible in a scan with
0 bonds and with 1 bond stored and no window open (rssi -37/-38, name `Akyirr`, NUS UUID); it stops
advertising while a link is up (0 sightings in an 8 s scan taken during the connection) and is back
on the air within 3 s of the drop (6 sightings); `Action = 0` still pairs a new peer, Just Works with
`000000` and passkey/MITM with a code (a wrong code is refused, and a pairing left unanswered is
dropped 30.4 s after the connect). Two things did NOT hold on the air: the connect filter does not
admit the peer it is meant to admit (E66), and the bond forget cannot run on an idle radio, which
also disables the passkey-change revocation (E65).

**E61 `BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN` cannot be carried by a NimBLE-cpp characteristic.**
It is `0x10000` and `NimBLECharacteristic`'s `properties` argument is `uint16_t`, so the notify
path cannot ask for authentication. `NOTIFY_INDICATE_ENC` is used instead: with a passkey set,
MITM pairing makes encryption and authentication the same event, and `AppBLE.h`'s connect gate
re-checks `isAuthenticated()` on every link, so an unauthenticated peer cannot reach the notify
path either way.

**E62 The BLE passkey is not part of `.SV`, so a backup/restore or a setup upload does not carry it.**
The passkey lives in NVS (namespace `ble`, key `passkey`) rather than in the persistent register
space, because `.SV` sits in the `storage` partition and the Storage service streams files out by
name - a persistent field is not a place for a secret. Consequences the app side (t_1d304c9c) has
to live with: `Passkey` reads back as the stored value or `000000`, but it is never in a `.SV`
mirror, so an archive that copies `.SV` restores `Enabled` and `Display name` and leaves the code
where it was; and a storage `Format()` wipes `Enabled`/`Display name` while NVS keeps the passkey.
No code defect - recorded so the asymmetry is not mistaken for a missing persistence flag.

**E65 The bond forget cannot run while the device is advertising: `Action = 1` is refused, and a
passkey change revokes nothing until the next boot - while a reconnect in between erases the
evidence.** Found on the rig (build 26.10.28.27, card t_4fc81fc7). `AppBLEForgetAllBonds()` deletes
through `NimBLEDevice::deleteAllBonds()` -> `ble_gap_unpair()`, and NimBLE refuses that call while
advertising or discovery is active (`ble_gap.c`: "We cannot delete entry from resolving list if
there is ongoing discovery or advertising in progress", `BLE_HS_EBUSY`). This firmware advertises
whenever no session is up (E64), so that is the normal state:
- `Action = 1` over USB with no link: reply `FLAG_FAIL` (flags `0x4E`), store unchanged (Bond count
  1 before and after). The same write succeeds while a link is up (1 -> 0, block Bond texts
  cleared) and deterministically with `Enabled = false` - both only because advertising is off.
- a passkey write (`000000` -> `123456`) with the radio up: `AppBLEEnforceBondPolicy()` calls the
  same forget, it fails the same way, and the epoch is not stamped (correctly). The old bond kept
  full authenticated access afterwards (a complete BLE register read with the pre-change bond).
  The next boot does revoke when the epoch still disagrees (`passkey changed since the bonds were
  made: forgetting 1 bond(s)` / `bonds forgotten: 1 -> 0 (ok=1)`), but any session in between
  re-stamps the epoch with the current code from `onAuthenticationComplete`
  (`AppBLEBondEpochStamp`), which silences that retry - observed: after the change plus one
  authenticated session, the next boot logged `radio up ... bonds=1` and nothing else.
Fix direction: stop advertising (and restart it) around the unpair, or delete through the store API
(`ble_store_util_delete_peer`) instead of the GAP procedure; and do not re-stamp the epoch from a
link authenticated with a bond older than the current code.
**Answered and fixed (2026-10-28, card t_1b380cee).** The forget clears the air itself instead of
being refused by it. `AppBLEForgetAllBonds()` asks the stack whether advertising is up
(`NimBLEAdvertising::isAdvertising()` -> `ble_gap_adv_active()`), stops it (`ble_gap_adv_stop` is a
synchronous HCI command, so the controller is idle when it returns), deletes through
`deleteAllBonds()` -> `ble_gap_unpair()`, and then clears `BleAdvStateApplied` - which is the tick's
own request to rebuild and restart the advertisement, in the same loop pass, because the register
write and `AppBLETick()` both run from the app task (`AppInterface::Pump` dispatches the queued
frame and then calls the tick). Both halves are gated on an advertisement that was actually up, so
the paths that never needed the stop (a live link, a disabled radio) lose nothing.
- The GAP procedure is kept on purpose. Deleting through the store API
  (`ble_store_util_delete_peer`) needs no air, but it also skips `ble_gap_unpair`'s resolving-list
  removal, and a revoke that leaves the peer's IRK in the controller is not a revoke - the
  forgotten phone would still be resolvable. The stop costs microseconds.
- The passkey change goes through the same function, so `AppBLEEnforceBondPolicy()` -> the forget
  now revokes inside the loop the passkey is written in (`AppBLEApplySecurityPolicy`) instead of
  failing with `BLE_HS_EBUSY` and leaving the epoch unstamped for a boot-time retry a session could
  silence. The epoch rule is unchanged: the stamp still follows the STORE (only `After == 0`), so a
  partial delete leaves the mismatch for the next radio start. (**Corrected by E68, 2026-10-28:** the
  claim that followed here - "no change was needed in `onAuthenticationComplete`'s stamp: a link that
  reaches it authenticated can only have been bonded under the code in force" - is false. A bond made
  under the old passkey authenticates from its stored LTK, and stamping on that reconnect is what
  silenced the retry in the observation two bullets above. The stamp is gone from that path.)
- The reply follows that same state, not the per-entry rc: the function returns `After == 0`, so
  `Action = 1` cannot answer `FLAG_FAIL` over a store that reads back empty (a repeat `Action = 1`
  on an empty store is a success, not a refusal). `ok=<deleteAllBonds rc>` stays in the log.
Native: the E65 source guard in `test/native/run.sh` (the stop precedes the delete, the restart
request follows it, both gated on advertising being up) - RED against the pre-fix `AppBLE.h`, GREEN
after. Rig-verified on 2026-10-28 (build 2026.10.28 iteration 39, card t_3215094d, cable attached,
host hci0 as the central): with one bond stored (`Bond[0] = 4C:23:38:74:E6:F6`) and `Window 0` while
the device was advertising, `Action = 1` answered `flags=0x0E` (no `FLAG_FAIL`) in 15 ms, `Bond
count` read 1 -> 0 with the Bond texts cleared, the air carried `Akyirr` again 0.010 s after the
reply, and the old peer's stored key no longer authenticated (`LE.Disconnected -
org.bluez.Reason.Authentication, Connection terminated due to authentication failure`). The
device's own lines from the same session: `bonds forgotten: 1 -> 0 (ok=1)` and `advertisement
returns: the bond delete is done (E65)`. A fresh pair after the forget still succeeds (the window
re-opens), so the stop/restart leaves the device repairable.

**E66 The whitelist-only connect filter refuses the bonded peer it exists to admit.** Same build and
card. With one bond stored and the window closed (boot log: `connect filter: whitelist-only=1`,
`radio up ... bonds=1 whitelist=1`), the bonded central's connect times out (bleak `TimeoutError`,
no link-layer answer) while the device keeps advertising (seen in a scan immediately after). With a
window open the same peer connects in 1.4-1.9 s and the stored bond authenticates (Bond count
unchanged, full BLE register read). The bond and the whitelist entry are the identity address
(`Bond[0] = 4C:23:38:74:E6:F6`, the central's public address) while the central connects from a
resolvable private address under controller privacy (`ll-privacy` on; the image links
`ble_hs_pvcy_*` and has no `ble_hs_resolv_*`, i.e. controller-based privacy), so the controller's
accept list does not match the connect address. Because the filter drops everyone alike, "a bonded
peer reconnects with no window" and "a stranger is refused" cannot be told apart on the air today.
Next check: an accept-list/privacy read-out (or a boot-time log naming the list) - the device log is
not usable for this, see E67.
**Answered and fixed (2026-10-28, card t_31e9201a).** The open question - does the device
controller's resolving list hold the peer's IRK - is **yes**: the list holds it and the RPA does
resolve, and the accept-list *filter* is the defect.
- The peer's IRK is in the bond and the controller resolves the RPA. With a window open (no filter)
  the same central connects from an RPA and the stored bond authenticates - and that is only
  possible if the connection reports the peer's IDENTITY address: `ble_sm_ltk_req_rx` looks the LTK
  up by `ble_hs_conn_addrs`' `peer_id_addr` and `ble_sm_retrieve_ltk` matches (type, address)
  exactly against `our_sec`. An RPA cannot match a bond stored under `4C:23:38:74:E6:F6`, and the
  host has no resolver to rewrite the address, so the controller must have resolved it (the
  connection-complete event reports a resolved peer as types 2/3; `ble_hs_conn_addrs()` normalises
  that type to 0/1 on `peer_id_addr` and puts the peer's RPA on `peer_ota_addr`).
- The accept list is not resolved against. The failing boot is silent exactly where a failed
  resolving-list add is loud (`ble_hs_misc_restore_one_irk` logs `failed to configure restored IRK`,
  and no such line appears with `bonds=1`), so the RL entry was added; the advertisement carried
  `adv_filter_policy=2` with `entry-0={addr_type=0 addr=4c:23:38:74:e6:f6}` (NimBLE GAP log, build
  26.10.28.27) and the connect request was still ignored - the one HCI input that differs between
  the failing run and the working one.
- Capability is not the limit: the C3 controller implements the whole privacy surface
  (`hci_le_rd_rslv_list_size_cmd_handler` answers size **10**, `hci_le_add_dev_to_rslv_list_cmd_handler`,
  `hci_le_set_addr_resol_en_cmd_handler` storing the enable flag at `llm_env+198`,
  `hci_le_set_priv_mode_cmd_handler`, `lld_peer_rpa_res`), yet the RPA-resolving white-list helper
  `lld_wl_rpa_res` is referenced exactly once in the whole image - from
  `hci_le_rmv_dev_from_wlst_cmd_handler`, not from the connect-request path.
**Fix, one line:** the accept list is gone (nothing sets an advertising filter policy, nothing is
mirrored into the controller's white list); a link becomes a session only when it is encrypted AND
bonded with a bond that existed before the link, and a pairing on a link with no window may not
create one - so the bonded peer gets in and a stranger's link is dropped at the security procedure
with the bond it just made forgotten. The cost, stated rather than hidden: a stranger's CONNECT_REQ
is answered before the link is refused, so the drop is no longer in the link layer (E64's wording) -
the advertisement and its scan response were unfiltered in either design, so nothing new is visible.
Native: `test/native/ble_admission_test.cpp` (identity compare across the two type spaces, who may
bond, who may hold a session) plus the E64/E66 source guard in `test/native/run.sh`. Rig half - one
bond and `Window 0` connecting and reading a field, a stranger getting no session - is card
t_98acbebd and is unverified here: flashing was not authorised in this card.

**Corrected premise (review round 1, 2026-10-28).** The "the accept-list *filter* is the defect"
answer above still rests on a SOURCE fact that holds - decoded from `libbtdm_app.a`, the
RPA-resolving accept-list helper `lld_wl_rpa_res` is referenced only from
`hci_le_rmv_dev_from_wlst_cmd_handler`, never from the connect-request path - but the RIG measurement
cannot carry it. Today this host connects from its PUBLIC address (`Last peer` = `Last air` =
`4C:23:38:74:E6:F6`), and the pre-fix accept-list entry was
`entry-0={addr_type=0 addr=4c:23:38:74:e6:f6}` - exactly that address - so the iteration-27 timeouts
are NOT explained by the accept list on this rig. What the measurement does prove is narrower and
still the operational point: with a filter in force the peer was not admitted, with the filter gone
it is admitted 3/3 (bond count unchanged). The link-layer resolution gap stays a source fact, not a
rig measurement.

**The evidence half (2026-10-28, card t_0b1cd623).** The mechanism is settled and its cost stated:
the fix is the DEVICE-SIDE identity gate, not a programmed resolving list, because the resolving-list
route is not available on this silicon - the controller resolves an RPA for the *connection* (which
is how the stored bond authenticates) but `lld_wl_rpa_res` never runs on the connect-request path, so
an accept-list entry on the identity cannot match a privacy-enabled central. Programming the list is
already automatic and needs no app code: `ble_hs_misc_restore_irks()` adds every bond whose `peer_irk`
is present (`ble_hs_pvcy_add_entry()` -> `HCI_LE_Add_Device_To_Resolving_List` +
`HCI_LE_Set_Privacy_Mode(device)`), and the peer MUST distribute its IRK (SMP IdKey) - the host does,
which is why the resolution happens at all. The cost of the device-side gate is the one already
stated: a stranger's CONNECT_REQ is answered and the link refused at the security procedure instead of
being ignored in the link layer.
**Read-out.** `BLE Settings` gained three read-only fields: `Last peer` (9, the identity the gate
compared with the bond store, `ConnInfo.getIdAddress()`), `Last air` (10, the address the peer
connected from, `ConnInfo.getAddress()`), `Last link` (11, the verdict - 0 none, 1 pending, 2
admitted, 3 refused: not a bond, 4 refused: paired outside the window, 5 refused: unsecured, 6
refused: busy). The two addresses differ exactly when the controller resolved an RPA
(`ble_hs_conn_addrs()` keeps the event's identity on `peer_id_addr` and its peer-RPA on
`peer_ota_addr`), so the pair IS the resolution, and the verdict separates "refused as a stranger"
from "the address was not recognised" - the gap this entry was opened for. It has to be a register
read rather than a log line: E67.
**Measured on the rig (2026-10-28 19:20-19:40 UTC, core on USB, image `26.10.28` iteration 27 - the
build BEFORE the fix; the fix is iteration 34 in the tree).**
- `btmgmt info`: one controller, `current settings: ... ll-privacy ...` - the peer host runs LE
  privacy, as this entry says.
- Paired the host inside a window opened over USB (`pairing SUCCEEDED after 14.1 s`, Just Works); the
  block then read `Bond count 1`, `Bond[0] = 4C:23:38:74:E6:F6` (the host's own public address) and
  `Window 30984`.
- A connect 40 s later took 0.60 s - the advertisement had been rebuilt while the window was still
  open, so it carried no filter. ~3 minutes on, with the window closed and the advertisement rebuilt
  since, two connects timed out (bleak `TimeoutError`, no link) while a scan still showed `Akyirr`
  advertising and the bond store still read 1. That is this entry's symptom, reproduced on demand.
  (The filter's presence lagging the window is the consistent explanation - it is decided when the
  advertisement is rebuilt - but it is an inference: the log is unreadable, see E67.)
- A second identity cannot be produced on this host: `btmgmt static-addr 42:11:22:33:44:55` answers
  `Set static address failed with status 0x14 (Permission Denied)` and `btmgmt privacy on <irk>`
  answers `Set Privacy for hci0 failed with status 0x14 (Permission Denied)`, with a single
  controller. The "a genuine second IRK is refused" half is therefore UNPROVEN, not passed.
- The acceptance itself needs the fixed image on the device and flashing was not authorised in this
  card. Recipe: `firmware/.pio/probe/ble_lastlink.py --connect 3 --addr <ADV ADDR>` (prints
  `Last peer` / `Last air` / `Last link` after every link).

**Measured with the fix ON THE DEVICE (2026-10-28 21:09-21:20, core on USB, `Tamu_v2_0A` iteration 39
and then 43 - the fix is iteration 34; card t_0b1cd623, owner-authorised flashing).** The device was
already running a fixed image (the E65 card flashed iteration 39), so nothing had to be forced: the
running image was proven to be the tree's image by reading flash 0x70000 back and comparing it
byte-for-byte with `.pio/build/Tamu_v2_0A/firmware.bin` (`cmp` silent, sha256 `6b4c823e…`). The
type-carrying read-out below was then built and flashed once (iteration 43, `cmp` silent, sha256
`985fe306…`, read-back `Read 687296 bytes`).
- **The bonded central reconnects with the window CLOSED.** Block: `Bond count 1`,
  `Bond[0] = 4C:23:38:74:E6:F6`, `Window 0`. Three reconnects (`ble_lastlink.py --connect 3`):
  each `Last peer=4C:23:38:74:E6:F6/0 Last air=4C:23:38:74:E6:F6/0 LastLink=2 (ADMITTED)`, bond count
  still 1 afterwards. On the pre-fix image the same peer timed out twice in exactly that state.
- **The device's own log names the identity** (E67 ring, read within a second of the link; the image is
  `462b89a`'s tree - iteration 43, the one recorded at the head of this section, 687,296 B, sha256
  `985fe306…`, the build that still carried the ring. The ring itself came in with `3288ba4` at
  iteration 39 (687,040 B, sha256 `6b4c823e…`, the read-back above). `b01e635` withdrew the
  ring, so this witness cannot be re-taken on today's image; the register read-out is what replaces
  it):
  `[27621] APPBLE: link up: peer=4C:23:38:74:E6:F6 t=0 air=4C:23:38:74:E6:F6 t=0`,
  `[27678] APPBLE: link secured: enc=1 bonded=1 auth=0 bonds=1 bits=16 peer=… t=0 air=… t=0`, then
  `adv restart: uuid=1 name=1 started=1 filter=none` - the accept list is gone from the advertisement.
- **A link that cannot prove the bond is refused while advertising continues.** With the device's
  bond kept and `Window 0`, the host forgot its keys (`bluetoothctl remove`); the connect came up and
  was dropped at 6.0 s → `LastLink=5 (refused: unsecured (no bond, or a passkey not used))`, the log
  line `link refused (enc=0 bonded=0): dropping`, and a scan immediately after still answered
  `ADVERTISING (1 match(es) for 'Akyirr')`. The bond was then re-made inside a 60 s window
  (`pairing SUCCEEDED after 13.1 s`) and the rig left bonded with `Window 0`.
- **Correction to this entry's premise (measured, not inferred): on today's rig the central does NOT
  connect from an RPA.** The read-out reports type `0` (public) for the peer on every link.
  `btmgmt info` shows `ll-privacy` but not `privacy`, and turning it on is refused to an unprivileged
  user: `Set Privacy for hci0 failed with status 0x14 (Permission Denied)` (also with the adapter
  powered off), `btmgmt static-addr …` likewise, and `btmon` answers `Failed to bind channel:
  Operation not permitted`. So "a bonded central reconnects from rotating RPAs with the resolved
  identity shown" stays **UNPROVEN** - the fix that was proven here is the one this entry needed
  operationally: the same peer that timed out twice on iteration 27 is admitted with no filter.
- **The first cut of the read-out could not have shown a resolution even with an RPA, and the second
  cut was wrong about why.** The first cut claimed "Last peer != Last air is the resolution" and was
  said to be impossible because NimBLE sets `peer_ota_addr = peer_id_addr`. The correction in
  iteration 43 then claimed the reported address TYPE was the witness and fields 9/10 could read
  `2`/`3`. Both readings stop at the first two lines of `ble_hs_conn_addrs()`; the `switch` below
  them (ble_hs_conn.c:552-565) is the part that matters: for `BLE_ADDR_PUBLIC_ID`/`_RANDOM_ID` it
  sets `peer_id_addr.type` back to 0/1 **and** `peer_ota_addr = conn->bhc_peer_rpa_addr`. So the type
  the host is handed can never read 2/3 (`/N` is information only), and the ADDRESS PAIR is the
  witness: identity vs the RPA the peer actually used, differing exactly when the controller resolved
  one. `BleCameInResolved()` is that check again (review round 1, card t_0b1cd623). A peer that
  connects from its own address reads the pair EQUAL - the expected reading for this rig's host,
  which cannot enable privacy, not a broken read-out.
- **A genuine second identity (a different IRK) remains IMPOSSIBLE on this host** - one controller,
  `static-addr` and `privacy` both `Permission Denied` - so that half is UNPROVEN, as the card
  allows.

**Review round 1: witness corrected, rebuilt against HEAD, flashed, re-measured (2026-10-28 22:00-22:25
CET, card t_0b1cd623).** The review showed that the type suffix introduced above cannot be a witness
(`ble_hs_conn_addrs()` rewrites 2/3 to 0/1; the `switch` at ble_hs_conn.c:552-565 also sets
`peer_ota_addr = bhc_peer_rpa_addr`), so the address PAIR is the witness again and the type is carried
as information only. Code: `BleCameInResolved()` is used again (the log now appends `(resolved: …)`
when the pair differs), `BleReportedAsIdentity()` and its four checks are gone, and every comment that
made the old claim is corrected (`BleAdmissionPolicy.h`, `BleSettings.h`, `AppBLE.h`,
`BleSettingsGlue.h`, `ble_lastlink.py`). `ble_lastlink.py --spacing` now defaults to 6 s (2-4 s raced
the post-link USB read, once finding the device reset).
- **Built from the then-current HEAD (`29dece5`) and flashed** (owner-authorised):
  `pio run -e Tamu_v2_0A -e DAS_v0_1` -> both SUCCESS, `Tamu_v2_0A RAM 77876/327680, Flash
  685472/2752512`, `DAS_v0_1 RAM 2048/2048, Flash 13440/14336`; native suite exit 0
  (`ble_settings: 180 checks, 0 failures`, `ble admission: 65 checks` - four fewer because the dead
  predicate went). Flash: `Wrote 686144 bytes (421335 compressed) at 0x00070000 ... Hash of data
  verified`; read-back `Read 686144 bytes at 0x00070000`, `cmp` silent, sha256
  `5543bb1c4037d9e08710f28ee03292e192a4b9a10da7bd4b9dfe04feadd2110b` both sides, device reports
  `2026.10.28 iteration 45` (`Version=0x355C002D`).
- **Rig state after the flash:** `Bond count 0`, window open - E69 had fired on the previous session
  (a repeat pairing request from a bonded identity deletes the bond and opens the window), so the host
  re-paired inside a window (`Pairing successful`), giving `Bond count 1`,
  `Bond[0] = 4C:23:38:74:E6:F6`; the window was then allowed to close (`Window 0`).
- **Acceptance, measured with the window closed** (`ble_lastlink.py --connect 3 --addr
  E4:B0:63:C8:20:72`, default 6 s spacing): three links, each `Last peer=4C:23:38:74:E6:F6/0`,
  `Last air=4C:23:38:74:E6:F6/0`, `LastLink=2 (ADMITTED)`; `RESULT: 3 link(s) judged; ... verdicts=[2]`;
  the bond count read 1 afterwards. The pair reading EQUAL is the CORRECT reading here and was
  recorded deliberately: this host connects from its public address, so there is no RPA to resolve.
  Equal is not "no read-out" - the witness only separates when a private address was resolved.
- **Refusal still holds on this image.** With the device's bond kept and `Window 0`, the host removed
  its own keys (`bluetoothctl remove`) and then connected: the link came up and the device dropped it
  during discovery -> `Last peer=4C:23:38:74:E6:F6/0 Last air=.../0 LastLink=5 (refused: unsecured
  (no bond, or a passkey not used))`, `Bond count 1`, `Window 0`, and a scan right after answered
  `RESULT: ADVERTISING (1 match(es) for 'Akyirr')`. Re-pairing right after that failed twice
  (`AuthenticationCanceled` / `Connection timeout`) until the device was reset - that is E69's
  RAM-side state, not an E66 regression; after the reset the pair succeeded.
- **The log cannot carry the resolution on this build, and the read-out is why it exists.**
  `b01e635` withdrew the CID 3 text ring (E67), and `DeviceLog` is dropped while USB is attached, so
  `e66_log_after_connect.py` now fails with `no GetTextLogs reply (tag 0xF001)`. The APPBLE lines
  (`link up: peer=… air=… (resolved: …)`) do carry the pair for a console that can be read; the
  register read-out (fields 9-11 of the then twelve-field block; removed 2026-10-29, so the air and
  the `Display name` field are the read-out today) is the evidence on this rig. The card's "log showing the resolved
  identity" is therefore satisfied by the READ-OUT, and the resolution half itself stays UNPROVEN
  here (type 0 on every link; privacy and static-addr both refused, one controller).

**Doc change owed (2026-10-29, card `t_41038b88`) - `Docs/Services/App Interface.md` still specifies
the twelve-field block.** This replaces the two items that stood here (which asked for `Last peer` /
`Last air` / `Last link` doc rows - fields that no longer exist, and would have sent the docs lane to
add three phantom rows - and a partial `app/lib/core/ble_security.dart` fix). `Docs/` is the parent's
to edit, with Akyirr's approval; nothing below is applied. Measured divergence, `App Interface.md:31-40`
against the device read-back in the note at the head of this section:

| Doc row today | Owed |
| --- | --- |
| `Bond count` (`Index`, RO) + `Bond` (`Text`, RO, keyed `0..N`) | **one `Bonds` field, index 4, `Undefined`, 66 B, `Trigger`**: 3 fixed-size entries of 22 B = 6-byte identity address (printed order) + 16-byte NUL-padded peer name; an all-zero address is an empty slot |
| `Window` (`Uint32`, RO) | `Window`, index 5, `Uint32`, 4 B, **`Trigger`** - reads ms left, a write sets the deadline (0 closes) |
| `Action` (`Enum`, W, "0 opens the pairing window, 1 forgets every bond") | **remove the row.** A window is opened by writing `Window`; forgetting is zeroing `Bonds` |
| `Display name` - "Applies from the next advertising start" | "**Is** the advertised name; a write re-advertises at once. Empty means the System `Name`" |
| (no row carries a field index - the `Key` column reads `0` everywhere except `Bond`'s `0..N`; the doc's rows run Enabled, Passkey, Mode, **Display name**, then `Bond count`, `Bond`, `Window`, `Action`) | index every row `0..5` the way the System block table states its numbers: 0 Enabled, 1 Passkey, 2 Mode, 3 Display name, 4 Bonds, 5 Window |
| `:44` "`Action = 1` clears them, and so does changing the passkey" | "zeroing the `Bonds` array clears them, and so does changing the passkey" |

No `Advertised name`, `Last peer`, `Last air` or `Last link` row belongs in that table - the first is
the `Display name` field itself and the other three were removed on 2026-10-29. Still owed from the
original entry: the field indexes on the System/BLE tables and both static types (`0x0B`, `0x0C`) in
the `Docs/Services/Register.md:24` block-type list.

The app side is **card `t_c432c8ac`**, not a partial edit here: `app/lib/core/ble_security.dart` no
longer matches the block (field constants, the `bleAction*` constants, keyed `Bond` texts), and
`app/test/firmware_contract_test.dart:200` parses `BleSettings_Entries` out of the header, which now
holds six rows - the removed identifiers occur **151 times over 141 lines** in seven files and need the
new shape (field/type/size list and the `Bonds` write semantics are in the card's comment thread).
Recount:

    git grep -o -E 'bleFieldBondCount|bleFieldBond|bleFieldAction|bleFieldAdvertisedName|bleFieldLast(Air|Link|Peer)|bleAction[A-Za-z]*|bleVerdict[A-Za-z]*|bleBondSlots' -- app/ | wc -l

-> `151`; the same `git grep` without `-o` counts the 141 lines, with `-l` the seven files.


**E67 `DeviceLog` is suppressed while a host holds the USB port, and its lines cannot reach the log
store - so a rig session still reads the boot banner only.** The suppression is real and deliberate:
`Core/Functions/AppInterface.h` sets `Connected = AppUSBActive() || AppBLEActive()` and
`Devices/Tamu_v2.0A/Log.h` drops the console copy while it is true; `AppUSBActive()` is
`usb_serial_jtag_is_connected()`, which on this board reads true within ~0.4 s of a reset whenever the
USB cable is attached to a live host - not only while a process is actually speaking the protocol. A rig
session therefore saw the boot banner and nothing after it, which is why E65/E66 had to be pinned by
timing and by register block reads (and why a plain `idf.py monitor` can never show them).
**The premise that the existing store can carry the lines - route them to `GetLogs` (CID 1) - does not
hold (measured 2026-10-28, card t_c4d240e5).** Three structural facts, the first two read off the running
device with `firmware/.pio/probe/log_store.py` (read-only, no reset, no modem lines touched):
- **The store has no text field.** A record is `u16 device | u16 count | Log Struct`, and the Log Struct
  is `source u32 | code u32 | timestamp u32` (`Core/Functions/Log.h:43`; `Docs/Services/Log Handler.md`).
  A formatted line has nowhere to go. The app turns a code into a sentence from its own table
  (`app/lib/ui/log_page.dart`), so a code with no row there reads as "reported code 0xNNNN".
- **The store is a counter, not a transcript.** Entries are deduplicated per (device, source, code) and a
  repeat only increments `Count` and moves the timestamp (`Core/Services/LogHandler.h`, CID 0 path).
  Measured on the rig (core on USB, image 2026.10.28 iteration 43, `Version=0x355C002B`): before any
  stimulus, CID 1 returned exactly ONE record - the boot report, `dev 39937 count 1 service 0 code
  0x0000 ts 0 src 0xFFC00000`. Three identical runtime reports then stored as ONE record reading
  `count 3`. So ">= 20 runtime lines out of CID 1" is not a tuning problem: 20 identical events are one
  entry, and 20 distinct ones would have to be 20 distinct codes that still carry no text.
- **The console is not the alternative while an app is attached:** it IS the app link's byte stream
  (`sdkconfig.Tamu_v2_0A`: `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`), and a line written between two of
  the pump's frame writes corrupts the reply it lands in - which is exactly what the connected check
  prevents. The host only survives stray bytes by resyncing its framer, so the cost of ignoring the check
  is a lost reply, not a lost log line.
**The withdrawn design is out of the tree (2026-10-28).** A 4 KB ring of formatted lines, appended
unconditionally by `DeviceLog` and streamed by a new CID 3 `GetTextLogs` (`3288ba4`), was withdrawn by the
owner ("I didn't ask for anything like that"): `Core/Functions/LogText.h` and
`test/native/log_text_test.cpp` are deleted, the CID 3 block and the ring fields are gone from
`Core/Services/LogHandler.h`, `LogHandler.InitText()` is gone from the boot path and the E67 source guard
is gone from `test/native/run.sh`; `Devices/Tamu_v2.0A/Log.h` is back to the console-only form, with a
comment pointing here. Cost of the removal, clean builds of both envs: core flash 686,616 -> 685,082 B
(-1,534 B), static RAM 77,892 -> 77,876 B (-16 B), image 687,296 -> 685,760 B, and the 4,096 B of heap
the ring took at boot is no longer taken. DAS is **0 B**: identical size lines (flash 13,440/14,336 =
93.8 %, RAM 2,048/2,048 = 100 %), image 13,444 B on both sides, and `cmp -l` against a control build of
the tree WITH the CID 3 code differs in exactly one byte - offset 0xEAA, `addi a4,a4,17` -> `addi
a4,a4,18` in `RegisterService::SystemGetEth`, the version-iteration immediate.
**What it would take, for the owner to choose** (none of it is implemented, and none of it is a new
block):
- a text-carrying field in the protocol - the withdrawn design, in either shape;
- structured conversion of the call sites that matter (`ReportLog(MakeLog(false, service, code, 0))` per
  site): no wire change and it reaches CID 1, but the free-form values (addresses, ms, percentages) are
  lost, repeats fold into `Count`, each code needs an app-side table row (app lane), and it is a change
  per call site rather than one in `DeviceLog`;
- **UART0 as the console**, on the UART pins instead of USB. `CONFIG_ESP_CONSOLE_SECONDARY_UART0` is not a
  real option: the pinned ESP-IDF 6.0.1 (`components/esp_stdio/Kconfig:35-55`) offers only
  `CONFIG_ESP_CONSOLE_SECONDARY_NONE` and `..._SECONDARY_USB_SERIAL_JTAG` in that choice, and the
  secondary runs the other direction - USB-Serial-JTAG output while UART0 is PRIMARY. What delivers this
  route is the primary choice: `CONFIG_ESP_CONSOLE_UART_DEFAULT` with the secondary left `NONE`. Its
  pins, though, are U0TXD GPIO21 / U0RXD GPIO20 (`soc/esp32c3/include/soc/uart_pins.h`), and on this
  board those two pads ARE the RS-485 transceiver (`Devices/Tamu_v2.0A/RSBus.h:6-7`, UART1 at 460800
  Bd), so it needs `CONFIG_ESP_CONSOLE_UART_CUSTOM` with UART0 on two spare pins - plus a USB-UART
  adapter on them for the rig. Still no protocol change and no app change, and it still prints nothing
  on its own: `Devices/Tamu_v2.0A/Log.h` returns before the `ESP_LOGI` whenever
  `AppInterface.Connected` is true, so the same change has to narrow that guard to "the USB console is
  the console" (only meaningful while `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`);
- keep the console copy while the USB link is idle (`AppUsbSilent()`: ~500 ms without an app frame)
  instead of suppressing it on cable presence: cheap, and it makes a raw capture work while nothing
  speaks the protocol, but it still shows nothing while an app is talking.

**Route (d) is implemented (2026-10-28, card t_b3a9e08d; owner ruling "D is sufficient").** The guard
moved from "a cable is attached" to "the link is busy", and the decision is now a host-tested predicate
instead of an expression inside a header the host cannot link:
- `Devices/Tamu_v2.0A/Log.h`, before -> after (the wire format, the `LOG_` codes and the block layouts
  are untouched - this is one guard):
  `if (AppInterface.Connected) return;` ->
  `if (AppConsoleBusy(AppUSBActive(), AppUsbSilent(), AppInterface.TxHead, AppInterface.TxTail)) return;`
- `Core/Functions/AppConsole.h` holds the rule, on its own for the same reason
  `Blocks/BleWindowPolicy.h` does (neither caller can be linked on the host):
  `UsbActive && (!UsbSilent || TxHead != TxTail)`. A host that TALKS (a frame arrived inside the
  ~500 ms window) suppresses the line, and so does a frame still queued in the TX ring - the pump hands
  out 60 B at a time, so a line landing between two chunks splits the frame it lands in, which is not
  only a reply (an unsolicited subscription update needs no request in flight either). Cable presence
  on its own now suppresses nothing, which is the whole fix.
- `AppUsbSilent()` (`Devices/Tamu_v2.0A/AppUSB.h`) also counts "no frame has arrived since boot" as
  silent. Without that, the first 500 ms of uptime read as an active session (uptime < the window), so
  the boot lines would have been suppressed instead - the opposite of what the entry is about.
- Native: `test/native/appconsole_test.cpp` drives the predicate through its truth table (24 checks:
  no host, silent host, talking host, queued frame, wrapped ring counter, plus the exhaustive 8-state
  table), and `test/native/log_guard.sh` pins the CALLER's shape (no `AppInterface.Connected`, the
  predicate asked with all three inputs, the guard before `vsnprintf`/`ESP_LOGI`, still exactly one
  sink). RED on HEAD's headers - six FAIL lines, the first naming the presence gate - and GREEN after.
- Cost, clean builds of both envs: core flash 685,482 -> 685,630 B (+148 B), static RAM unchanged at
  77,876 B, version 26.10.28.46 -> **.49** (`version.json` and the build log agree; an earlier version of
  this entry said `.48`, which is not the stamped image). DAS **0 B**: identical size lines (flash
  13,440/14,336 = 93.8 %, RAM 2,048/2,048 = 100.0 %) and, with the version iteration pinned to the
  control's `.20`, a byte-identical image (`cmp` silent, sha256 `fa139a11...`); with the real stamp
  (`.21`, minted because the new header sits under `src/Core`) exactly one byte differs, the version
  immediate at 0xEAA.
- **The rig half is NOT measured: the device runs 2026.10.28 iteration 46** (read-only System field 0;
  the fix is **.49**) and this card carried no authorisation to flash. What could be measured read-only,
  on the pre-fix image, with the core up 27 min (System field 3 = 1,647,235 ms, so nothing reset it): the
  port opened and held for 30 s with the cable attached ->
  `seconds: 30.1  bytes: 0  lines: 0` (`.pio/probe/console_e67.py`, which does NOT touch the modem
  lines - `logcat.py` sets DTR/RTS and reboots the core). Recipe once authorised: write
  `.pio/build/Tamu_v2_0A/firmware.bin` at 0x70000 with a `read_flash`/`cmp` read-back, then
  `firmware/.pio/probe/console_e67.py --seconds N --out ...` with the host holding the port and sending
  no app frames.
- **How many lines a capture can hold, and why this entry's first recipe could not reach its own bar
  (corrected 2026-10-28).** The tree has 136 `DeviceLog()` call sites under `src/`, and at rest only one
  PAIR is timer-driven: `AppBLE.h:1234` (`no BLE session for 120 s, forcing adv rebuild`) plus the
  `adv restart: ...` line `StartAppAdvertising()` emits behind it at `:950` (the 3 s `advertising down
  %u ms, rebuilding` loop at `:1228` only runs while the advertising is already down). Everything else
  waits for an event. So with a host holding the port, sending nothing, and no BLE session either, the
  rate is **2 lines per 120 s**: ">= 20 runtime lines" needs **~20 minutes** of that state, and the
  130 s capture the first recipe proposed yields ~2. The rig card (t_0f5c04b6) now states the length
  instead of the old 130 s.
- **That floor is what SILENCE yields, not what the console can carry** - measured on the same image,
  E70: a 10 s capture taken while a probe wrote one field, `.pio/probe/cap_enabled_off.log`, recorded
  **76 lines**, every one `DISP` (`local interest IdTgt=39937 IdSrc=39938/39939 ...`, ~7.6 per second -
  frames carrying OTHER device ids, so not the probe's own writes), and not one `BLESEC`/`APPBLE` line,
  because those were triggered by a write and the write is the frame that silences them. Anything that
  happens while the USB link is quiet comes through, including traffic over the air or on the RS-485
  bus. A short capture therefore honestly shows: no boot banner in it (nothing resets the core), plus
  whatever the last <= 500 ms of silence carried - for a rig session **>= 1 `[ms]`-stamped runtime line
  emitted after the read started**, and 20 of them are free if the capture spans a reset (boot lines)
  or BLE/bus traffic. Fudging the count is not an option: the rig card blocks with the figure it saw.

**Verified:** both envs build (`pio run -e Tamu_v2_0A -e DAS_v0_1`, size lines above) and the native
suite (`firmware/test/native/run.sh`) is green with the ring test gone; `log_store.py` is the probe behind
the two measurements above (it also pokes an unimplemented service to store a structured runtime report,
which changes nothing on the device and is wiped by the next reboot).

**E68 A changed passkey still revoked nothing after E65, and the code that was meant to revoke had
two holes of its own.** Card t_c1376256, from the rig session in t_4fc81fc7 (build 26.10.28.27):
after a passkey change the old bond kept full authenticated access and the store still read Bond
count 1. The path looked complete - epoch comparison, `AppBLEForgetAllBonds`, session drop - so the
gap is in the details, and there are three, one per way the rule "a passkey write revokes exactly
what a CHANGED code invalidates" can leak:
- **The delete itself could not run** on an advertising device (`ble_gap_unpair` -> `BLE_HS_EBUSY`),
  which since E64 is the idle state. E65 fixed exactly that by taking the advertisement down around
  the delete, and the passkey path calls the same function, so it inherits it. Nothing to do here
  beyond keeping that path single-sourced.
- **A no-op write dropped a live session, and a real change in the `passkey -> 000000` direction kept
  it.** The drop was gated on `Policy.Mitm`, i.e. on the policy *after* the write: re-writing the same
  code disconnected a correctly authenticated peer for nothing, while removing the passkey left the
  peer that authenticated with the old code sitting in a session over a bond the same apply had just
  deleted. The drop is now decided by the revocation answer itself (`BleBondsInvalidated`, reached
  through `AppBLEEnforceBondPolicy`) and by nothing else: no change -> no forget and no drop, any
  change -> forget, then drop whatever link is up.
- **A reconnect erased the retry.** `onAuthenticationComplete` stamped the epoch on every admitted
  session, on the theory that a link that reaches it authenticated "can only have been bonded under
  the code in force" (the sentence E65 wrote). The rig disproves it: a bond made under the old
  passkey authenticates from its stored LTK and comes back with full access. After a revoke that
  failed, one such session rewrote the epoch, the next radio start found the stamp equal and never
  retried, and the rotated code left that bond in place for good - observed as `radio up ...
  bonds=1` and nothing else. The stamp is gone from that path: the epoch only moves where the STORE
  is known to be wholly under the current code, i.e. an empty store after a forget, or a store with
  nothing to revoke.
- The decision is now one host-tested helper, `BleBondsInvalidated(EpochKnown, Epoch, Now)`
  (`Blocks/BleSecurityPolicy.h`), so "the bonds are revoked" and "the link they authenticated is
  dropped" are the same answer by construction; `test/native/ble_settings_test.cpp` drives it (180
  checks) and the E68 source guard in `test/native/run.sh` pins the three rules on `AppBLE.h` - RED
  on the pre-fix source (five predicates, log in `.pio/probe/`), GREEN after.
**UNPROVEN on the air: the device runs 2026.10.28 iteration 27** (read-only System field 0,
`.pio/probe/dev_version.py`), the fix is iteration 36 in the tree, and firmware flashing was not
authorised in this card. The acceptance - one bond plus a connected peer, change the passkey, `Bond
count == 0`, the old peer refused on reconnect, a peer that only had the old code cannot pair - is
therefore not passed. Recipe once authorised: `esptool.py ... write_flash 0x70000
.pio/build/Tamu_v2_0A/firmware.bin` + a `read_flash`/`cmp` read-back, then `.pio/probe/ble_settings_read.py`
(Bond count before/after) around a USB `Passkey` write, and a `bluetoothctl` re-pair with the old
code (it must be refused) plus one with the new code inside a window (it must succeed).
**Landed outside `firmware/`, both closed on 2026-10-28:** `Docs/Services/App Interface.md:44` now
says "`Action = 1` clears them, and so does changing the passkey" (docs commit `e6162da`, reviewed on
`t_944a5f2d`), and the closing paragraph of the panel in `app/lib/ui/ble_security_page.dart:704` now
says the bonds are "cleared by \"Forget all bonds\" or by changing the passkey" (`7064526`, reviewed
on `t_dd5f927c`). Neither text disagrees with the device any more.

**E69 A peer that presents a bonded identity and asks to pair deletes that bond, and the then-empty
store hands out a 5-minute pairing window.** Measured on the rig 2026-10-28 during items 3 and 5 of
t_98acbebd (build 2026.10.28 iteration 44, the E64/E65/E66/E68 tree): device holds `Bond count 1`,
`Window 0`; the central removes its OWN bond (`bluetoothctl remove`) and makes one connect. Same
shape three times:
- `RESULT: the device dropped the link at t=10.1 s` / `t=6.2 s` / `t=6.3 s`;
- device read-out `LastLink=5 (refused: unsecured (no bond, or a passkey not used))` - the session gate
  does its job, no session leaks;
- `RESULT: ADVERTISING` on a scan right after;
- but the store went `Bond count 1 -> 0` and `Window` jumped to `286053` / `286221` / `258310` ms.
The deletion is not firmware code. `managed_components/h2zero__esp-nimble-cpp/src/NimBLEServer.cpp:607-624`
handles `BLE_GAP_EVENT_REPEAT_PAIRING` by calling `ble_store_util_delete_peer()` and returning
`BLE_GAP_REPEAT_PAIRING_RETRY` ("This app sacrifices security for convenience"), from inside the GAP
event handler - i.e. before `BleServerCallbacks::onConnect`/`onAuthenticationComplete` judge anything -
and this wrapper offers no override point (no `onRepeatPairing` in `NimBLEServer.h`; the framework's
`CONFIG_BT_NIMBLE_HANDLE_REPEAT_PAIRING_DELETION` is off and would delete the entry anyway). Repeat
pairing fires whenever a peer the store already has a bond for asks to pair again, which is exactly
what a phone that discarded its keys (OS reset, "forget this device") or a peer using a spoofed public
identity address does after our `ble_gap_security_initiate`.
The consequence is in the interaction with the empty-store rule, not in the refusal: at iteration 44
`AppBLETick` opened `BLE_AUTO_WINDOW_MS` (300 s) as soon as `AppBLEBondCount() == 0`
(AppBLE.h:1113-1123 as it then read), and inside
a window `BleSessionAllowed` admits **any** peer that pairs. So a peer able to present the owner's
identity address can turn a locked device into a 5-minute open pairing window and then bond to it. The
benign half of the same path: an owner's phone whose OS drops the bond silently wipes the device's
bond and makes the device offer itself to the room.
**Resolved 2026-10-28** - card `t_c1a0678e`; Akyirr ruled for the first of the two directions ("go
with the option you recommended"): the window follows an EVENT the owner caused, not a reading of the
store, and the repeat-pairing deletion itself is left alone (the peer still loses its bond and has to
pair again):
- **Two events, and only two.** The radio coming up with an empty bond store (headless first use) and
  an explicit forget that emptied the store - `Action = 1`, a passkey change, the epoch revoke, all
  three of which run through `AppBLEForgetAllBonds`. The rule is `BleAutoWindowGranted` in
  `Blocks/BleWindowPolicy.h` (host-tested, `test/native/ble_window_test.cpp`, 13 checks), the event is
  a `BleWindowCause` that `AppBLETick` consumes once per pass, and the count-driven
  `BleAutoWindowDone` machinery is gone. A store that merely reads empty grants nothing, which is the
  whole of E69.
- **The residual, on purpose.** After a link-driven deletion the store IS empty, so the deleting peer
  - which by construction had to present a bonded identity - may re-pair without a window
  (`BleBondingAllowed`'s empty-store term). No stranger can reach that state: it takes a bond to
  start with, and a second peer is refused at connect (`BleLinkRefusedBusy`).
- **Rig check, iteration 46** (`685482` B flash / `77876` B RAM, flashed with `./upload.sh app tamu`:
  `Wrote 686160 bytes (421298 compressed) at 0x00070000 ... Hash of data verified.`), re-runnable as
  `bash firmware/.pio/probe/rig_e69.sh` (log `.pio/probe/rig_e69.log`, **14/14 assertions pass**):
  bond made then the device reset -> `Bond count 1`, `Window 0`; the host forgets its own bond and
  reconnects (the E69 action) -> `Bond count 1 -> 0` and **`Window 0`**, where iteration 44 read
  `286053 / 286221 / 258310` ms for the same three-time sequence, with `LastLink=5` (the session gate
  refused the link - nothing leaked); `Action = 1` -> `Bond count 0`, `Window 299953` (the forget's
  own recovery period, unchanged); reset with an empty store -> `Window 293659` (first use, unchanged).
- **One behaviour deliberately given up:** switching the radio off and on again with an empty store no
  longer opens a window by itself - a re-enable is not one of the two events. Pairing is still
  admitted by the empty store, and the next boot or explicit forget grants the window.
- **RED:** `test/native/ble_window_guard.sh` run against the **pre-fix** `AppBLE.h` - revision
  `679480b^` (the `Issues` commit before the fix; `3a117e4` is the same file) -
  `git show 679480b^:firmware/src/Devices/Tamu_v2.0A/AppBLE.h` prints 13 FAIL lines and exits 1;
  against this tree it passes. Name that revision, not `HEAD`: `HEAD` is the fixed file now, so a
  `git show HEAD:` repro silently prints PASS. The native table also pins
  `BleAutoWindowGranted(BleWindowCauseNone, 0) == false`, the exact shape the defect needed to be true.

**Evidence kind:** register read-out (`BLE Settings` fields 2/4/9-11 via `ble_settings_read.py` +
`ble_lastlink.py`), host-side HCI transcripts (`ble_host.py`), and the air (bleak scan). The ATT-level
refusal of the refused link was not captured - the device drops it ~6 s in, and a register read inside
that window did not come up (`TimeoutError: BLE link did not come up`).

**E70 A line a WRITE triggers cannot reach the console: the write is the frame that silences it.**
Measured 2026-10-28 on build `26.10.28 iteration 49` (`185fa31`, the route-(d) tree), card `t_cb221e2e`.
Route (d) (`Core/Functions/AppConsole.h`, `AppConsoleBusy`) drops a `DeviceLog` while the host is
TALKING - an app frame arrived inside the ~500 ms `AppUsbSilent()` window - and while a frame still sits
in the TX ring. Every write to `BLE Settings` arrives over that same USB link, so the line the write
triggers (`radio disabled by the BLE Settings block`, `radio up: ...`, `advertised name -> '...'`,
`passkey written: mode=...`) is emitted microseconds after `UsbLastRx` moved (`AppUSB.h:181`) and is
dropped by construction. Measured, not inferred: a 10 s console capture taken **in the process that
sent** `Enabled = 0` (`.pio/probe/bleset_probe.py writecap 6 0 10`, log `.pio/probe/cap_enabled_off.log`)
recorded 76 text lines, **every one** tag `DISP` (the core's own dispatch chatter), and no
`BLESEC`/`APPBLE` line at all - while the same image prints those tags at boot
(`.pio/probe/cap_boot_name_passkey.log`: `I (352) BLESEC: passkey source: nvs (set)`,
`I (442) APPBLE: radio up: name='Akyirr' mode=passkey bonds=0`,
`I (1012) APPBLE: adv restart: uuid=1 name=1 started=1 filter=none`), because nothing has been received
on the link since boot. So a rig session can read the radio's own events and the boot path, but never
the line a USB write produces.
Consequence for evidence plans: for a setting the app or a probe writes over USB, the device log cannot
be the witness - the field read-back plus the air are (see the `t_cb221e2e` rig round, 0 failures).
Options if the owner wants write-triggered lines visible (a ruling, not an agent's call): (a) accept as
is; (b) defer the line into a small ring that `AppBLETick` flushes once the link has been idle for the
`USB_TX_SILENCE_MS` window (~100 B RAM plus a flush point); (c) have the trigger arm a flag and let the
loop log the state once, >=500 ms later. Not implemented - route (d) is the owner's ruling of
2026-10-28 and this is its measured blind spot.

**E71 A write to a `Persistent` static field lives in RAM until the app saves: a reset drops it.**
Measured 2026-10-28/29, card `t_cb221e2e`, build `26.10.28 iteration 49`. Controlled pair for
`Display name`, one reset each, the **only** difference between the two runs being the save
(`bash firmware/.pio/probe/e71_displayname_revert.sh`, log `.pio/probe/e71_displayname_revert.log`,
**15/15 assertions, 0 failures**):
- write `Display name = "Tamu Desk"`, **no** save: the block reads the write back immediately
  (`KV 7 0 Tamu Desk`, `KV 8 0 Tamu Desk` - fields 7 and 8 of the twelve-field block; `Display name`
  is field 3 today) - then one reset and the boot log says
  `I (442) APPBLE: radio up: name='Akyirr' mode=just works bonds=0`, the block reads `Display name`
  empty and `Advertised name = Akyirr` (the block's field 8 then; that read-out is gone - the air
  reading that follows is what proves it now), and the air carries `Akyirr` at rssi -38
  (`.pio/probe/boot_displayname_unsaved.log`; the string `Tamu Desk` appears nowhere in that capture).
- the **same** write plus Save All, the **same** reset: `I (442) APPBLE: radio up: name='Tamu Desk'
  mode=just works bonds=0`, the block reads `Tamu Desk`, the air carries `Tamu Desk` at rssi -38
  (`.pio/probe/boot_displayname_saved_recheck.log`).
Same shape for `Enabled`, measured in the same round: written `false` with **no** save -> the next boot
prints `I (442) APPBLE: radio up: name='Akyirr' ...` and the radio starts
(`.pio/probe/cap_boot_enabled_off.log:150`); written `false` **plus** Save All -> that boot contains no
`APPBLE:` line at all and the device stays off the air
(`.pio/probe/cap_boot_enabled_off_saved.log`, `grep -c 'APPBLE:'` = 0).
This is by design, not a defect: `Docs/Services/Register.md:56` - "The device can only overwrite or load
the whole file; targeted saves and recalls are handled by the app" - and `.SV` is written only by
`RegisterService::StaticSaveAll()` (`Core/Services/RegisterPersist.h:20`), which a field
write never reaches: over the register service that is **Save All = CID 5**
(`Docs/Services/Register.md:64-65`, `Core/Services/RegisterDefs.h:35`), while **CID 4 is Recall All**,
which loads `.SV` **over** the RAM copy - a client that sends 4 instead of 5 discards the change it just
wrote. The `Persistent` flag on a static field names the memory space, not an
auto-save-on-write.
Two consequences worth having written down: (1) a mistaken `Enabled = false` is undone by one USB
write (measured, `Enabled` false -> true, uptime monotonic, no reboot) **and** by a plain reset, so the
device is never USB-rescue-only; (2) a client that changes `Enabled` or `Display name` must follow with
Save All or the change is gone at the next reset - the app's settings page is the place that decides it
(card `t_1d304c9c`'s territory, not the firmware's). The passkey is unaffected either way: it lives in
NVS (`BlePasskeyStore.h`) and survived the same reset (`BLESEC: passkey source: nvs (set)`). Writing
`000000` erases that entry rather than merely relaxing the policy, and the read-back alone cannot show
it: the next boot after such a write prints `I (342) BLESEC: passkey source: none (just works)`
(`.pio/probe/boot_displayname_empty.log:141`), matching `BlePasskeyStoreSave`'s `nvs_erase_key`
(`BlePasskeyStore.h:98-101`).

**E72 `Action = 0`/`Action = 1` looked refused on the `BLE Settings` block - the probe was sending
the ASCII character `'0'`/`'1'` (0x30/0x31), not the number.** *(Resolved 2026-10-29: probe defect;
the whole `Action` field has since been removed - the block's six fields open the pairing window with
a write to `Window` and forget everything by zeroing `Bonds`, so this entry is history only. Card
t_41038b88.)* *(Kept as the measured record of a trap that produced a
false alarm about a recovery lever.)* Found on the rig 2026-10-29 during card `t_c0266baf`, corrected
the same day after review; **not a firmware defect and no `firmware/src` file was touched.** `Action` is
an `Enum(1)` trigger: `OnBleActionWrite` matches the *byte*, so its `default:`
branch refuses anything that is not `0`/`1`. The probe's `write()` encoded every non-Bool argument with
`text.encode()`, so the documented reproduction (`bleset_probe.py write 5 0`) put `0x30` on the wire and
recorded a refusal. One session, same block, same USB frame, raw bytes vs ASCII characters
(`.pio/probe/action_ascii_check.py`, which writes the bytes directly):
```
$ firmware/.pio/probe/venv/bin/python firmware/.pio/probe/action_ascii_check.py
Action = ASCII '0'  raw=30   -> flags=0x4E echo=
Action = ASCII '1'  raw=31   -> flags=0x4E echo=
Action = byte 0x00  raw=00   -> flags=0x0E echo=0005c0020c00010000
Action = byte 0x01  raw=01   -> flags=0x0E echo=0005c0020c00010001
control Enabled = b'\x01'      -> flags=0x0E echo=0006c0020400010001
```
So on `iteration 49` (flag-off image, one process, `/dev/ttyACM0`) **both actions are accepted**:
`0x0E` for `Action = 0` (the pairing window opens for `BLE_WINDOW_MS` = 60 s -
`AppBLE.h:50`, set by `AppBLEOpenWindow()`, `AppBLE.h:231` - `Window` went `0` -> `59957`, re-measured
`0` -> `59498`) and `0x0E` for `Action = 1` (the E65 forget path runs; `Bond count` stayed `0`, the
store was already empty, and the emptied store opens the *automatic* window for `BLE_AUTO_WINDOW_MS` =
300 s - `AppBLE.h:51`, reached through `AppBLEAutoWindow` / `BleAutoWindowCauseForgotten`,
`AppBLE.h:479` - `Window` then reads ~`299508`, re-measured `0` -> `299482`; the same 300 s window is
what the device opens on any boot with no bond stored, logging `APPBLE: no bonds stored: window open
for 300000 ms`). The
`default:` branch's refusal is evidence about the byte that arrived and nothing else - the earlier entry
that read it as "the recovery lever is unavailable, the device is reachable by the bonded peer only" was
wrong, and the repeat-pairing delete of E69 was only reached because of this false alarm.

Fixed in the probe, same day: `firmware/.pio/probe/bleset_probe.py` now encodes `Enum`/`Bool` writes
(fields 5, 6) with `int(value, 0)` -> `bytes([v])`, so `write 5 1` sends `0x01` and refuses to send a
non-number at all; Text fields (`Passkey`, `Display name`) still carry the literal string. Verified
against the device after the change: `write 5 0`, `write 5 1` and the `write 6 1` control all print
`OK [flags=0x0E ...]` with the echoes above. Lesson for any future probe: a trigger field's argument is
its wire encoding, and `b'\x01'` (1 byte) is not `b'1'` (ASCII 0x31).
