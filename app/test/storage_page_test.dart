import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/ui/formatters.dart';
import 'package:tamuapp/ui/storage_page.dart';
import 'package:tamuapp/ui/theme.dart';

/// Covers the Storage view's file-table page, which now renders through the
/// toolkit's table shell (`viewer_kit.dart`) instead of a hand-built `ListTile`
/// list: the live records print as columns, the superseded ones stay behind a
/// collapsible section, and unwritten slots are hidden.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  List<int> u32(int v) => [
    v & 0xFF,
    (v >> 8) & 0xFF,
    (v >> 16) & 0xFF,
    (v >> 24) & 0xFF,
  ];

  /// One 16-byte file record: u32 offset, u32 size, 8-byte name.
  List<int> record(int offset, int size, String name) => [
    ...u32(offset),
    ...u32(size),
    ...name.padRight(8).codeUnits,
  ];

  Future<void> pump(WidgetTester tester, List<int> data) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        home: FileTableViewPage(deviceId: 1, size: data.length, data: data),
      ),
    );
    await tester.pump();
    expect(tester.takeException(), isNull, reason: 'file table threw');
  }

  testWidgets('live records print as columns and unwritten slots are hidden', (
    tester,
  ) async {
    // A live file at offset 0x100 (1024 B), a superseded one (offset 0), and an
    // unwritten slot.
    final data = <int>[
      ...record(0x100, 1024, 'FOO'),
      ...record(0, 512, 'OLD'),
      ...record(0xFFFFFFFF, 0xFFFFFFFF, ''),
    ];
    await pump(tester, data);

    expect(find.text('FOO'), findsOneWidget);
    expect(find.text(formatFileSize(1024)), findsOneWidget); // 1.0 kB
    expect(find.text(hex32(0x100)), findsOneWidget);
    expect(
      find.text('File table (${data.length} B) - 1 live, 1 invalidated'),
      findsOneWidget,
    );

    // The superseded record is folded away until its section is opened.
    expect(find.text('(invalidated)'), findsNothing);
    expect(find.text('1 invalidated (old) records'), findsOneWidget);
  });

  testWidgets('opening the invalidated section reveals the old record', (
    tester,
  ) async {
    await pump(tester, [
      ...record(0x100, 1024, 'FOO'),
      ...record(0, 512, 'OLD'),
    ]);

    await tester.tap(find.text('1 invalidated (old) records'));
    await tester.pump();

    expect(find.text('(invalidated)'), findsOneWidget);
    // A superseded record has no offset and no live size to print.
    expect(find.text(hex32(0)), findsNothing);
  });

  testWidgets('row numbers are the table slots, so a superseded record leaves '
      'its gap', (tester) async {
    // Slot 0 is the table itself, slot 2 was superseded, slot 3 is live again: the numbering
    // must keep 0/1/3 rather than closing up over the dropped record.
    final data = <int>[
      ...record(0x1000, 16384, '.TABLE'),
      ...record(0x2000, 2048, 'FOO'),
      ...record(0, 512, 'OLD'),
      ...record(0x3000, 4096, 'BAR'),
      ...record(0xFFFFFFFF, 0xFFFFFFFF, ''),
    ];
    await pump(tester, data);

    expect(
      find.text('File table (${data.length} B) - 3 live, 1 invalidated'),
      findsOneWidget,
    );
    for (final slot in ['0', '1', '3']) {
      expect(find.text(slot), findsOneWidget, reason: 'slot $slot is listed');
    }
    // The superseded slot stays folded away until the section is opened.
    expect(find.text('2'), findsNothing);

    await tester.tap(find.text('1 invalidated (old) records'));
    await tester.pump();
    expect(find.text('2'), findsOneWidget); // ...and numbers as its own slot
    expect(find.text('(invalidated)'), findsOneWidget);
  });

  test('a file size reads in B, kB or MB', () {
    expect(formatFileSize(512), '512 B');
    expect(formatFileSize(1024), '1.0 kB');
    expect(formatFileSize(1536), '1.5 kB');
    expect(formatFileSize(1024 * 1024), '1.00 MB');
  });
}
