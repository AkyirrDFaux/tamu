import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/file_viewers.dart';
import 'package:tamuapp/ui/system_block_view.dart';
import 'package:tamuapp/ui/theme.dart';
import 'package:tamuapp/ui/value_editor.dart' show formatValue;

/// Renders the `.SV` / SUBREQ decoders against synthetic bytes matching the
/// firmware formats (StaticMemory.h / Subscriptions.h), and checks the
/// system-block capability decoding.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  List<int> u16(int v) => [v & 0xFF, (v >> 8) & 0xFF];

  List<int> u32(int v) => [
        v & 0xFF,
        (v >> 8) & 0xFF,
        (v >> 16) & 0xFF,
        (v >> 24) & 0xFF,
      ];

  Future<void> pump(WidgetTester tester, String name, List<int> data) async {
    await tester.pumpWidget(MaterialApp(
      theme: buildTheme(),
      home: Scaffold(body: MemoryBackupView(fileName: name, data: data)),
    ));
    await tester.pump();
    expect(tester.takeException(), isNull, reason: '$name threw while rendering');
  }

  testWidgets('.SV decodes a node System segment + static fields', (tester) async {
    // A node (no NetID): System segment 16 B + one ResistiveMeasure (0x08, 12 B:
    // SamplingRate@16).
    final data = List<int>.filled(16 + 12, 0);
    data.setRange(0, 8, 'DAS v0.1'.codeUnits); // System Name @ 0
    data.setRange(16, 20, numberToBytes(10)); // 0x08 SamplingRate @ 16
    final blocks = <({int type, int inst, ValueInfo meta, String name})?>[
      (type: BlockType.resistiveMeasure.value, inst: 0,
          meta: ValueInfo(type: DataType.number.value, size: 4), name: 'Meas1'),
    ];
    final staticFields = <int, List<({int field, int size, int type})>>{
      0x08: [
        (field: 0, size: 4, type: DataType.number.value),
        (field: 1, size: 1, type: DataType.enum_.value),
        (field: 2, size: 4, type: DataType.number.value),
      ],
    };
    await tester.pumpWidget(MaterialApp(
      theme: buildTheme(),
      home: Scaffold(
          body: MemoryBackupView(
              fileName: '.SV',
              data: data,
              blocks: blocks,
              staticFields: staticFields,
              hasNetId: false)),
    ));
    await tester.pump();
    expect(tester.takeException(), isNull, reason: '.SV threw');
  });

  testWidgets('SUBREQ decodes requester entries (24 B: provider+trid+table+target)',
      (tester) async {
    // u8 count + 24 B per entry (the requester wire prefix minus the timeout):
    // providerAddr u16, trid u16, subscription table (sourceReg u32, trigger u8,
    // minTime u24, period u32, deadzone Number 16.16), targetReg u32.
    final targetReg = makeBlockInfo(0, 0, 0, 0);
    final sourceReg = makeBlockInfo(8, 0, 4, 0);
    List<int> entry(int provider, int trid, double deadzone) => <int>[
          provider & 0xFF, (provider >> 8) & 0xFF,
          trid & 0xFF, (trid >> 8) & 0xFF,
          ...u32(sourceReg),
          TriggerType.deltaPeriodic.value,
          100 & 0xFF, (100 >> 8) & 0xFF, (100 >> 16) & 0xFF, // minTime 100
          ...u32(1000),
          ...numberToBytes(deadzone),
          ...u32(targetReg),
        ];
    // Two entries: a misaligned parser would read garbage from entry 2.
    final data = <int>[2, ...entry(2, 0x1000, 0), ...entry(3, 0x1001, 1.5)];
    await pump(tester, 'SUBREQ', data);
  });

  test('file type detection tolerates wire padding', () {
    // Older renames stored NUL-padded names (SUBREQ\0\0); the classification
    // must normalize both space and NUL padding.
    expect(storageFileType('SUBREQ\u0000\u0000'), StorageFileType.backup);
    expect(storageFileType('.SV     '), StorageFileType.backup);
    expect(storageFileType('SNREG   '), StorageFileType.snreg);
    expect(storageFileType('LAY_1   '), StorageFileType.layout);
    // Per-block dynamic persistence (Docs/Services/Register.md).
    expect(storageFileType('DT_0A   '), StorageFileType.dynamicTable);
    expect(storageFileType('DV_0A   '), StorageFileType.dynamicValues);
  });

  testWidgets('layout file uses the brightness limit + u8 width/height header',
      (tester) async {
    // Docs/Modules and blocks/LED display.md: u8 brightness limit (0-255 as a percentage),
    // u8 width, u8 height, then W*H u16 LE indexes.
    // An 11x10 grid = 3 + 110*2 = 223 bytes (the preloaded LAY_1 size).
    final data = <int>[
      178, 11, 10,
      for (var i = 0; i < 110; i++) ...u16(i == 0 ? 0 : 0xFFFF),
    ];
    expect(data.length, 223);
    await tester.pumpWidget(MaterialApp(
      theme: buildTheme(),
      home: Scaffold(
          body: FileViewPage(deviceId: 1, name: 'LAY_1', size: data.length, data: data)),
    ));
    await tester.pump();
    expect(tester.takeException(), isNull);
    // Parsed as u8 x3 -> 11x10 with the 70% cap; the old 2-byte header would be invalid.
    expect(find.text('11x10 LEDs · limit 178 (70%)'), findsOneWidget);
    expect(find.text('-'), findsWidgets); // 0xFFFF = missing cells
  });

  testWidgets('a layout header that overruns the file is rejected', (tester) async {
    // 11x10 announced, but only two index entries supplied.
    final data = <int>[178, 11, 10, 0, 0, 0, 0];
    await tester.pumpWidget(MaterialApp(
      theme: buildTheme(),
      home: Scaffold(
          body: FileViewPage(deviceId: 1, name: 'LAY_1', size: data.length, data: data)),
    ));
    await tester.pump();
    expect(tester.takeException(), isNull);
    expect(find.text('Invalid layout header (11x10)'), findsOneWidget);
  });

  test('formatValue renders size-flexible vectors and IDs', () {
    // A Vector2 (8 bytes) must not be rejected as "too short".
    expect(
        formatValue(DataType.vector,
            [...numberToBytes(1.5), ...numberToBytes(-2.0)]),
        '[1.500, -2]');
    final id = ((3 & 0x3F) << 10) | 7;
    expect(formatValue(DataType.id, [id & 0xFF, (id >> 8) & 0xFF]), '3.7');
  });

  test('formatSystemValue only decodes the version member as a version', () {
    // A 4-character Name must render as text, not as YY.MM.DD.II.
    expect(formatSystemValue(DataType.string, 'DAS1'.codeUnits, 6, 0xFF), 'DAS1');
    // The version is packed YY:MM:DD:II (7+4+5+16), not four raw bytes.
    expect(formatSystemValue(DataType.string, u32(0x33350001).toList(), 0, 2),
        '25.9.21.1');
  });

  test('capabilities format human-readable', () {
    // Core|Cli|DynamicMemory|StorageFiles|AppInterface|Subscriptions|Node
    expect(formatSystemValue(DataType.integer, u32(0x1ED).toList(), 0, 1),
        contains('Core'));
    expect(formatSystemValue(DataType.integer, u32(0x1ED).toList(), 0, 1),
        contains('Node'));
    expect(formatSystemValue(DataType.integer, u32(0x1ED).toList(), 0, 1),
        contains('Subs'));
  });

  testWidgets('DT_ dynamic block table renders', (tester) async {
    // Name (16 chars, NUL-padded), u16 entry_count, u16 reserved, then
    // (fieldKey, type, size, flags) per entry.
    final data = <int>[
      ...'Box'.codeUnits, ...List.filled(13, 0), // 16-byte name
      ...u16(2),
      0, 0, // reserved padding
      ...u16((0 << 8) | 0), ...u16(0),
      ...u16(DataType.number.value),
      4, ValueFlags.persistent,
      ...u16((1 << 8) | 0), ...u16(0),
      ...u16(DataType.string.value),
      5, 0,
    ];
    await pump(tester, 'DT_00  ', data);
    expect(find.textContaining('Box'), findsWidgets);
    expect(find.textContaining('Number'), findsWidgets);
    // Empty + corrupt tables must not throw.
    await pump(tester, 'DT_01  ', []);
    await pump(tester, 'DT_02  ', [9, 1, 2]);
  });

  testWidgets('FileViewPage renders formatted and raw hex', (tester) async {
    // `.SV` via the full page (formatted view).
    await tester.pumpWidget(MaterialApp(
      theme: buildTheme(),
      home: Scaffold(body: FileViewPage(
          deviceId: 1, name: '.SV', size: 16, data: [0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])),
    ));
    await tester.pump();
    expect(tester.takeException(), isNull, reason: 'FileViewPage formatted threw');
    // A binary file falls back to the raw hex table by default.
    await tester.pumpWidget(MaterialApp(
      theme: buildTheme(),
      home: Scaffold(body: FileViewPage(
          deviceId: 1, name: 'BIN.DAT', size: 48,
          data: [for (var i = 0; i < 48; i++) i])),
    ));
    await tester.pump();
    expect(tester.takeException(), isNull, reason: 'FileViewPage hex threw');
  });
}
