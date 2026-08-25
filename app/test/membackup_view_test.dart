import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/storage_page.dart';
import 'package:tamuapp/ui/theme.dart';

/// Renders MemoryBackupView against synthetic backup bytes matching the
/// firmware's SerializeRegistry / SerializeSystemBlocks layouts.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  List<int> u16(int v) => [v & 0xFF, (v >> 8) & 0xFF];

  Future<void> pump(WidgetTester tester, String name, List<int> data) async {
    await tester.pumpWidget(MaterialApp(
      theme: buildTheme(),
      home: Scaffold(body: MemoryBackupView(fileName: name, data: data)),
    ));
    await tester.pump();
    expect(tester.takeException(), isNull, reason: '$name threw while rendering');
  }

  testWidgets('DYNMEM backup renders', (tester) async {
    final data = <int>[
      ...u16(1), // one block
      3, 68, 89, 78, // name "DYN"
      ...u16(BlockType.undefined.value), // block type
      ...u16(2), // two entries
      ...BlockMeta(flagsAndType: DataType.number.value, size: 4).toBytes(),
      ...BlockMeta(flagsAndType: DataType.bool_.value, size: 1).toBytes(),
      ...u16(8), // data length
      ...numberToBytes(2.5),
      1, 0, 0, 0, // bool value + pad
    ];
    await pump(tester, 'DYNMEM', data);
    expect(find.textContaining('DYN'), findsWidgets);
    expect(find.textContaining('2.50'), findsWidgets);
  });

  testWidgets('KEYMEM backup renders', (tester) async {
    final data = <int>[
      ...u16(1), // one block
      3, 75, 69, 89,
      ...u16(BlockType.undefined.value),
      ...u16(1), // one dictionary
      ...BlockMeta(flagsAndType: DataType.undefined.value, size: 8).toBytes(),
      ...u16(8), // dict data length
      // entry: key 5, number, 4 bytes
      ...BlockMeta(flagsAndType: DataType.number.value, key: 5, size: 4).toBytes(),
      ...numberToBytes(3.75),
    ];
    await pump(tester, 'KEYMEM', data);
    expect(find.textContaining('Dictionary 0'), findsWidgets);
    expect(find.textContaining('3.75'), findsWidgets);
  });

  testWidgets('SYSMEM backup renders', (tester) async {
    final data = <int>[
      ...u16(1), // one writable block
      ...u16(0), // block index 0 (LEDButton)
      ...u16(1), // one field
      ...u16(0), // field index 0 (LEDState)
      ...u16(1), // vlen 1
      1, // value true
    ];
    await pump(tester, 'SYSMEM', data);
    expect(find.textContaining('Block 0'), findsWidgets);
  });

  testWidgets('empty backup renders', (tester) async {
    await pump(tester, 'DYNMEM', [0, 0]);
    expect(find.textContaining('empty backup'), findsOneWidget);
  });

  testWidgets('corrupt backup does not crash', (tester) async {
    await pump(tester, 'DYNMEM', [5, 0, 0xFF, 0xFF, 1, 2, 3]);
  });
}