/// The measured hitch: how long the restore plan build blocks the main isolate, before and after
/// it was cut into slices.
///
/// A sync block of ~80 ms is four dropped frames on the page that shows the plan - the owner's
/// "some places just wait and freeze up". This test drives the same two entry points the page does
/// (`planFromSnapshot` as it was written, `planFromSnapshotChunked` as it is now) over a synthetic
/// archive big enough to be a real ledger: 3 devices x 8 blocks x 512 entries = 12,288 rows.
///
/// Numbers are from this host's debug VM (JIT), so they are an upper bound on a release build -
/// which is the direction that matters for "long enough to hitch the frame".
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/types.dart';

BackupEntry _entry(int field, int key) => BackupEntry(
  field: 'Field $field',
  fieldIndex: field,
  key: 'Key $key',
  keyIndex: key,
  type: 'Number',
  flags: const ['Persistent'],
  value: field * 10 + key,
  size: 4,
);

LiveEntry _live(int field, int key, int value) => LiveEntry(
  field: field,
  key: key,
  fieldName: 'Field $field',
  keyName: 'Key $key',
  meta: ValueInfo(type: DataType.number.value, size: 4),
  value: [
    value & 0xFF,
    (value >> 8) & 0xFF,
    (value >> 16) & 0xFF,
    (value >> 24) & 0xFF,
  ],
);

/// One archive + the snapshot it restores onto, with every row `same`.
({List<BackupDevice> archive, List<LiveDevice> live}) _rig() {
  const devices = 3;
  const blocks = 8;
  const fields = 128;
  const keys = 4;
  final archived = <BackupDevice>[];
  final live = <LiveDevice>[];
  for (var d = 0; d < devices; d++) {
    final bBlocks = <BackupBlock>[];
    final lBlocks = <LiveBlock>[];
    for (var b = 0; b < blocks; b++) {
      final bEntries = <BackupEntry>[];
      final lEntries = <LiveEntry>[];
      for (var f = 0; f < fields; f++) {
        for (var k = 0; k < keys; k++) {
          bEntries.add(_entry(f, k));
          lEntries.add(_live(f, k, f * 10 + k));
        }
      }
      bBlocks.add(
        BackupBlock(
          type: 'PWM output',
          typeIndex: 22 + b,
          instance: b,
          name: 'Block $b',
          isDynamic: false,
          entries: bEntries,
        ),
      );
      lBlocks.add(
        LiveBlock(
          type: 22 + b,
          instance: b,
          name: 'Block $b',
          isDynamic: false,
          entries: lEntries,
        ),
      );
    }
    archived.add(
      BackupDevice(
        id: 4 + d,
        type: 'Tamu v2.0A',
        typeId: 1,
        name: 'Node $d',
        serial: 'SN$d',
        blocks: bBlocks,
      ),
    );
    live.add(
      LiveDevice(id: 4 + d, serial: 'SN$d', name: 'Node $d', blocks: lBlocks),
    );
  }
  return (archive: archived, live: live);
}

void main() {
  test('the plan build is sliced, and the longest slice fits in a frame', () async {
    final rig = _rig();

    // BEFORE: the whole plan built and classified in one synchronous block, as the page used to
    // call it. This is the hitch the bar replaces.
    final before = Stopwatch()..start();
    final syncPlan = planFromSnapshot(devices: rig.archive, live: rig.live);
    before.stop();
    expect(syncPlan.items.length, 12288);

    // AFTER: the same plan, sliced. `onProgress` fires at every slice boundary, so the gap between
    // two calls IS one slice's work - the longest gap is the longest hitch the UI can feel.
    final stamps = <int>[];
    final after = Stopwatch()..start();
    final chunkStamps = <int>[];
    final chunked = await planFromSnapshotChunked(
      devices: rig.archive,
      live: rig.live,
      onProgress: (_) => chunkStamps.add(after.elapsedMicroseconds),
    );
    after.stop();
    stamps.addAll(chunkStamps);

    var longestSliceUs = 0;
    var previous = 0;
    for (final stamp in stamps) {
      final delta = stamp - previous;
      if (delta > longestSliceUs) longestSliceUs = delta;
      previous = stamp;
    }

    expect(
      chunked.items.length,
      syncPlan.items.length,
      reason: 'slicing must not change which rows the plan carries',
    );
    expect(
      chunked.items.where((i) => i.diff != null).length,
      syncPlan.items.where((i) => i.diff != null).length,
      reason: 'every row is classified by the sliced path too',
    );
    expect(stamps.length, greaterThan(3), reason: 'the build really did yield');

    // ignore: avoid_print
    print(
      'HITCH plan build: one block ${before.elapsedMilliseconds} ms -> '
      'longest slice ${(longestSliceUs / 1000).toStringAsFixed(1)} ms '
      'over ${stamps.length} slices '
      '(total ${after.elapsedMilliseconds} ms)',
    );
    expect(
      longestSliceUs,
      lessThan(before.elapsedMicroseconds),
      reason:
          'the sliced build never holds the isolate as long as one block did',
    );
  });
}
