/// A restore run is not a black box: it reports *items of N*, and it can be stopped.
///
/// The owner's card asks for progress on the plan build and the bulk apply, and for a wait with
/// three exits. The plan build's progress is `planFromSnapshotChunked`'s callback (asserted here and
/// measured in `long_wait_hitch_test.dart`); the apply's is `applyRestorePlan(plan, onProgress:,
/// isCancelled:)`, which stops BETWEEN two writes - a restore writes item by item, so that is the
/// only safe place to stop - and reports what it never got to instead of dropping it silently.
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';

BackupEntry _entry(int field) => BackupEntry(
  field: 'Field $field',
  fieldIndex: field,
  key: 'Key 0',
  keyIndex: 0,
  type: 'Number',
  flags: const ['Persistent'],
  value: field,
  size: 4,
);

/// An archive of one block with [entries] rows. Restored against an empty live list, every row is
/// unready ("No target device"), so the apply walks them without touching a bus - which is what
/// makes the stop and the counting observable without a rig.
BackupDevice _archive(int entries) => BackupDevice(
  id: 4,
  type: 'Tamu v2.0A',
  typeId: 1,
  name: 'Node',
  serial: 'SN4',
  blocks: [
    BackupBlock(
      type: 'PWM output',
      typeIndex: 22,
      instance: 0,
      name: 'Fan',
      isDynamic: false,
      entries: [for (var i = 0; i < entries; i++) _entry(i)],
    ),
  ],
);

void main() {
  test('the plan build reports items scanned of N', () async {
    final seen = <RestoreProgress>[];
    final plan = await planFromSnapshotChunked(
      devices: [_archive(3)],
      live: const <LiveDevice>[],
      onProgress: seen.add,
    );

    expect(plan.items.length, 3);
    expect(
      seen.where((p) => p.phase == 'plan').length,
      1,
      reason: 'one archived device is one plan step',
    );
    final planStep = seen.firstWhere((p) => p.phase == 'plan');
    expect(planStep.determinate, isTrue);
    expect(planStep.completed, 1);
    expect(planStep.total, 1);

    final compare = seen.where((p) => p.phase == 'resolve').toList();
    expect(
      compare,
      isNotEmpty,
      reason: 'the classification is sliced, not one block',
    );
    expect(compare.last.completed, 3);
    expect(compare.last.total, 3, reason: 'the total is the real row count');
    expect(
      seen.where((p) => p.phase == 'classify').last.completed,
      3,
      reason: 'every row is classified, in slices',
    );
  });

  test('an apply reports items of N and stops where it is told to', () async {
    final plan = planFromSnapshot(
      devices: [_archive(3)],
      live: const <LiveDevice>[],
    );
    final seen = <RestoreProgress>[];
    var calls = 0;

    final report = await applyRestorePlan(
      plan,
      onProgress: seen.add,
      // Stop before the second item: the first one is walked (and fails as unready, which is the
      // run's own verdict, not the stop's).
      isCancelled: () => ++calls > 1,
    );

    final writes = seen.where((p) => p.phase == 'write').toList();
    expect(writes, isNotEmpty);
    expect(writes.first.total, 3, reason: 'items of N, N is the plan size');
    expect(
      writes.first.completed,
      0,
      reason: 'progress reports the items already done',
    );

    expect(report.cancelled, isTrue);
    expect(
      report.notRun,
      2,
      reason: 'the two items past the stop are counted, not dropped',
    );
    expect(
      report.text,
      contains('Stopped at your request: 2 item(s) were never written'),
    );
    expect(
      report.text,
      contains('nothing was deleted'),
      reason: 'the sweeps that delete are skipped on a partial run',
    );
  });

  test('a run nobody stopped is not a stopped run', () async {
    final plan = planFromSnapshot(
      devices: [_archive(2)],
      live: const <LiveDevice>[],
    );
    final report = await applyRestorePlan(plan);

    expect(report.cancelled, isFalse);
    expect(report.notRun, 0);
    expect(report.text, isNot(contains('Stopped at your request')));
  });
}
