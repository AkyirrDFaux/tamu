import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/backup_script.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/script_client.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/types.dart';

/// A core that answers the service requests a restore sends, so `applyRestorePlan` can be driven
/// without a rig - the same shape as the bring-up test's fake, extended with the things the script
/// slots phase has to survive and measure: the stored file contents (what the device HELD before a
/// write, which is how "changed" is decided), the report record (`state`), an unload or a load the
/// device refuses, and a slot map the device will not return.
class _FakeCore implements RequestChannel {
  _FakeCore({Map<int, int> loaded = const {}}) : loaded = {...loaded};

  /// loaded slot -> SCR file id. The load/unload commands mutate it, so a test can read the
  /// device state the restore left behind.
  final Map<int, int> loaded;

  /// Slots the device refuses to unload (it answers FAIL), as a stuck script would.
  final refuseUnload = <int>{};

  /// Script file ids the device refuses to load, as a damaged file would.
  final refuseLoad = <int>{};

  /// The state each loaded slot reports (Docs CID 3). Absent means `Running`, which is what a rig
  /// left by an earlier setup reports.
  final states = <int, int>{};

  /// Never answer the slot-map read (CID 0), as a timeout does.
  bool slotMapTimesOut = false;

  /// Never answer the report record (CID 3), as a device that cannot say what it runs.
  bool statesUnreadable = false;

  /// The slot count the core reports (a v2 core's script table, generously padded).
  static const slots = 16;

  /// Every request, in the order it was sent.
  final sent = <({ServiceType service, int cid, List<int> payload})>[];

  /// File contents on the device, keyed by the name the writes landed them under.
  final files = <String, List<int>>{};

  int _txId = 0x100;

  static const _ok = PacketResponse(payload: [], success: true, fail: false);
  static const _failed = PacketResponse(
    payload: [],
    success: false,
    fail: true,
  );

  List<int> _slotMap() {
    final map = List<int>.filled(slots, 0xFFFF);
    loaded.forEach((slot, fileId) {
      if (slot < slots) map[slot] = fileId;
    });
    return [
      for (final id in map) ...[id & 0xFF, (id >> 8) & 0xFF],
    ];
  }

  @override
  bool get isConnected => true;

  @override
  int takeTxId() => _txId++;

  @override
  void setSubscriptionListener(Function(List<int>)? listener) {}

  @override
  Future<List<int>> request(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
    bool requestFrag = false,
    int? transactionId,
  }) async {
    sent.add((service: service, cid: functionCid, payload: payload));
    if (service == ServiceType.script && functionCid == 0) {
      if (slotMapTimesOut) throw StateError('no reply');
      return _slotMap();
    }
    if (service == ServiceType.script && functionCid == 3) {
      if (statesUnreadable) throw StateError('no reply');
      final slot = payload[0];
      final state = states[slot] ?? ScriptState.running;
      return [state, 0, 0, 0]; // state, depth, line (u16)
    }
    if (service == ServiceType.storage && functionCid == 6) {
      // A write fragment: frag info (4), the file name (8), then its slice of the content.
      final name = StorageClient.unpadName(payload.sublist(4, 12));
      files.putIfAbsent(name, () => []).addAll(payload.sublist(12));
      return const [0, 0]; // the sequence the device accepted
    }
    if (service == ServiceType.storage && functionCid == 5) {
      // A file read: answer it the way the device does - per-fragment name echoes - or FAIL it
      // when no such file exists, which is what "the device would not return it" looks like.
      final name = StorageClient.unpadName(payload.sublist(0, 8));
      if (name == '.TABLE') return _tableReply();
      final content = files[name];
      if (content == null) throw StateError('no such file');
      final out = <int>[];
      for (var off = 0; off < content.length; off += 64) {
        final end = off + 64 > content.length ? content.length : off + 64;
        out
          ..addAll(StorageClient.padName(name))
          ..addAll(content.sublist(off, end));
      }
      return out;
    }
    return const [];
  }

  /// The `.TABLE` reply a device gives for the files it holds: `.TABLE` itself first (record 0),
  /// then one 16-byte record per file, chunked the way the Storage service fragments a read (an
  /// 8-byte name echo per 64 content bytes). The file sweep reads this table, so a fake that does
  /// not answer it looks like a device that would not say what it stores.
  List<int> _tableReply() {
    final names = files.keys.toList()..sort();
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
      record(name, files[name]!.length);
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

  @override
  Future<PacketResponse> requestWithFlags(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
    bool requestFrag = false,
    int? transactionId,
  }) async {
    sent.add((service: service, cid: functionCid, payload: payload));
    if (service == ServiceType.script) {
      switch (functionCid) {
        case 1: // load: file id (u16), slot (u8)
          final fileId = payload[0] | (payload[1] << 8);
          if (refuseLoad.contains(fileId)) return _failed;
          final slot = payload.length > 2 ? payload[2] : -1;
          if (slot >= 0) loaded[slot] = fileId;
        case 2: // unload: slot (u8)
          if (refuseUnload.contains(payload[0])) return _failed;
          loaded.remove(payload[0]);
      }
    }
    if (service == ServiceType.storage && functionCid == 4) {
      // A rename lands the staged content under the new name.
      final from = StorageClient.unpadName(payload.sublist(0, 8));
      final to = StorageClient.unpadName(payload.sublist(8, 16));
      final staged = files.remove(from);
      if (staged != null) files[to] = staged;
    }
    return _ok;
  }

  /// The script commands the restore sent (`cid` followed by its payload), in order.
  List<List<int>> get scriptCommands => [
    for (final c in sent)
      if (c.service == ServiceType.script && c.cid != 0) [c.cid, ...c.payload],
  ];

  /// The slots the restore asked to UNLOAD, in order.
  List<int> get unloads => [
    for (final c in scriptCommands)
      if (c.first == 2) c[1],
  ];

  /// The slots the restore asked to LOAD, in order.
  List<int> get loads => [
    for (final c in scriptCommands)
      if (c.first == 1) c[3],
  ];

  /// The `(slot, action)` pairs the restore asked for, in order (CID 4).
  List<(int, int)> get actions => [
    for (final c in scriptCommands)
      if (c.first == 4) (c[1], c[2]),
  ];
}

BackupDevice _device(
  List<BackupScript> scripts, {
  List<BackupFile> files = const [],
}) => BackupDevice(
  type: 'Tamu v2.0A',
  typeId: 1,
  id: idFromString('39.1')!,
  name: 'Tamu',
  blocks: const [],
  scripts: scripts,
  files: files,
);

LiveDevice _live({int id = 0x2711}) => LiveDevice(
  id: id,
  name: 'Tamu',
  capabilities: Capability.scripts,
  blocks: const [],
);

BackupScript _script(
  int slot,
  String name, {
  List<String> properties = const [],
}) => BackupScript(
  slot: slot,
  functionName: name,
  properties: properties,
  inputs: const [],
  outputs: const [],
  variables: const [],
  constants: const [],
  lines: const [],
);

/// The image a script item writes, so a test can seed the device with the SAME bytes (unchanged) or
/// different ones (changed).
List<int> _image(BackupScript script) => script.toDraft().toImage();

/// A script that CALLS one global function living in [library], so the image it writes carries a
/// function-reservation record naming that library - the measurement a library change has to act on
/// (and that a script which merely belongs to the same run does not have).
BackupScript _libraryCaller(
  int slot,
  String name,
  int library, {
  int memoryOffset = 0x20,
  int instructionOffset = 0x40,
}) => BackupScript(
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
      memoryOffset: memoryOffset,
      instructionOffset: instructionOffset,
    ),
  ],
);

RestorePlan _plan(
  BackupDevice device,
  LiveDevice live, {
  List<RestoreItem> extra = const [],
}) {
  final plan = RestorePlan(
    devices: [device],
    liveDevices: [live],
    items: [
      for (final script in device.scripts)
        RestoreItem(
          kind: RestoreKind.script,
          device: device,
          script: script,
          selected: true,
        ),
      ...extra,
    ],
    deviceTargets: {device.id: live},
    blockTargets: const {},
  );
  plan.resolve();
  for (final item in plan.items) {
    item.selected = item.ready;
  }
  return plan;
}

/// The rig as bambi's restore found it: slots 0-4 and 10 loaded from an earlier akyirr-era setup.
Map<int, int> _akyirrSlots() => {0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 10: 10};

/// bambi's archive: scripts 1, 2, 3, 4 and 10. No slot 0 - the `Temperature regulation` script
/// the old setup left running is not in it.
List<BackupScript> _bambiScripts() => [
  _script(1, 'Fan curve', properties: ['Load on boot']),
  _script(2, 'Selector', properties: ['Load on boot']),
  _script(3, 'LDR read'),
  _script(4, 'Beacon'),
  _script(10, 'ForceBlink', properties: ['Function library']),
];

String _reportLine(ScriptSlotPrune p) =>
    'SCRIPTS ${idToString(p.deviceId)} "${p.deviceName}": '
    'before=[${p.beforeText}] unloaded=[${p.unloadedText}] '
    'duplicates=[${p.duplicatesText}] off-slot=[${p.offSlotText}] '
    'refused=[${p.refusedText}] kept-by-selection=[${p.keptText}] '
    'remaining=[${p.remainingText}]'
    '${p.skipped ? ' SKIPPED: ${p.reason}' : ''}';

String _reloadLine(ScriptReloadReport r) =>
    'RELOAD ${idToString(r.deviceId)} "${r.deviceName}": '
    'changed=[${r.changedText}] reloaded=[${r.reloadedText}] '
    'left-alone=[${r.leftAloneText}] refused=[${r.refusedText}]'
    '${r.skipped ? ' SKIPPED: ${r.reason}' : ''}';

String _slotsText(Map<int, int> slots) =>
    slots.isEmpty ? '-' : ScriptSlotPrune.slotText(slots);

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

  test('a restore unloads the script slots the archive does not carry', () async {
    final slotsBefore = _akyirrSlots();
    install(_FakeCore(loaded: slotsBefore));
    final device = _device(_bambiScripts());
    final plan = _plan(device, _live());
    expect(plan.items.every((i) => i.ready), isTrue);

    final result = await applyRestorePlan(plan);

    final prune = result.scriptSlots.single;
    final reload = result.scriptReload.single;
    // Printed on purpose: the card asks for the transcript of this case, and a transcript a
    // reviewer can re-run is worth more than one pasted into a handoff.
    print('slots before the restore: ${_slotsText(slotsBefore)}');
    print('unload commands the restore sent: ${core.unloads}');
    print('load commands the restore sent: ${core.loads}');
    print('slots after the restore: ${_slotsText(core.loaded)}');
    print('reported: ${_reportLine(prune)}');
    print('reported: ${_reloadLine(reload)}');

    // Slot 0 held the akyirr-era `Temperature regulation` the archive does not carry: it is
    // unloaded and NAMED, not dropped silently.
    // The sweep reads AFTER the reload phase and after the bring-up: the library slot has been
    // released by then, so what it reports as `before` is what the device held at that point.
    expect(prune.before, {0: 0, 1: 1, 2: 2, 3: 3, 4: 4});
    expect(prune.unloaded, {0: 0});
    expect(prune.keptBySelection, isEmpty);
    expect(prune.refused, isEmpty);
    expect(prune.confirmed, isTrue);
    expect(prune.remaining, {1: 1, 2: 2, 3: 3, 4: 4});
    expect(prune.unloadedText, 'slot 0 (SCR_000)');
    expect(core.loaded, {1: 1, 2: 2, 3: 3, 4: 4});
    // The archive's own scripts are reloaded because their images changed, and the library still
    // never loads.
    expect(reload.reloaded.keys, [1, 2, 3, 4]);
    expect(reload.reloaded[1]!.why, contains('its image changed'));
    expect(reload.leftAlone, isEmpty);
    expect(core.loads, [1, 2, 3, 4]);
    expect(core.actions, [
      (1, ScriptAction.start),
      (2, ScriptAction.start),
      (3, ScriptAction.start),
      (4, ScriptAction.start),
    ], reason: 'a reload starts a script that was running again');
    // A function library is written as a file and never loaded.
    expect(core.loads, isNot(contains(10)));
    expect(result.failed, 0, reason: result.failures.toString());
  });

  test('a target that will not return its slot map is reported unswept', () async {
    // "Could not look" is not "holds nothing": the sweep must not report a clean device it never
    // read, or the very leftovers this exists to remove stay loaded and invisible.
    final fake = _FakeCore(loaded: _akyirrSlots());
    fake.slotMapTimesOut = true;
    install(fake);
    final device = _device(_bambiScripts());
    final plan = _plan(device, _live());

    final result = await applyRestorePlan(plan);

    final prune = result.scriptSlots.single;
    final reload = result.scriptReload.single;
    expect(prune.skipped, isTrue);
    expect(prune.confirmed, isFalse);
    expect(prune.unloaded, isEmpty);
    expect(prune.remainingText, 'unconfirmed');
    // Both phases rest on the same read, so both refuse to claim anything about it.
    expect(reload.skipped, isTrue);
    expect(reload.reloaded, isEmpty);
    expect(reload.leftAlone, isEmpty);
    // ...and so does the file sweep behind the slots: a file a slot still holds cannot be told
    // from a stale one, so nothing is deleted on a guess.
    final files = result.scriptFiles.single;
    expect(files.skipped, isTrue);
    expect(files.confirmed, isFalse);
    expect(files.deleted, isEmpty);
    expect(result.failed, 3);
    expect(result.failures.any((f) => f.contains('script slots read')), isTrue);
    expect(result.failures.any((f) => f.contains('script slots read')), isTrue);
    expect(result.failures.any((f) => f.contains('script files read')), isTrue);
    expect(fake.unloads, isEmpty);
  });

  test(
    'a script left unticked keeps its slot and is reported as skipped',
    () async {
      install(_FakeCore(loaded: {0: 0, 1: 1, 2: 2}));
      final device = _device([_script(1, 'Fan curve'), _script(2, 'Selector')]);
      final plan = _plan(device, _live());
      // The user unticks the script in slot 2: its slot is "not selected", not "not in the archive".
      plan.items.firstWhere((i) => i.script!.slot == 2).selected = false;

      final result = await applyRestorePlan(plan);

      final prune = result.scriptSlots.single;
      final reload = result.scriptReload.single;
      print('unticks slot 2 -> ${_reportLine(prune)}');
      print('unticks slot 2 -> ${_reloadLine(reload)}');
      expect(prune.unloaded, {0: 0}, reason: 'only the stale slot goes');
      expect(
        prune.keptBySelection,
        {2: 2},
        reason: 'the unticked script is named in the report, not unloaded',
      );
      expect(prune.remaining, {1: 1, 2: 2});
      expect(core.unloads, [
        1,
        0,
      ], reason: 'slot 2 is never named by an unload');
      expect(core.loads, [
        1,
      ], reason: 'the unticked script is not loaded either');
      // The reload phase does not touch what the user left out either: it is not in the run.
      expect(reload.reloaded.keys, [1], reason: 'slot 2 is not reloaded');
      expect(reload.leftAlone.containsKey(2), isFalse);
      expect(result.failed, 0, reason: result.failures.toString());
    },
  );

  test('only the script whose image changed is reloaded', () async {
    final scripts = [
      _script(1, 'Fan curve'),
      _script(2, 'Selector'),
      _script(3, 'LDR read'),
    ];
    final fake = _FakeCore(loaded: {1: 1, 2: 2, 3: 3});
    // The device holds slot 1 and 3 exactly as the archive has them, and slot 2 with other bytes:
    // only slot 2 changed, so only slot 2 is disturbed.
    fake.files['SCR_001'] = _image(scripts[0]);
    fake.files['SCR_002'] = [..._image(scripts[1]), 0xAA];
    fake.files['SCR_003'] = _image(scripts[2]);
    install(fake);
    final device = _device(scripts);
    final plan = _plan(device, _live());

    final result = await applyRestorePlan(plan);

    final reload = result.scriptReload.single;
    print('one changed script -> ${_reloadLine(reload)}');
    expect(fake.unloads, [2], reason: 'only the changed script is reloaded');
    expect(fake.loads, [2]);
    expect(reload.changed.keys, [2]);
    expect(reload.reloaded.keys, [2]);
    expect(
      reload.leftAlone,
      {1: 1, 3: 3},
      reason: 'unchanged loaded scripts are left running, and said so',
    );
    expect(result.failed, 0, reason: result.failures.toString());
  });

  test(
    'a second identical restore issues no unload and no reload at all',
    () async {
      final device = _device(_bambiScripts());
      // The state bambi's rig is in once the first restore has run: the archive's slots loaded, no
      // leftover, and the files holding exactly what the archive carries.
      install(_FakeCore(loaded: {1: 1, 2: 2, 3: 3, 4: 4}));
      final first = await applyRestorePlan(_plan(device, _live()));
      expect(first.failed, 0, reason: first.failures.toString());
      expect(core.loads, [1, 2, 3, 4]);

      core.sent.clear();
      final second = await applyRestorePlan(_plan(device, _live()));

      final prune = second.scriptSlots.single;
      final reload = second.scriptReload.single;
      print('second restore -> ${_reportLine(prune)}');
      print('second restore -> ${_reloadLine(reload)}');
      print('script commands the second restore sent: ${core.scriptCommands}');
      expect(
        core.unloads,
        isEmpty,
        reason: 'nothing is stale, and nothing changed, so nothing is unloaded',
      );
      expect(core.loads, isEmpty, reason: 'nothing is reloaded');
      expect(core.actions, isEmpty, reason: 'nothing is restarted');
      expect(prune.unloaded, isEmpty);
      expect(reload.reloaded, isEmpty);
      expect(reload.changed, isEmpty, reason: 'the images are byte-identical');
      expect(reload.leftAlone, {
        1: 1,
        2: 2,
        3: 3,
        4: 4,
      }, reason: 'every running script is reported as left alone');
      expect(second.failed, 0, reason: second.failures.toString());
    },
  );

  test('a raw script file rewritten under a loaded slot is reloaded', () async {
    // A script whose image did not parse travels as bytes; the slot holding it still runs the old
    // bytes, so the raw write has to reload it exactly like a semantic one.
    final fake = _FakeCore(loaded: {5: 5});
    fake.files['SCR_005'] = const [1, 2, 3];
    install(fake);
    final raw = BackupFile(
      name: 'SCR_005',
      kind: 'Script',
      data: base64Encode(const [9, 8, 7, 6]),
    );
    final device = _device(const [], files: [raw]);
    final plan = _plan(
      device,
      _live(),
      extra: [
        RestoreItem(
          kind: RestoreKind.file,
          device: device,
          file: raw,
          selected: true,
        ),
      ],
    );

    final result = await applyRestorePlan(plan);

    final reload = result.scriptReload.single;
    print('raw file rewrite -> ${_reloadLine(reload)}');
    expect(fake.unloads, [
      5,
    ], reason: 'the slot holding the old bytes is reloaded');
    expect(fake.loads, [5]);
    expect(reload.changed.keys, [5]);
    expect(reload.reloaded.keys, [5]);
    expect(reload.reloaded[5]!.why, contains('its image changed'));
    expect(reload.leftAlone, isEmpty);
    expect(result.failed, 0, reason: result.failures.toString());
  });

  test(
    'a reload the device refuses is reported as a failure, not swallowed',
    () async {
      final scripts = [_script(1, 'Fan curve')];
      final fake = _FakeCore(loaded: {1: 1});
      fake.files['SCR_001'] = const [
        0,
        0,
      ]; // different from the archive's image
      fake.refuseUnload.add(1);
      install(fake);
      final device = _device(scripts);
      final plan = _plan(device, _live());

      final result = await applyRestorePlan(plan);

      final reload = result.scriptReload.single;
      print('refused reload -> ${_reloadLine(reload)}');
      expect(reload.refused, {1: 1});
      expect(reload.reloaded, isEmpty);
      expect(reload.refusedText, 'slot 1 (SCR_001)');
      expect(result.failed, 1);
      expect(result.failures.single, contains('1 (SCR_001)'));
      expect(result.failures.single, contains('still runs the image it held'));
      // The script it could not reload is not then unloaded by the sweep: the archive carries it.
      expect(fake.loaded, {1: 1});
    },
  );

  test(
    'a reload that fails reports the code of the UNLOAD, not of the state read first',
    () async {
      // Two failures in a row on one client: the report record (CID 3) the reload reads first is
      // never answered, and the unload that follows is refused. The trace keeps the FIRST fault
      // until it is cleared, so without a clear before the unload the report would hand the reader
      // "no reply" for a write the device actually refused - the same class of defect as a bare
      // count, since it names the wrong cause.
      final scripts = [_script(1, 'Fan curve')];
      final fake = _FakeCore(loaded: {1: 1});
      fake.files['SCR_001'] = const [
        0,
        0,
      ]; // not the archive's image, so it is reloaded
      fake.statesUnreadable = true;
      fake.refuseUnload.add(1);
      install(fake);

      final result = await applyRestorePlan(_plan(_device(scripts), _live()));

      final failure = result.reasons.firstWhere(
        (r) => r.kindWord == 'script slot',
      );
      print('refused reload after an unreadable state -> ${failure.line}');
      expect(
        failure.code,
        startsWith('FAIL (FLAG_FAIL 0x40'),
        reason:
            'the code belongs to the unload, not to the state read before it',
      );
      expect(failure.code, isNot(contains('no reply')));
      expect(failure.meaning, contains('refused the unload'));
    },
  );

  test(
    'an unload the device refuses is reported as a failure, not swallowed',
    () async {
      final fake = _FakeCore(loaded: {0: 0, 1: 1, 2: 2});
      fake.refuseUnload.add(0);
      install(fake);
      final device = _device([_script(1, 'Fan curve'), _script(2, 'Selector')]);
      final plan = _plan(device, _live());

      final result = await applyRestorePlan(plan);

      final prune = result.scriptSlots.single;
      expect(prune.unloaded, isEmpty);
      expect(prune.refused, {0: 0});
      expect(prune.unloadedText, '-');
      expect(prune.refusedText, 'slot 0 (SCR_000)');
      expect(prune.remaining, {0: 0, 1: 1, 2: 2});
      expect(result.failed, 1);
      expect(result.failures.single, contains('0 (SCR_000)'));
      expect(result.failures.single, contains('still loaded'));
    },
  );

  test(
    'an unchanged script in Error is reloaded, and one that is paused is not',
    () async {
      final scripts = [_script(1, 'Fan curve'), _script(2, 'Selector')];
      final fake = _FakeCore(loaded: {1: 1, 2: 2});
      // Both files are exactly what the archive carries: nothing changed, so nothing but a fault
      // justifies disturbing the slot.
      fake.files['SCR_001'] = _image(scripts[0]);
      fake.files['SCR_002'] = _image(scripts[1]);
      fake.states
        ..[1] = ScriptState.error
        ..[2] = ScriptState.paused;
      install(fake);
      final device = _device(scripts);
      final plan = _plan(device, _live());

      final result = await applyRestorePlan(plan);

      final reload = result.scriptReload.single;
      print('error + paused slots -> ${_reloadLine(reload)}');
      expect(reload.changed, isEmpty);
      expect(reload.reloaded.keys, [
        1,
      ], reason: 'the faulted script is started again');
      expect(reload.reloaded[1]!.why, contains('Error'));
      expect(reload.leftAlone, {2: 2}, reason: 'the paused one is left alone');
      expect(fake.loads, [1]);
      expect(
        fake.actions,
        [(1, ScriptAction.start)],
        reason:
            'a faulted script is started again, which is what clears the error',
      );
      expect(result.failed, 0, reason: result.failures.toString());
    },
  );

  test('a changed library does not disturb a script that links nothing', () async {
    // The library changed, but neither user script carries a function-reservation record, so
    // neither can reference it: reloading them would be a disturbance nothing justifies - and the
    // report must not claim a link it never measured.
    final scripts = [
      _script(1, 'Fan curve'),
      _script(2, 'Selector'),
      _script(10, 'ForceBlink', properties: ['Function library']),
    ];
    final fake = _FakeCore(loaded: {1: 1, 2: 2});
    fake.files['SCR_001'] = _image(scripts[0]);
    fake.files['SCR_002'] = _image(scripts[1]);
    fake.files['SCR_00A'] = const [
      1,
      2,
      3,
    ]; // the library differs, so it changed
    install(fake);
    final device = _device(scripts);
    final plan = _plan(device, _live());

    final result = await applyRestorePlan(plan);

    final reload = result.scriptReload.single;
    print('library only -> ${_reloadLine(reload)}');
    expect(reload.changed.keys, [10], reason: 'only the library changed');
    expect(
      reload.reloaded,
      isEmpty,
      reason: 'no loaded script references the library, so none is restarted',
    );
    expect(
      reload.leftAlone,
      {1: 1, 2: 2},
      reason: 'both running scripts are reported as not disturbed',
    );
    expect(fake.unloads, isEmpty);
    expect(fake.loads, isEmpty);
    expect(result.failed, 0, reason: result.failures.toString());
  });

  test(
    'a changed library reloads exactly the script whose file references it',
    () async {
      // Slot 1 calls a global function in the library; slot 2 does not. Both images are byte-identical
      // to what the device holds, so the ONLY thing that can justify a reload is the measured link.
      final caller = _libraryCaller(1, 'Beacon', 10);
      final plain = _script(2, 'Selector');
      final library = _script(
        10,
        'ForceBlink',
        properties: ['Function library'],
      );
      final fake = _FakeCore(loaded: {1: 1, 2: 2});
      fake.files['SCR_001'] = _image(caller);
      fake.files['SCR_002'] = _image(plain);
      fake.files['SCR_00A'] = const [1, 2, 3];
      install(fake);
      final device = _device([caller, plain, library]);
      final plan = _plan(device, _live());

      final result = await applyRestorePlan(plan);

      final reload = result.scriptReload.single;
      print('linked library -> ${_reloadLine(reload)}');
      expect(reload.changed.keys, [10]);
      expect(reload.reloaded.keys, [
        1,
      ], reason: 'only the script that references it');
      expect(reload.reloaded[1]!.why, contains('SCR_00A'));
      expect(
        reload.reloaded[1]!.why,
        contains('function reservation'),
        reason: 'the reason quotes the record it was decided from',
      );
      expect(reload.leftAlone, {
        2: 2,
      }, reason: 'the script that links nothing is not disturbed');
      expect(fake.unloads, [1]);
      expect(fake.loads, [1]);
      expect(result.failed, 0, reason: result.failures.toString());
    },
  );

  test('a second copy of an archive script is unloaded and named', () async {
    // `scripts_page.dart` loads into the lowest FREE slot, so SCR_004 can be running in slot 0
    // while the archive names slot 4. Loading slot 4 as well would run the same script twice.
    final scripts = [_script(4, 'Beacon')];
    final fake = _FakeCore(loaded: {0: 4});
    fake.files['SCR_004'] = _image(scripts[0]); // identical: nothing to reload
    install(fake);
    final device = _device(scripts);
    final plan = _plan(device, _live());

    final result = await applyRestorePlan(plan);

    final prune = result.scriptSlots.single;
    final reload = result.scriptReload.single;
    print('duplicate copy -> unloads=${fake.unloads} loads=${fake.loads}');
    print('duplicate copy -> ${_reportLine(prune)}');
    print('duplicate copy -> ${_reloadLine(reload)}');
    expect(fake.loads, [
      4,
    ], reason: 'the archive names slot 4, so the bring-up fills it');
    expect(fake.loaded, {
      4: 4,
    }, reason: 'exactly one slot holds SCR_004 after the restore');
    expect(prune.duplicates, {(0): (fileId: 4, canonical: 4)});
    expect(prune.duplicatesText, 'slot 0 (SCR_004, also loaded at slot 4)');
    expect(prune.unloaded, {
      0: 4,
    }, reason: 'the extra copy is an unload like any other');
    expect(prune.remaining, {4: 4});
    expect(prune.offSlot, isEmpty);
    // The copy in the wrong slot is the sweep's to settle, not the reload phase's to restart.
    expect(reload.reloaded, isEmpty);
    expect(reload.leftAlone, isEmpty);
    expect(result.failed, 0, reason: result.failures.toString());
  });

  test(
    'an archive script only loaded in the wrong slot is kept, and said so',
    () async {
      // The archive names slot 4 and the load into it failed, so slot 0 is the ONLY running copy.
      // Unloading it would delete the script; passing over it silently would report a clean device.
      final scripts = [_script(4, 'Beacon')];
      final fake = _FakeCore(loaded: {0: 4});
      fake.files['SCR_004'] = _image(scripts[0]);
      fake.refuseLoad.add(4);
      install(fake);
      final device = _device(scripts);
      final plan = _plan(device, _live());

      final result = await applyRestorePlan(plan);

      final prune = result.scriptSlots.single;
      print('off-slot copy -> ${_reportLine(prune)}');
      expect(fake.loaded, {0: 4}, reason: 'the surviving copy is not deleted');
      expect(prune.duplicates, isEmpty);
      expect(prune.offSlot, {0: 4});
      expect(
        prune.offSlotText,
        'slot 0 (SCR_004, the archive names slot 4)',
        reason:
            'the report names the slot the archive wanted and the one that still runs it',
      );
      expect(prune.unloaded, isEmpty);
      expect(
        result.failed,
        1,
        reason: 'the refused load is the failure, not the kept copy',
      );
      expect(result.failures.single, contains('script 4 would not load'));
    },
  );

  test('a DELETE row the plan leaves unticked keeps the loaded slot', () async {
    // The restore plan page shows the leftover slot as a DELETE and the user turns the row off.
    // The sweep then keeps it and names it - an unticked row is "not selected", not a row that
    // quietly vanished from the page the moment the restore started.
    install(_FakeCore(loaded: _akyirrSlots()));
    final device = _device(_bambiScripts());
    final plan = _plan(device, _live());
    final target = plan.deviceTargets[device.id]!;
    // Slot 0 holds SCR_000, which the archive does not carry: the deletion the page shows.
    plan.removals.add(
      RestoreRemoval(
        kind: RestoreKind.script,
        device: device,
        target: target,
        slot: 0,
        fileId: 0,
        selected: false,
      ),
    );

    final result = await applyRestorePlan(plan);

    final prune = result.scriptSlots.single;
    print('unticked DELETE -> ${_reportLine(prune)}');
    expect(prune.unloaded, isEmpty, reason: 'nothing was deleted');
    expect(prune.refused, isEmpty);
    expect(prune.keptBySelection, {
      0: 0,
    }, reason: 'the slot is named as kept by selection');
    expect(core.loaded.containsKey(0), isTrue);
    expect(core.unloads, isNot(contains(0)));
    expect(
      result.failed,
      0,
      reason:
          'keeping a slot the user kept is not a failure: '
          '${result.failures}',
    );
  });

  test('two sources on one target: the plan deletes what the sweep deletes', () async {
    // Slots belong to the NODE, so two archive devices remapped onto one target (the page's
    // per-device dropdown allows it) share one slot table. Decided per SOURCE, each archive read
    // the other archive's script as a leftover, and the page printed two DELETE rows for scripts
    // the run keeps - the destructive claim this viewer exists to make truthfully. The archive's
    // slots are unioned per target, as `pruneScriptSlots` unions them.
    BackupDevice source(String address, List<BackupScript> scripts) =>
        BackupDevice(
          type: 'Tamu v2.0A',
          typeId: 1,
          id: idFromString(address)!,
          name: 'Tamu $address',
          blocks: const [],
          scripts: scripts,
          files: const [],
        );
    final a = source('39.1', [_script(1, 'Fan curve')]);
    final b = source('39.2', [_script(2, 'Selector')]);
    final live = _live();

    // The plan for both archives onto [live], classified against a device that runs [loaded] -
    // the same snapshot the fake core answers the sweep with.
    RestorePlan planFor(Map<int, int> loaded) {
      final plan = RestorePlan(
        devices: [a, b],
        liveDevices: [live],
        items: [
          for (final device in [a, b])
            for (final script in device.scripts)
              RestoreItem(
                kind: RestoreKind.script,
                device: device,
                script: script,
              ),
        ],
        deviceTargets: {a.id: live, b.id: live},
        blockTargets: const {},
        inventories: {live.id: LiveInventory(loadedSlots: loaded)},
      );
      plan.resolve();
      return plan;
    }

    // The plan's deletion rows as the page lists them.
    String planDeletes(RestorePlan plan) => plan.removals.isEmpty
        ? '-'
        : [
            for (final r in plan.removals)
              '${r.device.name}: slot ${r.slot} (${scriptFileName(r.fileId!)})',
          ].join(', ');

    // Both archives' scripts are loaded where their archive names them: nothing to delete.
    install(_FakeCore(loaded: {1: 1, 2: 2}));
    var plan = planFor({1: 1, 2: 2});
    var swept = await pruneScriptSlots(plan);
    print('two sources, nothing stale -> plan deletes: ${planDeletes(plan)}');
    print(
      'two sources, nothing stale -> sweep: ${_reportLine(swept.prunes.single)}',
    );
    expect(
      plan.removals,
      isEmpty,
      reason: 'each archive names the other archive\'s script too',
    );
    expect(swept.prunes.single.unloaded, isEmpty);
    expect(swept.failures, isEmpty);
    expect(core.unloads, isEmpty);

    // ...and one stale slot is deleted by both, so the agreement is not vacuous.
    install(_FakeCore(loaded: {1: 1, 2: 2, 9: 9}));
    plan = planFor({1: 1, 2: 2, 9: 9});
    swept = await pruneScriptSlots(plan);
    print('two sources, a stale slot -> plan deletes: ${planDeletes(plan)}');
    print(
      'two sources, a stale slot -> sweep: ${_reportLine(swept.prunes.single)}',
    );
    expect({for (final r in plan.removals) r.slot}, {9});
    expect(swept.prunes.single.unloaded, {9: 9});
    expect(swept.failures, isEmpty);
    expect(core.unloads, [9]);
  });
}
