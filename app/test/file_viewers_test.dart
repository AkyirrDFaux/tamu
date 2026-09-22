import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/file_viewers.dart';
import 'package:tamuapp/ui/system_block_view.dart';
import 'package:tamuapp/ui/theme.dart';

/// Renders the STATLOG / SUBREQ decoders against synthetic bytes matching the
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

  testWidgets('STATLOG decodes system + static entries', (tester) async {
    // Entry: BlockIndex[4] + BlockMeta[4] + value[4-aligned].
    // 1. System Name (block 0xFE, field 6): "DAS v0.1"
    final name = 'DAS v0.1'.codeUnits.toList();
    final entry1 = <int>[
      0xFE, 6, 0xFF, 0, // BlockIndex
      DataType.string.value & 0xFF, (DataType.string.value >> 8) & 0xFF, 0xFF, name.length,
      ...name,
    ];
    while (entry1.length % 4 != 0) {
      entry1.add(0);
    }
    // 2. Static block 0, field 0: Number 10.0
    final entry2 = <int>[
      0, 0, 0xFF, 0, // BlockIndex
      DataType.number.value & 0xFF, (DataType.number.value >> 8) & 0xFF, 0xFF, 4,
      ...numberToBytes(10),
    ];
    // 3. End marker
    final data = [...entry1, ...entry2, 0xFF];
    final blocks = <({int type, int inst, BlockMeta meta, String name})?>[
      (type: BlockType.resistiveMeasure.value, inst: 0,
          meta: BlockMeta(flagsAndType: DataType.number.value, size: 4), name: 'Meas1'),
    ];
    await tester.pumpWidget(MaterialApp(
      theme: buildTheme(),
      home: Scaffold(body: MemoryBackupView(fileName: 'STATLOG', data: data, blocks: blocks)),
    ));
    await tester.pump();
    expect(tester.takeException(), isNull, reason: 'STATLOG threw');
  });

  testWidgets('SUBREQ decodes requester entries (26 B incl. deadzone)', (tester) async {
    // u8 count + 26 B per entry: targetReg, sourceReg, providerAddr u16,
    // trigger u8 + 3 pad, periodMs u32, minTimeMs u32, deadzone Number (16.16).
    final targetReg = makeBlockInfo(0, 0, 0, 0);
    final sourceReg = makeBlockInfo(8, 0, 4, 0);
    List<int> entry(int provider, double deadzone) => <int>[
          ...u32(targetReg),
          ...u32(sourceReg),
          provider & 0xFF, (provider >> 8) & 0xFF,
          1, 0, 0, 0, // trigger + pad
          ...u32(1000),
          ...u32(100),
          ...numberToBytes(deadzone),
        ];
    // Two entries: a misaligned (22 B) parser would read garbage from entry 2.
    final data = <int>[2, ...entry(2, 0), ...entry(3, 1.5)];
    await pump(tester, 'SUBREQ', data);
  });

  test('file type detection tolerates wire padding', () {
    // Older renames stored NUL-padded names (SUBREQ\0\0); the classification
    // must normalize both space and NUL padding.
    expect(storageFileType('SUBREQ\u0000\u0000'), StorageFileType.backup);
    expect(storageFileType('STATLOG '), StorageFileType.backup);
    expect(storageFileType('SNREG   '), StorageFileType.snreg);
    expect(storageFileType('LAY_1   '), StorageFileType.layout);
    // Per-block dynamic persistence (Docs/Services/Register.md).
    expect(storageFileType('DT_0A   '), StorageFileType.dynamicTable);
    expect(storageFileType('DV_0A   '), StorageFileType.dynamicValues);
  });

  testWidgets('layout file uses the u8 width/height header', (tester) async {
    // Docs/Modules/LED display.md: u8 width, u8 height, then W*H u16 LE indexes.
    // An 11x10 grid = 2 + 110*2 = 222 bytes (the preloaded LAY_1 size).
    final data = <int>[
      11, 10,
      for (var i = 0; i < 110; i++) ...u16(i == 0 ? 0 : 0xFFFF),
    ];
    expect(data.length, 222);
    await tester.pumpWidget(MaterialApp(
      theme: buildTheme(),
      home: Scaffold(
          body: FileViewPage(deviceId: 1, name: 'LAY_1', size: data.length, data: data)),
    ));
    await tester.pump();
    expect(tester.takeException(), isNull);
    // Parsed as u8 x2 -> 11x10; the old uint16 header would have reported invalid.
    expect(find.text('11x10 LEDs'), findsOneWidget);
    expect(find.text('-'), findsWidgets); // 0xFFFF = missing cells
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
    // u8 name_len, name, u16 type, u16 entry_count, then (fieldKey, flagsAndType,
    // size, pad) per entry.
    final data = <int>[
      3, 66, 111, 120, // "Box"
      ...u16(BlockType.dynamic.value),
      ...u16(2),
      ...u16((0 << 8) | 0),
      ...u16(DataType.number.value | FieldFlags.persistent),
      4, 0,
      ...u16((1 << 8) | 0),
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
    // STATLOG via the full page (formatted view).
    await tester.pumpWidget(MaterialApp(
      theme: buildTheme(),
      home: Scaffold(body: FileViewPage(
          deviceId: 1, name: 'STATLOG', size: 16, data: [0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])),
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
