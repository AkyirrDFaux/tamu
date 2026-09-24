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
