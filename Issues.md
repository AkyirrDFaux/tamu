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
