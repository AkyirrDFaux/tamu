import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/ui/theme.dart';
import 'package:tamuapp/ui/widgets.dart';

/// The app-bar group of the contrast audit (TODO.md, "Contrast audit
/// (2026-10-17)"): Akyirr named several app-bar elements as hard to see.
///
/// `tool/contrast.dart` measures the ratios; this suite proves that the theme's
/// app-bar ink is what the widgets actually *render*, which token maths cannot
/// show - in particular that an IconButton in the bar takes the bar's ink rather
/// than the page-level IconButtonTheme, that a disabled action does not fall
/// back to `kTextDisabled` (1.59:1 on the bar), and that the busy spinner and
/// the autorefresh mark do not fall back to the theme's `kOrange` (1.17:1).
class _StubAuto implements AutoRefreshControl {
  _StubAuto({this.active = false});

  final bool active;

  @override
  bool get autoRefreshActive => active;

  @override
  Duration? get selectedInterval => null;

  @override
  void applyAuto(Duration? interval) {}
}

Future<void> _pumpBar(
  WidgetTester tester, {
  required bool refreshing,
  required bool autoActive,
}) async {
  await tester.pumpWidget(
    MaterialApp(
      theme: buildTheme(),
      home: Scaffold(
        appBar: PageAppBar(
          title: 'Contrast',
          leading: const ShellDrawerButton(),
          auto: _StubAuto(active: autoActive),
          onRefresh: () async {},
          refreshing: refreshing,
          actions: [PageActions.identify(onIdentify: () async {})],
        ),
      ),
    ),
  );
  await tester.pump();
}

/// The ink an icon renders in: the ambient [IconTheme] the IconButton resolved
/// the icon against.
Color? _iconInk(WidgetTester tester, Finder finder) =>
    IconTheme.of(tester.element(finder)).color;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('the bar keeps its orange and renders flat and scrolled alike', (
    tester,
  ) async {
    await _pumpBar(tester, refreshing: false, autoActive: false);

    final theme = Theme.of(tester.element(find.byType(Scaffold)));
    final bar = theme.appBarTheme;
    expect(bar.backgroundColor, kOrangeDark);
    expect(
      bar.scrolledUnderElevation,
      0,
      reason: 'the scrolled/elevated variant must not tint the bar',
    );
    expect(bar.surfaceTintColor, Colors.transparent);
    expect(bar.iconTheme?.color, kOnAccent);
    expect(bar.actionsIconTheme?.color, kOnAccent);

    final title = tester.widget<Text>(find.text('Contrast'));
    expect(title.style?.color ?? bar.titleTextStyle?.color, kOnAccent);
  });

  testWidgets('title, drawer icon, action icons and the refresh icon are the '
      'accent ink, not the page ink', (tester) async {
    // A compact surface is what makes ShellDrawerButton draw its leading icon.
    tester.view.physicalSize = const Size(400, 800);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);

    await _pumpBar(tester, refreshing: false, autoActive: false);

    expect(
      find.byIcon(Icons.menu),
      findsOneWidget,
      reason: 'the drawer button is the leading slot on a compact surface',
    );
    for (final icon in <Finder>[
      find.byIcon(Icons.menu),
      find.byIcon(Icons.refresh),
      find.byIcon(Icons.visibility_outlined),
    ]) {
      expect(icon, findsOneWidget);
      expect(_iconInk(tester, icon), kOnAccent);
    }
  });

  testWidgets('the busy spinner and the autorefresh mark do not use kOrange', (
    tester,
  ) async {
    await _pumpBar(tester, refreshing: true, autoActive: false);
    final spinner = tester.widget<CircularProgressIndicator>(
      find.byType(CircularProgressIndicator),
    );
    expect(spinner.color, kOnAccent);

    await _pumpBar(tester, refreshing: false, autoActive: true);
    final dots = tester
        .widgetList<Container>(find.byType(Container))
        .where((c) => c.decoration is BoxDecoration)
        .map((c) => c.decoration! as BoxDecoration)
        .where((d) => d.shape == BoxShape.circle)
        .toList();
    expect(dots, hasLength(1), reason: 'one autorefresh mark');
    expect(dots.single.color, kSuccess);
    expect((dots.single.border! as Border).top.color, kOnAccent);
  });

  testWidgets('a label-less app-bar badge paints the bar ink, not '
      'colorScheme.error', (tester) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        home: Scaffold(
          appBar: AppBar(
            title: const Text('Contrast'),
            actions: [
              // What log_page.dart:205 renders while a device filter is active.
              Badge(
                isLabelVisible: true,
                smallSize: 8,
                child: const Icon(Icons.filter_alt_outlined),
              ),
            ],
          ),
        ),
      ),
    );
    await tester.pump();

    final theme = Theme.of(tester.element(find.byType(Scaffold)));
    expect(
      theme.badgeTheme.backgroundColor,
      kBadgeFill,
      reason: 'the Material default is colorScheme.error: 1.23:1 on the bar',
    );

    final dotFills = tester
        .widgetList<Container>(find.byType(Container))
        .map((c) => c.decoration)
        .whereType<ShapeDecoration>()
        .where((d) => d.shape is StadiumBorder)
        .map((d) => d.color)
        .toList();
    expect(dotFills, [kBadgeFill], reason: 'the rendered badge dot');
  });

  testWidgets('a disabled app-bar action draws in the dimmed accent ink', (
    tester,
  ) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        home: Scaffold(
          appBar: PageAppBar(
            title: 'Contrast',
            actions: [
              const IconButton(
                onPressed: null,
                icon: Icon(Icons.delete_outline),
              ),
            ],
          ),
        ),
      ),
    );
    await tester.pump();

    final icon = find.byIcon(Icons.delete_outline);
    expect(icon, findsOneWidget);
    expect(_iconInk(tester, icon), kOnAccentDim);
  });
}
