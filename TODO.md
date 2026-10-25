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

## App UX pass (2026-10-15) - phase 1 approved, phase 2 planned, phase 3 open

Steering from Akyirr: streamline the UX, one consistent UI, and it must be understandable by a
non-programmer. `Docs/App/General Info.md` already promises this ("human-readable interpretation,
targeted at technically non-proficient users, while subtly displaying extra information for more
advanced users"), so the work closes a spec-vs-code gap, it does not change direction.
**Presentation only** - the five tabs and all seven service views stay; no feature, protocol or IA
change. Reordering is explicitly deferred.

Scope, measured on the tree: `app/lib/ui` is 34 files / 14 312 lines. `theme.dart` defines only
`bodyLarge/Medium/Small` and five component themes, so every page styles itself - **151 `TextStyle()`
literals** and **133 raw `Colors.*` references** outside the theme, 8 distinct `EdgeInsets.all` values
(18x12, 5x16, 4x24, 2x8, one-offs at 1/3/6) and 4 corner radii (4/6/8/12). A shared layer already
exists (`app/lib/ui/widgets.dart`, imported by 22 of 34 files) - adoption is the problem, not absence.

**End-state rule, and the test of this phase:** no page styles itself (no raw `Colors.*`, no
`Color(0x...)`, no `TextStyle(fontSize:)`, no bare `EdgeInsets`) and no situation is implemented
twice. Styling lives in the theme and the shared widgets only.

### Phase 1a - token layer (`ui/theme.dart`), first
**Status: done** (`c8a310a`), with the component themes filled in.


Spacing scale, radius scale, named text roles (the three body styles become roles and the missing
title/label/headline roles are added), plus the absent component themes: input/TextField, Card,
ListTile, Dialog, Chip, TabBar, Switch/Checkbox/Radio, IconButton, TextButton/OutlinedButton,
Dropdown, Tooltip.

### Phase 1b - one implementation per situation

**Status: built and committed (2026-10-17).** `ec67c17` the shared widgets (`StateMessage`,
`TamuPage`, `PageAppBar` + `PageActions`, `DeviceTile`, `TaskProgress`) and `dialogs.dart`
(`TamuDialog`, `showTamuDialog`, `showTamuPicker`, `PickerShell`), with all four pickers moved onto
the shell; `28eeb67` the viewer toolkit (`viewer_kit.dart`, 10 tests); `c2191f7` the value-editing
entry point (`showValueEditor`). `formatters.dart` landed earlier as 1b-A (`7830095`).
Gates at each step: `dart format` stable, `dart fix --apply` nothing to fix, `flutter analyze`
clean, `flutter test --exclude-tags hil` 243 passed + 1 skipped (baseline 233 + 10 new).
**Visible changes to veto at the 1c review stop:** `InfoRow` vertical padding 3 to 2; the colour
`H` readout shows `45 degrees` not `45`; simple editors render in the shared 400 px dialog body
(was about 280); matrix/transform cell padding uses `Spacing.xs`. When a page adopts `TamuChip`
its text grows 10 to 12.
**Deliberately deferred to 1c:** re-pointing `ChipLabel` at `TamuChip` - doing it now would restyle
the log page and register tiles before their own batch.


`ui/widgets.dart` is the home. Counts below are measured call sites today, not guesses.

- `SectionHeader` - exists, used by **1** file (`settings_page.dart`); becomes the only section heading.
- `InfoCard` / `InfoRow` - exists, used by **2** files; becomes the label:value row everywhere
  (Device View, Update, every service view).
- Already adopted, become the standard: `showSnack` 14 files, `DialogBody` 10, `RefreshButton` 9,
  `ShellDrawerButton` 5.
- New `TamuPage` - scaffold plus loading / empty / error states. Failure text is currently written
  inline in about 10 files (`log_page.dart` alone carries 23 `'Could not ...'` strings).
- New `StateMessage` - the only place an empty/error/loading state is rendered.
- New `PageAppBar` + action set - refresh/autorefresh, identify, sort, view switch. Autorefresh
  exists only in `connection_page.dart` + `widgets.dart`, yet `Docs/App/Devices.md` specifies it on
  Devices and Device View too.
- New `DeviceTile` - icon, name, id+type subtext, stale marker, badge; replaces the per-page rows in
  Connection / Devices / SNDB / Backup.
- New `showTamuDialog` - replaces **~35 hand-rolled `showDialog` call sites** across 15 files
  (`register_page_edit.dart` 5; the two value-editor backends 6 + 6; `script_editor_view.dart` 4).
- New `TaskProgress` - one phase/progress display for update / backup / restore / upload.
- New picker shell over the four existing pickers (block info, symbol, instruction, subscription).
- `formatters.dart` - one home for `idToString`, `dataTypeLabel`, `formatValue`, `fileTypeLabel`,
  `formatSystemValue`, `formatCapabilities`, `formatUptimeMs`, `formatOffsetMs` (spread over three
  files today, plus inline formatting elsewhere). No display formatting outside it.
- **Value editing** - was a facade (`value_editor.dart`) over three backends plus a parallel script
  implementation (`script_value_dialog.dart`, 318 lines): 1 770 lines doing one job. Since 1b there is
  one entry point (`showValueEditor`), with `script_value_dialog.dart` as a caller of it, and 1c.3
  (`` below) finished the family: the five files carry no raw colour, `TextStyle(fontSize:)` or bare
  `EdgeInsets`, and the controls they had written twice (the size stepper, the numeric cell grid, the
  hex field's byte decode, the dropdown value guard) are one implementation each. Left alone, with a
  reason: the per-type numeric dialogs differ by contract (the byte-width-preserving enum entry against
  `_editInt`'s fixed uint32), and the dialog bodies keep their per-content width bounds.
- **Viewers** - four near-parallel table/tree implementations (`file_viewers.dart` 695,
  `log_page.dart` 519, `storage_page.dart` 444, `register_backup_view.dart` 279) and they hold the
  tree's worst styling debt (`register_page_tiles.dart`: 23 `Colors.` / 29 `TextStyle()`). Target is
  one viewer toolkit - table shell, hex/ASCII view, paging, row chips - on top of `formatters.dart`.

### Phase 1c - roll the pages onto the library, worst-first

1. Register cluster - 2 853 lines (page 886, tiles 954, edit 734, backup view 279)
2. Script cluster - 3 645 lines (8 files)
3. Value editor + `script_value_dialog.dart`
4. Viewers - file, storage, log, register backup, SNDB, subscriptions
5. Connection / Devices / Device View (list + graph), adopting `DeviceTile`
6. Update / Backup / Settings - small, already mostly shared

**Status (2026-10-17):** 1 Register cluster `3e6f505`; 2 Script cluster `71fbc59` + the loading-frame
fix `3047d01`; 3 the value editor cluster `3632a68` (below). The five value-editing files were already
free of raw colour, `TextStyle(fontSize:)` and bare insets when 1b finished, so 1c.3 closed the
duplication inside the family instead: one size stepper (a vector's length, a matrix's rows/columns),
one numeric cell grid (the matrix editor, the transform editor's raw mode), one hex-field byte decode
(the colour field and the hex fallback), one guarded value dropdown (`dialogValueItems`, now in
`dialogs.dart`, for the script dialog's Type/UI-style and the BlockInfo Block lists) and
`ScriptValueDialog` shown through `showTamuPicker` like every other editor that renders its own chassis.
**Visible changes of 1c.3 to veto:** the BlockInfo dialog body 360 to 400 - its Block dropdown
overflowed the old width by 4.5 px on the longest label (`Resistive Measure`); the matrix Rows/Cols
field 40 to 44 px (the shared stepper's field width); and a BlockInfo whose type is not in the pickable
list (a fresh/zeroed one, or a script/dynamic marker) now lists that entry *first* where the old code
appended it last - same items, same labels, same selection.

**1c.4 (the viewer cluster).** `file_viewers.dart`, `log_page.dart`, `storage_page.dart`,
`sndb_page.dart` and `subscriptions_page.dart` render through `viewer_kit.dart` (one table shell, one
hex/ASCII view, one paging shell, one chip row) and `formatters.dart`; `register_backup_view.dart` was
already there. The six files now carry 0 `Colors.`, 0 `TextStyle(` and 0 `fontSize:`, and no bare
`EdgeInsets` - at `f6b8e05` they carried 27 lines holding a `Colors.` reference (31 occurrences), 38
`TextStyle(` literals, 27 `fontSize:` and 31 `EdgeInsets`, with `register_backup_view.dart`
contributing none of them (`file_viewers` 11 / 19 / 18 / 13, `log_page` 4 / 5 / 4 / 5,
`storage_page` 6 / 5 / 3 / 2, `sndb_page` 0 / 2 / 2 / 0, `subscriptions_page` 6 / 7 / 0 / 11).
`widgets.dart`'s `ChipLabel` - whose last user was the log page - is deleted; `HexView.widths` replaced
the file viewer's private `bytesPerLineOptions`; `MonoText` grew a `tone` for a struck (tombstoned)
cell; `formatFileSize` moved from the Storage page into `formatters.dart`. Kept local, with the
reason: the LAY LED grid (a spatial map where the cell's position is the datum, not rows of columns),
the selectable text preview (`SelectableText` is what makes a text file copyable out of the app;
`MonoText` is a plain `Text`) and the byte-per-line `PopupMenuButton` (a control, not a viewer shape).
Every table sizes its columns by `flex` and pins a width only where the datum is fixed width, so the
viewers reflow between a portrait and a landscape window instead of resting on one pixel width.
**Visible changes of 1c.4 to veto** - every size in px, every ratio from `dart run tool/contrast.dart`
(any change that moves a glyph's size, weight or brightness is here even when it is the point of the
fix):
- Log page: the tile's chips 10 to 12 px with 1 to 2 px vertical padding (`ChipLabel` to `TamuChip`;
  the ink stays `kTextSecondary`); the footer 11 to 13 px and the device header's count 10 to 12 px,
  both now `kTextMuted` - white54 where they were white38, so *brighter*, deliberately; the device
  header's title 16 to 15 px, weight unchanged at w600; a failing entry's title and icon `redAccent`
  to `kError` `#FF8A80` (3.58 to 5.01 on a card - *lighter*, deliberately); the App-diagnostics
  dialog's dump 11 to 13 px and its ink white to `kTextMuted` (white54) - *dimmer*: the old style
  carried no colour and `SelectableText` merged the dialog body's white, the new one is the muted
  token. The page's own empty state is untouched: it still prints `No logs recorded` / `No logs
  match the filter`, not the toolkit's.
- File viewer: hex offsets `0x00000000` lowercase to `hex32` uppercase, 11 to 13 px and `kTextMuted`
  (was white38); the hex bytes and the ASCII gutter 11 to 13 px (the gutter white54 to `kTextMuted`,
  same value); the selectable text preview (a text file, and a `.LAY` header that will not decode)
  12 to 13 px with its ink unchanged at white (`kTextPrimary`); an empty file now goes through the
  toolkit's empty state - an inbox icon (36 px, `kTextMuted`) over the message - and an empty
  `.SNREG` prints `(no registry entries)`, which the page printed nothing at all for before. `.SNREG`
  becomes a two-column table with a label row (ID 12 px `kOrange` out of a fixed 88 px box into a
  flex column; serial 12 to 13 px, white70 to `kTextSecondary` - same value); a tombstoned row's ID
  white24 to `kTextDisabled` (*brighter*, `white38`) and its serial the same token at 12 to 13 px;
  `.DV_` becomes a three-column table (label mono 13 px, type muted 13 px, value mono 13 px, all from
  12 px), with no
  label row, under the block-name line (11 to 13 px); the `.DT_`/`.SV` rows become three-column tables
  and `.SUBREQ` a two-column one - the 10-12 px title/subtext goes to 13 px, the `.DT_`/`.SV` per-row
  left indent and the `Colors.black26` band under the decoded rows are gone (those rows now sit on the
  card surface, and the embedded tables drop their label row), the `N entries` subtitle becomes a chip
  (10 to 12 px), the block title 16 to 15 px (w600 unchanged), the SUBREQ leading icon 20 to 18 px and
  its trigger/period/provider line 11 to 13 px; the LAY grid's header 11 to 13 px with its ink
  white38 to `kTextMuted` - *brighter* - its cell text 10 to 12 px (`labelSmall`), its missing-cell
  ink white24 to `kTextDisabled` (white38, *brighter*) and its cell gap 1 to 2 px (the 44x22 cell box
  is unchanged).
- Storage: the list's two-line tile (name over detail) becomes one table row with a label row - name
  and detail side by side at flex 4:6, so in a narrow window the name ellipsises where it once had the
  full width; the name 16 to 13 px (a file-table entry's name white38 to `kTextMuted`, *brighter*)
  and the detail 15 to 13 px muted; the type icon 24 to 18 px, the lock 16 px white24 to
  `kTextDisabled` (white38, *brighter*), no row dividers, `Insets.row` padding; an empty list keeps
  `No file table` and gains the inbox icon. The file-table page's records become four columns (`#`,
  File, Size, Offset) with a label row: the headline 15 to 13 px and the record name 12 to 13 px
  (invalidated white24 to `kTextDisabled`, *brighter*), the per-row type/delete icon is gone, the
  size is `formatFileSize` per column where the row printed `0.5 kB` in the title and `512 B` in the
  subtitle, and an empty live table prints `No live records` where it printed nothing; the
  invalidated toggle's headline 12 to 13 px.
- SNDB: the registry becomes a table with a label row - the ID leaves a leading `Text` (15 px,
  proportional) for a mono column (13 px), the serial goes 12 to 13 px and white to
  `kTextMuted` (white54 - two steps *dimmer*: the cell is `muted: true`, and the muted token still
  measures 4.61:1 on a card), the type icon moves from trailing to leading (18 px on both sides of
  the move), and the orange row dividers are gone;
  `Database empty` keeps its words and gains the inbox icon.
- Subscriptions: the detail fields become a two-column table (label 15 to 13 px `kTextMuted`, value
  mono 15 to 13 px) with the label column sharing the row by flex instead of a fixed 120 px; the
  unavailable-tab message 15 to 13 px (padding 16 to `Spacing.xl`, the same 16); the error line 15 to
  13 px (ink already `kError`); the delete icon `Colors.red` to `kError` and 24 to 20 px; "Add
  Subscription" loses its 14 px padding override (the theme's 16/12 applies), making the button 4 px
  shorter; both empty tabs keep their words and gain the inbox icon, white54 15 px to `kTextPrimary`
  15 px - *brighter*.
No visible change where the diff only swapped a literal for its own token: the log tile's card colour
(`kSurfaceAlt` is the theme's card colour), its 20 px icon, the 12 px page padding, the 6/2 chip gaps
and the 2 px card margins all keep their old values.

**1c.5 (Connection / Devices / Device view).** The two device lists are `DeviceTile` rows (one row shape on
both pages, its trailing content the page's own: the Connection row's signal readout plus autoconnect star,
the Devices row's core star), and Device view's app-bar title is `DeviceTitle` - a row whose name
ellipsizes inside a `Flexible` while the rename mark keeps its declared 16 px slot. The three fixed-pixel
rows this batch owned are gone: the Devices filter bar's `SizedBox(height: 56)` + `Row` is a `Wrap`
(measured with its longest real labels - "Net 255" and "Type: Tamu v2.0A" - the three controls want
140 + 176 + 48 px plus two `Spacing.md` gaps = 380 px, so a 360 px window moved the sort menu to its own
line; the wrapped bar is 324 px wide and two lines at 360, one line at 800), and the graph's 160 px block
is derived from the width the canvas is given - the even share of `width - 2 * Spacing.xxl` across the
blocks that fit, floored at 128 px and capped at 200 px - with rows of three kept as the *wide* arrangement
only (360x640 gives 150 px blocks, two per line; 800x360 gives 200 px blocks, three per line).
`Docs/App/Devices.md:24`'s "rows of three" stays unresolved in the spec, so the canvas stays pannable and
zoomable instead of gaining a fixed grid. Tokenised while there: the graph's 24/32/12/6 px insets and the
Device view's 12/24 px ones (`Spacing.*`, `Insets.page`), the RSSI gap (`Spacing.xs`), and the three fixed
icon slots are now declared constants (`_signalIconSize` 18, `_coreStarSize` 18, `_editIconSize` 16).
Pinned by `test/devices_layout_test.dart` (16 cases: the filter bar, both pages, the device row, the graph
and the Device view title at 360x640 **and** 800x360, all `takeException() == null`, the title also at a 2x
text scale); the title row fails on the pre-fix shape with `A RenderFlex overflowed by 116 pixels on the
right.` at 360x640, and 243 px at 2x.
**Visible changes of 1c.5 to veto:**
- Devices: a stale entry's marker is now `  ·  stale` in `labelSmall`/`kTextMuted` after the name - it was
  `' (stale)'` in the title's own style, same word, dimmer and smaller; the sort menu no longer sits at the
  right edge (it follows the type chip in the wrap, and moves to the next line when the width does not hold
  all three); the bar no longer pins 56 px of height - a line is the 48 px chip, so two lines measure 100
  with the `Spacing.xs` run gap (measured: 324x100 at 360 wide, 380x48 at 800).
- Connection: the row order, star button and tooltips are unchanged; the readout's icon ink is `kSuccess`
  (the value `greenAccent` had), `kError` `#FF8A80` where it was `redAccent` `#FF5252` and `kTextMuted`
  (white54) where `white38` for a missing reading - *brighter*, deliberately (3.58 to 5.01, 3.41 to 5.26).
- Device view: the rename mark's gap 6 to `Spacing.sm` (the same 6); the rename-cancel mark
  `Colors.redAccent` to `kError` (`#FF8A80`, *lighter*), the confirm mark `Colors.greenAccent` to
  `kSuccess` (same value); the section divider's 24 px and the list's 12 px are the same as `Spacing.xxl` /
  `Insets.page`.
- Graph: the block is a derived 150 px at 360 wide and 200 px at 800 (it was a constant 160); the card's
  own 4 px margin is gone (the wrap's `Spacing.lg` gutter replaces it); the subtext is one ellipsized line
  where it wrapped freely; rows are centred as a group where they spread with `spaceEvenly`.
No visible change where the diff only swapped a literal for its own token: the `DeviceTile` row itself (a
plain `ListTile`), the Connected/Connecting session rows (untouched `ListTile`s - session status, not list
entries), the graph's icon, card colour and 12 px block padding, and the Device view's `InfoCard` /
`InfoRow` sections (already folded onto `InfoRow` at `9729800`, so this batch only had the title row to
fix). Deliberately out of this batch: the shell/`ShellLayout` two-axis fix (b1), which belongs to the
parked responsive lane, and any `Docs/` edit.

Same behaviour per page; one batch per cluster, stopped for Akyirr's review after each. Per batch:
`dart format`, then `dart fix --apply --code=curly_braces_in_flow_control_structures`, then
`dart format` again; `flutter analyze` clean; `flutter test --exclude-tags hil` green; `version.json`
in the same commit. The agent can run the app gates in-tree (`app/.dart_tool` is writable, unlike the
firmware's ESP-IDF tree), but it cannot run the GUI - so a batch ends with a build handed over plus a
what-changed-where checklist, and the HIL suite for anything touching bus behaviour.
`dart format` is not tree-clean under the installed SDK (3.12.2): six test files were formatted by an
older one and want a rewrite (`backup_value_test.dart`, `backup_restore_test.dart`,
`backup_script_options_test.dart`, `backup_script_raw_test.dart`, `backup_provider_capture_test.dart`,
`hil_bootloader_upload_test.dart`). 1c.4 formatted only the files it touched and left that churn in
place, so a `dart format .` sweep is its own card.

### Responsive shared layer (2026-10-21) - two-axis shell, dialog bodies that fit

From the read-only study of the same date: the app must fit both vertically and horizontally oriented
screens, and the decision per element comes from that element's own minimum size against the space it
is given - never from a window width alone. Two breaks were measured and fixed in the shared layer, so
every page inherits the fix:

- **Shell (`ShellLayout`, `widgets.dart`).** The gate was width only (600 px), but the labelled
  `NavigationRail` needs ~389 px of height for five destinations plus the leading badge and Flutter's
  rail does not scroll. A landscape phone window (640x360, 800x360, fresh mount) overflowed it by
  `23 pixels on the bottom`. `isCompact` is now `width < 600 || height < 400`: a short window gets the
  Drawer and the app bar's hamburger, however wide it is. 1024x600 and 1280x400 keep the rail.
- **Dialogs/pickers (`DialogBody`, `widgets.dart`).** The body used to hand its content whatever
  `SizedBox` the caller asked for after a `window - 180` guess, so a content-sized body (a form column,
  a field, a radio list) laid out at its natural height inside a 158 px box at 640x360 and overflowed
  (882 px with 20 rows; 346-466 px for a 12-field form) - clipped and unreachable. A body that fills
  its box (a picker list, a viewport, or a `Column` with an `Expanded` child) still gets the box; every
  other body now gets a scroll view, so its rows stay reachable. The caller's `height` stays the
  largest box it may ask for and the dialog's own constraints clamp it, which is why
  `PickerShell(500, 400)` still measures 500x158 at 640x360 but now scrolls to its last row.
  The decision is the shared `fitToBox`, so a shell that wraps the body in its own flexible row
  applies the same rule inside the wrapper: `PickerShell`'s hint line keeps an `Expanded` around the
  body, and a content-sized body there used to be stretched to the leftover height and overflowed by
  `903 pixels on the bottom` at 640x360 instead of scrolling.

Pinned by `test/responsive_layout_test.dart` (real shell at 640x360 / 800x360 / 1024x600 / 1280x400;
`PickerShell(500, 400)` with a viewport body and with a content-sized body at 640x360 and 360x480,
plus the hint line above a content-sized body at 640x360); the same test fails on the pre-fix code
with the 23 px rail exception, the 882 px form overflow and the 903 px hint overflow.
Per-element variants stay per batch - the 1c.5 / 1c.6 pages own their own layouts.

### Contrast audit (2026-10-17) - theme level fixed, per-page offenders parked

Akyirr: "some UI elements are hard to see because foreground and background colours sit too close
together", and (added mid-run) the offending group is multiple **app-bar** elements. Measured, not
guessed: `dart run tool/contrast.dart` from `app/` reads the tokens out of `lib/ui/theme.dart`,
composites the translucent ones over the surface they are drawn on and prints the WCAG ratio of all
73 pairs the theme and the shared widgets render (4.5:1 body text, 3:1 large text and non-text UI;
exit 1 on any failure). Ad-hoc pair: `dart run tool/contrast.dart --pair <fg> <bg>`.
The "before" column is reproducible - the pre-fix palette is committed as a fixture:

    dart run tool/contrast.dart --theme tool/fixtures/theme_before_2026_10_17.dart

Two of its app-bar rows have no pre-fix counterpart (the busy spinner and the ringed autorefresh
mark's ring) and one later fill (`kActiveLineTint`) is not in that palette at all: those rows carry
their own label or `n/a` marker instead of a pre-fix ratio, so the number next to a row that did not
exist cannot be read as "it passed before". The pre-fix spinner's own pair is
`--theme tool/fixtures/theme_before_2026_10_17.dart --pair kOrange kOrangeDark` = 1.17:1 - the
palette-qualified form, because *both* of that pair's tokens moved in this audit (the fixture's
`kOrange` `#F57600` / `kOrangeDark` `#E16D00` are now `#FF8A1F` / `#F57600`), so the bare
`--pair kOrange kOrangeDark` on the current palette prints 1.19:1 instead.

**Before (fixture): 29 pairs below threshold + 6 close + 5 n/a (exit 1). After: 0 below, 7 close**
(all >= threshold, exit 0). The 7 close calls are 4.88 app-bar TabBar label, 4.61 `bodySmall` on a card,
4.85 accent on a card, 3.12 x3 control boundary/checkbox/switch outline on a card, 4.79
`kSymConstant` on a card. (An earlier handoff quoted "23 below + 6 close" from an uncommitted
pre-fix copy; the fixture run above supersedes it - the numbers differ because the fixture is the
real `d982495^` palette, including the badge pair and the pre-fix rendered inks.)

Theme-level changes (all in `ui/theme.dart`; commits `d982495` + this one, app version 26.10.17.6):

- surfaces darkened: `kSurface` `#353C3F` -> `#262C2F`, `kSurfaceAlt` `#4F575C` -> `#333B3E`, so
  every white-alpha text role reaches 4.5:1 on a card (`bodySmall` was 3.45);
- `kOrange` lightened `#F57600` -> `#FF8A1F` (4.00 page / 2.62 card -> 6.00 / 4.85) and `kOrangeDark`
  is now the old accent (`#F57600`), i.e. the app bar keeps its orange;
- app bar (the group Akyirr named): ink pinned to `kOnAccent` - title 7.47:1, leading and action
  icons 7.47:1 - with `actionsIconTheme` set so the action icons cannot drift, `kOnAccentDim` (3.36)
  for a disabled action (`kTextDisabled` was 1.59 on the bar), the refresh spinner moved off
  `kOrange` (1.17 -> 7.47), and the autorefresh mark is now a ringed dot (ring 7.47, fill 14.7
  against the ring; the bare dot was 2.30 green / 1.03 red on the bar). `PageAppBar` wraps its
  actions in that ink theme, so a disabled page action is covered too; the Logs app bar's filter
  badge (a label-less `Badge`, `log_page.dart:205`) was painting `colorScheme.error` at 1.03:1 on
  the pre-fix bar (1.23 on the new one) - a `badgeTheme` now fills it with `kBadgeFill`
  (`kOnAccent`), 7.47:1;
- `kError` `redAccent` -> redAccent 100 `#FF8A80` (3.52 page / 2.31 card -> 6.20 / 5.01), `kLine`
  `white24` -> `white38` (borders: 2.07/1.82 -> 3.41/3.12), input hints `kTextDisabled` ->
  `kTextMuted`, unselected switch track a `kSurface` well with the `kLine` outline, `kSubtleTint`
  now a fill *below* its surface (`#14000000`) so ink on it improves, chips carry their tone in the
  text on that fill, `kSymOutput`/`kSymConstant` a Material step lighter.
  The `kError` page figure was corrected 2026-10-17 (the leftovers card): an earlier draft of this
  bullet said 6.35, `dart run tool/contrast.dart --pair kError kSurface` prints 6.20:1 - the number
  is `theme.dart`'s too, and its comment was corrected with this one. `kOrange` on the page is 6.00
  and `kLine` on the page 3.41 for the same reason (drafts said 6.14 / 3.43);
  `kOnAccentDim` on the bar is 3.36 and black on `kSurfaceAlt` 1.84.

Leftovers for the batches that own the files (do **not** fix out of batch; ratios are against the
new tokens, measured with `tool/contrast.dart --pair`):

- **Batch 4 (viewers) - closed by 1c.4.** `log_page.dart:295` (`white38`, 11 px, 3.41), `:317`
  (`white38`, 10 px, 3.41), `:325`/`:341` (`Colors.redAccent`, 13 px, 3.58 on a card);
  `storage_page.dart:247`, `:388`, `:432` (`white38` 3.12-3.41; `white24` icons on `:255` 2.08 - below
  3:1); `file_viewers.dart:172`, `:266`, `:320`, `:527`, `:591` (`white38` 3.12-3.41), `:222`, `:233`,
  `:290` (`white24` 2.08). All of these were raw Material steps where the tokens existed:
  `kTextMuted` (5.26 page / 4.61 card) and `kLine`/`kTextDisabled` (3.41/3.12).
  `subscriptions_page.dart:137`, `:148`, `:211`, `:223`, `:304` (`white54`) and `:269` (`Colors.red`)
  were styling debt, not failures - the TabBar override at `:90-96` now measures 4.88 (black at 0.7
  alpha on the new bar; the old bar was 4.39), so it passes.
  **Closed:** the muted-text sites took `kTextMuted`, the icon/line sites `kTextDisabled`, the two
  `redAccent` title icons and the delete icon `kError` (5.01 on a card), and the five `white54` labels
  `kTextMuted` (same value, now a token). Re-measured after the batch with `--pair`: `kTextMuted`
  on `kSurface` 5.26 / on `kSurfaceAlt` 4.61, `kError` on `kSurfaceAlt` 5.01, `kTextDisabled` on
  `kSurfaceAlt` 3.12, `kOrange` on `kSurface` 6.00. `dart run tool/contrast.dart` still prints
  `73 pairs: 57 ok, 7 close, 0 FAIL, 9 exempt` - the 7 close and 9 exempt rows did not grow.
- **Batch 5 (Connection / Devices / Device View) - closed by 1c.5.** The RSSI readout's "no reading" ink
  took `kTextMuted` (white38 3.41 to 5.26 on the page) and a weak link `kError` where it was `redAccent`
  (3.58 to 5.01); `device_view_page.dart`'s rename-cancel mark took `kError` (3.58 to 5.01 on a card) and
  its confirm mark `kSuccess` (the value `greenAccent` already carried). Left as recorded, not fixed: the
  banner's `kOrange.withAlpha(38)` fill - a second accent alpha the token layer should own; the
  `devices_page.dart` graph/filter `kOrange` icons already pass at 4.85 on a card.
- **Batch 3 (value editors).** Only the wire-data swatches (`Color.fromARGB` from a device colour,
  `value_editor_visual.dart:16`, `:170`) - deliberate, not a design colour.
- **Batch 6 (Update / Backup / Settings).** `update_page.dart:358-359` (`redAccent` 3.58 on a card);
  `backup_page.dart:438` (`Colors.orangeAccent` 6.07, passes - styling debt).
- **No batch owns it (recorded 2026-10-17, not fixed):** `app/lib/main.dart:103`/`:110` - the shell's
  link badge - draw their inactive state in `Colors.white38` (the value `kTextDisabled` carries), so
  the rail's `OFF` label (10 px text) measures
  `dart run tool/contrast.dart --pair '#62FFFFFF' kSurfaceAlt` = 3.12:1 against the 4.5:1 it needs.
  The fix token is `kTextMuted` (4.61 on the rail); the 24/30 px icon is non-text (3:1) and 3.12
  passes. This is *not* the checker's exempt "disabled control / icon ink (`kTextDisabled`)" row -
  that row is a disabled control's ink, `IconButtonThemeData.disabledForegroundColor` (`theme.dart`
  `_iconButton`, the checker's where-text `theme.dart:425 _iconButton disabledForegroundColor`) -
  and it is not an accepted exemption either: WCAG 1.4.3 exempts an *inactive user interface
  component*, and the `OFF` badge is a status label rather than a component that can be inoperative,
  so it stays an offender. Like the batch-owned entries above it is listed here and left out of the
  checker, which keeps its exit 0.
- **Found while re-measuring the numbers above (2026-10-17, recorded, not fixed):**
  `script_editor_line.dart:123` (the line index) and `:271` (a symbol chip's role hint) draw 12 px
  informative text in `kTextDisabled` - 3.12 on a card (`--pair kTextDisabled kSurfaceAlt`) and 3.22
  on the `kSubtleTint` wash an inactive line draws (`--pair kTextDisabled '#2F3639'`), 2.72 on the
  active line's old `kAccentTint` wash (`--pair kTextDisabled '#584938'`; both literals are that wash
  composited over the card) - where the token means "unavailable", not "de-emphasised"; and
  `:155` labels the instruction chip in `kOrange` on its `kAccentTint` fill = 3.67
  (`--pair kOrange '#584938'`). The tokens they would take are `kTextMuted` (5.26 page / 4.61 card)
  and `kTextPrimary` (8.66 on that wash).
- **The checker's own breadcrumbs have drifted (recorded 2026-10-17, not fixed):** ~30 of the
  `theme.dart:NNN` line numbers in `tool/contrast.dart`'s `where` texts are stale, and this section
  quotes several of them, so a reader who follows one lands on the wrong rendering. The two
  spot-checked at app version 26.10.17.8: the tab-divider breadcrumb `theme.dart:368` is off by 81
  lines (that line is a `),`; `dividerColor: kLine` is `theme.dart:449`) and the checkbox side
  `theme.dart:382` by 70 (`CheckboxThemeData` is `theme.dart:452`). Line numbers in a 550-line theme
  file go stale on every insertion, so the durable fix is symbol names - `theme.dart _iconButton`,
  which the disabled-ink row above now carries - as a `tool/contrast.dart` sweep on its own card.
- **Not rendered, so not fixed:** table row striping (the viewers have no zebra rows) and a disabled
  action in a page's own `AppBar` (`subscriptions_page.dart:99` is never disabled) - see the
  `tool/contrast.dart` exempt section for the pairs that WCAG does not hold to a threshold.

**Closed 2026-10-17 (the leftovers card, app version 26.10.17.8).** The Script editor's active line
no longer draws the syntax colours on the `kAccentTint` wash (which is *lighter* than the surface
under it, so the four symbol colours on it sat below 4.5): `script_editor_line.dart:84` fills the
active line with `kActiveLineTint` - `#2E000000`, one step stronger than `kSubtleTint`'s wash, i.e. a
fill *below* its surface. On a card: `kSymOutput` 3.88 -> 5.98, `kSymVariable` 3.91 -> 6.03,
`kSymConstant` 3.62 -> 5.59, `kSymPredefine` 4.30 -> 6.63 (`kSymInput` 6.14 -> 9.46). The row keeps
its `kOrange` border and its `ACTIVE` chip, so the accent marker is unchanged, and the "before"
numbers (current tokens on the old wash) stay re-runnable:
`dart run tool/contrast.dart --pair kSymOutput '#584938'` = 3.88:1, where `#584938` is that wash
composited over the card; with `--theme tool/fixtures/theme_before_2026_10_17.dart` the same literal
measures the pre-fix pink instead. The five pairs are in `tool/contrast.dart` now - with the pre-fix
palette they print `n/a`, since that fill did not exist - so a later change cannot quietly
re-lighten the active line.

### Phase 2 - vocabulary, now including localization

One string layer (`ui/vocabulary.dart`) resolving every user-facing string: term -> user label, with
the technical term as the secondary line (the spec's "subtly display extra information"). Current
offenders: page titles `Register - 1.0`, `Storage - 1.0`, `SN Database`; labels `TRID`, `NetID`,
`Serial number (28 hex chars)`, `BlockInfo`, `Entry flags (field 4)`, `Add dynamic block`, `SNDB 12`,
`Sub #3`, `Matrix (2x3)`, `Raw`.

Rules: name the thing the user acts on, not the firmware service; never a bare number without
meaning; no `#3`, no hex without a plain form.

**Localization rides on the same layer** (Akyirr, 2026-10-15). Nothing exists today - no `l10n.yaml`,
no `lib/l10n/`, and no `flutter_localizations`/`intl` in `app/pubspec.yaml`. Intended shape: the
catalogue is ARB via `flutter gen-l10n`, so the vocabulary table *is* the message catalogue instead of
a second copy of it. English is the default locale; the "avoid unnecessary libraries" rule in
`General Info.md` still stands, so the only additions are `flutter_localizations` + `intl` (required
for ARB) and no third-party i18n package. Needs an approved `Docs/App/**` batch (page names, labels,
locale rules) - Akyirr owns the wording, delivered as batches of four-at-a-time decisions.

### Phase 3 - "the design should lead them" - open, deliberately deferred

Held out of the plan until Akyirr picks it up. Candidate rules, to be applied per page during 1c when
it lands: one primary action, visually dominant; prevent rather than report (disable what cannot
work, reason on the control); default everything (prefill the likely value, remember the last
choice); fewer choices (rare options collapse into the existing menus, no new tabs); no bare failures
(what happened -> what to do). Per Akyirr, the service menu stays and shows everything.

## Script system (2026-10-11) - implemented; three items open

The staged plan landed on both sides (the `SCR_XXX` header, the Service instruction set, the CID 3
report record, CID 5's call stack, the streamed CID 0 slot map and local/macro/global functions with
link-at-load, plus the extended Log struct); gate green, HIL 55/55 after the later waves. The detail
is in the git history.

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
no size regression — for DAS the ceiling is 13824 B of code space (`0x800`-`0x3E00`) and 2048 B RAM
(1056 B statics + 992 B stack, full by design), against a current image of 13748 B, i.e. 76 B of
headroom. Platformio's 14336 B denominator is the storage-inclusive App size (`0x800`-`0x4000`), not
code space, so its 95.9 % is not a fullness figure; the image occupies 99.45 % of code space (13748
of 13824 B). The `<= 13440 B flash / <= 2028 B RAM` pair this criterion carried from
2026-10-11 was a measurement, not a budget - see `Issues.md` E30.
The rest: the Android APK installs and connects to the rig; the update flow completes end to end on
Android.

Open:

- **The update flow end to end on Android** (connect -> detect -> update app/bootloader) - the
  sequence the tester performs, and the last acceptance criterion. Tracked with `Issues.md` E20.
- **The app interface pass** (`Docs/App/**`) - superseded by the "App UX pass (2026-10-15)"
  section above. The older ranked fix order and its audit
  (`~/.hermes/notes/tamu-ui-audit-2026-10-08.md`) are historical: phase 1 was approved 2026-10-15,
  phase 2 is planned, phase 3 is open.
- **Phase 5 - optimization + final verification**: DAS headroom (99.45 % of the 13824 B code space,
  76 B left; RAM full by design), a final gate run, and the release checklist.

## Open work

- **Autorefresh on Devices and Device View** - `Docs/App/Devices.md` specifies it on both pages, but it
  exists only in `connection_page.dart` and `widgets.dart` today. The 1b library now provides it
  (`PageActions.autoRefresh`), so adding it is a page adoption for 1c, not new plumbing. Raised during the
  UX pass (doc-conformance gap, not a plan change).

- Router capability bit: reserve one for the router service so the documented capability can be set.
- App Active enum: add `WiFi` (`Enums.h:34-38`) when that work starts. E10 dropped `LegacyBt` from
  the documents, so it is no longer listed as a planned state.
- [ ] **Valu app works standalone but NOT when launched by the bootloader (open; candidate fix
      applied, unverified).** Verified
      2026-10-07: the identical app image, ISP-flashed at 0x0 (no bootloader), runs and enumerates
      as `1a86:6001 "Valu v2.0"` and reaches its main loop; launched from the bootloader
      (`((void(*)())0x3000)()` from `JumpToApp`) it never reaches `main` - the LED stays dark and no
      USB device appears. Bisected with raw-register LED blinks injected into the custom startup:
      the app's `.init` (its very first instruction) **does** run, so the jump lands, but the CRT
      does not finish. App-side attempts that did NOT fix it: (a) skipping the framework's
      `SystemInit` when the PLL is already up (`ValuClockInit` via the custom
      `startup_ch32v20x_D6.S`), (b) putting SYSCLK back on HSI before the jump (this one makes the
      *bootloader* hang on SWS). Leading suspects: the CPU state the bootloader leaves (its
      `mtvec` points into the bootloader, so an early app fault vectors into bootloader code; CSRs
      `0xbc0`/`0x804`; a stale flash prefetch buffer).
      **Candidate applied 2026-10-15 (unverified - no hardware to confirm or revert):** `JumpToApp`
      now tears the USBD controller down before the jump - disable + clear the `USB_LP_CAN1_RX0_IRQn`
      in the PFIC, drop `EXTEN_USBD_PU_EN`, reset the peripheral - so the app is not entered with a
      live, pending USB interrupt the moment its startup sets `mstatus.MIE`. This is the sweep
      report's "parked Valu's likely root cause" (medium confidence). **Needs a bench run.**
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
- **Valu v2.0 layout and USB facts** (recorded so they need not be rediscovered). Bootloader
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
- [ ] **Rig looks.** `Polygon`/`Star` after the `atan2` fix; a rounded `Square`/`Rectangle`
      (eye-only; the evaluation scene sets no `Rounding`). Needs the display rig.
- **BLE large-reply drain (B11, fixed 2026-10-15).** `AppInterfaceService::TxFlush` now pumps the
      BLE tick mid-stream (`TxPeek`/`TxCommit` in `AppBLETick`) when BLE is the active link, so a
      reply larger than the TX ring can complete instead of losing its tail fragments. Builds +
      native gate pass; **needs a rig run to confirm a whole-file read completes over BLE.**

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
  deadzone); requester 28 B / provider 32 B; the **provider** holds a 120 s lease renewed by
  keepalive/updates, while a **requester row never expires** - it goes only on an explicit cancel,
  an orphan announcement or a new setup (`t_eb35360c`), with the retry cadence as the bus guard
  (fast while unanswered, capped at 5, then the 60 s keepalive); running out of fast renewals is
  **logged once per episode** (`t_62e8f3d3`: one `SUB` line with TRID, provider, target register and
  uptime, saying the row is kept and on the keepalive - the flag is RAM-only and clears on an
  answer); CIDs
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
- **The DAS's flash image does not cover the storage region** (code ends ~0x3DB4, storage at
  **0x3E00**), so reflashing preserves the `.SV`; the erased-mirror handling makes a reflash recover.
- **`.SUBREQ` is removed when the requester table empties** (`SaveRequesterTable` deletes it at
  count 0).
- **Backup zips** are semantic format 2, one JSON per device (no manifest); entries are built from
  UTF-8 bytes. Large files are skipped above 128 kB by default. An LED layout (`LAY_1`) is semantic
  like a script: `layouts[]` carries the decoded map (brightness byte, width, height, LED indices,
  `null` for an unused cell) and the restore rebuilds the file, so `files[]` holds only a file with
  no semantic form - a layout the decoder rejects (older 2-byte header, size mismatch) keeps its
  bytes there. `format` stays 2: `layouts` is additive, and an archive without it restores its raw
  `LAY_1` exactly as before.

- **Android builds.** All distributable APKs are built on Akyirr's machine; a build made here signs
  with a different debug key, so one must never be installed over an existing install or handed to
  anyone (agent builds are verification only). Java must be 21 - AGP 8.11 rejects Studio's JBR 25.
  `app/build/` holds Akyirr's artifacts, so an agent build fails at the copy step
  (`Could not set file mode 664`). Flutter 3.44 warns that `build.gradle.kts` and
  `flutter_libserialport` still apply the Kotlin Gradle Plugin (universal_ble and file_picker
  migrated to built-in Kotlin in the 2026-10-15 package bump); a future Flutter will fail on that.
