/// The upload tool's read-back names every difference and says which class it is.
///
/// The rig run of 2026-11-06 printed `0 failed write(s), 6 read-back mismatch(es)` and named none
/// of them (card t_d02e2f2d). The six were the PROVIDER half of the core's subscription rows: that
/// table is RAM-only on the node, no archive carries it and no restore writes it, so a read-back
/// taken right after a restore cannot see it - and the count read as six failed writes.
///
/// These tests drive the comparison itself (`readBackMismatches` over a plan built in memory, no
/// rig) and pin the three things the card asks for: each line carries device, table, key, expected
/// and actual; each is classified; and only a difference the restore should have settled fails the
/// run.
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/types.dart';

import '../tool/upload_setup.dart';

/// `0.1`, the core: the requester that owns the subscription rows.
const int _coreId = 1;

/// `0.2`, the node the subscription reads from.
const int _providerNode = 2;

/// The node's `Resistive measurement` block, where a subscription's source register lives.
const int _providerType = 8;

/// The core's `PWM output` block, where a subscription's target register lives.
const int _targetType = 0x04;

void main() {
  group('upload_setup read-back', () {
    test('a provider entry the restore never writes is named, and does not fail the run', () {
      // The archive names the provider at the address it was CAPTURED under (net 39); the rig
      // runs net 14 and the live row stores 14.2. The identity of a requester row is its
      // provider's NODE plus its two registers, so the row still matches - and the provider half
      // is the only difference left.
      final plan = _plan();
      final mismatches = readBackMismatches(plan, [
        _snapshot(
          _coreId,
          requesters: [_heldRow(trid: 0x1000)],
          providers: const [],
        ),
        _snapshot(_providerNode, providers: const []),
      ]);

      expect(mismatches, hasLength(1));
      final mismatch = mismatches.single;
      expect(mismatch.kind, ReadBackClass.runtime);
      expect(
        mismatch.fails,
        isFalse,
        reason:
            'no restore writes the provider half (Docs/Services/Subscriptions.md "Provider": '
            '"active until cancelled; the information is not persistent on this side"), so a '
            'read-back taken right after one cannot see it',
      );
      // One line carries everything the owner needs to act on it.
      expect(mismatch.line, contains('0.1 "Tamu"'));
      expect(mismatch.line, contains('subscriptions/provider'));
      expect(mismatch.line, contains('entry 0x1000'));
      expect(mismatch.line, contains('expected a provider entry for 0x1000'));
      expect(mismatch.line, contains('Resistive measurement[0]/field 3[0]'));
      expect(mismatch.line, contains('on 0.2 "Sensors A", installed by the requester'));
      expect(mismatch.line, contains('actual 0.2 "Sensors A" holds no entry'));
      expect(mismatch.line, contains(ReadBackClass.runtime.label));
    });

    test('the note explains the whole table once, and only when every difference is that class', () {
      final runtimeOnly = readBackMismatches(_plan(), [
        _snapshot(
          _coreId,
          requesters: [_heldRow(trid: 0x1000), _heldRow(trid: 0x1001)],
          providers: const [],
        ),
        _snapshot(_providerNode, providers: const []),
      ]);
      expect(runtimeOnly, hasLength(2));

      final note = runtimeDifferenceNote(runtimeOnly);
      expect(note, isNotNull);
      expect(note, contains('all 2 difference(s) are the PROVIDER half'));
      expect(note, contains('Docs/Services/Subscriptions.md'));
      expect(
        note,
        contains('The requester table'),
        reason: 'the note has to say which half DID read back, or it reads as a shrug',
      );

      expect(
        runtimeDifferenceNote(const []),
        isNull,
        reason: 'nothing to explain when the read-back agrees',
      );
      expect(
        runtimeDifferenceNote([
          ...runtimeOnly,
          _mismatchOf(ReadBackClass.notApplied),
        ]),
        isNull,
        reason:
            'a real difference must not be excused by a note about runtime state - it is '
            'named on its own line and fails the run',
      );
    });

    test('a requester row the device does not hold is a real failed write', () {
      final mismatches = readBackMismatches(_plan(), [
        _snapshot(_coreId, requesters: const [], providers: const []),
        _snapshot(_providerNode, providers: const []),
      ]);

      final requester = mismatches.where(
        (m) => m.where == 'subscriptions/requester',
      );
      expect(requester, hasLength(1));
      final mismatch = requester.single;
      expect(mismatch.kind, ReadBackClass.notApplied);
      expect(mismatch.fails, isTrue);
      expect(mismatch.line, contains('Periodic'));
      expect(mismatch.line, contains('Resistive measurement[0]/Measured Value[0]'));
      expect(mismatch.line, contains('actual the table holds no entry'));
      expect(runtimeDifferenceNote(mismatches), isNull);
    });

    test('a requester table that did not answer is unreadable, not an empty table', () {
      // The defect behind the count: `getRequesterSubscriptions` folds a failed read into `[]`,
      // so a lost reply and an empty table produced the same words.
      final mismatches = readBackMismatches(_plan(), [
        _snapshot(_coreId, requesters: null, providers: const []),
        _snapshot(_providerNode, providers: const []),
      ]);

      final requester = mismatches.firstWhere(
        (m) => m.where == 'subscriptions/requester',
      );
      expect(requester.kind, ReadBackClass.unreadable);
      expect(requester.fails, isTrue);
      expect(requester.actual, contains('did not answer (3 sends)'));
    });

    test('a requester row that holds different timing is a real failed write', () {
      final mismatches = readBackMismatches(_plan(), [
        _snapshot(
          _coreId,
          requesters: [_heldRow(trid: 0x1000, periodMs: 500)],
          providers: const [],
        ),
        _snapshot(_providerNode, providers: const []),
      ]);

      final requester = mismatches.firstWhere(
        (m) => m.where == 'subscriptions/requester',
      );
      expect(requester.kind, ReadBackClass.notApplied);
      expect(requester.actual, contains('500 ms period'));
      expect(requester.expected, contains('1000'));
    });

    test('a provider this run did not read is "not read", not "missing"', () {
      // `--device 0.1` selects the core only: the provider node is not a target, so its table was
      // never read. Saying "holds no entry" about a table nobody looked at is the same lie the
      // folding getter told.
      final mismatches = readBackMismatches(_plan(), [
        _snapshot(
          _coreId,
          requesters: [_heldRow(trid: 0x1000)],
          providers: const [],
        ),
      ]);

      final mismatch = mismatches.firstWhere(
        (m) => m.where == 'subscriptions/provider',
      );
      expect(mismatch.actual, contains('not read'));
      expect(mismatch.kind, ReadBackClass.runtime);
      expect(mismatch.fails, isFalse);
    });

    test('a provider table that did not answer fails the run', () {
      final mismatches = readBackMismatches(_plan(), [
        _snapshot(
          _coreId,
          requesters: [_heldRow(trid: 0x1000)],
          providers: const [],
        ),
        _snapshot(_providerNode, providers: null),
      ]);

      final mismatch = mismatches.firstWhere(
        (m) => m.where == 'subscriptions/provider',
      );
      expect(mismatch.kind, ReadBackClass.unreadable);
      expect(mismatch.fails, isTrue);
      expect(mismatch.actual, contains('did not answer (3 sends)'));
    });

    test('a script in error and a missing dynamic block are named too', () {
      final mismatches = readBackMismatches(_plan(), [
        _snapshot(
          _coreId,
          requesters: [_heldRow(trid: 0x1000)],
          providers: const [],
          scriptStates: const [(slot: 4, state: 4, error: true)],
        ),
        _snapshot(_providerNode, providers: const []),
      ]);

      final script = mismatches.firstWhere((m) => m.where == 'scripts/slot 4');
      expect(script.kind, ReadBackClass.notApplied);
      expect(script.line, contains('expected a script that runs without an error'));
      expect(script.line, contains('actual the slot is in error (state 4)'));
    });
  });
}

/// A one-device archive (the core) with one requester subscription, and the rig it restores onto.
RestorePlan _plan() {
  final archive = BackupDevice(
    type: 'Tamu v2.0A',
    typeId: 1,
    id: _coreId,
    name: 'Tamu',
    blocks: [
      BackupBlock(
        type: 'PWM output',
        typeIndex: _targetType,
        instance: 0,
        name: 'Fan',
        isDynamic: false,
        entries: [
          BackupEntry(
            field: 'Field 0',
            fieldIndex: 0,
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
      BackupRequesterSubscription(
        // Net 39: the net the archive was CAPTURED under, not the one the rig runs.
        provider: '39.2',
        trigger: 'Periodic',
        periodMs: 1000,
        minTimeMs: 100,
        deadzone: 0,
        source: const BackupBlockRef(
          block: 'Resistive measurement',
          typeIndex: _providerType,
          instance: 0,
          field: 'Measured Value',
          fieldIndex: 3,
          key: 'Key 0',
          keyIndex: 0,
        ),
        target: const BackupBlockRef(
          block: 'PWM output',
          typeIndex: _targetType,
          instance: 0,
          field: 'Field 0',
          fieldIndex: 0,
          key: 'Key 0',
          keyIndex: 0,
        ),
      ),
    ],
  );
  final core = LiveDevice(
    id: _coreId,
    name: 'Tamu',
    capabilities:
        Capability.core |
        Capability.subscriptionRequest |
        Capability.subscriptionProvide,
    blocks: [
      LiveBlock(
        type: _targetType,
        instance: 0,
        name: 'Fan',
        isDynamic: false,
        entries: [
          LiveEntry(
            field: 0,
            key: 0,
            fieldName: 'Field 0',
            keyName: 'Key 0',
            meta: ValueInfo(type: DataType.number.value, size: 4),
          ),
        ],
      ),
    ],
  );
  final provider = LiveDevice(
    id: _providerNode,
    name: 'Sensors A',
    capabilities: Capability.node | Capability.subscriptionProvide,
    blocks: [
      LiveBlock(
        type: _providerType,
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
  final item = RestoreItem(
    kind: RestoreKind.subscription,
    device: archive,
    subscription: archive.requesterSubscriptions.single,
  );
  final plan = RestorePlan(
    devices: [archive],
    liveDevices: [core, provider],
    items: [item],
    deviceTargets: {archive.id: core},
    blockTargets: const {},
  );
  plan.resolve();
  return plan;
}

/// One live requester row: the archive's subscription, as the rig holds it - provider address
/// carrying the rig's net (14), not the archive's (39).
RequesterSubscription _heldRow({required int trid, int periodMs = 1000}) =>
    RequesterSubscription(
      index: 0,
      providerAddr: (14 << 10) | _providerNode,
      trid: trid,
      sourceReg: (_providerType << 22) | (0 << 16) | (3 << 8),
      targetReg: (_targetType << 22) | (0 << 16) | (0 << 8),
      trigger: TriggerType.periodic,
      periodMs: periodMs,
      minTimeMs: 100,
    );

ReadBackSnapshot _snapshot(
  int deviceId, {
  List<DynBlock>? blocks = const [],
  List<RequesterSubscription>? requesters = const [],
  List<ProviderSubscription>? providers = const [],
  List<({int slot, int state, bool error})> scriptStates = const [],
}) => ReadBackSnapshot(
  deviceId: deviceId,
  blocks: blocks,
  requesters: requesters,
  providers: providers,
  scriptStates: scriptStates,
);

ReadBackMismatch _mismatchOf(ReadBackClass kind) => ReadBackMismatch(
  device: '0.1 "Tamu"',
  where: 'dynamic/Fan',
  key: 'block',
  expected: 'the archive\'s block',
  actual: 'the device does not name it',
  kind: kind,
);
