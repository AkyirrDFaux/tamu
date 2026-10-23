import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/backup_script.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/types.dart';

/// A core that answers the service requests a restore sends, with a FILE TABLE the sweep can
/// read and delete from - the thing the slot sweep's fake did not have, and the reason `SCR_000`
/// survived a restore: the slot was unloaded, the file stayed.
class _FakeCore implements RequestChannel {
  _FakeCore({
    Map<int, int> loaded = const {},
    Map<String, int> table = const {},
  }) : loaded = {...loaded},
       table = {...table};

  /// loaded slot -> SCR file id; the load/unload commands mutate it.
  final Map<int, int> loaded;

  /// The device's file table: name -> recorded size. A create adds a name, a delete removes it,
  /// a rename moves it, so a test reads the table the device would report afterwards.
  final Map<String, int> table;

  /// The bytes of each stored file (what a read returns).
  final Map<String, List<int>> files = {};

  /// Names the device refuses to delete (it answers FAIL), as a stuck record would.
  final refuseDelete = <String>{};

  /// Slots the device refuses to unload.
  final refuseUnload = <int>{};

  /// Answer the file-table read with nothing, as a device that cannot say what it stores.
  bool tableUnreadable = false;

  /// Every request, in the order it was sent.
  final sent = <({ServiceType service, int cid, List<int> payload})>[];

  static const slots = 16;

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

  /// The `.TABLE` reply for the table the device holds: `.TABLE` first, then one 16-byte record
  /// per file, chunked the way the Storage service fragments a read.
  List<int> _tableReply() {
    final names = table.keys.toList()..sort();
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
      record(name, table[name]!);
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
      table[name] = files[name]!.length;
      return const [0, 0];
    }
    if (service == ServiceType.storage && functionCid == 5) {
      final name = StorageClient.unpadName(payload.sublist(0, 8));
      if (name == '.TABLE') {
        if (tableUnreadable) throw StateError('no reply');
        return _tableReply();
      }
      final content = files[name] ?? List<int>.filled(table[name] ?? 0, 0);
      final out = <int>[];
      for (var off = 0; off < content.length; off += 64) {
        final end = off + 64 > content.length ? content.length : off + 64;
        out
          ..addAll(StorageClient.padName(name))
          ..addAll(content.sublist(off, end));
      }
      return out;
    }
    throw StateError('$service/$functionCid not answered');
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
          if (refuseUnload.contains(payload[0])) return _failed;
          loaded.remove(payload[0]);
      }
    }
    if (service == ServiceType.storage) {
      switch (functionCid) {
        case 1: // create: name (8), size (u32)
          final name = StorageClient.unpadName(payload.sublist(0, 8));
          final size = uint32FromBytes(payload, 8);
          table[name] = size;
          files[name] = [];
        case 2: // delete: name (8)
          final name = StorageClient.unpadName(payload.sublist(0, 8));
          if (refuseDelete.contains(name)) return _failed;
          table.remove(name);
          files.remove(name);
        case 4: // rename: old (8), new (8)
          final from = StorageClient.unpadName(payload.sublist(0, 8));
          final to = StorageClient.unpadName(payload.sublist(8, 16));
          final size = table.remove(from);
          final bytes = files.remove(from);
          if (size != null) table[to] = size;
          if (bytes != null) files[to] = bytes;
      }
    }
    return _ok;
  }

  /// The `SCR_XXX` files the restore asked the device to DELETE, in order. The staging name a
  /// write uses (`~SCR_00`) is not one of them.
  List<String> get scriptDeletes => [
    for (final c in sent)
      if (c.service == ServiceType.storage && c.cid == 2)
        if (scriptFileId(StorageClient.unpadName(c.payload.sublist(0, 8))) !=
            null)
          StorageClient.unpadName(c.payload.sublist(0, 8)),
  ];
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
    final table = {'SCR_000': 4, 'SCR_005': 8, 'SCR_001': 0};
    final loaded = {1: 1};
    final device = _device([_script(1, 'Fan curve')]);
    install(_FakeCore(loaded: loaded, table: table));
    final plan = _plan(device, _live(), table, loaded);

    final planRows = _planFileRows(plan);
    final result = await applyRestorePlan(plan);
    final swept = result.scriptFiles.single;

    print('delete rows: $planRows');
    print('delete commands: ${core.scriptDeletes}');
    expect(planRows.toSet(), {'SCR_000', 'SCR_005'});
    expect(swept.deleted.keys.toSet(), planRows.toSet());
    expect(core.scriptDeletes.toSet(), planRows.toSet());
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

  test(
    'a file a slot still holds is kept, so a running script is not broken',
    () async {
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
      print('stuck slot -> ${_fileReport(prune)}');
      expect(core.table.containsKey('SCR_000'), isTrue);
      expect(prune.deleted, isEmpty);
      expect(prune.keptLoaded, {'SCR_000': 4});
      expect(core.scriptDeletes, isEmpty);
    },
  );

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
              f.contains('deleting script file SCR_000') &&
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
        result.failures.any((f) => f.contains('script files not swept')),
        isTrue,
        reason: result.failures.toString(),
      );
    },
  );
}
