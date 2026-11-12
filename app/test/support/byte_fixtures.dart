/// The little-endian integer encoders and the `.DT_` table builder the backup fixtures share.
///
/// The device's dynamic block table (`.DT_`) was hand-built in three suites -
/// `device_backup_test` as a helper with the offsets computed, `file_viewers_test` and
/// `membackup_view_test` as inline literals with the offsets typed out - and the two
/// `u16`/`u32` encoders under them were pasted into five. A copy that drifted would model a
/// different table than the decoder under test parses, so the layout lives here once.
library;

import 'package:tamuapp/core/types.dart';

/// [v] as the two little-endian bytes the wire carries.
List<int> u16(int v) => [v & 0xFF, (v >> 8) & 0xFF];

/// [v] as the four little-endian bytes the wire carries.
List<int> u32(int v) => [
  v & 0xFF,
  (v >> 8) & 0xFF,
  (v >> 16) & 0xFF,
  (v >> 24) & 0xFF,
];

/// A `.DT_` table: the block's name NUL-padded to 16 bytes, the entry count as u16, two reserved
/// bytes, then one row per entry - field/key as u16, the value's offset as u16, its type as u16,
/// its size in bytes and its flags.
///
/// Each entry is `(fieldKey, type, size, flags)`. The offset is the entry's position in its own
/// value space: persistent entries count from the persistent space, volatile ones from the
/// volatile space, which is what the firmware's `Memory.h` layout does.
List<int> dynamicTable(
  String name,
  List<(int fieldKey, int type, int size, int flags)> entries,
) {
  final nameBytes = name.codeUnits.take(16).toList();
  while (nameBytes.length < 16) {
    nameBytes.add(0x00);
  }
  final out = <int>[...nameBytes, ...u16(entries.length), 0, 0];
  var persistentOffset = 0;
  var volatileOffset = 0;
  for (final entry in entries) {
    final (fieldKey, type, size, flags) = entry;
    final persistent = flags & ValueFlags.persistent != 0;
    final offset = persistent ? persistentOffset : volatileOffset;
    if (persistent) {
      persistentOffset += size;
    } else {
      volatileOffset += size;
    }
    out.addAll([...u16(fieldKey), ...u16(offset), ...u16(type), size, flags]);
  }
  return out;
}
