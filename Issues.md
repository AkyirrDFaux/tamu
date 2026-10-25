# Issues

Open items only. Resolved findings, the 2026-10-04 per-area audits and the doc-fact-check rulings
that were applied are in the git history.

Item codes: **B** = code-fix items (the 2026-10-09 fact check); **E** = the remaining open backlog items, numbered for later handling. Closed on 2026-10-15 and removed: **B11** (BLE large-reply drain fixed), **E12** (storage hooks converted to a class), **E19** (structurally requester-only), **E24** (Register string clamp documented), **E28** (no capability bit needed), **E4** (obsolete, replaced), **E22** (not needed), **E23** (fixed).

## B. Code fixes still open (the document is the specification)

**B27 Script-file length validation wraps in uint32.** *(WITHDRAWN 2026-10-14 by the owner: a script file cannot approach the 32-bit limit, so the wrap is not reachable. The `474489e` change was reverted. Kept for the record - the sweep's other arithmetic findings were checked and stand.)* `ScriptProgram.h:209-217` summed four header-supplied uint32 lengths, so a crafted `SCR_XXX` header passed `Need > Size` with the running offsets ~4 GB past the buffer.

**B1 Router and branch broadcast are documented and unimplemented.** - **scope settled**: after the beta. The rig has no multi-bus device to test a tree topology on. `RSBus and Packets.md:3-7` and
`Data Formats.md:37` (`0x3FE`) describe a router tree; `Dispatcher.h:36-40` says the multi-bus topology
is not implemented and `Docs/Services/Router.md` is a stub.

**B5 System block field 8.1 has no code.** `System Block...md:24` documents `WiFi connection
information` at 8.1; there is no WiFi code in the firmware.

**B6 The UDP framing has no code.** `App Interface.md:25-27` documents a UDP packet; the firmware
implements USB and BLE only.

## D. Unverifiable from code

`RSBus and Packets.md:1` supply voltage and USB PD (hardware), `Data Formats.md:7` "14 byte UUID"
(14 bytes confirmed, UUID loose), `Data Formats.md:53` the generic dictionary marker encoding,
`Register.md:34` the Internal/External Access columns, `Register.md:48` "sorting by the whole uint32",
`Storage.md:86-96` the dynamic block descriptor ordering (runtime only), and the Valu feature rows that
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
  stale.** Settled 2026-10-17 (owner decision, Akyirr, via kanban `t_2143e21b`, audit `t_e1d8c148`):
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
  stated - 112 B now.
  Carrying the edge counter on the 0401 answer too was measured at a further ~120 B and does not
  fit; that is why the answer stays value-only (`SubscriptionsEdge.h`).
  The RAM figure is by design: statics (1040 B = 112 B `.data` + 928 B `.bss`) + stack (1008 B) fill
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
  edge trigger's bullet (`Subscriptions.md:65-68`, owner-approved in `c0a0ac6`) already states the
  counter-as-payload rule, parity-as-level, a jump of `n` = `n` edges, and that the requester stores
  the derived `Bool` and never the counter; `t_378cc55c` implements exactly that (the whole payload is
  the counter, `uint16` little-endian, two bytes, nothing appended; odd = true/pressed, even =
  false/lifted). What the section does not say:
  (a) the provider's counter counts **every** level change, in both directions, while the trigger only
  selects which of those changes are sent - without that a reader cannot see why parity may be read as
  the level, since a rising-only counter would stay odd after the release;
  (b) the "Inter-device Commands (040x)" Value-update row (`Subscriptions.md:81`) gives the response as
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
