/// WCAG contrast checker for the app theme (TODO.md, "App UX pass", contrast
/// audit 2026-10-17).
///
/// Why: Akyirr reported UI elements that are hard to see because the foreground
/// and the background sit too close together. "Too close" is measurable, so this
/// script measures it instead of guessing: it reads the `k*` colour tokens out of
/// `lib/ui/theme.dart`, composites the translucent ones over the surface they are
/// drawn on, and prints the WCAG 2.1 contrast ratio of every pair the app
/// actually renders, against the ratio that pair needs.
///
/// Usage (from `app/`, one command, no arguments needed):
///
///     dart run tool/contrast.dart
///
/// Options:
///     `--theme <path>`   theme file to read (default `lib/ui/theme.dart`). Point
///                        it at another palette to compare two revisions - the
///                        pre-fix palette is committed as a fixture, so the
///                        "before" numbers in TODO.md are reproducible:
///
///                            dart run tool/contrast.dart \
///                                --theme tool/fixtures/theme_before_2026_10_17.dart
///
///                        A token the palette does not define prints `n/a` and is
///                        counted as such instead of aborting the run: the
///                        fixture is the pre-fix palette, so a pair that names a
///                        token the audit's own fix added (e.g. `kActiveLineTint`,
///                        the active line's fill) reports that the pair did not
///                        exist yet rather than borrowing this revision's maths.
///                        The fixture *does* define `kOnAccent`/`kOnAccentDim`/
///                        `kBadgeFill`, because those name what the old app bar
///                        actually rendered, which is what the pre-fix app-bar
///                        pairs are about. Two of those rows - the busy spinner and
///                        the autorefresh mark's ring - have no pre-fix
///                        counterpart at all; their labels and `where` text say so,
///                        so their number here is not read as "this pair existed
///                        and passed".
///     `--pair <fg> <bg>` print one ad-hoc pair (a token name, or a `#RRGGBB` /
///                        `#AARRGGBB` literal) and stop; honours `--theme`
///     `--quiet`          print only the failures, the close calls and the summary
///
/// Exit status is 1 when any pair is below its threshold, so it can be used as a
/// gate; 0 when everything passes.
///
/// Thresholds (WCAG 2.1 SC 1.4.3 + 1.4.11, as the card specifies them):
///   4.5:1 body text, including every text role smaller than 18.66 px bold /
///         24 px regular - which in this app is every role except `titleLarge`;
///   3:1   large text, and non-text UI (borders, icons, control boundaries, state
///         indicators).
/// Pairs that WCAG exempts (disabled controls, decoration) are printed in an
/// exempt section with their numbers, so a later change cannot quietly make them
/// invisible.
///
/// Deliberately a plain Dart script, not a Flutter one: `dart run` cannot import
/// `package:flutter/material.dart` (no `dart:ui` outside the engine), so the
/// tokens are read from the theme source instead. The theme stays the single
/// source of truth - change a token there and the numbers here move by
/// themselves. The `Colors.<name>` values the theme references are the SDK's own
/// steps, checked against
/// `packages/flutter/lib/src/material/colors.dart` (white 0xFFFFFFFF, white70
/// 0xB3FFFFFF, white60 0x99FFFFFF, white54 0x8AFFFFFF, white38 0x62FFFFFF,
/// white24 0x3DFFFFFF, black54 0x8A000000, black26 0x42000000, greenAccent
/// 0xFF69F0AE, amberAccent 0xFFFFD740, redAccent 0xFFFF5252, lightBlueAccent
/// 0xFF40C4FF).
library;

import 'dart:io';
import 'dart:math' as math;

void main(List<String> args) {
  final themePath = _argValue(args, '--theme') ?? 'lib/ui/theme.dart';
  final quiet = args.contains('--quiet');

  final pairIndex = args.indexOf('--pair');
  if (pairIndex >= 0) {
    if (args.length < pairIndex + 3) {
      stderr.writeln('--pair needs a foreground and a background');
      exit(2);
    }
    _printPair(args[pairIndex + 1], args[pairIndex + 2], themePath);
    return;
  }

  final file = File(themePath);
  if (!file.existsSync()) {
    stderr.writeln('theme file not found: $themePath (run from app/)');
    exit(2);
  }
  final theme = ThemeTokens.parse(file.readAsStringSync());

  final failures = <String>[];
  final close = <String>[];
  final exempt = <String>[];
  final skipped = <String>[];

  stdout.writeln('theme: $themePath');
  stdout.writeln('tokens read: ${theme.names.length}');
  stdout.writeln('');
  stdout.writeln(
    '${_pad('ratio', 6)} ${_pad('needs', 5)} ${_pad('verdict', 7)} pair',
  );
  stdout.writeln('-' * 100);

  for (final group in auditGroups) {
    if (!quiet) {
      stdout.writeln('');
      stdout.writeln('${group.name}  (${group.pairs.length} pairs)');
    }
    for (final pair in group.pairs) {
      final missing = _missingToken(theme, pair);
      if (missing != null) {
        final row =
            '${_pad('n/a', 6)} ${_pad(pair.needs.toStringAsFixed(1), 5)} '
            '${_pad('n/a', 7)} ${pair.label}\n'
            '${' ' * 20}token not in $themePath: $missing - ${pair.where}';
        skipped.add(row);
        if (!quiet) stdout.writeln(row);
        continue;
      }
      final line = _measure(theme, pair);
      final verdict = pair.exempt
          ? 'exempt'
          : line.ratio >= pair.needs * 1.1
          ? 'ok'
          : line.ratio >= pair.needs
          ? 'CLOSE'
          : 'FAIL';
      final row =
          '${_pad(line.ratio.toStringAsFixed(2), 6)} '
          '${_pad(pair.needs.toStringAsFixed(1), 5)} '
          '${_pad(verdict, 7)} ${line.label}';
      switch (verdict) {
        case 'FAIL':
          failures.add('$row\n${' ' * 20}${line.detail}');
        case 'CLOSE':
          close.add('$row\n${' ' * 20}${line.detail}');
        default:
          if (pair.exempt) exempt.add(row);
      }
      if (!quiet || verdict == 'FAIL' || verdict == 'CLOSE') {
        stdout.writeln(row);
      }
    }
  }

  if (close.isNotEmpty) {
    stdout.writeln('');
    stdout.writeln(
      'Close calls (within 10 % of the threshold, do not overshoot):',
    );
    for (final row in close) {
      stdout.writeln(row);
    }
  }
  if (exempt.isNotEmpty) {
    stdout.writeln('');
    stdout.writeln(
      'Exempt (WCAG 1.4.3/1.4.11: inactive controls, decoration):',
    );
    for (final row in exempt) {
      stdout.writeln(row);
    }
  }

  if (skipped.isNotEmpty) {
    stdout.writeln('');
    stdout.writeln(
      'Not in this palette (token added by a later change; n/a, not a failure):',
    );
    for (final row in skipped) {
      stdout.writeln(row);
    }
  }

  stdout.writeln('');
  stdout.writeln('-' * 100);
  final total = auditGroups.fold<int>(0, (sum, g) => sum + g.pairs.length);
  final ok =
      total - failures.length - close.length - exempt.length - skipped.length;
  stdout.writeln(
    '$total pairs: $ok ok, '
    '${close.length} close, ${failures.length} FAIL, ${exempt.length} exempt'
    '${skipped.isEmpty ? '' : ', ${skipped.length} n/a'}',
  );
  if (failures.isNotEmpty) {
    stdout.writeln('');
    stdout.writeln('Below threshold:');
    for (final row in failures) {
      stdout.writeln(row);
    }
  }
  exit(failures.isEmpty ? 0 : 1);
}

String _pad(String s, int width) =>
    s.length >= width ? s : s + ' ' * (width - s.length);

String? _argValue(List<String> args, String name) {
  final i = args.indexOf(name);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : null;
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

/// One pair the app renders: [fg] and [bg] name a token (or a literal), [bg] may
/// be a [Wash] drawn over a surface, [needs] is the ratio WCAG asks for, [where]
/// says which rendering this is, and [exempt] marks a pair WCAG does not hold to
/// a threshold (disabled controls, decoration) - it is still measured.
class Pair {
  const Pair(
    this.label,
    this.fg,
    this.bg,
    this.needs,
    this.where, {
    this.exempt = false,
    this.note,
  });

  final String label;

  /// Either a token name, or a [Wash] (a translucent token over a surface).
  final Object fg;

  /// Either a token name, or a [Wash] (a translucent token over a surface).
  final Object bg;
  final double needs;
  final String where;
  final bool exempt;
  final String? note;
}

/// A translucent token drawn over a surface: `Wash('kAccentTint', 'kSurfaceAlt')`.
class Wash {
  const Wash(this.token, this.surface);
  final String token;
  final String surface;
}

/// A token drawn at a reduced opacity as a foreground, so the contrast maths
/// composites it over the pair's own background: `Alpha('kOnAccent', 0.7)` is
/// what `Color.withValues(alpha: 0.7)` renders on the bar.
class Alpha {
  const Alpha(this.token, this.opacity);
  final String token;
  final double opacity;
}

class Group {
  const Group(this.name, this.pairs);
  final String name;
  final List<Pair> pairs;
}

class Line {
  Line(this.ratio, this.label, this.detail);
  final double ratio;
  final String label;
  final String detail;
}

Line _measure(ThemeTokens theme, Pair pair) {
  final fg = _foreground(theme, pair.fg);
  final bg = _background(theme, pair.bg);
  final ratio = contrast(fg, bg);
  final detail =
      '${_describe(pair.fg)} ${_hexOfStack(theme, pair.fg)} on '
      '${_describe(pair.bg)} ${_hexOfStack(theme, pair.bg)} - ${pair.where}'
      '${pair.note == null ? '' : ' - ${pair.note}'}';
  return Line(ratio, pair.label, detail);
}

/// The pair's foreground: a token keeps its alpha (the contrast maths composites
/// it over the pair's background), a [Wash] is already composited over the
/// surface it is drawn on, and an [Alpha] takes the token's rgb at that opacity.
List<double> _foreground(ThemeTokens theme, Object fg) {
  if (fg is Wash) {
    return compose(theme.colour(fg.token), theme.colour(fg.surface));
  }
  if (fg is Alpha) {
    final rgb = theme.colour(fg.token);
    return [rgb[0], rgb[1], rgb[2], fg.opacity];
  }
  return theme.colour(fg as String);
}

List<double> _background(ThemeTokens theme, Object bg) {
  if (bg is Wash) {
    return compose(theme.colour(bg.token), theme.colour(bg.surface));
  }
  if (bg is Alpha) {
    final rgb = theme.colour(bg.token);
    return [rgb[0], rgb[1], rgb[2], bg.opacity];
  }
  return compose(theme.colour(bg as String), const [0, 0, 0, 1]);
}

String _describe(Object side) => switch (side) {
  Wash() => '${side.token} over ${side.surface}',
  Alpha() => '${side.token} @ ${(side.opacity * 100).round()} %',
  _ => side as String,
};

/// The first token a pair names that the palette does not define, or null when
/// every token resolves. A palette fixture can predate a token (the pre-fix copy
/// has no `kOnAccent`), so a missing token is reported, not fatal.
String? _missingToken(ThemeTokens theme, Pair pair) =>
    _missingIn(theme, pair.fg) ?? _missingIn(theme, pair.bg);

String? _missingIn(ThemeTokens theme, Object side) {
  if (side is Wash) {
    if (!theme.contains(side.token)) return side.token;
    if (!theme.contains(side.surface)) return side.surface;
    return null;
  }
  if (side is Alpha) return theme.contains(side.token) ? null : side.token;
  final name = side as String;
  return name.startsWith('#') || theme.contains(name) ? null : name;
}

String _hexOfStack(ThemeTokens theme, Object side) {
  if (side is Alpha) {
    final rgb = theme.colour(side.token);
    return '#${rgb.take(3).map((c) => c.round().toRadixString(16).padLeft(2, '0')).join()}'
        .toUpperCase();
  }
  final rgb = _background(theme, side);
  return '#${rgb.take(3).map((c) => c.round().toRadixString(16).padLeft(2, '0')).join()}'
      .toUpperCase();
}

// ---------------------------------------------------------------------------
// The audit: every pair the theme and the shared widgets render
// ---------------------------------------------------------------------------

const List<Group> auditGroups = [
  Group(
    'App bar (kOrangeDark surface, flat and scrolled/elevated alike: '
    'scrolledUnderElevation 0 + transparent surfaceTint, theme.dart)',
    [
      Pair(
        'app-bar title (titleLarge 22 w600)',
        'kOnAccent',
        'kOrangeDark',
        4.5,
        'theme.dart:224-233 titleTextStyle over backgroundColor',
      ),
      Pair(
        'app-bar action icon (enabled)',
        'kOnAccent',
        'kOrangeDark',
        3.0,
        'theme.dart:231 iconTheme; the AppBar hands the actions the same ink '
            '(app_bar.dart:964-1139)',
      ),
      Pair(
        'app-bar action icon (disabled)',
        'kOnAccentDim',
        'kOrangeDark',
        3.0,
        'widgets.dart PageAppBar wraps the actions: a disabled action ink cannot '
            'be kTextDisabled here (it is 1.59:1 on the bar)',
      ),
      Pair(
        'app-bar leading icon (drawer/back)',
        'kOnAccent',
        'kOrangeDark',
        3.0,
        'widgets.dart:188 ShellDrawerButton, AppBar leading',
      ),
      Pair(
        'app-bar refresh spinner - post-fix ink only (pre-fix it drew kOrange, '
            '1.17:1)',
        'kOnAccent',
        'kOrangeDark',
        3.0,
        'widgets.dart RefreshButton refreshing state takes kOnAccent now: there '
            'is no pre-fix pair for this row - the old spinner drew the theme '
            'kOrange on the old bar, 1.17:1 (`--theme '
            'tool/fixtures/theme_before_2026_10_17.dart --pair kOrange '
            'kOrangeDark`)',
      ),
      Pair(
        'autorefresh dot ring - post-fix only (pre-fix there was no ring)',
        'kOnAccent',
        'kOrangeDark',
        3.0,
        'widgets.dart RefreshButton dot: on the bar only a dark ink reaches '
            '3:1, so the mark is ringed rather than drawn as a bare dot; no '
            'pre-fix pair either - the bare dot is the exempt pair below, 2.30 '
            'green / 1.03 red on the old bar',
      ),
      Pair(
        'autorefresh dot fill (active) against its ring',
        'kSuccess',
        'kOnAccent',
        3.0,
        'widgets.dart RefreshButton dot',
      ),
      Pair(
        'autorefresh dot fill (error) against its ring',
        'kError',
        'kOnAccent',
        3.0,
        'widgets.dart RefreshButton dot',
      ),
      Pair(
        'autorefresh dot fill (active) against the bar',
        'kSuccess',
        'kOrangeDark',
        3.0,
        'the ring supplies the boundary, not the fill',
        exempt: true,
      ),
      Pair(
        'app-bar filter badge dot (label-less Badge)',
        'kBadgeFill',
        'kOrangeDark',
        3.0,
        'log_page.dart:205 Badge(isLabelVisible: _deviceFilter != null) - the '
            'fill is theme.dart badgeTheme.backgroundColor; before the theme set '
            'it, Material fell back to colorScheme.error (kError), which the '
            'pre-fix fixture models as the pre-fix kError',
      ),
      Pair(
        'app-bar surface vs page surface',
        'kOrangeDark',
        'kSurface',
        3.0,
        'theme.dart:225 the bar against the body it sits on',
        exempt: true,
      ),
      Pair(
        'app-bar TabBar label (unselected)',
        Alpha('kOnAccent', 0.7),
        'kOrangeDark',
        4.5,
        'subscriptions_page.dart:90-96 draws onPrimary at alpha 0.7 by hand, so '
            'the pair is modelled at that alpha (opaque kOnAccent would print '
            '7.47 and overstate it)',
        note: 'page-owned (batch 4), listed in TODO.md',
      ),
    ],
  ),
  Group('Text roles on the page surface (kSurface)', [
    Pair(
      'titleLarge/Medium/Small, bodyLarge/Medium (primary)',
      'kTextPrimary',
      'kSurface',
      4.5,
      'theme.dart:178-204 on scaffoldBackgroundColor',
    ),
    Pair(
      'labelMedium/labelSmall (secondary)',
      'kTextSecondary',
      'kSurface',
      4.5,
      'theme.dart:202-203, e.g. InfoRow/TaskProgress counters',
    ),
    Pair(
      'bodySmall (muted)',
      'kTextMuted',
      'kSurface',
      4.5,
      'theme.dart:196 on the page',
    ),
    Pair(
      'input hint text',
      'kTextMuted',
      'kSurface',
      4.5,
      'theme.dart hintStyle (filled with kSurface, theme.dart:274-307)',
    ),
    Pair(
      'input helper/counter text',
      'kTextMuted',
      'kSurface',
      4.5,
      'theme.dart:305-306',
    ),
    Pair(
      'input focused label (accent)',
      'kOrange',
      'kSurface',
      4.5,
      'theme.dart:303 floatingLabelStyle, 13 px',
    ),
    Pair(
      'section label / selected tile / text button (accent)',
      'kOrange',
      'kSurface',
      4.5,
      'widgets.dart:216 SectionLabel, theme.dart:263,334',
    ),
    Pair(
      'accent icon (selected tile, spinner)',
      'kOrange',
      'kSurface',
      3.0,
      'theme.dart:263 selectedColor, 311 progress color',
    ),
    Pair(
      'input border, outlined-button side, checkbox/radio side (kLine)',
      'kLine',
      'kSurface',
      3.0,
      'theme.dart:283-289, 348, 382, 405',
    ),
    Pair(
      'active panel fill (kSubtleTint)',
      'kSubtleTint',
      'kSurface',
      3.0,
      'theme.dart:264 selectedTileColor, 313 progress track',
      exempt: true,
    ),
  ]),
  Group('Text roles on the card/dialog/rail surface (kSurfaceAlt)', [
    Pair(
      'titles, body (primary)',
      'kTextPrimary',
      'kSurfaceAlt',
      4.5,
      'theme.dart:237-253 cards and dialogs',
    ),
    Pair(
      'labels (secondary)',
      'kTextSecondary',
      'kSurfaceAlt',
      4.5,
      'theme.dart:202-203; also the rail and the table header',
    ),
    Pair(
      'bodySmall (muted)',
      'kTextMuted',
      'kSurfaceAlt',
      4.5,
      'widgets.dart:304 InfoRow label, :982 DeviceTile subtext, :1076 '
          'TaskProgress counter',
    ),
    Pair(
      'card title / rail label (accent)',
      'kOrange',
      'kSurfaceAlt',
      4.5,
      'widgets.dart:270 InfoCard title (15 px), theme.dart:437 rail selected '
          'label (13 px), text buttons in dialogs',
    ),
    Pair(
      'card/rail accent icon',
      'kOrange',
      'kSurfaceAlt',
      3.0,
      'theme.dart:437 rail selected icon is black on the indicator, the pill '
          'itself is the marker',
    ),
    Pair(
      'control boundary in a card (kLine)',
      'kLine',
      'kSurfaceAlt',
      3.0,
      'theme.dart:368 tab divider, 382 checkbox side, 399 switch outline',
    ),
    Pair(
      'rail selection indicator vs the rail (kOrangeDark)',
      'kOrangeDark',
      'kSurfaceAlt',
      3.0,
      'theme.dart:434 indicatorColor',
    ),
    Pair(
      'selected list row ink (accent on the subtle wash)',
      'kOrange',
      Wash('kSubtleTint', 'kSurfaceAlt'),
      4.5,
      'theme.dart:262-264 selectedColor over selectedTileColor',
    ),
    Pair(
      'selected list row ink on the page',
      'kOrange',
      Wash('kSubtleTint', 'kSurface'),
      4.5,
      'same row on a page-level list',
    ),
    Pair(
      'selected list row text (primary)',
      'kTextPrimary',
      Wash('kSubtleTint', 'kSurfaceAlt'),
      4.5,
      'theme.dart:261 textColor',
    ),
  ]),
  Group('Status colours (text and icons)', [
    Pair(
      'success (link up, stored)',
      'kSuccess',
      'kSurfaceAlt',
      4.5,
      'viewer_kit chip fills, device_view_page.dart:270',
    ),
    Pair('warning', 'kWarning', 'kSurfaceAlt', 4.5, 'status text in cards'),
    Pair(
      'error (failure text, error icon)',
      'kError',
      'kSurfaceAlt',
      4.5,
      'widgets.dart:617 StateMessage.error, script_editor_view.dart:54',
    ),
    Pair(
      'error on the page',
      'kError',
      'kSurface',
      4.5,
      'log rows, script list',
    ),
    Pair('info (neutral notice)', 'kInfo', 'kSurfaceAlt', 4.5, 'theme.dart:62'),
    Pair(
      'error border (kError, control boundary)',
      'kError',
      'kSurface',
      3.0,
      'theme.dart:294-301',
    ),
  ]),
  Group('Chips and badges', [
    Pair(
      'subtle chip ink (kTextSecondary)',
      'kTextSecondary',
      Wash('kSubtleTint', 'kSurfaceAlt'),
      4.5,
      'viewer_kit.dart TamuChip subtle, theme.dart:366 ChipTheme label',
      note: 'labelSmall 12 px, so body text',
    ),
    Pair(
      'subtle chip ink on the page',
      'kTextSecondary',
      Wash('kSubtleTint', 'kSurface'),
      4.5,
      'same chip outside a card',
    ),
    Pair(
      'accent chip ink (on the warm accent wash)',
      'kTextPrimary',
      Wash('kAccentTint', 'kSurfaceAlt'),
      4.5,
      'viewer_kit.dart:62 ChipTone.accent, widgets.dart ChipLabel accent',
    ),
    Pair(
      'accent chip ink on the page',
      'kTextPrimary',
      Wash('kAccentTint', 'kSurface'),
      4.5,
      'the same chip on a page-level surface',
    ),
    Pair(
      'success chip ink (the tone is the ink)',
      'kSuccess',
      Wash('kSubtleTint', 'kSurfaceAlt'),
      4.5,
      'viewer_kit.dart:64 the tone is the text, the fill is the plain tag',
    ),
    Pair(
      'warning chip ink',
      'kWarning',
      Wash('kSubtleTint', 'kSurfaceAlt'),
      4.5,
      'viewer_kit.dart:65',
    ),
    Pair(
      'error chip ink',
      'kError',
      Wash('kSubtleTint', 'kSurfaceAlt'),
      4.5,
      'viewer_kit.dart:66',
    ),
    Pair(
      'accent chip fill vs its surface (kAccentTint)',
      Wash('kAccentTint', 'kSurfaceAlt'),
      'kSurfaceAlt',
      1.2,
      'the fill is a shade, not a state indicator',
      exempt: true,
    ),
    Pair(
      'tag fill vs its surface (kSubtleTint)',
      Wash('kSubtleTint', 'kSurfaceAlt'),
      'kSurfaceAlt',
      1.2,
      'decorative shade; the ink carries the state',
      exempt: true,
    ),
  ]),
  // The restore plan's diff classes (backup_page.dart `_classTone`/`_classInk`, and the filter
  // bar's chips): add = success, replace = warning, delete = error, same = the plain tag. The
  // class is carried by these tokens only - the page invents no colour of its own - so each ink
  // is measured on both surfaces it lands on: a card (the device sections' rows) and the page
  // (the filter bar above them).
  Group('Restore plan diff classes (add / replace / delete)', [
    Pair(
      'add chip ink on a card',
      'kSuccess',
      Wash('kSubtleTint', 'kSurfaceAlt'),
      4.5,
      'backup_page.dart _classTone(add) = ChipTone.success',
    ),
    Pair(
      'add chip ink on the page',
      'kSuccess',
      Wash('kSubtleTint', 'kSurface'),
      4.5,
      'the filter bar sits on the page, above the device cards',
    ),
    Pair(
      'replace chip ink on a card',
      'kWarning',
      Wash('kSubtleTint', 'kSurfaceAlt'),
      4.5,
      'backup_page.dart _classTone(change) = ChipTone.warning',
    ),
    Pair(
      'replace chip ink on the page',
      'kWarning',
      Wash('kSubtleTint', 'kSurface'),
      4.5,
      'the same chip in the filter bar',
    ),
    Pair(
      'delete chip ink on a card',
      'kError',
      Wash('kSubtleTint', 'kSurfaceAlt'),
      4.5,
      'backup_page.dart _classTone(remove) = ChipTone.error',
    ),
    Pair(
      'delete chip ink on the page',
      'kError',
      Wash('kSubtleTint', 'kSurface'),
      4.5,
      'the same chip in the filter bar',
    ),
    Pair(
      'diff row icon (add) on a card',
      'kSuccess',
      'kSurfaceAlt',
      3.0,
      'a non-text state indicator: 3:1 is the bar it has to clear',
    ),
    Pair(
      'diff row icon (replace) on a card',
      'kWarning',
      'kSurfaceAlt',
      3.0,
      'backup_page.dart _classInk(change)',
    ),
    Pair(
      'diff row icon (delete) on a card',
      'kError',
      'kSurfaceAlt',
      3.0,
      'backup_page.dart _classInk(remove)',
    ),
    Pair(
      'delete row heading (titleSmall) on a card',
      'kError',
      'kSurfaceAlt',
      4.5,
      '"Delete from the device" is 15 px, so body text',
    ),
    Pair(
      'delete row detail (bodySmall) on a card',
      'kError',
      'kSurfaceAlt',
      4.5,
      'the row\'s second line under a deletion',
    ),
    Pair(
      'delete count in the footer (bodySmall) on the page',
      'kError',
      'kSurface',
      4.5,
      'the destructive count, which must not be missed',
    ),
    Pair(
      'same chip ink on a card',
      'kTextSecondary',
      Wash('kSubtleTint', 'kSurfaceAlt'),
      4.5,
      'the plain tag: an untouched row is not a state',
    ),
    Pair(
      'unplaceable row icon (kTextDisabled) on a card',
      'kTextDisabled',
      'kSurfaceAlt',
      3.0,
      'an inactive marker, WCAG-exempt but still measured',
      exempt: true,
    ),
  ]),
  Group('Sorting: Switch / Checkbox / Radio / progress', [
    Pair(
      'checkbox/radio boundary (kLine)',
      'kLine',
      'kSurfaceAlt',
      3.0,
      'theme.dart:382, 405',
    ),
    Pair(
      'checkbox/radio boundary on the page',
      'kLine',
      'kSurface',
      3.0,
      'the same control outside a card',
    ),
    Pair(
      'selected fill (accent) vs its surface',
      'kOrange',
      'kSurfaceAlt',
      3.0,
      'theme.dart:386-388, 405',
    ),
    Pair(
      'checkbox check (kOnAccent on the accent fill)',
      'kOnAccent',
      'kOrange',
      3.0,
      'theme.dart:386-388',
    ),
    Pair(
      'switch outline, unselected (kLine)',
      'kLine',
      'kSurfaceAlt',
      3.0,
      'theme.dart:400 the outline is the control boundary',
    ),
    Pair(
      'switch thumb, unselected (kTextDisabled)',
      'kTextDisabled',
      Wash('kSurface', 'kSurfaceAlt'),
      3.0,
      'theme.dart:394 thumb against the unselected track well',
    ),
    Pair(
      'switch track, selected (accent)',
      'kOrange',
      'kSurfaceAlt',
      3.0,
      'theme.dart:397',
    ),
    Pair(
      'switch thumb, selected (kOnAccent)',
      'kOnAccent',
      'kOrange',
      3.0,
      'theme.dart:393-395',
    ),
    Pair(
      'switch track fill, unselected (the well)',
      'kSurface',
      'kSurfaceAlt',
      3.0,
      'theme.dart:396 the well is a shade; the outline carries 3:1',
      exempt: true,
    ),
    Pair(
      'progress bar on its track',
      'kOrange',
      Wash('kSubtleTint', 'kSurface'),
      3.0,
      'theme.dart:311-316',
    ),
    Pair(
      'disabled control / icon ink (kTextDisabled)',
      'kTextDisabled',
      'kSurfaceAlt',
      3.0,
      'theme.dart:425 _iconButton disabledForegroundColor',
      exempt: true,
    ),
  ]),
  Group('Script editor syntax colours (13 px mono text)', [
    Pair(
      'symbol colour on a card (kSymInput)',
      'kSymInput',
      'kSurfaceAlt',
      4.5,
      'script_editor_line.dart, token theme.dart',
    ),
    Pair(
      'symbol colour on a card (kSymOutput)',
      'kSymOutput',
      'kSurfaceAlt',
      4.5,
      'script_editor_line.dart, token theme.dart',
    ),
    Pair(
      'symbol colour on a card (kSymVariable)',
      'kSymVariable',
      'kSurfaceAlt',
      4.5,
      'script_editor_line.dart, token theme.dart',
    ),
    Pair(
      'symbol colour on a card (kSymConstant)',
      'kSymConstant',
      'kSurfaceAlt',
      4.5,
      'script_editor_line.dart, token theme.dart',
    ),
    Pair(
      'symbol colour on a card (kSymPredefine)',
      'kSymPredefine',
      'kSurfaceAlt',
      4.5,
      'script_editor_line.dart, token theme.dart',
    ),
    Pair(
      'symbol colour on the active line (kSymInput)',
      'kSymInput',
      Wash('kActiveLineTint', 'kSurfaceAlt'),
      4.5,
      'script_editor_line.dart:84 the active-line fill over a card; it replaced '
          'the kAccentTint wash, which is lighter than the surface under it '
          '(6.14 on it)',
    ),
    Pair(
      'symbol colour on the active line (kSymOutput)',
      'kSymOutput',
      Wash('kActiveLineTint', 'kSurfaceAlt'),
      4.5,
      'script_editor_line.dart:84 the active-line fill over a card; with the '
          'kAccentTint fill it replaced, this pair measured 3.88',
    ),
    Pair(
      'symbol colour on the active line (kSymVariable)',
      'kSymVariable',
      Wash('kActiveLineTint', 'kSurfaceAlt'),
      4.5,
      'script_editor_line.dart:84 the active-line fill over a card; with the '
          'kAccentTint fill it replaced, this pair measured 3.91',
    ),
    Pair(
      'symbol colour on the active line (kSymConstant)',
      'kSymConstant',
      Wash('kActiveLineTint', 'kSurfaceAlt'),
      4.5,
      'script_editor_line.dart:84 the active-line fill over a card; with the '
          'kAccentTint fill it replaced, this pair measured 3.62',
    ),
    Pair(
      'symbol colour on the active line (kSymPredefine)',
      'kSymPredefine',
      Wash('kActiveLineTint', 'kSurfaceAlt'),
      4.5,
      'script_editor_line.dart:84 the active-line fill over a card; with the '
          'kAccentTint fill it replaced, this pair measured 4.30',
    ),
    Pair(
      'symbol colour on the page (kSymConstant)',
      'kSymConstant',
      'kSurface',
      4.5,
      'script_editor_line.dart outside a card',
    ),
  ]),
  Group('Overlays and menus', [
    Pair(
      'tooltip / snackbar / popup menu text',
      'kTextPrimary',
      'kSurfaceAlt',
      4.5,
      'theme.dart:409-429',
    ),
    Pair(
      'filled button label',
      'kTextPrimary',
      'kSurfaceAlt',
      4.5,
      'theme.dart:319-331',
    ),
    Pair(
      'divider (accent) on the page',
      'kOrange',
      'kSurface',
      3.0,
      'theme.dart:269-270',
      exempt: true,
    ),
    Pair(
      'dialog scrim (kShadow)',
      'kShadow',
      'kSurface',
      1.2,
      'theme.dart:50 decoration',
      exempt: true,
    ),
  ]),
];

// ---------------------------------------------------------------------------
// WCAG maths
// ---------------------------------------------------------------------------

double relativeLuminance(List<double> rgb) {
  double channel(double c) {
    final v = c / 255.0;
    return v <= 0.04045
        ? v / 12.92
        : math.pow((v + 0.055) / 1.055, 2.4) as double;
  }

  return 0.2126 * channel(rgb[0]) +
      0.7152 * channel(rgb[1]) +
      0.0722 * channel(rgb[2]);
}

/// Contrast ratio between [fg] (alpha allowed) and the opaque [bg].
double contrast(List<double> fg, List<double> bg) {
  final f = _alpha(fg[3]) >= 1 ? fg : compose(fg, bg);
  final l1 = relativeLuminance(f);
  final l2 = relativeLuminance(bg);
  final hi = math.max(l1, l2);
  final lo = math.min(l1, l2);
  return (hi + 0.05) / (lo + 0.05);
}

/// [fg] (alpha allowed) composited over the opaque [bg] -> opaque rgb.
List<double> compose(List<double> fg, List<double> bg) {
  final a = _alpha(fg[3]);
  return [
    fg[0] * a + bg[0] * (1 - a),
    fg[1] * a + bg[1] * (1 - a),
    fg[2] * a + bg[2] * (1 - a),
    1,
  ];
}

double _alpha(double stored) => stored > 1 ? stored / 255.0 : stored;

void _printPair(String fg, String bg, String themePath) {
  final theme = ThemeTokens.parse(
    File(themePath).existsSync() ? File(themePath).readAsStringSync() : '',
  );
  final f = theme.colour(fg);
  final b = theme.colour(bg);
  stdout.writeln(
    '$fg on $bg: ${contrast(f, compose(b, const [0, 0, 0, 1])).toStringAsFixed(2)}:1',
  );
}

// ---------------------------------------------------------------------------
// Theme source reader
// ---------------------------------------------------------------------------

/// The `k*` colours declared in a theme file, resolved to `[r, g, b, a]`.
///
/// Reads both `const Color kName = Color(0xAARRGGBB);` and
/// `const Color kName = Colors.<step>;` (the Material steps in [materialSteps]).
class ThemeTokens {
  ThemeTokens(this.names, this.values);

  final List<String> names;
  final Map<String, List<double>> values;

  static final RegExp _literal = RegExp(
    r'const\s+Color\s+(k[A-Za-z0-9_]+)\s*=\s*Color\(0x([0-9A-Fa-f]{8})\)',
  );
  static final RegExp _material = RegExp(
    r'const\s+Color\s+(k[A-Za-z0-9_]+)\s*=\s*Colors\.([A-Za-z0-9]+)',
  );
  static final RegExp _tint = RegExp(
    r'const\s+Color\s+(k[A-Za-z0-9_]+)\s*=\s*(k[A-Za-z0-9_]+)\.withAlpha\((\d+)\)',
  );
  static final RegExp _alias = RegExp(
    r'const\s+Color\s+(k[A-Za-z0-9_]+)\s*=\s*(k[A-Za-z0-9_]+)\s*;',
  );

  static const Map<String, int> materialSteps = {
    'transparent': 0x00000000,
    'black': 0xFF000000,
    'black87': 0xDD000000,
    'black54': 0x8A000000,
    'black38': 0x61000000,
    'black26': 0x42000000,
    'black12': 0x1F000000,
    'white': 0xFFFFFFFF,
    'white70': 0xB3FFFFFF,
    'white60': 0x99FFFFFF,
    'white54': 0x8AFFFFFF,
    'white38': 0x62FFFFFF,
    'white30': 0x4DFFFFFF,
    'white24': 0x3DFFFFFF,
    'white12': 0x1FFFFFFF,
    'greenAccent': 0xFF69F0AE,
    'amberAccent': 0xFFFFD740,
    'redAccent': 0xFFFF5252,
    'lightBlueAccent': 0xFF40C4FF,
    'orangeAccent': 0xFFFFAB40,
    'orange': 0xFFFF9800,
  };

  static ThemeTokens parse(String source) {
    final values = <String, List<double>>{};
    final names = <String>[];

    void add(String name, int argb) {
      if (values.containsKey(name)) return;
      values[name] = [
        ((argb >> 16) & 0xFF).toDouble(),
        ((argb >> 8) & 0xFF).toDouble(),
        (argb & 0xFF).toDouble(),
        ((argb >> 24) & 0xFF) / 255.0,
      ];
      names.add(name);
    }

    for (final m in _literal.allMatches(source)) {
      add(m.group(1)!, int.parse(m.group(2)!, radix: 16));
    }
    for (final m in _material.allMatches(source)) {
      final step = materialSteps[m.group(2)!];
      if (step == null) {
        stderr.writeln('unknown Material colour: Colors.${m.group(2)}');
        exit(2);
      }
      add(m.group(1)!, step);
    }
    // Tints declared from another token (kSuccessTint = kSuccess.withAlpha(46))
    // and plain aliases (kBadgeFill = kOnAccent), resolved in dependency order.
    var pending = true;
    while (pending) {
      pending = false;
      for (final m in _tint.allMatches(source)) {
        final base = values[m.group(2)!];
        if (base == null || values.containsKey(m.group(1)!)) continue;
        values[m.group(1)!] = [
          base[0],
          base[1],
          base[2],
          int.parse(m.group(3)!) / 255.0,
        ];
        names.add(m.group(1)!);
        pending = true;
      }
      for (final m in _alias.allMatches(source)) {
        final base = values[m.group(2)!];
        if (base == null || values.containsKey(m.group(1)!)) continue;
        values[m.group(1)!] = base;
        names.add(m.group(1)!);
        pending = true;
      }
    }
    return ThemeTokens(names, values);
  }

  bool contains(String name) =>
      values.containsKey(name) || name.startsWith('#');

  List<double> colour(String name) {
    final direct = values[name];
    if (direct != null) return direct;
    if (name.startsWith('#')) {
      final digits = name.substring(1);
      final hex = digits.length == 6 ? 'FF$digits' : digits.padLeft(8, '0');
      return [
        int.parse(hex.substring(2, 4), radix: 16).toDouble(),
        int.parse(hex.substring(4, 6), radix: 16).toDouble(),
        int.parse(hex.substring(6, 8), radix: 16).toDouble(),
        int.parse(hex.substring(0, 2), radix: 16) / 255.0,
      ];
    }
    stderr.writeln('pair names an unknown token: $name');
    exit(2);
  }
}
