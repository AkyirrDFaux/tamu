import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/types.dart';

/// Host coverage for the Storage file-table policy: the offset-0 invalidation rule (a record
/// with offset 0 is a superseded/invalidated entry, skipped on every target now that all
/// devices run the full multi-file filesystem) and the all-0xFF terminator.
void main() {
  List<int> record(int offset, int size, String name) => [
        ...uint32ToBytes(offset),
        ...uint32ToBytes(size),
        ...StorageClient.padName(name),
      ];

  group('parseFileTable', () {
    final contents = [
      ...record(0x100, 16, 'A'),
      ...record(0, 40, 'GONE'), // invalidated: offset zeroed, size left
      ...record(0x200, 8, 'B'),
    ];

    test('drops the offset-0 (invalidated) record', () {
      final table = StorageClient.parseFileTable(contents);
      expect(table.map((r) => r.name), ['A', 'B']);
      expect(table.map((r) => r.offset), [0x100, 0x200]);
      // Indexes are renumbered over the live records only.
      expect(table.map((r) => r.index), [0, 1]);
    });

    test('an all-0xFF entry terminates the table', () {
      final withTail = [...contents, ...record(0x300, 1, 'C')];
      // Overwrite the tail record's first word-pair with the unwritten marker.
      withTail.setAll(contents.length, [0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF]);
      final table = StorageClient.parseFileTable(withTail);
      expect(table.map((r) => r.name), ['A', 'B']);
    });

    test('an empty table parses to no records', () {
      expect(StorageClient.parseFileTable(const []), isEmpty);
    });
  });

  group('padName', () {
    test('pads a short name with NULs to the 8-byte wire field (B10)', () {
      expect(StorageClient.padName('A'),
          [0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]);
    });

    test('truncates an over-long name to 8 bytes', () {
      expect(StorageClient.padName('ABCDEFGHIJ'), 'ABCDEFGH'.codeUnits);
    });
  });

  group('stripFragmentNames', () {
    // A reassembled CID-5 stream is [name (8)][content] per fragment (B12); the reassembly
    // layer has already removed the 4-byte frag info from each.
    final name = StorageClient.padName('A');
    final full = List<int>.generate(StorageClient.fragmentContentMax, (i) => i & 0xFF);

    test('single short fragment', () {
      expect(StorageClient.stripFragmentNames([...name, 1, 2, 3]), [1, 2, 3]);
    });

    test('walks a full fragment and the short tail', () {
      final tail = [9, 8, 7];
      final reply = [...name, ...full, ...name, ...tail];
      expect(StorageClient.stripFragmentNames(reply), [...full, ...tail]);
    });

    test('an exactly-full single fragment', () {
      expect(StorageClient.stripFragmentNames([...name, ...full]), full);
    });

    test('an empty file carries the name only', () {
      expect(StorageClient.stripFragmentNames(name), isEmpty);
    });
  });
}
