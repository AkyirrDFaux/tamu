/// App theme and design tokens.
///
/// The look is fixed by `Docs/App/General Info.md`: grey-ish blue surface,
/// orange accent, white text. [buildTheme] hands the whole thing to Flutter; the
/// token classes below ([Spacing], [Insets], [Radii], [IconSize]) and the `k*`
/// colours are what the pages use directly.
///
/// The rule this file exists for (TODO.md, "App UX pass", phase 1): a page never
/// invents a colour, a text size, a radius or a gap - it takes a token from here
/// or a widget from `widgets.dart`. The scales are not invented either; every
/// step is a value the tree already uses most (measured: 8 and 12 dominate the
/// gaps, 10-13 px the type sizes, four radii, and `white54`/`white38` carry
/// almost all of the text hierarchy).
library;

import 'package:flutter/material.dart';

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

/// Page background: the grey-ish blue the whole app sits on.
///
/// Contrast audit 2026-10-17 (`tool/contrast.dart`, TODO.md "App UX pass"): the
/// surfaces sit a step darker than they used to (`#353C3F` / `#4F575C`). On the
/// old pair every text role below `white70` fell under 4.5:1 on a card
/// (`bodySmall` 3.45), the accent under 4.5:1 on both (`orange` 4.00 page /
/// 2.62 card) and every hairline under 3:1 (`kLine` 2.07 / 1.82). Darker
/// surfaces are what buys the contrast back without changing the design.
const Color kSurface = Color(0xFF262C2F);

/// Raised surfaces: cards, dialogs, popup menus, the navigation rail, inputs.
const Color kSurfaceAlt = Color(0xFF333B3E);

/// The accent: section headings, selected states, quiet/primary text actions.
/// Lightened from `#F57600` in the contrast audit: as text (12-15 px) it needs
/// 4.5:1 on both surfaces, and the old value reached 4.00 on the page and 2.62
/// on a card (it now measures 6.00 / 4.85).
const Color kOrange = Color(0xFFFF8A1F);

/// The accent one notch darker, for the app bar and the rail's selection pill.
/// This is the old `kOrange`: it is a *surface*, so it must carry the dark ink
/// ([kOnAccent]) rather than be ink itself (black on it is 7.47:1).
const Color kOrangeDark = Color(0xFFF57600);

/// The ink that sits on the accent (`kOrangeDark` bar, `kOrange` fills): app-bar
/// title and icons, a check mark, a selected switch thumb.
const Color kOnAccent = Colors.black;

/// The same ink, dimmed for an inactive control *on the accent bar*: on
/// `kOrangeDark` only a dark ink reaches 3:1, so a disabled app-bar action uses
/// this rather than [kTextDisabled] (which is an ink for the dark surfaces).
const Color kOnAccentDim = Colors.black54;

/// The badge dot's fill: the app-bar ink, because the Material default
/// (`colorScheme.error`) is 1.23:1 on the bar. A named token rather than
/// `kOnAccent` directly so the pair the audit measures and the palette fixture
/// can name the same side of the pair across a revision.
const Color kBadgeFill = kOnAccent;

/// Headings, and the value you came to the page to read.
const Color kTextPrimary = Colors.white;

/// Labels and secondary values.
const Color kTextSecondary = Colors.white70;

/// Subtext under a title, row subtext, inactive icons, input hints.
const Color kTextMuted = Colors.white54;

/// Unavailable text, disabled icons. (WCAG exempts inactive controls, but the
/// value is still measured - see `tool/contrast.dart`.)
const Color kTextDisabled = Colors.white38;

/// Hairline borders and outlines on a dark surface: input borders, checkbox and
/// radio sides, switch outlines, tab dividers, table rules. Raised from
/// `white24` in the contrast audit - a control boundary needs 3:1 and `white24`
/// only reached 2.07 on the page and 1.82 on a card (it now measures 3.41 /
/// 3.12).
const Color kLine = Colors.white38;

/// Scrim / shadow.
const Color kShadow = Colors.black26;

/// Link up, success, the autorefresh dot.
const Color kSuccess = Colors.greenAccent;

/// A problem worth noticing but not a failure.
const Color kWarning = Colors.amberAccent;

/// Failure. Raised from `redAccent` (`#FF5252`) in the contrast audit: as text
/// it needs 4.5:1, and the old value reached 3.52 on the page and 2.31 on a
/// card (it now measures 6.20 / 5.01). `#FF8A80` is Material's redAccent 100.
const Color kError = Color(0xFFFF8A80);

/// Neutral notice, where a status has no success/failure meaning.
const Color kInfo = Colors.lightBlueAccent;

/// Tinted fills. Both are *fills*, so they darken the surface they are drawn on
/// (a fill lighter than its surface costs contrast for every ink on it):
/// [kAccentTint] is the warm one that marks the notable row/badge, [kSubtleTint]
/// is the plain one (a tag, a selected row, a progress track).
/// Re-measured 2026-10-17: [kSubtleTint] is a shade *below* the surface it sits
/// on, so [kOrange] (5.19 on a card) and [kTextSecondary] (6.93) both pass on it.
const Color kAccentTint = Color(0x2EFF8A1F);
const Color kSubtleTint = Color(0x14000000);

/// The fill of the line the instruction counter points at (the Script editor's
/// active line): the plain wash one step stronger, so it darkens the surface it
/// is drawn on, exactly like [kSubtleTint].
///
/// It used to be [kAccentTint], which is *lighter* than the card under it, so
/// every syntax ink on the active line lost contrast instead of gaining it:
/// measured on a card, `kSymOutput` 3.88, `kSymVariable` 3.91, `kSymConstant`
/// 3.62 and `kSymPredefine` 4.30 against the 4.5:1 a 13 px symbol label needs
/// ([kSymInput] was 6.14 and passed). On this fill they are 5.98 / 6.03 / 5.59
/// / 6.63 (9.46 for the input colour). The row keeps the warm marker in its
/// [kOrange] border and its `ACTIVE` chip, so the accent itself is untouched.
const Color kActiveLineTint = Color(0x2E000000);

/// Script symbol colours: the editor's syntax colouring, by symbol kind. They
/// live here rather than inside `script_editor_line.dart` (where they were
/// private constants) because the pickers need the same palette.
///
/// [kSymOutput] and [kSymConstant] moved a Material step lighter in the contrast
/// audit: as 13 px mono text they need 4.5:1 on a card, where the old pink
/// (`#F06292`) measured 3.74 and the old purple (`#BA68C8`) 3.21.
const Color kSymInput = Color(0xFFFFD54F); // amber
const Color kSymOutput = Color(0xFFF48FB1); // pink
const Color kSymVariable = Color(0xFF64B5F6); // blue
const Color kSymConstant = Color(0xFFCE93D8); // purple
const Color kSymPredefine = Color(0xFF81C784); // green

// ---------------------------------------------------------------------------
// Scales
// ---------------------------------------------------------------------------

/// The spacing ladder, in logical pixels. Every gap and inset picks a step, so
/// a page should never need a bare number.
abstract final class Spacing {
  /// Hairline: a label against its value, the inside of a chip.
  static const double hairline = 2;
  static const double xs = 4;
  static const double sm = 6;
  static const double md = 8;
  static const double lg = 12;
  static const double xl = 16;
  static const double xxl = 24;
  static const double xxxl = 32;

  /// One nesting level inside a list (a row inside another list's row).
  static const double indent = 40;
}

/// The composed insets the app repeats, named by where they go.
abstract final class Insets {
  /// Inside a list row or a card row.
  static const EdgeInsets row = EdgeInsets.symmetric(
    horizontal: Spacing.lg,
    vertical: Spacing.sm,
  );

  /// Inside a card.
  static const EdgeInsets card = EdgeInsets.symmetric(
    vertical: Spacing.md,
    horizontal: Spacing.xl,
  );

  /// A page body.
  static const EdgeInsets page = EdgeInsets.all(Spacing.lg);

  /// A nested list row: a field inside a block card (indented one level).
  static const EdgeInsets nestedRow = EdgeInsets.only(
    left: Spacing.indent,
    right: Spacing.lg,
  );

  /// A row one level deeper again: a key or struct member inside a field.
  static const EdgeInsets memberRow = EdgeInsets.only(
    left: Spacing.indent + Spacing.xl,
    right: Spacing.lg,
  );

  /// The outside of a card stacked in a list of cards (the Script editor's
  /// section cards): the list carries no padding of its own, so the gap lives
  /// here.
  static const EdgeInsets cardGap = EdgeInsets.symmetric(
    horizontal: Spacing.md,
    vertical: Spacing.xs,
  );

  /// The inside of a card that holds a form or a section (as opposed to a card
  /// that is itself a list, where the rows own their insets).
  static const EdgeInsets cardBody = EdgeInsets.all(Spacing.lg);
}

/// Corner radii: three steps only - a tag, a control or input, a card/dialog.
abstract final class Radii {
  static const double sm = 4;
  static const double md = 8;
  static const double lg = 12;

  static const BorderRadius tag = BorderRadius.all(Radius.circular(sm));
  static const BorderRadius control = BorderRadius.all(Radius.circular(md));
  static const BorderRadius card = BorderRadius.all(Radius.circular(lg));
}

/// Icon sizes. [md] (18) is the app's de-facto default, [sm] suits dense rows,
/// [lg] app-bar and list-leading icons, [xl] a hero or empty-state icon.
abstract final class IconSize {
  static const double sm = 16;
  static const double md = 18;
  static const double lg = 20;
  static const double xl = 36;
}

// ---------------------------------------------------------------------------
// Text roles
// ---------------------------------------------------------------------------

/// Text roles.
///
/// The body size is **15 px**, which is what the app actually rendered before the
/// token layer: the old theme declared `bodyMedium: 15` and every unstyled `Text`
/// inherited it, so anchoring the ladder on the pages' own 10-13 px literals made
/// the whole app look smaller. The small end is lifted to match - the 10 and 11 px
/// literals are too tight to read, so the label roles carry 12-13 px.
///
/// `w600` is the only emphasis weight that matters (21 uses).
///
/// Read them through the theme (`Theme.of(context).textTheme.bodyMedium`) so a
/// page carries no size of its own.
TextTheme get _textTheme => const TextTheme(
  titleLarge: TextStyle(
    color: kTextPrimary,
    fontSize: 22,
    fontWeight: FontWeight.w600,
  ),
  titleMedium: TextStyle(
    color: kTextPrimary,
    fontSize: 18,
    fontWeight: FontWeight.w600,
  ),
  titleSmall: TextStyle(
    color: kTextPrimary,
    fontSize: 15,
    fontWeight: FontWeight.w600,
  ),
  bodyLarge: TextStyle(color: kTextPrimary, fontSize: 16),
  bodyMedium: TextStyle(color: kTextPrimary, fontSize: 15),
  bodySmall: TextStyle(color: kTextMuted, fontSize: 13),
  labelLarge: TextStyle(
    color: kTextPrimary,
    fontSize: 14,
    fontWeight: FontWeight.w600,
  ),
  labelMedium: TextStyle(color: kTextSecondary, fontSize: 13),
  labelSmall: TextStyle(color: kTextSecondary, fontSize: 12),
);

// ---------------------------------------------------------------------------
// Component themes
// ---------------------------------------------------------------------------

ColorScheme get _scheme => const ColorScheme(
  brightness: Brightness.dark,
  primary: kOrange,
  onPrimary: kOnAccent,
  secondary: kSurfaceAlt,
  onSecondary: kTextPrimary,
  surface: kSurface,
  onSurface: kTextPrimary,
  error: kError,
  onError: kTextPrimary,
);

/// The app bar: the darker orange with the [kOnAccent] ink. Title, leading and
/// actions all take that ink, and `actionsIconTheme` is set explicitly so the
/// action icons cannot drift onto another colour (the AppBar derives the action
/// IconButton theme from it - `app_bar.dart:964-1139`). Measured: the title
/// (titleLarge 22 w600) and the icons are 7.47:1 on the bar; a *disabled*
/// action cannot use the dark-surface ink there (kTextDisabled on the bar is
/// 1.59:1), so `PageAppBar` hands the actions [kOnAccentDim] instead (3.36:1).
AppBarThemeData get _appBar => AppBarThemeData(
  backgroundColor: kOrangeDark,
  foregroundColor: kOnAccent,
  elevation: 0,
  scrolledUnderElevation: 0,
  surfaceTintColor: Colors.transparent,
  centerTitle: false,
  iconTheme: const IconThemeData(color: kOnAccent),
  actionsIconTheme: const IconThemeData(color: kOnAccent),
  titleTextStyle: _textTheme.titleLarge?.copyWith(color: kOnAccent),
);

/// Badges: the one call site is the Logs app bar's device filter
/// (`log_page.dart:205`), a label-less dot that marks "a filter is active". The
/// Material default fill is `colorScheme.error`, which on this bar measures
/// 1.23:1 - a state indicator needs 3:1 - so the dot takes the bar's ink
/// ([kBadgeFill], 7.47:1 as a filled dot, the same logic as the ringed autorefresh
/// mark). A badge placed on a *dark* surface would need its own fill: black on
/// `kSurfaceAlt` is 1.84:1, so the theme default here is the app-bar ink, not a
/// universal one. The label ink is for the labelled variant (none renders today).
BadgeThemeData get _badge =>
    const BadgeThemeData(backgroundColor: kBadgeFill, textColor: kTextPrimary);

/// Cards: the raised grey, flat, with the page owning the gap around them
/// (no built-in margin). This is what `InfoCard` already looked like.
CardThemeData get _card => const CardThemeData(
  color: kSurfaceAlt,
  surfaceTintColor: Colors.transparent,
  elevation: 0,
  margin: EdgeInsets.zero,
  shape: RoundedRectangleBorder(borderRadius: Radii.card),
);

/// Dialogs: raised grey and the card radius, with the card-title role.
DialogThemeData get _dialog => DialogThemeData(
  backgroundColor: kSurfaceAlt,
  surfaceTintColor: Colors.transparent,
  elevation: 0,
  shape: const RoundedRectangleBorder(borderRadius: Radii.card),
  titleTextStyle: _textTheme.titleMedium,
  contentTextStyle: _textTheme.bodyMedium,
);

/// List rows (the app's most-used widget, 53 call sites): dense, muted icons,
/// orange when selected.
ListTileThemeData get _listTile => const ListTileThemeData(
  dense: true,
  visualDensity: VisualDensity.compact,
  minTileHeight: 40,
  iconColor: kTextSecondary,
  textColor: kTextPrimary,
  selectedColor: kOrange,
  selectedTileColor: kSubtleTint,
  contentPadding: Insets.row,
);

/// Dividers keep the existing look: a 1 px orange rule.
DividerThemeData get _divider =>
    const DividerThemeData(color: kOrange, thickness: 1, space: 1);

/// Inputs (47 call sites): filled with the page surface, a hairline outline that
/// turns orange on focus, dense enough to stack two in one dialog.
InputDecorationThemeData get _input => InputDecorationThemeData(
  isDense: true,
  filled: true,
  fillColor: kSurface,
  contentPadding: const EdgeInsets.symmetric(
    horizontal: Spacing.lg,
    vertical: Spacing.lg,
  ),
  border: const OutlineInputBorder(
    borderRadius: Radii.control,
    borderSide: BorderSide(color: kLine),
  ),
  enabledBorder: const OutlineInputBorder(
    borderRadius: Radii.control,
    borderSide: BorderSide(color: kLine),
  ),
  focusedBorder: const OutlineInputBorder(
    borderRadius: Radii.control,
    borderSide: BorderSide(color: kOrange),
  ),
  errorBorder: const OutlineInputBorder(
    borderRadius: Radii.control,
    borderSide: BorderSide(color: kError),
  ),
  focusedErrorBorder: const OutlineInputBorder(
    borderRadius: Radii.control,
    borderSide: BorderSide(color: kError),
  ),
  labelStyle: _textTheme.labelMedium,
  floatingLabelStyle: const TextStyle(color: kOrange, fontSize: 13),
  hintStyle: const TextStyle(color: kTextMuted, fontSize: 15),
  helperStyle: const TextStyle(color: kTextMuted, fontSize: 13),
  counterStyle: const TextStyle(color: kTextMuted, fontSize: 12),
);

/// Progress: the accent, on a faint track, at the 2 px stroke the refresh
/// button already used.
ProgressIndicatorThemeData get _progress => const ProgressIndicatorThemeData(
  color: kOrange,
  linearTrackColor: kSubtleTint,
  circularTrackColor: kSubtleTint,
  strokeWidth: 2,
);

/// Filled = the committing action: bubble grey on white text (unchanged look).
FilledButtonThemeData get _filledButton => FilledButtonThemeData(
  style: FilledButton.styleFrom(
    backgroundColor: kSurfaceAlt,
    foregroundColor: kTextPrimary,
    elevation: 0,
    shape: const RoundedRectangleBorder(borderRadius: Radii.card),
    padding: const EdgeInsets.symmetric(
      horizontal: Spacing.xl,
      vertical: Spacing.lg,
    ),
    textStyle: _textTheme.labelLarge,
  ),
);

/// Text = the quiet action (cancel, "show more"): the accent, no chrome.
TextButtonThemeData get _textButton => TextButtonThemeData(
  style: TextButton.styleFrom(
    foregroundColor: kOrange,
    padding: const EdgeInsets.symmetric(
      horizontal: Spacing.lg,
      vertical: Spacing.md,
    ),
    textStyle: _textTheme.labelLarge,
  ),
);

OutlinedButtonThemeData get _outlinedButton => OutlinedButtonThemeData(
  style: OutlinedButton.styleFrom(
    foregroundColor: kTextPrimary,
    side: const BorderSide(color: kLine),
    shape: const RoundedRectangleBorder(borderRadius: Radii.card),
    textStyle: _textTheme.labelLarge,
  ),
);

IconButtonThemeData get _iconButton => IconButtonThemeData(
  style: IconButton.styleFrom(
    foregroundColor: kTextPrimary,
    disabledForegroundColor: kTextDisabled,
  ),
);

/// Chips/badges: the subtle tint, the tag radius, the small label role.
ChipThemeData get _chip => ChipThemeData(
  backgroundColor: kSubtleTint,
  side: BorderSide.none,
  shape: const RoundedRectangleBorder(borderRadius: Radii.tag),
  labelStyle: _textTheme.labelSmall,
  padding: const EdgeInsets.symmetric(
    horizontal: Spacing.sm,
    vertical: Spacing.hairline,
  ),
);

TabBarThemeData get _tabBar => const TabBarThemeData(
  labelColor: kOrange,
  unselectedLabelColor: kTextSecondary,
  indicatorColor: kOrange,
  indicatorSize: TabBarIndicatorSize.label,
  dividerColor: kLine,
);

CheckboxThemeData get _checkbox => CheckboxThemeData(
  side: const BorderSide(color: kLine, width: 2),
  shape: const RoundedRectangleBorder(borderRadius: Radii.tag),
  fillColor: WidgetStateProperty.resolveWith(
    (states) =>
        states.contains(WidgetState.selected) ? kOrange : Colors.transparent,
  ),
  checkColor: const WidgetStatePropertyAll<Color>(kOnAccent),
);

/// Switch: the unselected track is a well in the page colour with the [kLine]
/// outline (the outline is the control boundary, so it carries the 3:1), the
/// unselected thumb [kTextDisabled] stands out of that well (3.41:1), and the
/// selected state is the accent with the [kOnAccent] thumb on it (8.91:1).
SwitchThemeData get _switch => SwitchThemeData(
  thumbColor: WidgetStateProperty.resolveWith(
    (states) =>
        states.contains(WidgetState.selected) ? kOnAccent : kTextDisabled,
  ),
  trackColor: WidgetStateProperty.resolveWith(
    (states) => states.contains(WidgetState.selected) ? kOrange : kSurface,
  ),
  trackOutlineColor: const WidgetStatePropertyAll<Color>(kLine),
);

RadioThemeData get _radio => RadioThemeData(
  fillColor: WidgetStateProperty.resolveWith(
    (states) => states.contains(WidgetState.selected) ? kOrange : kLine,
  ),
);

/// Popup menus (13 call sites).
PopupMenuThemeData get _popupMenu => PopupMenuThemeData(
  color: kSurfaceAlt,
  surfaceTintColor: Colors.transparent,
  elevation: 4,
  shape: const RoundedRectangleBorder(borderRadius: Radii.control),
  textStyle: _textTheme.bodyMedium,
);

TooltipThemeData get _tooltip => const TooltipThemeData(
  decoration: BoxDecoration(color: kSurfaceAlt, borderRadius: Radii.control),
  textStyle: TextStyle(color: kTextPrimary, fontSize: 13),
  padding: EdgeInsets.symmetric(horizontal: Spacing.md, vertical: Spacing.sm),
  waitDuration: Duration(milliseconds: 500),
);

SnackBarThemeData get _snackBar => SnackBarThemeData(
  backgroundColor: kSurfaceAlt,
  contentTextStyle: _textTheme.bodyMedium,
  behavior: SnackBarBehavior.floating,
  shape: const RoundedRectangleBorder(borderRadius: Radii.control),
);

/// The wide-layout rail (a compact window gets the drawer instead). The selected
/// pill is the accent surface, so its icon takes the [kOnAccent] ink (7.47:1) and
/// only the pill's own colour carries the boundary against the rail (4.07:1).
NavigationRailThemeData get _rail => const NavigationRailThemeData(
  backgroundColor: kSurfaceAlt,
  indicatorColor: kOrangeDark,
  selectedIconTheme: IconThemeData(color: kOnAccent),
  unselectedIconTheme: IconThemeData(color: kTextSecondary),
  selectedLabelTextStyle: TextStyle(color: kOrange, fontSize: 13),
  unselectedLabelTextStyle: TextStyle(color: kTextSecondary, fontSize: 13),
);

/// The app theme. Dark only: the design has no light variant.
ThemeData buildTheme() {
  return ThemeData(
    useMaterial3: true,
    brightness: Brightness.dark,
    colorScheme: _scheme,
    scaffoldBackgroundColor: kSurface,
    textTheme: _textTheme,
    appBarTheme: _appBar,
    badgeTheme: _badge,
    cardTheme: _card,
    dialogTheme: _dialog,
    listTileTheme: _listTile,
    dividerTheme: _divider,
    inputDecorationTheme: _input,
    progressIndicatorTheme: _progress,
    filledButtonTheme: _filledButton,
    textButtonTheme: _textButton,
    outlinedButtonTheme: _outlinedButton,
    iconButtonTheme: _iconButton,
    chipTheme: _chip,
    tabBarTheme: _tabBar,
    checkboxTheme: _checkbox,
    switchTheme: _switch,
    radioTheme: _radio,
    popupMenuTheme: _popupMenu,
    tooltipTheme: _tooltip,
    snackBarTheme: _snackBar,
    navigationRailTheme: _rail,
  );
}
