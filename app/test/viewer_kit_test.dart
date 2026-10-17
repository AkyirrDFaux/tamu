import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/formatters.dart';
import 'package:tamuapp/ui/theme.dart';
import 'package:tamuapp/ui/viewer_kit.dart';

/// A minimal tabular record the way a viewer would hand one over.
typedef Rec = ({int id, String name, int size});

/// Proves the viewer toolkit end to end: the table shell lays out its header and
/// formatted cells, incremental loading reveals rows on demand, the hex/ASCII
/// view prints through `formatters.dart`, and the chips render from the tokens.
///
/// This is the toolkit's own proof - the four viewers keep their own suites until
/// phase 1c adopts the toolkit.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  Future<void> pump(WidgetTester tester, Widget body) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        home: Scaffold(body: body),
      ),
    );
    await tester.pump();
    expect(tester.takeException(), isNull);
  }

  group('TamuTable', () {
    final rows = <Rec>[
      (id: 1, name: 'Alpha', size: 16),
      (id: 2, name: 'Beta', size: 32),
      (id: 3, name: 'Gamma', size: 48),
    ];

    List<TamuColumn<Rec>> columns() => [
      TamuColumn<Rec>(
        label: 'Device',
        text: (r) => idToString(r.id),
        mono: true,
      ),
      TamuColumn<Rec>(label: 'Name', text: (r) => r.name),
      TamuColumn<Rec>(
        label: 'Size',
        text: (r) => '${r.size} B',
        muted: true,
        width: 80,
      ),
    ];

    testWidgets('renders the header and a formatted cell per row', (
      tester,
    ) async {
      await pump(tester, TamuTable<Rec>(columns: columns(), rows: rows));

      expect(find.text('Device'), findsOneWidget);
      expect(find.text('Name'), findsOneWidget);
      expect(find.text('Size'), findsOneWidget);

      // Cell text is the formatter output, not a hand-formatted literal.
      expect(find.text(idToString(2)), findsOneWidget);
      expect(find.text('Beta'), findsOneWidget);
      expect(find.text('32 B'), findsOneWidget);
    });

    testWidgets('renders an optional leading slot and a per-row chip strip', (
      tester,
    ) async {
      await pump(
        tester,
        TamuTable<Rec>(
          columns: columns(),
          rows: rows,
          leading: (context, r) => const Icon(Icons.memory),
          chips: (context, r) => TamuChipRow(
            chips: [
              TamuChip('RO', tone: ChipTone.accent),
              TamuChip('${r.size} B', tone: ChipTone.subtle),
            ],
          ),
        ),
      );

      expect(find.byIcon(Icons.memory), findsNWidgets(rows.length));
      expect(find.text('RO'), findsNWidgets(rows.length));
    });

    testWidgets('reveals rows incrementally and reports the shown count', (
      tester,
    ) async {
      await pump(
        tester,
        TamuTable<Rec>(columns: columns(), rows: rows, pageSize: 2),
      );

      // First step: two rows + the footer.
      expect(find.text('Alpha'), findsOneWidget);
      expect(find.text('Beta'), findsOneWidget);
      expect(find.text('Gamma'), findsNothing);
      expect(find.text('Showing 2 of 3'), findsOneWidget);

      await tester.tap(find.text('Show more'));
      await tester.pump();

      expect(find.text('Gamma'), findsOneWidget);
      // Fully shown: the footer and its button are gone.
      expect(find.text('Show more'), findsNothing);
      expect(find.textContaining('Showing'), findsNothing);
    });

    testWidgets('shows the empty state when there are no rows', (tester) async {
      await pump(
        tester,
        TamuTable<Rec>(
          columns: columns(),
          rows: const [],
          emptyMessage: 'No records',
        ),
      );

      expect(find.text('No records'), findsOneWidget);
      // An empty table hides its header.
      expect(find.text('Device'), findsNothing);
    });
  });

  group('HexView', () {
    testWidgets('prints offset, grouped hex and ASCII through formatters', (
      tester,
    ) async {
      // 0x41..0x43 = "ABC"; 0x00 is non-printable and renders as '.'.
      final data = [0x41, 0x42, 0x43, 0x00];
      await pump(tester, HexView(data: data));

      expect(find.text(hex32(0)), findsOneWidget);
      expect(find.text(formatHexBytes(data)), findsOneWidget);
      expect(find.text(formatAsciiBytes(data)), findsOneWidget);
      expect(find.text('ABC.'), findsOneWidget);
    });

    testWidgets('splits rows by bytesPerLine', (tester) async {
      await pump(
        tester,
        HexView(data: List<int>.generate(16, (i) => i), bytesPerLine: 8),
      );

      expect(find.text(hex32(0)), findsOneWidget);
      expect(find.text(hex32(8)), findsOneWidget);
    });

    testWidgets('renders the empty state for an empty dump', (tester) async {
      await pump(tester, const HexView(data: [], emptyMessage: 'Empty file'));
      expect(find.text('Empty file'), findsOneWidget);
    });
  });

  group('TamuPagedList', () {
    testWidgets('pages a non-tabular tile list', (tester) async {
      await pump(
        tester,
        TamuPagedList<int>(
          items: [0, 1, 2, 3],
          pageSize: 2,
          itemBuilder: (context, item, index) =>
              ListTile(title: Text('t$item')),
        ),
      );

      expect(find.text('t0'), findsOneWidget);
      expect(find.text('t1'), findsOneWidget);
      expect(find.text('t2'), findsNothing);
      expect(find.text('Showing 2 of 4'), findsOneWidget);

      await tester.tap(find.text('Show more'));
      await tester.pump();
      expect(find.text('t3'), findsOneWidget);
    });
  });

  testWidgets('chips render their text in every tone', (tester) async {
    await pump(
      tester,
      const Row(
        children: [
          TamuChip('accent', tone: ChipTone.accent),
          TamuChip('subtle'),
          TamuChip('ok', tone: ChipTone.success),
          TamuChip('warn', tone: ChipTone.warning),
          TamuChip('err', tone: ChipTone.error),
        ],
      ),
    );

    expect(find.text('accent'), findsOneWidget);
    expect(find.text('subtle'), findsOneWidget);
    expect(find.text('ok'), findsOneWidget);
  });

  test('the hex view offers the standard byte widths', () {
    expect(HexView.widths, [8, 16, 32, 64]);
  });
}
