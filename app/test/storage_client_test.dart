import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'support/fake_core.dart';

/// Host coverage for the Storage file-table policy: the offset-0 invalidation rule (a record
/// with offset 0 is a superseded/invalidated entry, skipped on every target now that all
/// devices run the full multi-file filesystem) and the all-0xFF terminator.
void main() {
  group('parseFileTable', () {
    final contents = [
      ...fileRecord(0x100, 16, 'A'),
      ...fileRecord(0, 40, 'GONE'), // invalidated: offset zeroed, size left
      ...fileRecord(0x200, 8, 'B'),
    ];

    test('drops the offset-0 (invalidated) record', () {
      final table = StorageClient.parseFileTable(contents);
      expect(table.map((r) => r.name), ['A', 'B']);
      expect(table.map((r) => r.offset), [0x100, 0x200]);
      // Indexes are renumbered over the live records only.
      expect(table.map((r) => r.index), [0, 1]);
    });

    test('an all-0xFF entry terminates the table', () {
      final withTail = [...contents, ...fileRecord(0x300, 1, 'C')];
      // Overwrite the tail record's first word-pair with the unwritten marker.
      withTail.setAll(contents.length, [
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
        0xFF,
      ]);
      final table = StorageClient.parseFileTable(withTail);
      expect(table.map((r) => r.name), ['A', 'B']);
    });

    test('an empty table parses to no records', () {
      expect(StorageClient.parseFileTable(const []), isEmpty);
    });

    test('includeInvalidated also returns the superseded record', () {
      final table = StorageClient.parseFileTable(
        contents,
        includeInvalidated: true,
      );
      expect(table.map((r) => r.name), ['A', 'GONE', 'B']);
      expect(table.map((r) => r.invalidated), [false, true, false]);
      // A superseded record keeps the size the table still holds, with no offset.
      expect(table[1].offset, 0);
      expect(table[1].size, 40);
      // Slots are the records' own positions, so the returned record still numbers 1 and a
      // view that lists them shows the place the superseded record occupies.
      expect(table.map((r) => r.slot), [0, 1, 2]);
      // The list position is over the returned records, as in the default parse.
      expect(table.map((r) => r.index), [0, 1, 2]);
    });

    test('includeInvalidated leaves every live row exactly as the default', () {
      final live = StorageClient.parseFileTable(contents);
      final all = StorageClient.parseFileTable(
        contents,
        includeInvalidated: true,
      );
      // Same slots, offsets, sizes, names and order - the superseded record is the only
      // addition, which is the proof that the Storage page's list did not change.
      expect(
        [
          for (final r in all)
            if (!r.invalidated) (r.slot, r.offset, r.size, r.name),
        ],
        [for (final r in live) (r.slot, r.offset, r.size, r.name)],
      );
      // `index` is the position among the records a parse RETURNED, so it is 2 for B in the
      // include-invalidated list; nothing renders it (`slot` numbers the rows).
      expect(all.map((r) => r.index), [0, 1, 2]);
    });

    test('the all-0xFF terminator stops an includeInvalidated parse too', () {
      final withTail = [
        ...contents,
        ...fileRecord(0, 8, 'TAIL'), // a superseded record after the terminator
      ];
      withTail.setAll(contents.length, List<int>.filled(8, 0xFF));
      final table = StorageClient.parseFileTable(
        withTail,
        includeInvalidated: true,
      );
      expect(table.map((r) => r.name), ['A', 'GONE', 'B']);
    });
  });

  group('padName', () {
    test('pads a short name with NULs to the 8-byte wire field (B10)', () {
      expect(StorageClient.padName('A'), [
        0x41,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
      ]);
    });

    test('truncates an over-long name to 8 bytes', () {
      expect(StorageClient.padName('ABCDEFGHIJ'), 'ABCDEFGH'.codeUnits);
    });
  });

  group('stripFragmentNames', () {
    // A reassembled CID-5 stream is [name (8)][content] per fragment (B12); the reassembly
    // layer has already removed the 4-byte frag info from each.
    final name = StorageClient.padName('A');
    final full = List<int>.generate(
      StorageClient.fragmentContentMax,
      (i) => i & 0xFF,
    );

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
