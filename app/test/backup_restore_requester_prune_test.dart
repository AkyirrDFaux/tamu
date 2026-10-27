import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/types.dart';

/// A device's requester table that answers the Subscriptions CIDs a restore uses - Get (0x10),
/// Set (0x11, a None trigger cancels) and Save-all (0x13) - so the prune can be driven without a
/// rig. `entries` is the live table: the commands mutate it, so a test reads the state the restore
/// left behind.
class _FakeTable implements RequestChannel {
  _FakeTable(List<RequesterSubscription> entries) : entries = [...entries];

  final List<RequesterSubscription> entries;

  /// Every request, in the order it was sent.
  final sent =
      <({ServiceType service, int cid, List<int> payload, int? trid})>[];

  /// Refuse every cancel, as a device that answers FAIL would.
  bool refuseCancel = false;

  /// Refuse the Save-all, as a device whose `.SUBREQ` write failed would.
  bool failSave = false;

  /// Answer the Get (0x10) with FAIL from the Nth read on (1-based; 0 = never). A read that fails
  /// is NOT an empty table, which is the whole point of the read this drives.
  int failGetFrom = 0;

  /// Answer the Get with FAIL once a cancel has landed: the read-back case, where the cancels
  /// applied and were saved but the device then stopped returning its table.
  bool failGetAfterCancel = false;

  /// Never answer the Get at all, as a timeout does - `SubscriptionClient._request` turns the
  /// thrown error into "no reply".
  bool getTimesOut = false;

  /// How many Get (0x10) requests the run sent.
  int reads = 0;

  bool _unreadable = false;

  /// How many Save-all requests the run sent.
  int saves = 0;

  int _txId = 0x40;

  static const _ok = PacketResponse(payload: [], success: true, fail: false);
  static const _failed = PacketResponse(
    payload: [],
    success: false,
    fail: true,
  );

  /// The TRIDs the table holds, in order.
  List<int> get trids => [for (final e in entries) e.trid];

  /// The cancels sent (Set with a None trigger), as TRIDs.
  List<int> get cancelTrids => [
    for (final c in sent)
      if (c.service == ServiceType.subscriptions &&
          c.cid == 0x11 &&
          c.payload[8] == TriggerType.none.value)
        c.trid!,
  ];

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
    sent.add((
      service: service,
      cid: functionCid,
      payload: payload,
      trid: transactionId,
    ));
    // A register write echoes BlockInfo + ValueInfo + the value, so the reply IS the request's own
    // payload. The device answers it; a fake that answered nothing would make every entry write
    // fail and drown the requester table this test is about.
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
    sent.add((
      service: service,
      cid: functionCid,
      payload: payload,
      trid: transactionId,
    ));
    // A register write echoes the address, the accepted ValueInfo and the value - the same reply
    // the payload path returns, now carrying the SUCCESS flag the write is judged by.
    if (service == ServiceType.register && functionCid == RegisterCid.write) {
      return PacketResponse(payload: payload, success: true, fail: false);
    }
    if (service != ServiceType.subscriptions) return _ok;
    switch (functionCid) {
      case 0x10: // Get subscriptions: the table, 32-byte entries (hash at offset 28), no count
        // prefix in the payload.
        reads++;
        if (getTimesOut) throw StateError('no reply');
        if (_unreadable || (failGetFrom > 0 && reads >= failGetFrom)) {
          return _failed;
        }
        return PacketResponse(
          payload: [for (final e in entries) ...e.toWireBytes()],
          success: true,
          fail: false,
        );
      case 0x11: // Set subscription: a None trigger is a cancel. The payload is the 28 B prefix.
        final entry = RequesterSubscription.fromBytes(0, payload);
        if (entry.trigger == TriggerType.none) {
          if (refuseCancel) return _failed;
          entries.removeWhere((e) => e.trid == entry.trid);
          if (failGetAfterCancel) {
            _unreadable = true;
          }
          return _ok;
        }
        final at = entries.indexWhere((e) => e.trid == entry.trid);
        if (at >= 0) {
          entries[at] = entry;
        } else {
          entries.add(entry);
        }
        return _ok;
      case 0x13: // Save all subscriptions.
        saves++;
        return failSave ? _failed : _ok;
    }
    return _ok;
  }
}

final int _targetType = BlockType.pwm.value;
final int _requesterId = idFromString('39.1')!;
final int _providerId = idFromString('39.2')!;

int _sourceReg() => makeBlockInfo(8, 0, 3, 0);
int _targetReg(int field) => makeBlockInfo(_targetType, 0, field, 0);

const _sourceRef = BackupBlockRef(
  block: 'Resistive measurement',
  typeIndex: 8,
  instance: 0,
  field: 'Measured Value',
  fieldIndex: 3,
  key: 'Key 0',
  keyIndex: 0,
);

BackupBlockRef _targetRef(int field) => BackupBlockRef(
  block: 'PWM output',
  typeIndex: _targetType,
  instance: 0,
  field: 'Field $field',
  fieldIndex: field,
  key: 'Key 0',
  keyIndex: 0,
);

/// The archive: [count] requester subscriptions, each feeding one field of the same target block
/// from the same provider source.
BackupDevice _archive({int count = 4, String provider = '0.2'}) => BackupDevice(
  type: 'Tamu v2.0A',
  typeId: 1,
  id: idFromString('0.1')!,
  name: 'Tamu',
  blocks: [
    BackupBlock(
      type: 'PWM output',
      typeIndex: _targetType,
      instance: 0,
      name: 'Fan',
      isDynamic: false,
      entries: [
        for (var field = 0; field < count; field++)
          BackupEntry(
            field: 'Field $field',
            fieldIndex: field,
            key: 'Key 0',
            keyIndex: 0,
            type: 'Number',
            flags: const [],
            value: 0.0,
            size: 4,
          ),
      ],
    ),
  ],
  requesterSubscriptions: [
    for (var field = 0; field < count; field++)
      BackupRequesterSubscription(
        provider: provider,
        trigger: 'Periodic',
        periodMs: 1000,
        minTimeMs: 100,
        deadzone: 0,
        source: _sourceRef,
        target: _targetRef(field),
      ),
  ],
);

LiveDevice _liveRequester() => LiveDevice(
  id: _requesterId,
  name: 'Tamu',
  capabilities: Capability.subscriptionRequest,
  blocks: [
    LiveBlock(
      type: _targetType,
      instance: 0,
      name: 'Fan',
      isDynamic: false,
      entries: [
        for (var field = 0; field < 6; field++)
          LiveEntry(
            field: field,
            key: 0,
            fieldName: 'Field $field',
            keyName: 'Key 0',
            meta: ValueInfo(type: DataType.number.value, size: 4),
          ),
      ],
    ),
  ],
);

LiveDevice _liveProvider() => LiveDevice(
  id: _providerId,
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

/// One requester entry as the rig holds it: [field] of the target block, fed from the provider.
RequesterSubscription _held(int trid, int field) => RequesterSubscription(
  index: 0,
  providerAddr: _providerId,
  trid: trid,
  sourceReg: _sourceReg(),
  targetReg: _targetReg(field),
  trigger: TriggerType.periodic,
  periodMs: 1000,
  minTimeMs: 100,
);

/// The rig as the bug left it: six entries, the last two stale (an old temperature feed and an LDR
/// both writing `Dynamic.0` field 0 through their own target registers).
List<RequesterSubscription> _sixEntries() => [
  for (var i = 0; i < 6; i++) _held(0x1000 + i, i),
];

RestorePlan _plan(BackupDevice archive, List<LiveDevice> live) {
  final target = live.firstWhere((d) => d.id == _requesterId);
  final items = <RestoreItem>[
    for (final block in archive.blocks)
      for (final entry in block.entries)
        if (restoresEntryValue(entry))
          RestoreItem(
            kind: RestoreKind.entry,
            device: archive,
            block: block,
            entry: entry,
          ),
    for (final sub in archive.requesterSubscriptions)
      RestoreItem(
        kind: RestoreKind.subscription,
        device: archive,
        subscription: sub,
      ),
  ];
  final plan = RestorePlan(
    devices: [archive],
    liveDevices: live,
    items: items,
    deviceTargets: {archive.id: target},
    blockTargets: const {},
  );
  plan.resolve();
  for (final item in plan.items) {
    item.selected = item.ready;
  }
  return plan;
}

List<String> _hex(List<int> trids) => [
  for (final t in trids) '0x${t.toRadixString(16)}',
];

void main() {
  late RequestChannel? previous;
  late _FakeTable device;

  setUp(() => previous = requestChannel);
  tearDown(() => installRequestChannel(previous));

  test('six entries restored from a four-entry archive end at four', () async {
    device = _FakeTable(_sixEntries());
    installRequestChannel(device);
    final archive = _archive();

    final result = await applyRestorePlan(
      _plan(archive, [_liveRequester(), _liveProvider()]),
    );

    final prune = result.requesterPrune.single;
    print(
      'PRUNE 6->4: before=${_hex(prune.before)} cancelled=${_hex(prune.cancelled)} '
      'remaining=${_hex(prune.remaining)}',
    );
    expect(_hex(prune.before), [
      '0x1000',
      '0x1001',
      '0x1002',
      '0x1003',
      '0x1004',
      '0x1005',
    ], reason: 'the rig held six entries before the restore');
    expect(
      _hex(prune.cancelled),
      ['0x1004', '0x1005'],
      reason: 'only the two the archive does not name are cancelled',
    );
    expect(_hex(prune.remaining), [
      '0x1000',
      '0x1001',
      '0x1002',
      '0x1003',
    ], reason: 'the four the archive names remain');
    expect(
      _hex(device.trids),
      _hex(prune.remaining),
      reason: 'the report is a read-back of the device, not the plan',
    );
    expect(device.entries, hasLength(4));
    expect(
      result.failed,
      0,
      reason: 'a prune that lands is not a failure: ${result.failures}',
    );
    expect(result.failures, isEmpty);
    expect(
      device.sent.where(
        (c) =>
            c.service == ServiceType.subscriptions &&
            (c.cid == 0x01 || c.cid == 0x21),
      ),
      isEmpty,
      reason: 'a restore never writes a provider row',
    );
  });

  test('the Save-all is issued after the cancels', () async {
    device = _FakeTable(_sixEntries());
    installRequestChannel(device);

    await applyRestorePlan(
      _plan(_archive(), [_liveRequester(), _liveProvider()]),
    );

    expect(device.cancelTrids, [0x1004, 0x1005]);
    expect(device.saves, 1);
    final lastCancel = device.sent.lastIndexWhere(
      (c) =>
          c.service == ServiceType.subscriptions &&
          c.cid == 0x11 &&
          c.payload[8] == TriggerType.none.value,
    );
    final save = device.sent.indexWhere(
      (c) => c.service == ServiceType.subscriptions && c.cid == 0x13,
    );
    expect(save, greaterThan(lastCancel));
    final cancelPositions = [
      for (var i = 0; i < device.sent.length; i++)
        if (device.sent[i].service == ServiceType.subscriptions &&
            device.sent[i].cid == 0x11 &&
            device.sent[i].payload[8] == TriggerType.none.value)
          i,
    ];
    print(
      'PRUNE ORDER: cancels at $cancelPositions, save at $save '
      '(trids ${_hex(device.cancelTrids)}, saves=${device.saves})',
    );
    expect(
      device.sent
          .skip(lastCancel + 1)
          .where(
            (c) => c.cid == 0x10 && c.service == ServiceType.subscriptions,
          ),
      isNotEmpty,
      reason: 'the table is read back after the save',
    );
  });

  test('a second restore of the same archive is a no-op', () async {
    device = _FakeTable(_sixEntries());
    installRequestChannel(device);

    await applyRestorePlan(
      _plan(_archive(), [_liveRequester(), _liveProvider()]),
    );
    final afterFirst = [...device.trids];
    final mark = device.sent.length;
    final savesAfterFirst = device.saves;

    final second = await applyRestorePlan(
      _plan(_archive(), [_liveRequester(), _liveProvider()]),
    );
    final again = device.sent.sublist(mark);

    expect(device.trids, afterFirst);
    expect(second.requesterPrune.single.cancelled, isEmpty);
    expect(second.requesterPrune.single.before, afterFirst);
    print(
      'PRUNE IDEMPOTENT: after run 1=${_hex(afterFirst)}, run 2 '
      'before=${_hex(second.requesterPrune.single.before)} '
      'cancelled=${_hex(second.requesterPrune.single.cancelled)}, '
      'saves run 1=$savesAfterFirst run 2=${device.saves}',
    );
    expect(device.saves, savesAfterFirst, reason: 'nothing to persist');
    expect(
      again.where(
        (c) => c.service == ServiceType.subscriptions && c.cid == 0x13,
      ),
      isEmpty,
      reason: 'no Save-all when no entry was cancelled',
    );
    expect(
      again.where(
        (c) =>
            c.service == ServiceType.subscriptions &&
            c.cid == 0x11 &&
            c.payload[8] == TriggerType.none.value,
      ),
      isEmpty,
      reason: 'nothing is cancelled a second time',
    );
  });

  test('a target with fewer entries only gains the missing ones', () async {
    device = _FakeTable([_held(0x1000, 0), _held(0x1001, 1)]);
    installRequestChannel(device);

    final result = await applyRestorePlan(
      _plan(_archive(), [_liveRequester(), _liveProvider()]),
    );

    final prune = result.requesterPrune.single;
    expect(device.entries, hasLength(4));
    expect(
      prune.cancelled,
      isEmpty,
      reason: 'nothing the archive does not name',
    );
    expect(_hex(device.trids.take(2).toList()), ['0x1000', '0x1001']);
    expect(device.saves, 0);
    expect(result.failed, 0, reason: result.failures.toString());
  });

  test('a refused cancel is reported, not swallowed', () async {
    device = _FakeTable(_sixEntries());
    device.refuseCancel = true;
    installRequestChannel(device);

    final result = await applyRestorePlan(
      _plan(_archive(), [_liveRequester(), _liveProvider()]),
    );

    final prune = result.requesterPrune.single;
    expect(prune.cancelled, isEmpty);
    expect(
      device.entries,
      hasLength(6),
      reason: 'a refused cancel changes nothing on the device',
    );
    expect(result.failed, 2, reason: result.failures.join(' | '));
    expect(
      result.failures.where((f) => f.contains('cancelling requester entry')),
      hasLength(2),
    );
    expect(result.failures.join('\n'), contains('0x1004'));
    expect(device.saves, 0, reason: 'a failed run must not claim a save');
  });

  test(
    'an archive subscription that does not resolve leaves the table alone',
    () async {
      device = _FakeTable(_sixEntries());
      installRequestChannel(device);
      // The archive names provider `0.3`, which is not on this rig: the identity of the entry it
      // means cannot be computed, so nothing may be deleted on a guess.
      final archive = _archive(provider: '0.3');

      final result = await applyRestorePlan(
        _plan(archive, [_liveRequester(), _liveProvider()]),
      );

      final prune = result.requesterPrune.single;
      expect(prune.skipped, isTrue);
      expect(prune.reason, isNotNull);
      expect(prune.cancelled, isEmpty);
      expect(device.entries, hasLength(6));
      expect(device.cancelTrids, isEmpty);
      expect(device.saves, 0);
      expect(
        result.failures.join('\n'),
        contains('does not resolve'),
      );
    },
  );

  test(
    'a target that answers the table read with FAIL is reported, not pruned',
    () async {
      device = _FakeTable(_sixEntries());
      device.failGetFrom =
          1; // every Get fails, as a device that answers FAIL does
      installRequestChannel(device);

      final result = await applyRestorePlan(
        _plan(_archive(), [_liveRequester(), _liveProvider()]),
      );

      final prune = result.requesterPrune.single;
      print(
        'PRUNE UNREAD: failed=${result.failed} skipped=${prune.skipped} '
        'confirmed=${prune.confirmed} before=[${prune.beforeText}] '
        'cancelled=[${prune.cancelledText}] remaining=[${prune.remainingText}] '
        'saves=${device.saves} reads=${device.reads} '
        'entries=${_hex(device.trids)}',
      );
      expect(
        prune.skipped,
        isTrue,
        reason: 'an unread table leaves the device alone',
      );
      expect(prune.confirmed, isFalse, reason: 'nothing was read back');
      expect(
        prune.remainingText,
        'unconfirmed',
        reason: 'an unread table is never reported as an empty one',
      );
      expect(prune.before, isEmpty);
      expect(prune.cancelled, isEmpty);
      expect(device.cancelTrids, isEmpty);
      expect(device.saves, 0);
      expect(
        device.entries,
        hasLength(6),
        reason: 'the stale entries the read would have found are still there',
      );
      expect(
        result.failed,
        1,
        reason:
            'a prune that could not look is a reported failure: '
            '${result.failures}',
      );
      expect(
        result.failures.join('\n'),
        contains('requester table read'),
      );
    },
  );

  test(
    'a requester-table read that times out is reported, not read as empty',
    () async {
      device = _FakeTable(_sixEntries());
      device.getTimesOut = true; // no reply at all: _request returns null
      installRequestChannel(device);

      final result = await applyRestorePlan(
        _plan(_archive(), [_liveRequester(), _liveProvider()]),
      );

      final prune = result.requesterPrune.single;
      print(
        'PRUNE TIMEOUT: failed=${result.failed} skipped=${prune.skipped} '
        'confirmed=${prune.confirmed} remaining=[${prune.remainingText}] '
        'cancelled=${prune.cancelled.length} saves=${device.saves}',
      );
      expect(prune.skipped, isTrue);
      expect(prune.confirmed, isFalse);
      expect(prune.before, isEmpty);
      expect(device.cancelTrids, isEmpty);
      expect(device.saves, 0);
      expect(device.entries, hasLength(6));
      expect(result.failed, 1, reason: result.failures.toString());
      expect(
        result.failures.join('\n'),
        contains('requester table read'),
      );
    },
  );

  test(
    'a failed read-back is reported as unconfirmed, not as a clean table',
    () async {
      device = _FakeTable(_sixEntries());
      device.failGetAfterCancel = true;
      installRequestChannel(device);

      final result = await applyRestorePlan(
        _plan(_archive(), [_liveRequester(), _liveProvider()]),
      );

      final prune = result.requesterPrune.single;
      print(
        'PRUNE UNCONFIRMED: cancelled=${_hex(prune.cancelled)} '
        'confirmed=${prune.confirmed} remaining=[${prune.remainingText}] '
        'saves=${device.saves} entries=${_hex(device.trids)} '
        'failed=${result.failed}',
      );
      expect(_hex(prune.cancelled), ['0x1004', '0x1005']);
      expect(
        device.saves,
        1,
        reason: 'the cancels landed and were persisted before the read-back',
      );
      expect(_hex(device.trids), ['0x1000', '0x1001', '0x1002', '0x1003']);
      expect(prune.confirmed, isFalse);
      expect(
        prune.remainingText,
        'unconfirmed',
        reason:
            'the table holds four, and the report may not claim it holds none',
      );
      expect(prune.remaining, isEmpty);
      expect(
        result.failures.join('\n'),
        contains('could not be read back'),
      );
      expect(result.failed, 1, reason: result.failures.toString());
      expect(
        result.failures.where((f) => f.contains('is still in the table')),
        isEmpty,
        reason:
            'a table nobody read cannot be asked whether a TRID is still in it',
      );
    },
  );

  test('a DELETE row the plan leaves unticked is not cancelled', () async {
    // The page lists the two stale entries as DELETEs; the user turns ONE of them off. The prune
    // must cancel only the other, and report the kept one - a delete the user turned down is not
    // a delete that happened quietly.
    device = _FakeTable(_sixEntries());
    installRequestChannel(device);
    final archive = _archive();
    final live = [_liveRequester(), _liveProvider()];
    final plan = _plan(archive, live);
    final target = plan.deviceTargets[archive.id]!;
    // Field 5 is the second stale entry (the archive names fields 0..3).
    final keptKey = requesterKey(_providerId, _sourceReg(), _targetReg(5));
    plan.removals.add(
      RestoreRemoval(
        kind: RestoreKind.subscription,
        device: archive,
        target: target,
        trid: 0x1005,
        key: keptKey,
        selected: false,
      ),
    );

    final result = await applyRestorePlan(plan);

    final prune = result.requesterPrune.single;
    print(
      'unticked DELETE: before=${_hex(prune.before)} '
      'cancelled=${_hex(prune.cancelled)} kept=${_hex(prune.kept)} '
      'remaining=${_hex(prune.remaining)}',
    );
    expect(_hex(prune.cancelled), ['0x1004'], reason: 'only the ticked delete');
    expect(_hex(prune.kept), ['0x1005'], reason: 'the unticked one is named');
    expect(_hex(prune.remaining), [
      '0x1000',
      '0x1001',
      '0x1002',
      '0x1003',
      '0x1005',
    ], reason: 'the kept entry is still in the table');
    expect(device.cancelTrids, [0x1004]);
    expect(result.failed, 0, reason: result.failures.toString());
  });
}
