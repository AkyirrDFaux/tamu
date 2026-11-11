/// The two replies a fake core has to give a restore, spelled out once.
///
/// Every restore fake in this suite models the same device: a `.TABLE` file listing and a script
/// slot map. Both are pure byte encoders, and both were copied into four test files - where a
/// copy that drifted would make one suite's "device" disagree with another's about the same wire
/// format. [tableReply] and [scriptSlotMap] are the formats themselves.
library;

import 'package:tamuapp/core/storage_client.dart';

/// The `.TABLE` reply a device gives for the files it holds.
///
/// Record 0 is `.TABLE` itself, sized to `(count + 1) * 16`. One 16-byte record follows per file:
/// a 4-byte header, the file's size as u32 little-endian, and its 8-byte padded name. The body is
/// then chunked the way the Storage service fragments a read - an 8-byte `.TABLE` name echo
/// before every 64 content bytes - because that is how the device's own reply looks.
///
/// [names] is emitted in the order given, so a fake that lists files its device does NOT hold
/// (a stale row) keeps that row's position; [sizeOf] answers each name's length.
List<int> tableReply(List<String> names, int Function(String) sizeOf) {
  final content = <int>[];
  void record(String name, int size) {
    content
      ..addAll([0x40, 0, 0, 0])
      ..addAll([
        size & 0xFF,
        (size >> 8) & 0xFF,
        (size >> 16) & 0xFF,
        (size >> 24) & 0xFF,
      ])
      ..addAll(StorageClient.padName(name));
  }

  record('.TABLE', (names.length + 1) * 16);
  for (final name in names) {
    record(name, sizeOf(name));
  }
  final out = <int>[];
  for (var off = 0; off < content.length; off += 64) {
    final end = off + 64 > content.length ? content.length : off + 64;
    out
      ..addAll(StorageClient.padName('.TABLE'))
      ..addAll(content.sublist(off, end));
  }
  return out;
}

/// The script slot map the core answers Script CID 0 with: one uint16 file id per slot,
/// `0xFFFF` for an empty slot. [loaded] is slot -> file id; [slots] is the table size the device
/// reports (a v2 core's table is 16, a smaller fake uses 8).
List<int> scriptSlotMap(Map<int, int> loaded, {int slots = 16}) {
  final map = List<int>.filled(slots, 0xFFFF);
  loaded.forEach((slot, fileId) {
    if (slot < slots) map[slot] = fileId;
  });
  return [
    for (final id in map) ...[id & 0xFF, (id >> 8) & 0xFF],
  ];
}
