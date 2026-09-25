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
- **Register backup view.** `Docs/App/Service views/Register.md` describes a Current/Backup
  view toggle with Save/Recall; the app exposes per-field "Save/Recall to backup" menus and
  a global Save/Recall all instead of a view toggle.
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
- **The "Not Saved" active flag is never set.** `Docs/Services/Register.md` defines an active
  "Not Saved" flag ("a change has been made compared to the saved state") and the CLI prints
  `[NS]`, but nothing sets it. A static Write only reaches RAM until an explicit Save (CID 3),
  so a value can look applied and still vanish on reboot with nothing signalling it - which is
  exactly how the display render-block was lost unnoticed. Set/clear the flag on write/save (or
  document that callers must track it themselves).

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

## Subscriptions / time sync (noted gaps, not currently triggered)
- **The app is a manager, never a requester/provider.** `SubscriptionClient` only writes the
  *devices'* requester tables; the app never registers a subscription with itself as requester
  or provider (so the `OnChangeConfirm` confirmation path does not apply to it). A test guards
  this.
- **Node time-sync interval is 60-75 s, not the docs' 2-3 min.** The DAS's internal RC
  oscillator drifts ~1% and its drift changes by ~0.02% between syncs (~10 ms per 60 s), so
  the <10 ms accuracy target needs a shorter interval than the docs specify. An external
  crystal (HSE) on the DAS would allow the documented 2-3 min cadence to meet the target.
- **A node stays out of sync for up to one sync interval after a core restart.** The node
  applies its offset only at its own TimeSync, so a core reboot leaves the node's clock stale
  until the next sync (now <=~75 s). By design; a core "time changed" broadcast would let
  nodes re-sync immediately.

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
