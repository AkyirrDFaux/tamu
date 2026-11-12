import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/backup_script.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_instructions.dart';

import 'support/restore_core.dart';
import 'package:tamuapp/core/types.dart';

/// The shared restore device (see `support/restore_core.dart`) with the file TABLE modelled: this
/// suite's sweep reads the table, deletes from it and renames into it, and `SCR_000` surviving a
/// restore is exactly the defect a device without a table model cannot show.
class _FakeCore extends RestoreCore {
  _FakeCore({super.loaded, super.table})
    : super(
        modelsFileTable: true,
        answersFileReads: true,
        throwOnUnknown: true,
      );
}

BackupDevice _device(List<BackupScript> scripts) => BackupDevice(
  type: 'Tamu v2.0A',
  typeId: 1,
  id: 1,
  name: 'Tamu',
  blocks: const [],
  scripts: scripts,
);

LiveDevice _live() => LiveDevice(
  id: 1,
  name: 'Tamu',
  capabilities: Capability.scripts,
  blocks: const [],
);

BackupScript _script(int slot, String name) => BackupScript(
  slot: slot,
  functionName: name,
  properties: const [],
  inputs: const [],
  outputs: const [],
  variables: const [],
  constants: const [],
  lines: const [],
);

/// A script that CALLS a global function living in [library], so its image references that
/// library file - the file a restore must never delete.
BackupScript _libraryCaller(int slot, String name, int library) => BackupScript(
  slot: slot,
  functionName: name,
  properties: const [],
  inputs: const [],
  outputs: const [],
  variables: const [],
  constants: const [],
  lines: [BackupLine.of(ScriptLine.callTo(0))],
  functions: [
    BackupFunction(
      name: 'Blink',
      kind: 'Global',
      library: library,
      memoryOffset: 0x20,
      instructionOffset: 0x40,
    ),
  ],
);

/// The plan a page would hold: the archive's scripts against a device whose inventory is
/// [table] / [loaded], so the deletions it lists are built by the same code the page uses.
RestorePlan _plan(
  BackupDevice device,
  LiveDevice live,
  Map<String, int> table,
  Map<int, int> loaded,
) {
  final plan = RestorePlan(
    devices: [device],
    liveDevices: [live],
    items: [
      for (final script in device.scripts)
        RestoreItem(kind: RestoreKind.script, device: device, script: script),
    ],
    deviceTargets: {device.id: live},
    blockTargets: const {},
    inventories: {
      live.id: LiveInventory(
        fileNames: {for (final n in table.keys) n.toUpperCase()},
        fileSizes: {
          for (final e in table.entries) e.key.toUpperCase(): e.value,
        },
        loadedSlots: loaded,
      ),
    },
  );
  plan.resolve();
  for (final item in plan.items) {
    item.selected = item.ready;
  }
  return plan;
}

/// The plan's file deletions as the page lists them.
List<String> _planFileRows(RestorePlan plan) => [
  for (final r in plan.removals)
    if (r.kind == RestoreKind.file) r.fileName!,
];

String _tableText(Map<String, int> table) => table.isEmpty
    ? '-'
    : [
        for (final e in (table.keys.toList()..sort()))
          if (scriptFileId(e) != null) '$e (${table[e]} B)',
      ].join(', ');

String _fileReport(ScriptFilePrune p) =>
    'FILES ${idToString(p.deviceId)} "${p.deviceName}": '
    'before=[${p.beforeText}] deleted=[${p.deletedText}] '
    'kept-selection=[${p.keptText}] kept-row=[${p.keptRowText}] '
    'kept-loaded=[${p.keptLoadedText}] refused=[${p.refusedText}] '
    'remaining=[${p.remainingText}]'
    '${p.skipped ? ' SKIPPED: ${p.reason}' : ''}';

void main() {
  late RequestChannel? previous;
  late _FakeCore core;

  void install(_FakeCore fake) {
    core = fake;
    installRequestChannel(fake);
  }

  setUp(() {
    previous = requestChannel;
    install(_FakeCore());
  });

  tearDown(() => installRequestChannel(previous));

  test(
    'a stored script the archive does not carry is deleted and reported',
    () async {
      // bambi's rig after the slot sweep landed: `SCR_000` is still in the file table, so the
      // script is unloaded but loadable, and a later "load all" brings it back.
      final table = {'SCR_000': 4, 'SCR_001': 0, 'SCR_002': 0, 'notes.txt': 12};
      final loaded = {1: 1, 2: 2};
      final device = _device([_script(1, 'Fan curve'), _script(2, 'Selector')]);
      install(_FakeCore(loaded: loaded, table: table));
      final plan = _plan(device, _live(), table, loaded);

      final planRows = _planFileRows(plan);
      final result = await applyRestorePlan(plan);

      final prune = result.scriptFiles.single;
      print('file table before the restore: ${_tableText(table)}');
      print('plan DELETE rows: $planRows');
      print('files the restore deleted: ${core.scriptDeletes}');
      print('file table after the restore: ${_tableText(core.table)}');
      print('reported: ${_fileReport(prune)}');

      expect(planRows, [
        'SCR_000',
      ], reason: 'the stale file is a delete row on the page');
      expect(prune.deleted.keys, ['SCR_000']);
      expect(prune.deletedText, 'SCR_000 (4 B, slot 0)');
      expect(core.scriptDeletes, ['SCR_000']);
      expect(core.table.containsKey('SCR_000'), isFalse);
      // Everything else is untouched: the archive's own scripts and a user file.
      expect(core.table.containsKey('SCR_001'), isTrue);
      expect(core.table.containsKey('SCR_002'), isTrue);
      expect(core.table['notes.txt'], 12);
      expect(prune.remaining, {'SCR_001', 'SCR_002'});
      expect(prune.confirmed, isTrue);
      expect(result.failed, 0, reason: result.failures.toString());
    },
  );

  test('the plan\'s delete rows are exactly the files the sweep deletes', () async {
    // The invariant `efe71c3` established for slots, carried to the file table: the diff view and
    // the apply read one predicate, so the page cannot print a delete the run keeps.
    //
    // [loaded] is the rig's OWN state (`t_b89ffb07`: bambi still runs akyirr-era slot 0): SCR_000
    // sits in slot 0 at plan time. The slot phase unloads that slot before the file sweep reads
    // the map, so the file IS deleted - and therefore has to be a row. Reading the plan-time map
    // instead (the defect this pins) printed no row for SCR_000 while the apply deleted it.
    final table = {'SCR_000': 4, 'SCR_005': 8, 'SCR_001': 0};
    final loaded = {0: 0, 1: 1};
    final device = _device([_script(1, 'Fan curve')]);
    install(_FakeCore(loaded: loaded, table: table));
    final plan = _plan(device, _live(), table, loaded);

    final planRows = _planFileRows(plan);
    final result = await applyRestorePlan(plan);
    final swept = result.scriptFiles.single;
    final slots = result.scriptSlots.single;

    print('slot phase unloaded: ${slots.unloaded}');
    print('delete rows: $planRows');
    print('delete commands: ${core.scriptDeletes}');
    expect(slots.unloaded, {
      0: 0,
    }, reason: 'the stale slot goes before the file is read');
    expect(planRows.toSet(), {'SCR_000', 'SCR_005'});
    expect(swept.deleted.keys.toSet(), planRows.toSet());
    expect(core.scriptDeletes.toSet(), planRows.toSet());
    expect(result.failed, 0, reason: result.failures.toString());
  });

  test('a stale file still loaded in its slot is planned and deleted', () async {
    // The state the plan used to miss: loaded={0: 0, 1: 1}, archive carrying slot 1 only. The
    // file is released by this run's own slot phase, so it is stale afterwards - the plan says so,
    // the sweep agrees, and the deletion is a row the user can turn off.
    final table = {'SCR_000': 4, 'SCR_001': 0};
    final loaded = {0: 0, 1: 1};
    final device = _device([_script(1, 'Fan curve')]);
    install(_FakeCore(loaded: loaded, table: table));
    final plan = _plan(device, _live(), table, loaded);

    final planRows = _planFileRows(plan);
    final result = await applyRestorePlan(plan);
    final prune = result.scriptFiles.single;

    print('loaded-stale plan FILE rows: $planRows');
    print('loaded-stale -> ${_fileReport(prune)}');
    expect(planRows, ['SCR_000']);
    expect(prune.deleted.keys, ['SCR_000']);
    expect(prune.keptLoaded, isEmpty, reason: 'its slot was unloaded first');
    expect(core.scriptDeletes, ['SCR_000']);
    expect(core.table.containsKey('SCR_000'), isFalse);
    expect(core.table.containsKey('SCR_001'), isTrue);
    expect(result.failed, 0, reason: result.failures.toString());
  });

  test('an archive carrying no scripts plans every stored script file', () async {
    // `needed` and `uncovered` are both empty, so this run IS the whole of the target's scripts
    // (there are none): every stored SCR_XXX is stale, including the one still in a loaded slot
    // that the slot phase unloads. The plan must list both, or the diff view under-reports.
    final table = {'SCR_000': 4, 'SCR_001': 0};
    final loaded = {0: 0};
    final device = _device([]);
    install(_FakeCore(loaded: loaded, table: table));
    final plan = _plan(device, _live(), table, loaded);

    final planRows = _planFileRows(plan);
    final result = await applyRestorePlan(plan);
    final prune = result.scriptFiles.single;

    print('no-script archive plan FILE rows: $planRows');
    print('no-script archive -> ${_fileReport(prune)}');
    expect(planRows.toSet(), {'SCR_000', 'SCR_001'});
    expect(prune.deleted.keys.toSet(), planRows.toSet());
    expect(core.table.keys.where((n) => scriptFileId(n) != null), isEmpty);
    expect(prune.remaining, isEmpty);
    expect(result.failed, 0, reason: result.failures.toString());
  });

  test('a slot row the user unticked keeps the file behind it', () async {
    // Unticking the slot DELETE is "keep this script running", so the file behind it is not stale
    // either: the slot has no selected row, the file is still held after the slot phase, and the
    // plan drops the file row with it (and the sweep keeps the file, naming it kept-loaded).
    final table = {'SCR_000': 4, 'SCR_001': 0};
    final loaded = {0: 0, 1: 1};
    final device = _device([_script(1, 'Fan curve')]);
    install(_FakeCore(loaded: loaded, table: table));
    final plan = _plan(device, _live(), table, loaded);
    plan.removals
            .firstWhere((r) => r.kind == RestoreKind.script && r.slot == 0)
            .selected =
        false;
    plan.classify();

    final planRows = _planFileRows(plan);
    final result = await applyRestorePlan(plan);
    final prune = result.scriptFiles.single;

    print('slot row unticked -> FILE rows: $planRows');
    print('slot row unticked -> ${_fileReport(prune)}');
    expect(planRows, isEmpty);
    expect(prune.deleted, isEmpty);
    expect(prune.keptLoaded, {'SCR_000': 4});
    expect(core.scriptDeletes, isEmpty);
    expect(core.table.containsKey('SCR_000'), isTrue);
    expect(result.failed, 0, reason: result.failures.toString());
  });

  test('a second identical restore deletes nothing', () async {
    final table = {'SCR_000': 4, 'SCR_001': 0};
    final loaded = {1: 1};
    final device = _device([_script(1, 'Fan curve')]);
    install(_FakeCore(loaded: loaded, table: table));

    final first = await applyRestorePlan(_plan(device, _live(), table, loaded));
    expect(first.scriptFiles.single.deleted.keys, ['SCR_000']);
    expect(first.failed, 0, reason: first.failures.toString());

    core.sent.clear();
    final second = await applyRestorePlan(
      _plan(device, _live(), core.table, core.loaded),
    );

    final prune = second.scriptFiles.single;
    print('second restore -> ${_fileReport(prune)}');
    print('second restore delete commands: ${core.scriptDeletes}');
    expect(prune.deleted, isEmpty, reason: 'nothing stale is left to delete');
    expect(core.scriptDeletes, isEmpty);
    expect(prune.remaining, {'SCR_001'});
    expect(second.failed, 0, reason: second.failures.toString());
  });

  test('a partially selected script keeps its file, and is named', () async {
    // The user unticks script 2: its file is "not selected", not "not in the archive", so the
    // sweep must leave it - otherwise unticking a script would delete it from the device.
    final table = {'SCR_000': 4, 'SCR_001': 0, 'SCR_002': 0};
    final loaded = {1: 1, 2: 2};
    final device = _device([_script(1, 'Fan curve'), _script(2, 'Selector')]);
    install(_FakeCore(loaded: loaded, table: table));
    final plan = _plan(device, _live(), table, loaded);
    plan.items.firstWhere((i) => i.script!.slot == 2).selected = false;
    plan.classify();

    final planRows = _planFileRows(plan);
    final result = await applyRestorePlan(plan);

    final prune = result.scriptFiles.single;
    print('untick script 2 -> delete rows: $planRows');
    print('untick script 2 -> ${_fileReport(prune)}');
    expect(planRows, [
      'SCR_000',
    ], reason: 'the unticked script is not a delete row');
    expect(prune.deleted.keys, ['SCR_000']);
    expect(prune.keptBySelection, {'SCR_002': 0});
    expect(core.table.containsKey('SCR_002'), isTrue);
    expect(core.scriptDeletes, ['SCR_000']);
    expect(result.failed, 0, reason: result.failures.toString());
  });

  test('a file a restored script calls into is never deleted', () async {
    // Slot 1 calls a global function living in SCR_007, which this archive does not carry (a
    // partial archive). Deleting SCR_007 would break the script this run just installed.
    final table = {'SCR_000': 4, 'SCR_001': 0, 'SCR_007': 24};
    final loaded = {1: 1};
    final device = _device([_libraryCaller(1, 'Beacon', 7)]);
    install(_FakeCore(loaded: loaded, table: table));
    final plan = _plan(device, _live(), table, loaded);

    final planRows = _planFileRows(plan);
    final result = await applyRestorePlan(plan);

    final prune = result.scriptFiles.single;
    print('library link -> delete rows: $planRows');
    print('library link -> ${_fileReport(prune)}');
    expect(planRows, [
      'SCR_000',
    ], reason: 'SCR_007 backs a library the script calls');
    expect(prune.deleted.keys, ['SCR_000']);
    expect(core.table.containsKey('SCR_007'), isTrue);
    expect(prune.remaining, {'SCR_001', 'SCR_007'});
    expect(result.failed, 0, reason: result.failures.toString());
  });

  test('a file a slot still holds is kept, so a running script is not broken', () async {
    // The slot sweep could not unload slot 0 (the device refused), so SCR_000 is still RUNNING:
    // deleting the file under it would be worse than leaving a stale script stored.
    final table = {'SCR_000': 4, 'SCR_001': 0};
    final loaded = {0: 0, 1: 1};
    final device = _device([_script(1, 'Fan curve')]);
    final fake = _FakeCore(loaded: loaded, table: table);
    fake.refuseUnload.add(0);
    install(fake);
    final plan = _plan(device, _live(), table, loaded);

    final result = await applyRestorePlan(plan);

    final prune = result.scriptFiles.single;
    print('stuck slot -> plan FILE rows: ${_planFileRows(plan)}');
    print('stuck slot -> ${_fileReport(prune)}');
    // The plan DID list the file, and that is deliberate: at plan time this device is
    // indistinguishable from a cooperative one (same table, same slot map), so a rule that
    // printed no row here would also print none in the state the sweep does delete - the
    // under-report this whole change exists to remove. The refusal is a device-time fact, so the
    // sweep's live read keeps the file and names it kept-loaded: the plan over-reports a delete
    // the run did not make, never the reverse. The row also gives the user the control the
    // missing row used to take away - untick it and the file is kept by row instead.
    expect(_planFileRows(plan), ['SCR_000']);
    expect(core.table.containsKey('SCR_000'), isTrue);
    expect(prune.deleted, isEmpty);
    expect(prune.keptLoaded, {'SCR_000': 4});
    expect(core.scriptDeletes, isEmpty);
  });

  test(
    'a delete the device refuses is reported as a failure, not swallowed',
    () async {
      final table = {'SCR_000': 4, 'SCR_001': 0};
      final loaded = {1: 1};
      final device = _device([_script(1, 'Fan curve')]);
      final fake = _FakeCore(loaded: loaded, table: table);
      fake.refuseDelete.add('SCR_000');
      install(fake);
      final plan = _plan(device, _live(), table, loaded);

      final result = await applyRestorePlan(plan);

      final prune = result.scriptFiles.single;
      print('refused delete -> ${_fileReport(prune)}');
      expect(prune.deleted, isEmpty);
      expect(prune.refused, {'SCR_000': 4});
      expect(prune.refusedText, 'SCR_000 (4 B, slot 0)');
      expect(core.table.containsKey('SCR_000'), isTrue);
      expect(result.failed, greaterThanOrEqualTo(1));
      expect(
        result.failures.any(
          (f) =>
              f.contains('SCR_000') &&
              f.contains('deleting failed') &&
              f.contains('still stored'),
        ),
        isTrue,
        reason: result.failures.toString(),
      );
    },
  );

  test('a DELETE row the plan leaves unticked keeps the file', () async {
    // The page shows the stale file as a DELETE and the user turns the row off: the sweep keeps
    // it and names it - an unticked row is "not selected", not a row that vanished.
    final table = {'SCR_000': 4, 'SCR_001': 0};
    final loaded = {1: 1};
    final device = _device([_script(1, 'Fan curve')]);
    install(_FakeCore(loaded: loaded, table: table));
    final plan = _plan(device, _live(), table, loaded);
    final row = plan.removals.firstWhere((r) => r.kind == RestoreKind.file);
    row.selected = false;

    final result = await applyRestorePlan(plan);

    final prune = result.scriptFiles.single;
    print('unticked DELETE -> ${_fileReport(prune)}');
    expect(prune.deleted, isEmpty, reason: 'nothing was deleted');
    expect(prune.keptByRow, {'SCR_000': 4});
    expect(core.scriptDeletes, isEmpty);
    expect(core.table.containsKey('SCR_000'), isTrue);
    expect(result.failed, 0, reason: result.failures.toString());
  });

  test(
    'a device whose file table does not answer is not reported as clean',
    () async {
      // "Could not look" is not "holds nothing": the sweep declines and says why.
      final loaded = {1: 1};
      final device = _device([_script(1, 'Fan curve')]);
      final fake = _FakeCore(loaded: loaded)..tableUnreadable = true;
      install(fake);
      final plan = _plan(device, _live(), {'SCR_000': 4, 'SCR_001': 0}, loaded);

      final result = await applyRestorePlan(plan);

      final prune = result.scriptFiles.single;
      print('unreadable table -> ${_fileReport(prune)}');
      expect(prune.skipped, isTrue);
      expect(prune.confirmed, isFalse);
      expect(prune.deleted, isEmpty);
      expect(prune.remainingText, 'unconfirmed');
      expect(
        result.failures.any((f) => f.contains('script files read')),
        isTrue,
        reason: result.failures.toString(),
      );
    },
  );
}
