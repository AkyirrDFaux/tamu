// A restore reports WHY each item failed (card t_7cfa3a99, owner report: "17 failures with no
// reason").
//
// Every case below drives a REAL `applyRestorePlan` against a fake bus, and asserts the whole
// reason a person has to read: the device and the target it was written to, the item kind, the
// address as the PLAN names it, the code the device answered with - and, for a failure the app
// decided on its own, the explicit "no code from the device" instead of a blank.
//
// The deliberately failing restore is the first two cases: the fake refuses the write with the
// firmware's own answer (FLAG_FAIL and no payload, `RespondStatus(Frame, false)`). The last case is
// the positive control: an accepted run reports no failures at all, so a report full of noise
// cannot pass for a report of failures.

import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/backup_script.dart';
import 'package:tamuapp/core/diagnostics.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/types.dart';

/// A bus that answers - or refuses - the requests a restore sends, so a failing restore can be
/// driven without a rig.
///
/// A refusal is the firmware's: `FLAG_FAIL` with no payload. [refuse] names the commands to refuse
/// as `service/cid` (e.g. `register/3`, `storage/1`), and everything else is answered the way the
/// device answers it.
class _RefusingBus implements RequestChannel {
  _RefusingBus({
    Map<int, int> loaded = const {},
    Map<String, List<int>> files = const {},
  }) : loaded = {...loaded},
       files = {...files};

  /// The commands refused, as `service/cid`.
  final refuse = <String>{};

  /// The commands the device never answers at all, as `service/cid` - a timeout.
  final silent = <String>{};

  /// Answers for the requester writes (`subscriptions/17`), consumed in order. `null` is an entry
  /// the device never answers; an empty queue means the write is accepted, the normal answer.
  ///
  /// Two requester rows in one run need two different answers before the report can be asked
  /// whether each row carries its OWN reason instead of the one before it.
  final requesterWriteAnswers = <PacketResponse?>[];

  /// The firmware's refusal, to queue in [requesterWriteAnswers]: FLAG_FAIL and no payload.
  static const refusal = PacketResponse(
    payload: [],
    success: false,
    fail: true,
  );

  /// Loaded script slots: slot -> SCR file id.
  final Map<int, int> loaded;

  /// The names the device's file table lists.
  final tableNames = <String>[];

  /// The files the device stores.
  final Map<String, List<int>> files;

  /// Every request, in the order it was sent.
  final sent = <({ServiceType service, int cid, List<int> payload})>[];

  int _txId = 0x100;

  static const _ok = PacketResponse(payload: [], success: true, fail: false);
  static const _failed = PacketResponse(
    payload: [],
    success: false,
    fail: true,
  );

  bool _refuses(ServiceType service, int cid) =>
      refuse.contains('${service.name}/$cid');

  List<int> _slotMap() {
    const slots = 8;
    final map = List<int>.filled(slots, 0xFFFF);
    loaded.forEach((slot, fileId) {
      if (slot < slots) map[slot] = fileId;
    });
    return [
      for (final id in map) ...[id & 0xFF, (id >> 8) & 0xFF],
    ];
  }

  List<int> _tableReply() {
    final names = [...tableNames];
    final content = <int>[];
    void record(String name, int size) {
      content
        ..addAll([0x40, 0, 0, 0])
        ..addAll([size & 0xFF, (size >> 8) & 0xFF, (size >> 16) & 0xFF, 0])
        ..addAll(StorageClient.padName(name));
    }

    record('.TABLE', (names.length + 1) * 16);
    for (final name in names) {
      record(name, files[name]?.length ?? 0);
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
    // A command the device never answers is a timeout, not a refusal: the two arrive at the app
    // as different reasons, so a case can tell them apart.
    final command = '${service.name}/$functionCid';
    if (silent.contains(command)) throw TimeoutException('$command: no reply');
    if (service == ServiceType.subscriptions &&
        functionCid == 0x11 &&
        requesterWriteAnswers.isNotEmpty) {
      final answer = requesterWriteAnswers.removeAt(0);
      if (answer == null) throw TimeoutException('$command: no reply');
      return answer;
    }
    if (_refuses(service, functionCid)) return _failed;
    if (service == ServiceType.register) {
      switch (functionCid) {
        case RegisterCid.write:
          // An accepted write echoes the address, the ValueInfo and the value.
          return PacketResponse(payload: payload, success: true, fail: false);
        case RegisterCid.saveAll:
        case RegisterCid.recallAll:
          return _ok;
      }
      return _ok;
    }
    if (service == ServiceType.storage) {
      switch (functionCid) {
        case 5: // read a file
          final name = StorageClient.unpadName(payload.sublist(0, 8));
          if (name == '.TABLE') {
            return PacketResponse(
              payload: _tableReply(),
              success: true,
              fail: false,
            );
          }
          final content = files[name];
          if (content == null) return _failed;
          final out = <int>[];
          for (var off = 0; off < content.length; off += 64) {
            final end = off + 64 > content.length ? content.length : off + 64;
            out
              ..addAll(StorageClient.padName(name))
              ..addAll(content.sublist(off, end));
          }
          return PacketResponse(payload: out, success: true, fail: false);
        case 6: // write a fragment: the sequence the device accepted
          return const PacketResponse(
            payload: [0, 0],
            success: true,
            fail: false,
          );
        default:
          return _ok;
      }
    }
    if (service == ServiceType.script) {
      if (functionCid == 0) {
        return PacketResponse(payload: _slotMap(), success: true, fail: false);
      }
      return _ok;
    }
    return _ok;
  }
}

BackupBlockRef _ref(String block, int type, int field, String fieldName) =>
    BackupBlockRef(
      block: block,
      typeIndex: type,
      instance: 0,
      field: fieldName,
      fieldIndex: field,
      key: 'Key 0',
      keyIndex: 0,
    );

/// One requester row: a field of the provider (`1.3`) read into the PWM's Duty.
BackupRequesterSubscription _subscription({
  int sourceField = 4,
  String sourceName = 'Measured Value',
}) => BackupRequesterSubscription(
  provider: '1.3',
  trigger: 'Periodic',
  source: _ref(
    'Resistive measurement',
    BlockType.resistiveMeasure.value,
    sourceField,
    sourceName,
  ),
  target: _ref('PWM output', BlockType.pwm.value, 1, 'Duty'),
  periodMs: 100,
  minTimeMs: 10,
  deadzone: 0,
);

/// One archive carrying a PWM entry, a script and a subscription - the three kinds a restore writes
/// through three different clients, so one failing run can report all of them.
///
/// [subscriptions] replaces the single default row, so a case can carry two.
BackupDevice _archive({List<BackupRequesterSubscription>? subscriptions}) =>
    BackupDevice(
      type: 'Tamu v2.0A',
      typeId: 1,
      id: idFromString('1.1')!,
      name: 'Tamu',
      blocks: [
        BackupBlock(
          type: 'PWM output',
          typeIndex: BlockType.pwm.value,
          instance: 0,
          name: 'Fan',
          isDynamic: false,
          entries: [
            BackupEntry(
              field: 'Duty',
              fieldIndex: 1,
              key: 'Key 0',
              keyIndex: 0,
              type: 'Number',
              flags: const ['Persistent'],
              value: 40.0,
              size: 4,
            ),
          ],
        ),
      ],
      scripts: [
        BackupScript(
          slot: 1,
          functionName: 'Fan curve',
          properties: const [],
          inputs: const [],
          outputs: const [],
          variables: const [],
          constants: const [],
          lines: const [],
        ),
      ],
      files: const [],
      layouts: const [],
      requesterSubscriptions: subscriptions ?? [_subscription()],
    );

LiveEntry _liveEntry(int field, String name, {List<int> value = const []}) =>
    LiveEntry(
      field: field,
      key: 0,
      fieldName: name,
      keyName: 'Key 0',
      meta: ValueInfo(type: DataType.number.value, size: 4),
      value: value,
    );

/// The live nodes the archive is restored onto: the requester it names (`1.1`), and the provider
/// (`1.3`) the subscription reads from - a subscription whose provider is off the bus would be
/// reported as an unresolved address, which is a different reason.
///
/// [secondSource] adds the provider field a second requester row reads, so two rows on one device
/// both resolve.
List<LiveDevice> _live({bool secondSource = false}) => [
  LiveDevice(
    id: idFromString('1.1')!,
    name: 'Tamu',
    capabilities: Capability.scripts | Capability.subscriptionRequest,
    blocks: [
      LiveBlock(
        type: BlockType.pwm.value,
        instance: 0,
        name: 'Fan',
        isDynamic: false,
        entries: [
          _liveEntry(1, 'Duty', value: [0, 0, 0, 0]),
        ],
      ),
    ],
  ),
  LiveDevice(
    id: idFromString('1.3')!,
    name: 'DAS',
    capabilities: Capability.subscriptionProvide,
    blocks: [
      LiveBlock(
        type: BlockType.resistiveMeasure.value,
        instance: 0,
        name: 'Resistive measurement',
        isDynamic: false,
        entries: [
          _liveEntry(4, 'Measured Value'),
          if (secondSource) _liveEntry(3, 'Raw'),
        ],
      ),
    ],
  ),
];

RestorePlan _plan({
  List<LiveDevice>? live,
  List<BackupRequesterSubscription>? subscriptions,
}) {
  final nodes = live ?? _live();
  return planFromSnapshot(
    devices: [_archive(subscriptions: subscriptions)],
    live: nodes,
    inventories: {
      for (final device in nodes)
        device.id: const LiveInventory(
          fileNames: {},
          loadedSlots: {},
          requesters: [],
          sndb: [],
        ),
    },
  );
}

void main() {
  tearDown(RestoreSession.clear);

  test(
    'a refused block-field write reports the FAIL code, the target and the address',
    () async {
      final bus = _RefusingBus()..refuse.add('register/3');
      installRequestChannel(bus);
      addTearDown(() => installRequestChannel(null));

      final report = await applyRestorePlan(_plan());

      final failure = report.reasons.first;
      // The device answered FLAG_FAIL with no payload - the firmware's whole answer.
      expect(failure.code, startsWith('FAIL (FLAG_FAIL 0x40'));
      expect(failure.code, contains('no reason payload'));
      expect(failure.kindWord, 'block field');
      expect(failure.target, 'Tamu (1.1)');
      expect(failure.subject, contains('PWM output "Fan" #0 / Duty [Key 0]'));
      expect(failure.meaning, isNotEmpty);
      expect(report.failed, report.reasons.length);
    },
  );

  test('a refused script write says which file and which code', () async {
    final bus = _RefusingBus()..refuse.add('storage/1'); // the staged create
    installRequestChannel(bus);
    addTearDown(() => installRequestChannel(null));

    final report = await applyRestorePlan(_plan());

    final script = report.reasons.firstWhere((r) => r.kindWord == 'script');
    expect(script.subject, contains('SCR_001'));
    expect(script.code, startsWith('FAIL (FLAG_FAIL 0x40'));
    expect(script.target, 'Tamu (1.1)');
    expect(script.meaning, isNotEmpty);
  });

  test(
    'a row the app refused itself says there is no device code, and why',
    () async {
      installRequestChannel(null);
      final report = await applyRestorePlan(
        planFromSnapshot(devices: [_archive()], live: const []),
      );

      expect(report.reasons, isNotEmpty);
      final failure = report.reasons.first;
      expect(failure.target, 'no target');
      expect(
        failure.code,
        'no code from the device (the app refused it: No target device)',
        reason: 'a blank is what the owner could not act on',
      );
      expect(failure.meaning, contains('never asked'));
    },
  );

  test('two requester rows on one device each carry their OWN answer', () async {
    // The subscription client keeps the FIRST fault until it is cleared, so without a clear
    // before each write item 2 reports item 1's code verbatim. The fake refuses the first write
    // with the firmware's answer and never answers the second, so a report that reuses one code
    // for both cannot pass this.
    final bus = _RefusingBus()
      ..requesterWriteAnswers.addAll([_RefusingBus.refusal, null]);
    installRequestChannel(bus);
    addTearDown(() => installRequestChannel(null));

    final report = await applyRestorePlan(
      _plan(
        live: _live(secondSource: true),
        subscriptions: [
          _subscription(),
          _subscription(sourceField: 3, sourceName: 'Raw'),
        ],
      ),
    );

    final rows = report.reasons
        .where((r) => r.kindWord == 'requester row')
        .toList();
    // Printed on purpose: the handoff quotes the text the owner would paste for this case.
    // ignore: avoid_print
    print('--- two requester rows ---\n${report.text}\n--- end ---');
    expect(rows, hasLength(2), reason: report.failures.join(' | '));
    final codes = [for (final row in rows) row.code];
    expect(
      codes.where((c) => c.startsWith('FAIL (FLAG_FAIL 0x40')),
      hasLength(1),
      reason: 'exactly one row was refused: $codes',
    );
    expect(
      codes.where((c) => c.contains('no reply')),
      hasLength(1),
      reason:
          'the row the device never answered says so in its own words: $codes',
    );
    expect(rows[0].subject, isNot(rows[1].subject));
  });

  test(
    'a requester row whose table read timed out reports the write\'s code',
    () async {
      // The table read and the write share one client and one trace. A read the device never answers
      // must not become the reason for a write the device refused: the reader would chase the wrong
      // cause.
      final bus = _RefusingBus()
        ..silent.add('subscriptions/16') // the requester table read
        ..refuse.add('subscriptions/17'); // the write itself
      installRequestChannel(bus);
      addTearDown(() => installRequestChannel(null));

      final report = await applyRestorePlan(_plan());

      final failure = report.reasons.firstWhere(
        (r) => r.kindWord == 'requester row',
      );
      expect(
        failure.code,
        startsWith('FAIL (FLAG_FAIL 0x40'),
        reason:
            'the code belongs to the write, not to the read that came before it',
      );
      expect(failure.code, isNot(contains('no reply')));
    },
  );

  test(
    'the copied text carries every reason, and the counts, and stays quotable',
    () async {
      final bus = _RefusingBus()
        ..refuse.addAll(['register/3', 'storage/1', 'subscriptions/17']);
      installRequestChannel(bus);
      addTearDown(() => installRequestChannel(null));

      final report = await applyRestorePlan(_plan());
      final text = report.text;
      // Printed on purpose: the handoff quotes what the owner would paste.
      // ignore: avoid_print
      print('--- copied restore report ---\n$text\n--- end ---');

      expect(text, contains('Restore report'));
      expect(text, contains('written ·'));
      expect(text, contains('Tamu (1.1)'));
      expect(text, contains('block field  PWM output "Fan" #0 / Duty [Key 0]'));
      expect(text, contains('code: FAIL (FLAG_FAIL 0x40, no reason payload)'));
      expect(text, contains('requester row'));
      expect(text, contains('why :'));
      // No line is wider than a phone: the report wraps its own rows rather than one long line.
      for (final line in text.split('\n')) {
        expect(
          line.length,
          lessThan(120),
          reason: 'too wide to read on a phone: $line',
        );
      }
    },
  );

  test(
    'the reasons survive the run: session result, metadata and diagnostics',
    () async {
      final bus = _RefusingBus()..refuse.add('register/3');
      installRequestChannel(bus);
      addTearDown(() => installRequestChannel(null));

      final report = await applyRestorePlan(_plan());

      // The session keeps the last run, so the dialog can be reopened after it was closed.
      expect(RestoreSession.last, same(report));

      // The metadata is what a card quotes: one record per failure, with the same fields.
      final metadata = report.metadata;
      expect(metadata['failed'], report.failed);
      final rows = metadata['failures']! as List<Object?>;
      expect(rows, hasLength(report.reasons.length));
      final row = rows.first! as Map<String, Object?>;
      expect(row['device'], 'Tamu (1.1)');
      expect(row['kind'], 'block field');
      expect(row['code'], startsWith('FAIL (FLAG_FAIL 0x40'));
      expect(row['meaning'], isNotEmpty);

      // And the diagnostics ring keeps the line, which the Logs page dumps and copies.
      expect(AppDiagnostics.dump(), contains('block field'));
    },
  );

  test('a run with no failures reports none - the control case', () async {
    final bus = _RefusingBus();
    installRequestChannel(bus);
    addTearDown(() => installRequestChannel(null));

    final report = await applyRestorePlan(_plan());

    expect(report.failed, 0, reason: report.failures.join(' | '));
    expect(report.reasons, isEmpty);
    expect(report.text, contains('Every selected item landed.'));
    expect(report.byDevice, isEmpty);
  });
}
