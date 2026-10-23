import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/backup_script.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/script_client.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/types.dart';

/// A core that answers the service requests a restore sends, so `applyRestorePlan` can be driven
/// without a rig. It models only what the script bring-up touches: the slot map, the load/unload
/// commands, the storage file writes and the save.
class _FakeCore implements RequestChannel {
  _FakeCore({Map<int, int> loaded = const {}}) : loaded = {...loaded};

  /// loaded slot -> SCR file id. The load/unload commands mutate it, so a test can read the
  /// device state the restore left behind.
  final Map<int, int> loaded;

  /// The slot count the core reports (a v2 core's script table, generously padded).
  static const slots = 16;

  /// Every request, in the order it was sent.
  final sent = <({ServiceType service, int cid, List<int> payload})>[];

  /// File contents staged by the storage writes, keyed by the name the rename landed them under.
  final files = <String, List<int>>{};

  int _txId = 0x100;

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
    if (service == ServiceType.script && functionCid == 0) return _slotMap();
    if (service == ServiceType.storage && functionCid == 6) {
      // A write fragment: frag info (4), the file name (8), then its slice of the content.
      final name = StorageClient.unpadName(payload.sublist(4, 12));
      files.putIfAbsent(name, () => []).addAll(payload.sublist(12));
      return const [0, 0]; // the sequence the device accepted
    }
    if (service == ServiceType.storage && functionCid == 5) {
      // The file sweep reads the file table; a device that does not answer it looks like one
      // nobody can tell a stale script file from.
      if (StorageClient.unpadName(payload.sublist(0, 8)) == '.TABLE') {
        return _tableReply();
      }
      throw StateError('no such file');
    }
    return const [];
  }

  /// The `.TABLE` reply for the files this device holds (`.TABLE` first, then one record per
  /// file, chunked with the per-fragment 8-byte name echo).
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
          final slot = payload.length > 2 ? payload[2] : -1;
          if (slot >= 0) loaded[slot] = fileId;
        case 2: // unload: slot (u8)
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
    return const PacketResponse(payload: [], success: true, fail: false);
  }

  /// The script commands the restore sent (`cid` followed by its payload), in order.
  List<List<int>> get scriptCommands => [
    for (final c in sent)
      if (c.service == ServiceType.script && c.cid != 0) [c.cid, ...c.payload],
  ];

  /// The slots the restore asked to LOAD, with the file id it named for each.
  List<int> get loads => [
    for (final c in scriptCommands)
      if (c.first == 1) c[3],
  ];
}

BackupDevice _device() => BackupDevice(
  type: 'Tamu v2.0A',
  typeId: 1,
  id: idFromString('39.1')!,
  name: 'Tamu',
  blocks: const [],
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
  late _FakeCore core;

  setUp(() {
    previous = requestChannel;
    core = _FakeCore();
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
