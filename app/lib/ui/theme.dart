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
const Color kSurface = Color(0xFF353C3F);

/// Raised surfaces: cards, dialogs, popup menus, the navigation rail, inputs.
const Color kSurfaceAlt = Color(0xFF4F575C);

/// The accent: section headings, selected states, quiet/primary text actions.
const Color kOrange = Color(0xFFF57600);

/// The accent one notch darker, for the app bar (black text sits on it).
const Color kOrangeDark = Color(0xFFE16D00);

/// Headings, and the value you came to the page to read.
const Color kTextPrimary = Colors.white;

/// Labels and secondary values.
const Color kTextSecondary = Colors.white70;

/// Subtext under a title, row subtext, inactive icons.
const Color kTextMuted = Colors.white54;

/// Unavailable text, disabled icons, hints.
const Color kTextDisabled = Colors.white38;

/// Hairline borders and outlines on a dark surface.
const Color kLine = Colors.white24;

/// Scrim / shadow.
const Color kShadow = Colors.black26;

/// Link up, success, the autorefresh dot.
const Color kSuccess = Colors.greenAccent;

/// A problem worth noticing but not a failure.
const Color kWarning = Colors.amberAccent;

/// Failure.
const Color kError = Colors.redAccent;

/// Neutral notice, where a status has no success/failure meaning.
const Color kInfo = Colors.lightBlueAccent;

/// Tinted fills for badges and selected rows: [kAccentTint] marks the notable
/// one, [kSubtleTint] is a plain tag. (These replace the old inline
/// `withAlpha` calls.)
const Color kAccentTint = Color(0x2EF57600);
const Color kSubtleTint = Color(0x0EFFFFFF);

/// Script symbol colours: the editor's syntax colouring, by symbol kind. They
/// live here rather than inside `script_editor_line.dart` (where they were
/// private constants) because the pickers need the same palette.
const Color kSymInput = Color(0xFFFFD54F); // amber
const Color kSymOutput = Color(0xFFF06292); // pink
const Color kSymVariable = Color(0xFF64B5F6); // blue
const Color kSymConstant = Color(0xFFBA68C8); // purple
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
  onPrimary: Colors.black,
  secondary: kSurfaceAlt,
  onSecondary: kTextPrimary,
  surface: kSurface,
  onSurface: kTextPrimary,
  error: kError,
  onError: kTextPrimary,
);

/// The app bar: the darker orange with black text. The title takes
/// [TextTheme.titleLarge] so the two cannot drift apart.
AppBarThemeData get _appBar => AppBarThemeData(
  backgroundColor: kOrangeDark,
  foregroundColor: Colors.black,
  elevation: 0,
  scrolledUnderElevation: 0,
  surfaceTintColor: Colors.transparent,
  centerTitle: false,
  iconTheme: const IconThemeData(color: Colors.black),
  titleTextStyle: _textTheme.titleLarge?.copyWith(color: Colors.black),
);

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
  hintStyle: const TextStyle(color: kTextDisabled, fontSize: 15),
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
  checkColor: const WidgetStatePropertyAll<Color>(Colors.black),
);

SwitchThemeData get _switch => SwitchThemeData(
  thumbColor: WidgetStateProperty.resolveWith(
    (states) =>
        states.contains(WidgetState.selected) ? Colors.black : kTextDisabled,
  ),
  trackColor: WidgetStateProperty.resolveWith(
    (states) => states.contains(WidgetState.selected) ? kOrange : kLine,
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

/// The wide-layout rail (a compact window gets the drawer instead).
NavigationRailThemeData get _rail => const NavigationRailThemeData(
  backgroundColor: kSurfaceAlt,
  indicatorColor: kOrangeDark,
  selectedIconTheme: IconThemeData(color: Colors.black),
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
