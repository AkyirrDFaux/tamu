import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/types.dart';

import 'support/viewer_pump.dart';

/// Renders MemoryBackupView (the `.SV` / SUBREQ / .DT_ decoders) against synthetic
/// bytes matching the firmware layouts (StaticMemory.h, Subscriptions.h, Memory.h).
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  List<int> u16(int v) => [v & 0xFF, (v >> 8) & 0xFF];

  testWidgets('.DT_ dynamic block table decodes entries', (tester) async {
    // Name (16 chars, NUL-padded), u16 entry_count, u16 reserved, then per entry
    // u16 fieldKey, u16 type, u8 size, u8 flags.
    final data = <int>[
      ...'DYN'.codeUnits, ...List.filled(13, 0), // 16-byte name
      ...u16(2),
      0, 0, // reserved padding
      ...u16((0 << 8) | 0), ...u16(0),
      ...u16(DataType.number.value),
      4, ValueFlags.persistent,
      ...u16((1 << 8) | 5), ...u16(0),
      ...u16(DataType.bool_.value),
      1, 0,
    ];
    await pumpMemoryBackup(tester, '.DT_00  ', data);
    expect(find.textContaining('DYN'), findsWidgets);
    expect(find.textContaining('Number'), findsWidgets);
  });

  testWidgets('empty and corrupt registry files do not crash', (tester) async {
    await pumpMemoryBackup(tester, '.DT_00', []);
    await pumpMemoryBackup(tester, '.DT_00', [5, 0, 0xFF, 0xFF, 1, 2, 3]);
    await pumpMemoryBackup(tester, '.SV', [0xFF]);
    await pumpMemoryBackup(tester, 'BOGUS', [1, 2, 3]);
  });
}
