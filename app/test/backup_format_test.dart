import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/backup_value.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/types.dart';

/// Format 2's STRUCTURAL row and the two places it has to work.
///
/// Akyirr's rule (2026-10-22): "The structure of the dynamic block should stay in the archive, but
/// the non-persistent values shouldn't (they're reset to default anyway on reboot). The issue is
/// mostly with the static blocks, which often contain non-persistent fields (such as
/// measurements)."
///
///  * a dynamic block's table (`.DT_<hex2>`) is persistent device state, so its non-persistent rows
///    are archived as structural rows - identity, type, flags and size, with NO value - and a
///    restore rebuilds the structure from them;
///  * a static block's layout is compile-time, so its non-persistent fields are simply not captured.

BackupEntry _row({
  required String type,
  required int fieldIndex,
  int keyIndex = 0,
  List<String> flags = const [],
  Object? value,
  int size = 4,
  bool structural = false,
  String? field,
  String? key,
}) => BackupEntry(
  field: field ?? 'Field $fieldIndex',
  fieldIndex: fieldIndex,
  key: key ?? 'Key $keyIndex',
  keyIndex: keyIndex,
  type: type,
  flags: flags,
  value: value,
  size: size,
  structural: structural,
);

/// A dynamic 'Subscriptions' block: one persistent row (field 4) and four volatile rows.
BackupBlock _subscriptionsBlock() => BackupBlock(
  type: 'Dynamic',
  typeIndex: dynamicTypeBase,
  instance: 0,
  name: 'Subscriptions',
  isDynamic: true,
  entries: [
    for (var field = 0; field < 4; field++)
      _row(type: 'Number', fieldIndex: field, structural: true),
    _row(
      type: 'Number',
      fieldIndex: 4,
      flags: const ['Persistent'],
      value: 42.0,
    ),
  ],
);

const _source = BackupBlockRef(
  block: 'Resistive measurement',
  typeIndex: 8,
  instance: 0,
  field: 'Measured Value',
  fieldIndex: 3,
  key: 'Key 0',
  keyIndex: 0,
);

/// The requester's target is a volatile row of the dynamic block: on a wiped target neither the
/// block's table nor the entry exists, so the address can only come from the archive.
const _target = BackupBlockRef(
  block: 'Dynamic',
  typeIndex: dynamicTypeBase,
  instance: 0,
  field: 'Field 0',
  fieldIndex: 0,
  key: 'Key 0',
  keyIndex: 0,
);

const _sub = BackupRequesterSubscription(
  provider: '0.2',
  trigger: 'Periodic',
  periodMs: 1000,
  minTimeMs: 100,
  deadzone: 0.5,
  source: _source,
  target: _target,
);

BackupDevice _archive({bool withStructuralRows = true}) => BackupDevice(
  type: 'Tamu v2.0A',
  typeId: 1,
  id: 1,
  name: 'Tamu',
  blocks: [
    BackupBlock(
      type: 'Dynamic',
      typeIndex: dynamicTypeBase,
      instance: 0,
      name: 'Subscriptions',
      isDynamic: true,
      entries: withStructuralRows
          ? _subscriptionsBlock().entries
          : [_subscriptionsBlock().entries.last],
    ),
  ],
  requesterSubscriptions: const [_sub],
);

/// The provider the requester reads from: an ordinary static block that has the source entry.
LiveDevice _provider() => LiveDevice(
  id: idFromString('0.2')!,
  name: 'DAS',
  blocks: [
    LiveBlock(
      type: 8,
      instance: 0,
      name: 'Resistive measurement',
      isDynamic: false,
      entries: [
        LiveEntry(
          field: 3,
          key: 0,
          fieldName: 'Measured Value',
          keyName: 'Key 0',
          meta: ValueInfo(type: DataType.number.value, size: 4),
        ),
      ],
    ),
  ],
);

/// A device that runs the firmware but carries no dynamic block at all: a wiped (reflashed) core.
/// Its STATIC blocks exist because their layout is compile-time.
LiveDevice _wiped() => LiveDevice(
  id: 1,
  name: 'Tamu',
  capabilities: Capability.core | Capability.subscriptionRequest,
  blocks: const [],
);

RestorePlan _plan(BackupDevice archive, LiveDevice target) {
  final plan = RestorePlan(
    devices: [archive],
    liveDevices: [target, _provider()],
    items: [
      RestoreItem(
        kind: RestoreKind.subscription,
        device: archive,
        subscription: _sub,
      ),
    ],
    deviceTargets: {archive.id: target},
    blockTargets: const {},
  );
  plan.resolve();
  return plan;
}

/// A Register service that accepts dynamic-block creates and entry writes, and refuses everything
/// else (so the live re-read after a create comes back empty, which is fine for the assertions).
class _FakeRegisterChannel implements RequestChannel {
  final sent = <({ServiceType service, int cid, List<int> payload})>[];

  @override
  bool get isConnected => true;

  int _txId = 0x100;

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
    // A Register write replies with the address, the accepted ValueInfo and the value bytes.
    if (service == ServiceType.register && functionCid == RegisterCid.write) {
      return payload;
    }
    return const [];
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
    // A Register write echoes the address, the accepted ValueInfo and the value: the reply is the
    // request's own payload, and the flag says the device accepted it.
    if (service == ServiceType.register && functionCid == RegisterCid.write) {
      return PacketResponse(payload: payload, success: true, fail: false);
    }
    return const PacketResponse(payload: [], success: true, fail: false);
  }
}

void main() {
  test('a structural row round-trips through fromJson/toJson with no value', () {
    final zip = buildBackupZip([
      BackupDevice(
        type: 'Tamu v2.0A',
        typeId: 1,
        id: 1,
        name: 'Tamu',
        blocks: [_subscriptionsBlock()],
      ),
    ]);
    final parsed = parseBackupZip(zip).single;
    final row = parsed.blocks.single.entries.first;

    expect(row.structural, isTrue);
    expect(row.value, isNull);
    expect(row.persistent, isFalse);
    final json = row.toJson();
    expect(
      json,
      {
        'field': 'Field 0',
        'fieldIndex': 0,
        'key': 'Key 0',
        'keyIndex': 0,
        'type': 'Number',
        'size': 4,
      },
      reason: 'a structural row is identity + type + flags + size, and nothing else',
    );
    expect(json.containsKey('value'), isFalse);
    // Re-encoding an already-decoded row is stable, so a capture/restore cycle cannot drift.
    expect(BackupEntry.fromJson(json).toJson(), json);

    // The persistent rows of the SAME block keep their values exactly as before.
    final persistent = parsed.blocks.single.entries.last;
    expect(persistent.structural, isFalse);
    expect(persistent.value, 42.0);
    expect(persistent.toJson()['value'], 42.0);
  });

  test('a value-carrying entry encodes byte for byte as before the change', () {
    // The pre-change encoder always wrote `value`, in this key order. The structural row only
    // OMITS the key on a row that has none, so every value-carrying row is unchanged on the wire
    // (and every archive captured earlier reads with structural == false).
    final entry = BackupEntry(
      field: 'Frequency',
      fieldIndex: 0,
      key: 'Key 0',
      keyIndex: 0,
      type: 'Number',
      flags: const ['Persistent'],
      unit: 'Hz',
      value: 25000.0,
      size: 4,
    );
    expect(
      json.encode(entry.toJson()),
      '{"field":"Frequency","fieldIndex":0,"key":"Key 0","keyIndex":0,'
      '"type":"Number","flags":["Persistent"],"unit":"Hz","value":25000.0,"size":4}',
    );
    // A reserved placeholder (type None, value null) stays a non-structural row, so an archive
    // that carries one still reads and plans exactly as it did.
    final placeholder = BackupEntry.fromJson({
      'fieldIndex': 1,
      'keyIndex': 0,
      'type': dataTypeWord(DataType.none),
      'value': null,
      'size': 0,
    });
    expect(placeholder.structural, isFalse);
    expect(placeholder.toJson().containsKey('value'), isTrue);
    expect(restoresEntryValue(placeholder), isFalse);
  });

  test('a structural row is not a restore item: it plans no value write', () async {
    final plan = await buildRestorePlan([_archive()]);
    final entries = plan.items.where((i) => i.kind == RestoreKind.entry);
    expect(
      entries,
      hasLength(1),
      reason: 'only the persistent row (field 4) is a value write',
    );
    expect(entries.single.entry!.fieldIndex, 4);
    expect(plan.items.where((i) => i.entry?.structural == true), isEmpty);
  });

  test('a wiped target resolves a subscription whose target is a structural row', () {
    for (final survives in [true, false]) {
      final archive = _archive(withStructuralRows: survives);
      final plan = _plan(archive, _wiped());
      final item = plan.items.single;
      if (survives) {
        // The row exists in the archive with no value, so its ADDRESS is still encodable.
        expect(
          archiveRefReg(archive, _target),
          makeBlockInfo(dynamicTypeBase, 0, 0, 0),
        );
        expect(item.issue, isNull, reason: 'archiveRefReg resolves the structural row');
        expect(item.ready, isTrue);
      } else {
        // Control: drop the row and the address is gone - this is the failure the structural row
        // exists to prevent (measured on akyirr as PLAN unready_on_wiped_target=4).
        expect(archiveRefReg(archive, _target), isNull);
        expect(item.issue, 'Address not found');
      }
    }
  });

  test('a restore creates the dynamic rows the archive carries, values default', () async {
    final previous = requestChannel;
    final channel = _FakeRegisterChannel();
    installRequestChannel(channel);
    addTearDown(() => installRequestChannel(previous));

    final archive = _archive();
    final plan = _plan(archive, _wiped());
    final created = await prepareDynamicBlocks(plan);

    expect(created, 5, reason: 'four structural rows and one persistent row');
    final calls = channel.sent
        .where((c) => c.service == ServiceType.register)
        .toList();
    final creates = calls.where((c) => c.cid == DynamicCid.create).toList();
    expect(creates, hasLength(1), reason: 'the block itself is created once');
    expect(creates.single.payload[0] | (creates.single.payload[1] << 8), 0);

    final writes = calls.where((c) => c.cid == RegisterCid.write).toList();
    int fieldOf(List<int> p) =>
        blockInfoField(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
    expect(
      [for (final w in writes) fieldOf(w.payload)],
      [0, 1, 2, 3, 4],
      reason: 'every archived row becomes an entry, in field order',
    );
    for (final w in writes) {
      final meta = ValueInfo.fromBytes(w.payload, 4);
      expect(meta.size, 4);
      expect(
        w.payload.sublist(8),
        everyElement(0),
        reason: 'no archived value is written: the structure lands at its default',
      );
    }
    // The flags come from the archive, so a row that is Read Only there is Read Only here.
    expect(ValueInfo.fromBytes(writes.first.payload, 4).flags, 0);
    expect(ValueInfo.fromBytes(writes.last.payload, 4).flags,
        flagsFromWords(const ['Persistent']));
  });
}
