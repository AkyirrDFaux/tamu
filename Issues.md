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
- **The app never confirms `OnChangeConfirm` subscriptions.** A high-priority change
  subscription repeats "until confirmed" (docs; `HandleProviderConfirmation` updates the
  provider hash from the requester's Subscriptions CID 0). The app has no FNV-1a and never
  sends that confirmation (`app/lib/core/subscription_client.dart` only listens), so an
  app-created `OnChangeConfirm` subscription would repeat at its retry interval (default
  100 ms) indefinitely. Implement the confirmation (or hide the trigger) before using it.
- **Node time-sync interval is 60-75 s, not the docs' 2-3 min.** The DAS's internal RC
  oscillator drifts ~1% and its drift changes by ~0.02% between syncs (~10 ms per 60 s), so
  the <10 ms accuracy target needs a shorter interval than the docs specify. An external
  crystal (HSE) on the DAS would allow the documented 2-3 min cadence to meet the target.
- **A node stays out of sync for up to one sync interval after a core restart.** The node
  applies its offset only at its own TimeSync, so a core reboot leaves the node's clock stale
  until the next sync (now <=~75 s). By design; a core "time changed" broadcast would let
  nodes re-sync immediately.
- **The app's default subscription trigger is `Periodic` (1000 ms)**
  (`ui/subscriptions_dialog.dart`), which sends regardless of change - so a subscription
  generates a steady 1/s of bus traffic while it exists. Consider defaulting to a change-based
  trigger (`OnChangePeriodic`/`DeltaPeriodic`).

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
- **The LDR lux needs a one-time calibration against a lux meter.** `MeasLDR10K` now follows
  the datasheet (GL55 5-10 kOhm part, gamma ~0.6 from Fig. 2) and is live on the DAS
  (verified: the room reads 4.5 lux where the old formula said ~18). The part is only specified
  as 5-10 kOhm at 10 lux and the firmware's `log` carries a ~10% bias, so read a reference lux
  next to the sensor and adjust `LDR_R10_KOHM` (level) / `LDR_GAMMA` (slope) in
  `Devices/DAS_v0.1/Measuring.h`.
- **A node reboot silently kills its subscriptions.** The provider table lives in the node's
  RAM ("active until canceled, not persistent"), and the core only pushes it when the requester
  is created (`ReRegisterSubscriptions` runs at the *core's* boot). Re-flashing/rebooting the
  DAS left the core's requester entries alive but the node's providers gone, so no values
  flowed until the setup was re-applied. Re-push a requester's provider config when its
  provider device (re-)registers.
- **A core that loses its SNDB orphans already-registered nodes.** The DAS run their
  discovery loop only once at boot (`while (ShortAddress == 0)`), and the core does not
  re-register an unknown sender, so after the core's SNDB was wiped (flash erase) the two
  nodes never came back until they were power-cycled. Consider periodic re-discovery or
  re-registering on an unknown source.
- **Reconfiguring the fan PWM frequency fails.** Writing PWM Frequency (1000 Hz) is rejected:
  `OnPWMFrequencyChange` calls `ledc_timer_config` and returns false, so the field write is
  refused. The setup leaves the default 25 kHz. Worth investigating (10-bit resolution at
  1 kHz should be achievable).
- **PWM Duty is a `uint32` (%), not a Number.** The Register view/tests must decode it as an
  unsigned int; reading it as 16.16 gives ~0.0005 for a real 30 %.
- **DAS static persistence is saved but not reboot-verified.** The builder now issues a Save
  for each DAS's resistive-measure block (the same STATLOG path the core uses, including the
  truncation fix), but a DAS power-cycle/refresh is needed to confirm the CH32 restores it;
  the HIL reset only reboots the core.
- **DAS provider subscriptions accumulate stale entries.** The DAS provider table holds 4, and
  a requester cancel does not always reach the DAS (busy bus / dropped packet), so stale
  providers linger and can block a new subscription (`ProviderFindFree` returns none). The
  setup builder clears both DAS provider tables first as a workaround; the cancel should
  retry/verify instead.
