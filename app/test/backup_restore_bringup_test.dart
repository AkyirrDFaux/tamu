import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/backup_script.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/script_client.dart';

import 'support/restore_core.dart';
import 'support/restore_script_fixtures.dart';
import 'package:tamuapp/core/types.dart';

/// The device model: `RestoreCore` at its shallowest, which is what this suite's restore touches -
/// the slot map, the load/unload commands, the write fragments and the rename. The deeper reads
/// (a per-slot state record, the file table) are off here on purpose: this suite pins that a
/// function library is written as a file and never loaded, and a device that answered more would
/// move the commands under the assertions.

BackupDevice _device() => scriptArchive(id: idFromString('39.1')!);

LiveDevice _live({int id = 0x2711}) => scriptTarget(id: id);

BackupScript _script(
  int slot,
  String name, {
  List<String> properties = const [],
}) => bareScript(slot, name, properties: properties);

RestorePlan _plan(List<RestoreItem> items, LiveDevice live) {
  final device = _device();
  final plan = RestorePlan(
    devices: [device],
    liveDevices: [live],
    items: items,
    deviceTargets: {device.id: live},
    blockTargets: const {},
  );
  plan.resolve();
  return plan;
}

RestoreItem _item(BackupDevice device, BackupScript script) => RestoreItem(
  kind: RestoreKind.script,
  device: device,
  script: script,
  selected: true,
);

void main() {
  late RequestChannel? previous;
  late RestoreCore core;

  setUp(() {
    previous = requestChannel;
    core = RestoreCore();
    installRequestChannel(core);
  });

  tearDown(() => installRequestChannel(previous));

  test('a restore never loads a function library into a slot', () async {
    final device = _device();
    final live = _live();
    // The rig as the bug left it: slots 0-4 hold the user scripts and the library sits LOADED in
    // slot 10 - the state every earlier restore produced.
    core.loaded.addAll({0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 10: 10});
    final plan = _plan([
      _item(device, _script(2, 'Selector', properties: ['Load on boot'])),
      _item(
        device,
        _script(10, 'ForceBlink', properties: ['Function library']),
      ),
    ], live);
    expect(plan.items.every((i) => i.ready), isTrue);

    final result = await applyRestorePlan(plan);

    // The library is written as a FILE ...
    expect(
      core.files.keys,
      containsAll(['SCR_002', 'SCR_00A']),
      reason: 'both scripts are written to the file table',
    );
    expect(core.files['SCR_00A'], isNotEmpty);
    // ... and never loaded.
    expect(
      core.loads,
      [2],
      reason:
          'only the user script is loaded; slot 10 is never named by a load',
    );
    // The slot the library was left in is released, so the run converges on "file, not slot".
    expect(
      core.loaded.keys,
      isNot(contains(10)),
      reason: 'no slot holds the library after the restore',
    );
    expect([
      for (final entry in core.loaded.entries)
        if (entry.value == 10) entry.key,
    ], isEmpty);
    // The library travels in the FILE table, and every load happens after the writes, so the link
    // reads the library as it now stands. What must NOT happen is a load naming the library's file
    // id: a library is never loaded (a slot it holds is released instead).
    expect(
      [
        for (final c in core.sent)
          if (c.service == ServiceType.script &&
              c.cid == 1 &&
              c.payload[0] == 10)
            c,
      ],
      isEmpty,
      reason: 'the library file id is never named by a load',
    );
    final libraryWrite = core.sent.indexWhere(
      (c) => c.service == ServiceType.storage && c.cid == 6,
    );
    final userLoad = core.sent.indexWhere(
      (c) => c.service == ServiceType.script && c.cid == 1 && c.payload[2] == 2,
    );
    expect(
      libraryWrite,
      lessThan(userLoad),
      reason: 'the files are written before any script is loaded',
    );
    expect(result.written, 2);
    expect(result.failed, 0, reason: result.failures.toString());
  });

  test('a library the device does not hold is written and never loaded', () async {
    final device = _device();
    final live = _live();
    core.loaded.addAll({0: 0, 1: 1, 2: 2, 3: 3, 4: 4});
    final plan = _plan([
      _item(
        device,
        _script(10, 'ForceBlink', properties: ['Function library']),
      ),
    ], live);

    final result = await applyRestorePlan(plan);

    expect(core.files.keys, contains('SCR_00A'));
    expect(
      [
        for (final c in core.scriptCommands)
          if (c.first == 1) c,
      ],
      isEmpty,
      reason: 'the library itself is never loaded',
    );
    // The slots an earlier setup left running are not in this archive, so the sweep releases
    // them: a library-only archive leaves the device with no script loaded at all.
    expect(core.loaded, isEmpty);
    expect(result.written, 1);
    expect(result.failed, 0, reason: result.failures.toString());
  });

  test('a library loaded in some other slot is released', () async {
    final device = _device();
    final live = _live();
    // Nothing says the file id and the slot it occupies match.
    core.loaded.addAll({0: 0, 2: 10});
    final plan = _plan([
      _item(
        device,
        _script(10, 'ForceBlink', properties: ['Function library']),
      ),
    ], live);

    await applyRestorePlan(plan);

    expect(core.scriptCommands, [
      [2, 2], // the library's slot is released ...
      [
        2,
        0,
      ], // ... and the sweep releases the akyirr-era slot this archive does not carry
    ]);
    expect(core.loaded, isEmpty);
  });

  test('a user script is still unloaded and reloaded so it re-links', () async {
    final device = _device();
    final live = _live();
    core.loaded.addAll({0: 0, 2: 2});
    final plan = _plan([
      _item(
        device,
        _script(2, 'Selector', properties: ['Load on boot', 'Run on load']),
      ),
    ], live);

    await applyRestorePlan(plan);

    expect(core.scriptCommands, [
      // The reload phase first reads what the slot is doing, so a running script is started again
      // after the reload; this core does not answer that read, and the archive's `Run on load` is
      // the fallback.
      [3, 2],
      [2, 2], // unload: its image changed, so the old one must not keep running
      [1, 2, 0, 2], // load file 2 into slot 2
      [4, 2, ScriptAction.start], // Run on load
      [2, 0], // the sweep releases the slot this archive does not carry
    ]);
  });
}
