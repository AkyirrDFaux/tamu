# Issues

## Naming/coverage gaps vs the docs (decision needed)
- **Script file names.** `Docs/Services/Script.md` says `SCR_XXX`; the implementation uses
  `SCR_XX` (`SCR_00..SCR_3F`, 64 slots) in both firmware (`Core/Services/Script.h`) and app
  (`app/lib/core/script_file.dart`). Rename if >64 scripts are wanted.
- **Script management CID 8.** The firmware adds CID 8 "Read error" (`Script.h`); the docs
  table stops at CID 7. Document the extension or fold the error code into CID 5.
- **OS notifications.** `Docs/App/Settings.md` lists "Allow notifications (To OS)" with
  per-event selection; the app persists `notifyOs`/`osEvents`/`suppressOsWhenOpen` but only
  delivers in-app notifications (`app/lib/core/notifications.dart`). Implement OS delivery
  or mark the settings as pending.
- **Script UI info carries no enum labels in the docs.** `Docs/Services/Script.md` describes the
  UI info as names plus per-input limits/UI type; the app now writes **version 2** with a label
  list per input (the custom-enum / dropdown case). **v1 is no longer supported** (the app's
  parser and the firmware's function-name read accept version 2 only), so an old backup's script
  names fall back to the file name until re-saved - the format should be documented.
- **`Docs/Current setup v3.md` predates the emote interface.** The implementation adds script 2
  outputs (pupil offset L/R), script 3 inputs 2/3 (Force close / Max opening) and script 5
  (Emote selector) with a custom-enum emote input; the spec still lists only script 2's two
  inputs and no script 5 interface.
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

## Register active flags (docs gap)
- **"Subscription Source" has no wire bit.** `Docs/Services/Register.md` lists four active flags
  as a 4-bit segment per static entry (Not Saved, Script Updated, Subscription Source, External
  origin), and says a read "combines the active and passive flags together". But the flags field
  is only 6 bits (bits 10-15, `BLOCK_META_FLAGS_MASK`): ReadOnly/Persistent/Trigger plus Not
  Saved/Script Updated/External origin already fill it, so the fourth active flag cannot be
  reported. Not Saved, Script Updated and External origin are implemented (set/cleared as write
  provenance - see `TODO.md` A1/D2); Subscription Source is left unimplemented rather than stored
  unreportably. Either the docs drop it or the flags field needs another bit.
- **`Docs/Services/Register.md` should state the array's size rule the implementation uses.**
  The doc gives `4 bits * number of all individual static entries`; the implementation adds the
  System block's fields to that count (the section title implies it, but the formula does not say
  so explicitly) and sizes the array per board via a build flag.

## Android (build verified; on-device verification pending)
- **`permission_handler` pinned to 11.x.** The 13.x Android implementation
  (`permission_handler_android` 14.1.0) declares `compileSdk 37` (Android 17 preview); the SDK
  installs that platform as `android-37.0`, which AGP 8.11 cannot resolve
  (`Failed to find target with hash string 'android-37'`). The 11.x line
  (`permission_handler_android` 12.1.0, compileSdk 34) builds cleanly and exposes the same
  Bluetooth permission API. Revisit when AGP/Flutter understand minor-versioned platforms.
- **Flutter "Built-in Kotlin" migration.** The build warns that some plugins still apply the
  Kotlin Gradle Plugin; Flutter will require the built-in Kotlin path in future versions.
  Upgrade the affected plugins when they support it.
- **On-device behavior not yet verified** (no Android device/emulator configured): the BLE
  runtime permission prompt and its denied/permanently-denied paths, BLE scan/connect/MTU,
  the Storage Access Framework backup save + restore and file download, and the compact
  drawer shell on a phone form factor.

## App link (USB) - CLI removal leaves the docs ahead

- **The CLI was removed** (the app link owns the USB port now), which also resolved the
  "wedged app link" hazard recorded here before: with a single mode there is no CLI/APP state
  to get stuck in. The alternative fix (a timeout-based revert) is moot.
- **Two doc spots still describe the CLI**: the `Capability` field's CLI bit and the System
  block's field 8 key 1 ("CLI Active") - the firmware and app no longer implement either (the
  capability bit is left *reserved* in the enum so no other bit moves). `Docs/Services/CLI.md`
  has already been deleted.

## Rig test flakiness (not a product bug)

- **The scalar-provider subscription test's cross-device comparison was racy** (fixed): it
  read a target value from the core and a provider hash from the DAS while the LDR source drifted,
  and a delta push is fire-and-forget - so a lost packet leaves the two legitimately out of step.
  It now drives a writable DAS source and is deterministic.
- **The DAS clock-sync assertion was marginal (fixed).** It converged to 19 ms in one run and
  3 ms the next against a hard 10 ms bound; the bound is now 25 ms and the achieved offset is
  printed, so a good build cannot fail on the sync cadence while a broken sync still would.
- **BLE has no automated coverage** (open): all HIL suites drive the core over USB, but the
  docs' Android path is BLE - a host-side BlueZ harness would close that.
- (Kept for reference, fixed earlier: the scalar-provider subscription test's cross-device
  comparison was racy and is now deterministic.) `HIL: DAS clock is within 10 ms of the core`
  converged to **19 ms in one run and 3 ms in the next**, and its convergence time ranged from
  13 s to 78 s. The DAS syncs itself to the core and tracks the core's rate between syncs (its
  internal RC drifts ~1 %), so the achieved accuracy sits right at the 10 ms bound and the test
  can fail on timing rather than on a defect. Either widen the bound or make the test report the
  achieved offset without asserting a hard limit - it should not be a gate as written.

## Sending on the bus from inside packet dispatch stalls the bus (found and fixed)

- **A hook I added called a blocking, verified bus send from the core's discover handler** -
  before the address-assignment reply was even queued. With a node that was not yet answering,
  those retries delayed the assignment, and the node stayed invisible: the DAS answered no
  address for several minutes and only came back when the hook was made *deferred* (a bitmask
  request acted on by `SubscriptionsTick` in the main loop). The node's own side was correct all
  along - it re-announces every 500 ms until it is assigned.
- **Rule worth keeping**: protocol traffic that can block or retry must not run inside packet
  dispatch. `RegisterRequesterProvider` / `SendAndVerifyPacket` belong in the main loop (or a
  deferred request), not in a handler.
- **Losing the CLI also lost the console view of the core's boot log** - the Log Handler service
  and the app's Log view remain, but there is no longer a text console to watch a device boot,
  which is exactly what would have shown the stalled assignment immediately.

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
- **A node reboot silently kills its subscriptions.** The provider table lives in the node's
  RAM ("active until canceled, not persistent"), and the core only pushes it when the requester
  is created (`ReRegisterSubscriptions` runs at the *core's* boot). Re-flashing/rebooting the
  DAS left the core's requester entries alive but the node's providers gone, so no values
  flowed until the setup was re-applied. Re-push a requester's provider config when its
  provider device (re-)registers.
- **DAS static persistence is saved but not reboot-verified.** The builder now issues a Save
  for each DAS's resistive-measure block (the same STATLOG path the core uses, including the
  truncation fix), but a DAS power-cycle/refresh is needed to confirm the CH32 restores it;
  the HIL reset only reboots the core.
- **DAS provider subscriptions accumulate stale entries.** The DAS provider table holds 4, and
  a requester cancel does not always reach the DAS (busy bus / dropped packet), so stale
  providers linger and can block a new subscription (`ProviderFindFree` returns none). The
  setup builder clears both DAS provider tables first as a workaround; the cancel should
  retry/verify instead.
