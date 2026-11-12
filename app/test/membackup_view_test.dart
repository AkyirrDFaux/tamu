import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/types.dart';

import 'support/byte_fixtures.dart';
import 'support/viewer_pump.dart';

/// Renders MemoryBackupView (the `.SV` / SUBREQ / .DT_ decoders) against synthetic
/// bytes matching the firmware layouts (StaticMemory.h, Subscriptions.h, Memory.h).
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('.DT_ dynamic block table decodes entries', (tester) async {
    // Name (16 chars, NUL-padded), u16 entry_count, u16 reserved, then per entry
    // u16 fieldKey, u16 type, u8 size, u8 flags.
    final data = dynamicTable('DYN', [
      (0, DataType.number.value, 4, ValueFlags.persistent),
      ((1 << 8) | 5, DataType.bool_.value, 1, 0),
    ]);
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
