import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/types.dart';

/// Host coverage for the Storage file-table policy: the fixed-vs-full detection from a
/// capability word and the offset-0 invalidation rule (a record with offset 0 is a real file
/// on the reduced/fixed FS, but an invalidated entry on the full FS).
void main() {
  List<int> record(int offset, int size, String name) => [
        ...uint32ToBytes(offset),
        ...uint32ToBytes(size),
        ...StorageClient.padName(name),
      ];

  group('fixed-vs-full detection', () {
    test('a device that reports capabilities without StorageFiles is the reduced FS', () {
      expect(StorageClient.isFixedStorage(0), isFalse); // not read yet: assume full
      expect(StorageClient.isFixedStorage(Capability.storageFiles), isFalse);
      expect(
          StorageClient.isFixedStorage(Capability.storageFiles | Capability.core), isFalse);
      expect(StorageClient.isFixedStorage(Capability.core), isTrue);
      expect(StorageClient.isFixedStorage(Capability.node | Capability.dynamicMemory), isTrue);
    });

    test('the constructor honours an explicit override', () {
      final fixed = StorageClient(deviceId: 1, fixedStorage: true);
      final full = StorageClient(deviceId: 1, fixedStorage: false);
      expect(fixed.fixedStorage, isTrue);
      expect(full.fixedStorage, isFalse);
    });
  });

  group('offset-0 skip', () {
    final contents = [
      ...record(0x100, 16, 'A'),
      ...record(0, 40, 'GONE'), // invalidated on the full FS
      ...record(0x200, 8, 'B'),
    ];

    test('full FS drops the offset-0 (invalidated) record', () {
      final table = StorageClient.parseFileTable(contents, fixedStorage: false);
      expect(table.map((r) => r.name), ['A', 'B']);
      expect(table.map((r) => r.offset), [0x100, 0x200]);
      // Indexes are renumbered over the live records only.
      expect(table.map((r) => r.index), [0, 1]);
    });

    test('fixed FS keeps the offset-0 record', () {
      final table = StorageClient.parseFileTable(contents, fixedStorage: true);
      expect(table.map((r) => r.name), ['A', 'GONE', 'B']);
      expect(table.map((r) => r.offset), [0x100, 0, 0x200]);
    });

    test('an all-0xFF entry terminates the table', () {
      final withTail = [...contents, ...record(0x300, 1, 'C')];
      // Overwrite the first tail word-pair with the unwritten marker.
      withTail.setAll(contents.length, [0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]);
      final table = StorageClient.parseFileTable(withTail, fixedStorage: true);
      expect(table.map((r) => r.name), ['A', 'GONE', 'B']);
    });

    test('an empty table parses to no records', () {
      expect(StorageClient.parseFileTable(const [], fixedStorage: false), isEmpty);
    });
  });
}
