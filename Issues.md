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
