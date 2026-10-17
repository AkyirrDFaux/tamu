/// The pre-fix palette of the 2026-10-17 contrast audit, as a fixture.
///
/// Why this file exists: the audit quotes "before" ratios, and a number nobody
/// can re-run is not evidence. This is the palette `lib/ui/theme.dart` carried
/// at `d982495^` (the commit before the theme fix), so
///
///     dart run tool/contrast.dart --theme tool/fixtures/theme_before_2026_10_17.dart
///
/// reproduces the before column of the audit in TODO.md ("Contrast audit
/// (2026-10-17)"). The checker only reads `const Color k*` declarations, so the
/// fixture carries the tokens and nothing else.
///
/// Two names are not from the old theme - they are the audit's names for what the
/// old app bar actually *rendered*, which is what the pair list needs:
///
///   kOnAccent     the old `AppBarTheme.foregroundColor` was `Colors.black`
///                 inline, so the title and the icons drew black.
///   kOnAccentDim  a disabled action fell back to `kTextDisabled` (`white38`),
///                 measured on a pre-fix bar at 1.59:1.
///
/// `kBadgeFill` is defined here as the *pre-fix* badge fill: the theme set no
/// `badgeTheme`, so Material painted a label-less badge with
/// `colorScheme.error`, i.e. the old `kError`.
///
/// The checker reports a token this palette does not define as `n/a` rather than
/// aborting, so an older copy can be compared without editing it first.
///
/// Two rows of the audit have *no* pre-fix counterpart and still print a number
/// here, because they name `kOnAccent`, which this palette does define: the busy
/// spinner and the ringed autorefresh mark. The old spinner drew `kOrange` on the
/// old bar - 1.17:1, which needs this palette, not the current one:
///
///     dart run tool/contrast.dart --theme tool/fixtures/theme_before_2026_10_17.dart --pair kOrange kOrangeDark
///
/// (the bare `--pair kOrange kOrangeDark` measures the current palette's `kOrange`
/// on its `kOrangeDark` instead and prints 1.19:1). No ring was drawn at all, so
/// read those two rows as "post-fix rendering", never as "this pair existed and
/// passed" - their labels and `where` text say so.
///
/// The active line's fill is a later addition (`kActiveLineTint`): the five pairs
/// that name it print `n/a` here. Pre-fix - and up to the fix that added the token
/// - that line drew the `kAccentTint` wash, which is lighter than the surface
/// under it, which is exactly what those pairs measure.
///
/// The Flutter import is only so `flutter analyze` accepts the declarations; the
/// fixture is never run, it is read as text by `tool/contrast.dart`.
library;

import 'package:flutter/material.dart';

const Color kSurface = Color(0xFF353C3F);
const Color kSurfaceAlt = Color(0xFF4F575C);

const Color kOrange = Color(0xFFF57600);
const Color kOrangeDark = Color(0xFFE16D00);
const Color kOnAccent = Colors.black;
const Color kOnAccentDim = Colors.white38;

/// The pre-fix badge fill: no `badgeTheme` was set, so Material used
/// `colorScheme.error`.
const Color kBadgeFill = kError;

const Color kTextPrimary = Colors.white;
const Color kTextSecondary = Colors.white70;
const Color kTextMuted = Colors.white54;
const Color kTextDisabled = Colors.white38;

const Color kLine = Colors.white24;
const Color kShadow = Colors.black26;

const Color kSuccess = Colors.greenAccent;
const Color kWarning = Colors.amberAccent;
const Color kError = Colors.redAccent;
const Color kInfo = Colors.lightBlueAccent;

const Color kAccentTint = Color(0x2EF57600);
const Color kSubtleTint = Color(0x0EFFFFFF);

const Color kSymInput = Color(0xFFFFD54F);
const Color kSymOutput = Color(0xFFF06292);
const Color kSymVariable = Color(0xFF64B5F6);
const Color kSymConstant = Color(0xFFBA68C8);
const Color kSymPredefine = Color(0xFF81C784);
