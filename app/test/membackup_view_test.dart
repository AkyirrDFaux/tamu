import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/file_viewers.dart';
import 'package:tamuapp/ui/theme.dart';

/// Renders MemoryBackupView (the STATLOG / SUBREQ / DT_ decoders) against synthetic
/// bytes matching the firmware layouts (StaticMemory.h, Subscriptions.h, Memory.h).
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

  testWidgets('DT_ dynamic block table decodes entries', (tester) async {
    // u8 name_len, name, u16 type, u16 entry_count, then per entry
    // u16 fieldKey, u16 flagsAndType, u8 size, u8 pad.
    final data = <int>[
      3, 68, 89, 78, // name "DYN"
      ...u16(BlockType.dynamic.value),
      ...u16(2),
      ...u16((0 << 8) | 0),
      ...u16(DataType.number.value | FieldFlags.persistent),
      4, 0,
      ...u16((1 << 8) | 5),
      ...u16(DataType.bool_.value),
      1, 0,
    ];
    await pump(tester, 'DT_00  ', data);
    expect(find.textContaining('DYN'), findsWidgets);
    expect(find.textContaining('Number'), findsWidgets);
  });

  testWidgets('empty and corrupt registry files do not crash', (tester) async {
    await pump(tester, 'DT_00', []);
    await pump(tester, 'DT_00', [5, 0, 0xFF, 0xFF, 1, 2, 3]);
    await pump(tester, 'STATLOG', [0xFF]);
    await pump(tester, 'BOGUS', [1, 2, 3]);
  });
}
