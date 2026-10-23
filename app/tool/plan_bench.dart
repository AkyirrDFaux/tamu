/// Timing bench for the restore plan (card t_6ffa42c6: the plan page became a diff viewer).
///
/// Why: the classification that turns the plan into a diff compares every row against the live
/// snapshot, and the card asked for the plan-build time before -> after that change. A number
/// nothing in the tree can re-run is a claim, not a measurement - so this tool builds the same
/// 110-row shape the card measured (103 register entries, 5 semantic scripts, 2 raw files) from a
/// synthetic archive and a synthetic live device, and times it here.
///
/// Usage (from `app/`, no arguments needed):
///
///     dart run tool/plan_bench.dart
///     dart run tool/plan_bench.dart --runs 500
///
/// It prints what it timed, the row count and their classes, so the number can be read against
/// the plan it belongs to:
///
///     build            the whole `planFromSnapshot` - placement, classification and the
///                      deletion list (what the page does when it opens a plan);
///     classify only    the comparison alone, over the same rows (`RestorePlan.classify`), which
///                      is the share the diff view added;
///     view             `RestorePlan.view(PlanFilter.diff)`, the default filter's pass.
///
/// All 110 rows classify as SAME here (the device is built from the archive's own values), which
/// is the worst case for the comparison and the one the card quoted. Nothing in this tool touches
/// the bus: the snapshot and its inventory are handed to `planFromSnapshot` directly, so it runs
/// without a rig, and the numbers are a millisecond cost on the host, not a bus cost.
library;

import 'dart:convert';
import 'dart:io';

import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/backup_script.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/types.dart';

/// The row shape the card measured: 103 entries + 5 scripts + 2 raw files.
const entryCount = 103;
const scriptCount = 5;
const fileCount = 2;

/// One archived register entry, a Number, the commonest row in a real archive.
BackupEntry _entry(int index) => BackupEntry(
  field: 'Field $index',
  fieldIndex: index,
  key: 'Key 0',
  keyIndex: 0,
  type: 'Number',
  flags: const ['Persistent'],
  value: index * 1.5,
  size: 4,
);

/// A script with a few lines, so rebuilding its image costs what a real one does.
BackupScript _script(int slot) => BackupScript(
  slot: slot,
  functionName: 'Script $slot',
  properties: const [],
  inputs: const [],
  outputs: const [],
  variables: const [],
  constants: const [],
  lines: [
    for (var line = 0; line < 4; line++) BackupLine.of(ScriptLine.callTo(0)),
  ],
);

BackupDevice _archive(List<BackupScript> scripts) => BackupDevice(
  type: 'Tamu v2.0A',
  typeId: 1,
  id: idFromString('39.1')!,
  name: 'Tamu',
  blocks: [
    BackupBlock(
      type: 'PWM output',
      typeIndex: BlockType.pwm.value,
      instance: 0,
      name: 'Fan',
      isDynamic: false,
      entries: [for (var i = 0; i < entryCount; i++) _entry(i)],
    ),
  ],
  scripts: scripts,
  files: [
    for (var i = 0; i < fileCount; i++)
      BackupFile(
        name: 'DATA_$i',
        kind: 'Binary',
        data: base64Encode([for (var b = 0; b < 512; b++) (i + b) & 0xFF]),
      ),
  ],
);

/// The same device, holding exactly what the archive carries: every row is SAME.
LiveDevice _device(BackupDevice archive) {
  final entries = <LiveEntry>[];
  for (final entry in archive.blocks.single.entries) {
    final target = LiveEntry(
      field: entry.fieldIndex,
      key: 0,
      fieldName: entry.field,
      keyName: 'Key 0',
      meta: ValueInfo(type: DataType.number.value, size: 4),
    );
    // The bytes the restore would write, so the comparison has something to find identical.
    entries.add(
      LiveEntry(
        field: target.field,
        key: target.key,
        fieldName: target.fieldName,
        keyName: target.keyName,
        meta: target.meta,
        value: resolveEntryBytes(entry, target)!,
      ),
    );
  }
  return LiveDevice(
    id: archive.id,
    name: 'Tamu',
    capabilities: Capability.scripts | Capability.subscriptionRequest,
    blocks: [
      LiveBlock(
        type: BlockType.pwm.value,
        instance: 0,
        name: 'Fan',
        isDynamic: false,
        entries: entries,
      ),
    ],
  );
}

/// What the device holds beyond its block walk: the scripts' files, the raw files' bytes and the
/// slots each script is loaded in - the reads that let a row be compared at all.
LiveInventory _inventory(BackupDevice archive, List<BackupScript> scripts) {
  final names = <String>{};
  final sizes = <String, int>{};
  final files = <String, List<int>>{};
  for (final script in scripts) {
    final name = scriptFileName(script.slot);
    final bytes = script.toDraft().toImage();
    names.add(name);
    sizes[name] = bytes.length;
    files[name] = bytes;
  }
  for (final file in archive.files) {
    names.add(file.name);
    sizes[file.name] = file.bytes.length;
    files[file.name] = file.bytes;
  }
  return LiveInventory(
    fileNames: names,
    fileSizes: sizes,
    files: files,
    loadedSlots: {for (final script in scripts) script.slot: script.slot},
    requesters: const [],
    sndb: const [],
  );
}

String _stats(List<int> micros) {
  final sorted = [...micros]..sort();
  final mean = micros.reduce((a, b) => a + b) / micros.length;
  return '${(mean / 1000).toStringAsFixed(3)} ms mean, '
      '${(sorted[sorted.length ~/ 2] / 1000).toStringAsFixed(3)} ms median, '
      '${(sorted.first / 1000).toStringAsFixed(3)} ms min, '
      '${(sorted.last / 1000).toStringAsFixed(3)} ms max '
      'over ${micros.length} runs';
}

void main(List<String> args) {
  var runs = 200;
  for (var i = 0; i < args.length; i++) {
    if (args[i] == '--runs' && i + 1 < args.length) {
      runs = int.tryParse(args[i + 1]) ?? runs;
    }
  }
  final scripts = [
    for (var slot = 1; slot <= scriptCount; slot++) _script(slot),
  ];
  final archive = _archive(scripts);
  final live = _device(archive);
  final inventory = _inventory(archive, scripts);

  RestorePlan build() => planFromSnapshot(
    devices: [archive],
    live: [live],
    inventories: {live.id: inventory},
  );

  // Warm the VM up (JIT, image rebuilds) before anything is timed.
  var plan = build();
  for (var i = 0; i < 50; i++) {
    plan = build();
  }

  final classes = <DiffKind?, int>{};
  for (final item in plan.items) {
    classes[item.diff] = (classes[item.diff] ?? 0) + 1;
  }
  stdout.writeln(
    'plan rows: ${plan.items.length} '
    '(${archive.blocks.single.entries.length} entries, '
    '${archive.scripts.length} scripts, ${archive.files.length} files) '
    '-> ${[for (final e in classes.entries) '${e.key?.word ?? 'unclassified'} ${e.value}'].join(', ')}'
    ', deletions ${plan.removals.length}',
  );

  final builds = <int>[];
  for (var i = 0; i < runs; i++) {
    final watch = Stopwatch()..start();
    plan = build();
    watch.stop();
    builds.add(watch.elapsedMicroseconds);
  }

  final classifies = <int>[];
  for (var i = 0; i < runs; i++) {
    final watch = Stopwatch()..start();
    plan.classify();
    watch.stop();
    classifies.add(watch.elapsedMicroseconds);
  }

  final views = <int>[];
  for (var i = 0; i < runs; i++) {
    final watch = Stopwatch()..start();
    plan.view(PlanFilter.diff);
    watch.stop();
    views.add(watch.elapsedMicroseconds);
  }

  stdout.writeln('build           ${_stats(builds)}');
  stdout.writeln('classify only   ${_stats(classifies)}');
  stdout.writeln('view (diff)     ${_stats(views)}');
  final differences =
      plan.items
          .where((i) => i.diff != null && i.diff != DiffKind.same)
          .length +
      plan.removals.length;
  stdout.writeln(
    'diff view shows ${plan.view().shown} of ${plan.items.length} row(s)'
    '${differences == 0 ? ' - this shape is all the same, so Diff has nothing to show' : ''}',
  );
  if (differences != 0) {
    // The shape this bench builds holds what the archive holds; a run that finds differences is
    // measuring a different plan, and the numbers must not be read as the card's.
    stdout.writeln(
      'NOTE: $differences row(s) are not the same - this is not the all-same shape '
      'the card measured',
    );
  }
}
