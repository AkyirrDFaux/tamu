import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/register_page.dart';
import 'package:tamuapp/ui/theme.dart';

import 'support/layout_gate.dart';

/// Ruling 5 (owner, 2026-11-12): `RO/P/TR` everywhere. The Register page's dynamic entry
/// subtitle used to print a reduced set (`RO · P`) while the block card above it and the
/// register header printed the full vocabulary (`RO · P · TR`). `TR` is the flag that explains
/// why a write sometimes behaves differently, so the reduced form is gone and the subtitle is
/// built from the one vocabulary, [ValueFlags.describe], through [ValueFlags.suffix].
///
/// This file pins the vocabulary and the two things that can go wrong with it: the subtitle
/// must show every set flag, and the narrowest supported window must still lay the row out.
///
/// Measured at 360x640 (the narrowest window the app supports): a `Number` row is 62.0 px with
/// and without `TR`, so the full vocabulary costs nothing for the common labels; the labels
/// whose line the extra ` · TR` crosses (`Serial number`, `Device type`) grow 62.0 -> 79.0 px,
/// one wrapped labelSmall line inside the same dense ListTile - never an overflow, and the
/// longest label (`Dictionary marker`) already wrapped before this change.
class _DynamicRegisterClient extends RegisterClient {
  _DynamicRegisterClient({
    required super.deviceId,
    required this.flags,
    this.type = DataType.number,
  });

  final int flags;
  final DataType type;

  @override
  Future<BlocksRead?> readBlocks({List<int>? scriptSlots}) async => (
    blocks: <ReportedBlock>[
      (
        type: dynamicTypeBase,
        inst: 0,
        meta: ValueInfo(type: dynamicTypeBase, size: 1),
        name: 'Fixture block',
      ),
    ],
    unreadTypes: const <int>{},
  );

  @override
  Future<StaticFieldLayout> readStaticFieldLayout() async => const {};

  @override
  Future<List<int>?> enumerateFieldKeys(int blockType, int instance) async =>
      const [0x0000];

  @override
  Future<({ValueInfo meta, List<int> value})?> readBlockField(
    int blockType,
    int instance,
    int field,
    int key,
  ) async => (
    meta: ValueInfo(type: type.value, size: 4, flags: flags),
    value: const [0, 0, 0, 1],
  );
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('ValueFlags', () {
    test('describe names every set bit, TR included', () {
      expect(ValueFlags.describe(ValueFlags.readOnly), ['RO']);
      expect(ValueFlags.describe(ValueFlags.persistent), ['P']);
      expect(ValueFlags.describe(ValueFlags.trigger), ['TR']);
      expect(
        ValueFlags.describe(
          ValueFlags.readOnly | ValueFlags.persistent | ValueFlags.trigger,
        ),
        ['RO', 'P', 'TR'],
      );
      expect(ValueFlags.describe(0), isEmpty);
    });

    test('suffix is the one-line form of describe, so it cannot drop TR', () {
      expect(
        ValueFlags.suffix(
          ValueFlags.readOnly | ValueFlags.persistent | ValueFlags.trigger,
        ),
        ' · RO · P · TR',
      );
      expect(ValueFlags.suffix(ValueFlags.trigger), ' · TR');
      expect(ValueFlags.suffix(0), '');
    });
  });

  /// Pumps the Register page at [size] with one dynamic field carrying [flags], expanded.
  ///
  /// The page is keyed on [flags]: re-pumping the same window with a different flag set must
  /// mount a fresh State, because the page builds its client once (`initState`).
  Future<void> pumpDynamicField(
    WidgetTester tester,
    int flags,
    Size size, {
    DataType type = DataType.number,
  }) async {
    useWindow(tester, size);
    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        home: RegisterPage(
          key: ValueKey('$flags-${type.name}'),
          deviceId: 1,
          isConnected: () => true,
          clientFactory: (_) =>
              _DynamicRegisterClient(deviceId: 1, flags: flags, type: type),
        ),
      ),
    );
    await tester.pump(); // the initial topology load
    await tester.tap(find.text('Fixture block')); // expand the block
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 100));
  }

  Finder rowOf(String subtitle) => find
      .ancestor(of: find.text(subtitle), matching: find.byType(ListTile))
      .first;

  testWidgets('the dynamic entry subtitle shows the full flag vocabulary', (
    tester,
  ) async {
    await pumpDynamicField(
      tester,
      ValueFlags.readOnly | ValueFlags.persistent | ValueFlags.trigger,
      portraitSize,
    );
    expect(tester.takeException(), isNull);

    expect(find.text('Number · RO · P · TR'), findsOneWidget);
    // The reduced form this card removed must not be what a row prints.
    expect(find.text('Number · RO · P'), findsNothing);

    await tester.pumpWidget(const SizedBox()); // dispose the auto-refresh timer
  });

  testWidgets('a short type label costs no row height at the narrowest window', (
    tester,
  ) async {
    // 360x640 is the narrowest window the app supports (test/support/layout_gate.dart).
    await pumpDynamicField(
      tester,
      ValueFlags.readOnly | ValueFlags.persistent,
      portraitSize,
    );
    expect(tester.takeException(), isNull);
    final withoutTrigger = tester.getSize(rowOf('Number · RO · P')).height;

    await pumpDynamicField(
      tester,
      ValueFlags.readOnly | ValueFlags.persistent | ValueFlags.trigger,
      portraitSize,
    );
    expect(tester.takeException(), isNull);
    final withTrigger = tester.getSize(rowOf('Number · RO · P · TR')).height;

    // The subtitle is a wrapping Text inside a dense ListTile, so a subtitle that no longer
    // fits its line grows the tile by one labelSmall line instead of overflowing. For the
    // common labels (Number, Bool, Index, Colour, Enum, ...) it still fits on one line, so the
    // full vocabulary costs nothing: measured 62.0 px row either way.
    expect(
      withTrigger,
      withoutTrigger,
      reason: 'labelled row grew: $withTrigger',
    );

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('the labels that no longer fit grow by exactly one line', (
    tester,
  ) async {
    // `Serial number` (13 characters) is the shortest label where the extra ` · TR` crosses
    // the 312 px subtitle box at 360 px; `Device type` (11) behaves the same. This is the
    // whole layout consequence of the ruling, pinned as a number rather than an impression.
    await pumpDynamicField(
      tester,
      ValueFlags.readOnly | ValueFlags.persistent,
      portraitSize,
      type: DataType.sn,
    );
    expect(tester.takeException(), isNull);
    final withoutTrigger = tester
        .getSize(rowOf('Serial number · RO · P'))
        .height;

    await pumpDynamicField(
      tester,
      ValueFlags.readOnly | ValueFlags.persistent | ValueFlags.trigger,
      portraitSize,
      type: DataType.sn,
    );
    expect(tester.takeException(), isNull);
    final withTrigger = tester
        .getSize(rowOf('Serial number · RO · P · TR'))
        .height;
    // Measured 62.0 -> 79.0 at 360x640: one wrapped labelSmall line, nothing clipped.
    expect(withTrigger - withoutTrigger, inInclusiveRange(1, 24));

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('the longest type label already wrapped, so TR adds nothing', (
    tester,
  ) async {
    // `Dictionary marker` is the longest `dataTypeLabel`; its row is two lines with the
    // reduced set already, so the full vocabulary adds no height there either.
    await pumpDynamicField(
      tester,
      ValueFlags.readOnly | ValueFlags.persistent,
      portraitSize,
      type: DataType.unknownKeyed,
    );
    expect(tester.takeException(), isNull);
    final withoutTrigger = tester
        .getSize(rowOf('Dictionary marker · RO · P'))
        .height;

    await pumpDynamicField(
      tester,
      ValueFlags.readOnly | ValueFlags.persistent | ValueFlags.trigger,
      portraitSize,
      type: DataType.unknownKeyed,
    );
    expect(tester.takeException(), isNull);
    const subtitle = 'Dictionary marker · RO · P · TR';
    expect(find.text(subtitle), findsOneWidget);
    // Measured 79.0 px either way: the long label wrapped before this change.
    expect(tester.getSize(rowOf(subtitle)).height, withoutTrigger);

    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('landscape keeps every label on one line, TR included', (
    tester,
  ) async {
    // 800x360 is the same phone held sideways (test/support/layout_gate.dart). The row is the
    // same dense ListTile with more width, so the widest subtitle the vocabulary can produce
    // must still sit on one line there - the structural statement for that orientation.
    await pumpDynamicField(
      tester,
      ValueFlags.readOnly | ValueFlags.persistent | ValueFlags.trigger,
      landscapeSize,
      type: DataType.unknownKeyed,
    );
    expect(tester.takeException(), isNull);
    const subtitle = 'Dictionary marker · RO · P · TR';
    expect(find.text(subtitle), findsOneWidget);
    // One labelSmall line is 17 px; a wrapped one is 34.
    expect(tester.getSize(find.text(subtitle)).height, lessThan(20));

    await tester.pumpWidget(const SizedBox());
  });
}
