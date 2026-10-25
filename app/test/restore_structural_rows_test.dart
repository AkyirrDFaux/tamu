// Card t_f55a1400: a dynamic block's zero-length rows are STRUCTURE, not writes.
//
// Measured on the rig (read-only probe, 2026-10-25) - `Dynamic #1 "Left Eye"` and
// `Dynamic #2 "Right Eye"`, nine fields each, every field's key 0:
//
//   f0 k0 type=Geometry dict size=0 flags=0x2 value=0B
//   f0 k1 type=Enum          size=1 flags=0x2 value=1B
//   f1 k0 type=Texture dict  size=0 flags=0x2 value=0B
//   ...
//
// Eighteen of those (9 fields x 2 eyes) are exactly the 18 `FAIL (FLAG_FAIL 0x40)` rows the owner
// report quoted. They can never land: the firmware refuses every write with no value bytes
// (`ValueUpdateApplicable`, `Core/Functions/MemoryTypes.h`), because storing one would set the
// row's Size to 0 and make the address dead for every later write. So the cases below pin the two
// halves of the fix - never send such a write, and report the row as skipped - and the other half
// of the same card: a row whose device-side type/size is NOT the archive's must be re-created, not
// skipped (`Dynamic[0]` field 4 key 0 holds `Number` at size 1 where a Number is 4 bytes, and the
// emote selector's read of it lands as SCRIPT_ERR_TYPE).

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/backup_value.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/types.dart';

/// A dynamic device over the same request surface the restore uses: it answers block meta, the
/// field/key table and per-row reads from [rows], and applies WRITES the way the firmware does.
///
/// The write rule is the one the whole card turns on: a value with no bytes is refused
/// (`ValueUpdateApplicable`), a rejected address answers `FLAG_FAIL` with no payload, and an
/// accepted write stores the row's new type and length - which is what re-creates a row that was
/// left at the wrong size.
class _DynamicDevice implements RequestChannel {
  /// `'global:field:key'` -> the row the device holds.
  final rows = <String, ({int type, List<int> value, int flags})>{};

  final names = <int, String>{};

  /// The addresses whose value write the device refuses, as `'global:field:key'`.
  final refuse = <String>{};

  /// Every request, in the order it was sent.
  final sent = <({ServiceType service, int cid, List<int> payload})>[];

  /// Writes that carried a value type but NO value bytes: the bug this card is about.
  int zeroLengthValueWrites = 0;

  /// Writes that carried no bytes ON PURPOSE (type None = delete).
  int deletions = 0;

  int _txId = 0x100;

  static const _failed = PacketResponse(
    payload: [],
    success: false,
    fail: true,
  );
  static const _ok = PacketResponse(payload: [], success: true, fail: false);

  static String _key(int global, int field, int key) => '$global:$field:$key';

  List<int> _fieldKeys(int global) {
    final words = <int>[];
    for (final row in rows.keys) {
      final parts = row.split(':').map(int.parse).toList();
      if (parts[0] != global) continue;
      words.add((parts[1] << 8) | parts[2]);
    }
    words.sort();
    return [
      for (final w in words) ...[w & 0xFF, (w >> 8) & 0xFF],
    ];
  }

  int _fieldCount(int global) =>
      rows.keys.where((k) => k.startsWith('$global:')).length;

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
  }) async => (await requestWithFlags(
    targetId,
    service,
    functionCid,
    payload: payload,
    timeout: timeout,
    requestFrag: requestFrag,
    transactionId: transactionId,
  )).payload;

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
    if (service == ServiceType.register) {
      // The Dynamic management commands are Register CIDs 0x10-0x13.
      if (functionCid == DynamicCid.getName) {
        final global = payload[0] | (payload[1] << 8);
        final name = names[global] ?? '';
        final padded = List<int>.filled(16, 0);
        for (var i = 0; i < name.length && i < 16; i++) {
          padded[i] = name.codeUnitAt(i);
        }
        return PacketResponse(payload: padded, success: true, fail: false);
      }
      switch (functionCid) {
        case RegisterCid.write:
          return _write(payload);
        case RegisterCid.enumerateFields:
          final packed = payload[0] | (payload[1] << 8);
          final type = packed >> 6;
          final inst = packed & 0x3F;
          final global = globalIndexFor(type, inst);
          return PacketResponse(
            payload: _fieldKeys(global),
            success: true,
            fail: false,
          );
        case RegisterCid.read:
          return _read(payload);
        case RegisterCid.saveAll:
        case RegisterCid.recallAll:
        case DynamicCid.create:
        case DynamicCid.delete:
        case DynamicCid.setName:
          return _ok;
      }
      return _ok;
    }
    if (service == ServiceType.script && functionCid == 0) {
      // No script slot occupied: the sweeps find nothing to unload.
      return PacketResponse(
        payload: List<int>.filled(16, 0xFF),
        success: true,
        fail: false,
      );
    }
    if (service == ServiceType.storage && functionCid == 5) {
      final name = StorageClient.unpadName(payload.sublist(0, 8));
      if (name == '.TABLE') {
        // An empty file table (its own record only).
        return PacketResponse(
          payload: [
            ...StorageClient.padName('.TABLE'),
            0x40,
            0,
            0,
            0,
            0x10,
            0,
            0,
            0,
            ...StorageClient.padName('.TABLE'),
          ],
          success: true,
          fail: false,
        );
      }
      return _failed;
    }
    return _ok;
  }

  PacketResponse _read(List<int> payload) {
    final bi =
        payload[0] |
        (payload[1] << 8) |
        (payload[2] << 16) |
        (payload[3] << 24);
    final type = blockInfoType(bi);
    final inst = blockInfoInstance(bi);
    final field = blockInfoField(bi);
    final key = blockInfoKey(bi);
    final global = globalIndexFor(type, inst);
    if (field == 0xFF) {
      // Block meta: the type and the row count, then the name.
      return PacketResponse(
        payload: [
          0,
          0,
          0,
          0,
          type & 0xFF,
          (type >> 8) & 0xFF,
          _fieldCount(global) & 0xFF,
          0,
        ],
        success: true,
        fail: false,
      );
    }
    final row = rows[_key(global, field, key)];
    if (row == null) return _failed;
    return PacketResponse(
      payload: [
        ...payload.sublist(0, 4),
        row.type & 0xFF,
        (row.type >> 8) & 0xFF,
        row.value.length & 0xFF,
        row.flags,
        ...row.value,
      ],
      success: true,
      fail: false,
    );
  }

  PacketResponse _write(List<int> payload) {
    final bi =
        payload[0] |
        (payload[1] << 8) |
        (payload[2] << 16) |
        (payload[3] << 24);
    final type = blockInfoType(bi);
    final field = blockInfoField(bi);
    final key = blockInfoKey(bi);
    final global = globalIndexFor(type, blockInfoInstance(bi));
    final meta = ValueInfo.fromBytes(payload, 4);
    final value = payload.sublist(8);
    final address = _key(global, field, key);
    if (meta.dataType == DataType.none) {
      // A delete: the documented zero-length request that means something. The device answers it
      // by echoing the request (`SendResponse(Frame, Frame.Payload, ...)`, not a bare status).
      deletions++;
      rows.remove(address);
      return PacketResponse(payload: payload, success: true, fail: false);
    }
    if (value.isEmpty) {
      // The firmware's gate: refuse, and answer nothing (`RespondStatus(Frame, false)`).
      zeroLengthValueWrites++;
      return _failed;
    }
    if (refuse.contains(address)) return _failed;
    // `SetEntry`: the stored length is the bytes present, so a row left at the wrong size is
    // re-created by the next write that carries one.
    rows[address] = (type: meta.type, value: value, flags: meta.flags);
    return PacketResponse(payload: payload, success: true, fail: false);
  }
}

/// The eye blocks as the rig holds them: nine fields, each with a zero-length key-0 dictionary
/// marker and its value keys beside it.
BackupBlock _eye(String name, int global) => BackupBlock(
  type: 'Dynamic',
  typeIndex: dynamicTypeForIndex(global),
  instance: global,
  name: name,
  isDynamic: true,
  entries: [
    for (var field = 0; field < 9; field++) ...[
      BackupEntry(
        field: 'Field $field',
        fieldIndex: field,
        key: 'Key 0',
        keyIndex: 0,
        type: field.isEven ? 'Geometry dict' : 'Texture dict',
        flags: const ['Persistent'],
        value: const <Object>[],
        size: 0,
      ),
      BackupEntry(
        field: 'Field $field',
        fieldIndex: field,
        key: 'Key 1',
        keyIndex: 1,
        type: 'Enum',
        flags: const ['Persistent'],
        value: 1,
        size: 1,
      ),
      if (field == 4)
        BackupEntry(
          field: 'Field $field',
          fieldIndex: field,
          key: 'Key 3',
          keyIndex: 3,
          type: 'Matrix',
          flags: const ['Persistent'],
          value: const {
            'rows': 2,
            'cols': 3,
            'values': [
              [1, 0, 0],
              [0, 1, 0],
            ],
          },
          size: 28,
        ),
    ],
  ],
);

BackupDevice _archive({List<BackupBlock>? blocks}) => BackupDevice(
  type: 'Tamu v2.0A',
  typeId: 1,
  id: 1,
  name: 'Tamu',
  blocks: blocks ?? [_eye('Left Eye', 1), _eye('Right Eye', 2)],
  serial: 'E4B063C8207000000000000000',
);

/// The identity 2x3 matrix the archive's Matrix row carries, in wire bytes: what a device that
/// already agrees with the archive holds.
final List<int> _identity23 = decodeSemantic(DataType.matrix, const {
  'rows': 2,
  'cols': 3,
  'values': [
    [1, 0, 0],
    [0, 1, 0],
  ],
})!;

/// The same blocks as the device holds them. [matrix] is the 28 bytes of the one Matrix row the
/// archive also carries, so a case can make it identical (nothing to write) or different (one
/// value write beside the eighteen markers).
LiveBlock _liveEye(String name, int global, {List<int>? matrix}) {
  final eye = _eye(name, global);
  return LiveBlock(
    type: eye.typeIndex,
    instance: global,
    name: name,
    isDynamic: true,
    entries: [
      for (final entry in eye.entries)
        LiveEntry(
          field: entry.fieldIndex,
          key: entry.keyIndex,
          fieldName: entry.field,
          keyName: entry.key,
          meta: ValueInfo(
            type: dataTypeFromWord(entry.type)!.value,
            size: entry.keyIndex == 0 ? 0 : entry.size,
            flags: ValueFlags.persistent,
          ),
          value: entry.keyIndex == 0
              ? const <int>[]
              : entry.size == 28
              ? (matrix ?? _identity23)
              : const [1],
        ),
    ],
  );
}

LiveDevice _live({List<int>? matrix, List<LiveBlock>? blocks}) => LiveDevice(
  id: 1,
  name: 'Tamu',
  serial: 'E4B063C8207000000000000000',
  capabilities: Capability.core,
  // Only the LEFT eye takes [matrix]: the right eye always agrees with the archive, so a case can
  // pin exactly one write beside the eighteen markers.
  blocks:
      blocks ??
      [_liveEye('Left Eye', 1, matrix: matrix), _liveEye('Right Eye', 2)],
);

RestorePlan _plan({
  BackupDevice? archive,
  LiveDevice? live,
  List<LiveBlock>? liveBlocks,
}) {
  final source = archive ?? _archive();
  final target = live ?? _live(blocks: liveBlocks);
  return planFromSnapshot(
    devices: [source],
    live: [target],
    inventories: {
      target.id: const LiveInventory(
        fileNames: {},
        loadedSlots: {},
        requesters: [],
        sndb: [],
      ),
    },
  );
}

/// Installs [device] for the duration of the test.
void _install(_DynamicDevice device) {
  installRequestChannel(device);
  addTearDown(() => installRequestChannel(null));
}

void main() {
  tearDown(RestoreSession.clear);

  test('eighteen zero-length marker rows are skipped, and not one of them is sent', () async {
    final device = _DynamicDevice();
    // The device holds both eyes exactly as the rig does, with ONE value row that differs - so
    // the run still has a write to make next to the markers a restore must not touch.
    final live = _live(matrix: List<int>.filled(28, 0));
    for (final eye in [1, 2]) {
      for (final row in _eye(
        eye == 1 ? 'Left Eye' : 'Right Eye',
        eye,
      ).entries) {
        if (row.keyIndex == 0) {
          // The marker: a zero-length row, present and persistent, as measured on the rig.
          device.rows['$eye:${row.fieldIndex}:0'] = (
            type: dataTypeFromWord(row.type)!.value,
            value: const <int>[],
            flags: ValueFlags.persistent,
          );
        } else {
          device.rows['$eye:${row.fieldIndex}:${row.keyIndex}'] = (
            type: dataTypeFromWord(row.type)!.value,
            value: live.blocks
                .firstWhere((b) => b.instance == eye)
                .entries
                .firstWhere(
                  (e) => e.field == row.fieldIndex && e.key == row.keyIndex,
                )
                .value,
            flags: ValueFlags.persistent,
          );
        }
      }
    }
    _install(device);

    final report = await applyRestorePlan(_plan(live: live));

    // The whole card: zero failures, the eighteen rows named as structural skips.
    expect(report.failed, 0, reason: report.text);
    expect(report.reasons, isEmpty);
    expect(report.skipped, hasLength(18));
    // 20 value rows are planned (10 per eye: 9 Enum rows and the one Matrix) and every one of
    // them is written; the 18 markers are the only rows that are not.
    expect(report.counts, '20 written · 18 skipped (structural) · 0 failed');
    expect(
      report.skips.where((line) => line.contains('Left Eye')),
      hasLength(9),
    );
    expect(
      report.skips.where((line) => line.contains('Right Eye')),
      hasLength(9),
    );
    // Every skip names the marker it is, not a code: nothing was sent, so there is no code.
    for (final skip in report.skips) {
      expect(skip, contains('[Key 0]'));
      expect(skip, contains('dictionary marker'));
      expect(skip, isNot(contains('FAIL')));
    }
    expect(report.text, contains('Skipped as structural (18)'));

    // NOTHING with an empty value ever reached the bus, and the value rows beside the markers
    // did - including the 28-byte Matrix, at its declared size.
    expect(device.zeroLengthValueWrites, 0);
    final writes = device.sent
        .where(
          (c) =>
              c.service == ServiceType.register && c.cid == RegisterCid.write,
        )
        .toList();
    expect(writes, hasLength(20));
    for (final write in writes) {
      expect(write.payload.sublist(8), isNotEmpty);
      expect(
        ValueInfo.fromBytes(write.payload, 4).size,
        write.payload.length - 8,
      );
    }
    expect(
      writes.where((w) => w.payload.length - 8 == 28),
      hasLength(2),
      reason: 'the two Matrix rows (one per eye)',
    );

    // The diagnostic line the owner can paste: counts first, then the rows.
    // ignore: avoid_print
    print('--- report ---\n${report.text}\n--- end ---');
  });

  test('a genuine refusal still fails with its code, and is not a skip', () async {
    final device = _DynamicDevice();
    final live = _live(matrix: List<int>.filled(28, 0));
    for (final eye in [1, 2]) {
      for (final row in _eye(
        eye == 1 ? 'Left Eye' : 'Right Eye',
        eye,
      ).entries) {
        device.rows['$eye:${row.fieldIndex}:${row.keyIndex}'] = (
          type: dataTypeFromWord(row.type)!.value,
          value: row.keyIndex == 0
              ? const <int>[]
              : row.size == 28
              ? List<int>.filled(28, 0)
              : const [0],
          flags: ValueFlags.persistent,
        );
      }
    }
    // The device refuses the one value write - FLAG_FAIL, no payload, as this firmware answers.
    device.refuse.add('1:4:3');
    _install(device);

    final report = await applyRestorePlan(_plan(live: live));

    expect(report.failed, 1);
    final failure = report.reasons.single;
    expect(failure.code, startsWith('FAIL (FLAG_FAIL 0x40'));
    expect(failure.subject, contains('Left Eye'));
    expect(failure.kindWord, 'block field');
    // The eighteen markers are untouched by the refusal: they were never writes.
    expect(report.skipped, hasLength(18));
    expect(report.counts, '19 written · 18 skipped (structural) · 1 failed');
  });

  test(
    'a row left at the wrong length is re-created with the declared type and size',
    () async {
      // `Dynamic[0]` field 4 key 0 on the rig: a Number stored at size 1 where a Number is 4, and
      // the emote selector's read of it lands as SCRIPT_ERR_TYPE.
      final archive = _archive(
        blocks: [
          BackupBlock(
            type: 'Dynamic',
            typeIndex: dynamicTypeBase,
            instance: 0,
            name: 'Subscriptions',
            isDynamic: true,
            entries: [
              BackupEntry(
                field: 'Field 4',
                fieldIndex: 4,
                key: 'Key 0',
                keyIndex: 0,
                type: 'Number',
                flags: const ['Persistent'],
                value: 0.25,
                size: 4,
              ),
            ],
          ),
        ],
      );
      final liveBlock = LiveBlock(
        type: dynamicTypeBase,
        instance: 0,
        name: 'Subscriptions',
        isDynamic: true,
        entries: [
          LiveEntry(
            field: 4,
            key: 0,
            fieldName: 'Field 4',
            keyName: 'Key 0',
            meta: ValueInfo(
              type: DataType.number.value,
              size: 1,
              flags: ValueFlags.persistent,
            ),
            value: const [0x01],
          ),
        ],
      );
      final plan = _plan(
        archive: archive,
        live: _live(blocks: [liveBlock]),
      );

      // The PLAN sees it as a replace, and computes the archive's four bytes, not the row's one.
      final item = plan.items.single;
      expect(item.diff, DiffKind.change);
      expect(item.diffNote, contains('1 byte(s)'));
      expect(item.diffNote, contains('4'));
      expect(item.resolvedBytes, hasLength(4));

      // The STRUCTURE phase re-creates it: a write carrying the declared type and length.
      final device = _DynamicDevice();
      device.rows['0:4:0'] = (
        type: DataType.number.value,
        value: const [0x01],
        flags: ValueFlags.persistent,
      );
      _install(device);
      await prepareDynamicBlocks(plan);

      final writes = device.sent
          .where(
            (c) =>
                c.service == ServiceType.register && c.cid == RegisterCid.write,
          )
          .toList();
      expect(writes, hasLength(1));
      final meta = ValueInfo.fromBytes(writes.single.payload, 4);
      expect(meta.type, DataType.number.value);
      expect(meta.size, 4);
      expect(writes.single.payload.sublist(8), hasLength(4));
      expect(device.zeroLengthValueWrites, 0);
      // And the device now holds a Number of four bytes: the address resolves again.
      expect(device.rows['0:4:0']!.value, hasLength(4));
    },
  );

  test(
    'the client refuses a write with no value bytes, and still deletes',
    () async {
      final device = _DynamicDevice();
      _install(device);
      final client = RegisterClient(deviceId: 1);
      final block = DynBlock(
        index: 1,
        meta: ValueInfo(type: dynamicTypeForIndex(1)),
        name: 'Left Eye',
      );

      final refused = await client.writeDynamicEntry(
        block,
        0,
        0,
        ValueInfo(type: DataType.geometry.value, flags: ValueFlags.persistent),
        const [],
      );

      expect(refused, isNull);
      expect(device.sent, isEmpty, reason: 'nothing was sent');
      expect(device.zeroLengthValueWrites, 0);
      final fault = client.trace.fault!;
      expect(fault.kind, RequestFaultKind.notSent);
      expect(fault.detail, contains('no value bytes'));

      // Deleting is the one zero-length request that means something: type None.
      client.trace.clear();
      final deleted = await client.writeDynamicEntry(
        block,
        0,
        0,
        ValueInfo(type: DataType.none.value),
        const [],
      );
      expect(deleted, isNotNull);
      expect(device.deletions, 1);
      expect(device.zeroLengthValueWrites, 0);
    },
  );

  test(
    'reordering a block that holds markers is refused, not silently destructive',
    () async {
      final device = _DynamicDevice();
      device.names[1] = 'Left Eye';
      device.rows['1:0:0'] = (
        type: DataType.geometry.value,
        value: const <int>[],
        flags: ValueFlags.persistent,
      );
      device.rows['1:0:1'] = (
        type: DataType.enum_.value,
        value: const [1],
        flags: ValueFlags.persistent,
      );
      _install(device);

      final client = RegisterClient(deviceId: 1);
      final ok = await client.reorderDynamicBlocks([1]);

      expect(ok, isFalse);
      // Nothing was tombstoned or recreated: a rebuild would have dropped the marker for good.
      expect(
        device.sent.where(
          (c) =>
              c.service == ServiceType.register &&
              (c.cid == DynamicCid.delete || c.cid == DynamicCid.create),
        ),
        isEmpty,
      );
      expect(
        device.sent.where(
          (c) =>
              c.service == ServiceType.register && c.cid == RegisterCid.write,
        ),
        isEmpty,
      );
      expect(client.trace.fault!.kind, RequestFaultKind.notSent);
      expect(client.trace.fault!.detail, contains('dictionary markers'));
    },
  );
}
