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
import 'package:tamuapp/core/unsaved_settings.dart';

import 'support/fake_bus.dart';
import 'support/fake_core.dart';
import 'package:tamuapp/core/types.dart';

/// A bus that answers - or refuses - the requests a restore sends, so a failing restore can be
/// driven without a rig.
///
/// A refusal is the firmware's: `FLAG_FAIL` with no payload. [refuse] names the commands to refuse
/// as `service/cid` (e.g. `register/3`, `storage/1`), and everything else is answered the way the
/// device answers it.
class _RefusingBus extends FakeBus {
  _RefusingBus({
    Map<int, int> loaded = const {},
    Map<String, List<int>> files = const {},
  }) : loaded = {...loaded},
       files = {...files};

  /// The commands refused, as `service/cid`.
  final refuse = <String>{};

  /// The commands the device never answers at all, as `service/cid` - a timeout.
  final silent = <String>{};

  /// The commands the device drops the FIRST time and answers afterwards, as `service/cid` - the
  /// shape of a lost reply that a re-send recovers.
  final silentOnce = <String>{};

  /// Answers for the requester writes (`subscriptions/17`), consumed in order. `null` is an entry
  /// the device never answers; an empty queue means the write is accepted, the normal answer.
  ///
  /// Two requester rows in one run need two different answers before the report can be asked
  /// whether each row carries its OWN reason instead of the one before it.
  final requesterWriteAnswers = <PacketResponse?>[];

  /// The firmware's refusal, to queue in [requesterWriteAnswers]: FLAG_FAIL and no payload.
  static const refusal = busRefused;

  /// Loaded script slots: slot -> SCR file id.
  final Map<int, int> loaded;

  /// Block fields the device holds, by BlockInfo word (`makeBlockInfo`) -> value bytes.
  ///
  /// A read of one of these answers the address, its ValueInfo and the value, which is the shape
  /// `RegisterClient.readBlockField` decodes; a read of any other address answers bare success,
  /// i.e. nothing a caller can use. That is what lets a case tell "the app read the panel's field
  /// back" from "the app assumed it".
  final Map<int, List<int>> fieldValues = {};

  /// The names the device's file table lists.
  final tableNames = <String>[];

  /// The files the device stores.
  final Map<String, List<int>> files;

  bool _refuses(ServiceType service, int cid) =>
      refuse.contains('${service.name}/$cid');

  @override
  Future<PacketResponse> flagged(
    int targetId,
    ServiceType service,
    int functionCid,
    List<int> payload,
  ) async {
    // A command the device never answers is a timeout, not a refusal: the two arrive at the app
    // as different reasons, so a case can tell them apart.
    final command = '${service.name}/$functionCid';
    if (silent.contains(command)) throw TimeoutException('$command: no reply');
    if (silentOnce.remove(command)) {
      throw TimeoutException('$command: no reply');
    }
    if (service == ServiceType.subscriptions &&
        functionCid == 0x11 &&
        requesterWriteAnswers.isNotEmpty) {
      final answer = requesterWriteAnswers.removeAt(0);
      if (answer == null) throw TimeoutException('$command: no reply');
      return answer;
    }
    if (_refuses(service, functionCid)) return busRefused;
    if (service == ServiceType.register) {
      switch (functionCid) {
        case RegisterCid.read:
          final held = payload.length < 4
              ? null
              : fieldValues[bytesToInt(payload.sublist(0, 4))];
          if (held == null) return busAccepted;
          // The reply a field read gives: the BlockInfo, then its ValueInfo, then the value.
          return PacketResponse(
            payload: [
              ...payload.sublist(0, 4),
              DataType.bool_.value,
              0,
              held.length,
              ValueFlags.persistent,
              ...held,
            ],
            success: true,
            fail: false,
          );
        case RegisterCid.write:
          // An accepted write echoes the address, the ValueInfo and the value.
          return registerWriteEcho(payload);
        case RegisterCid.saveAll:
        case RegisterCid.recallAll:
          return busAccepted;
      }
      return busAccepted;
    }
    if (service == ServiceType.storage) {
      switch (functionCid) {
        case 5: // read a file
          final name = StorageClient.unpadName(payload.sublist(0, 8));
          if (name == '.TABLE') {
            return PacketResponse(
              payload: tableReply([
                ...tableNames,
              ], (name) => files[name]?.length ?? 0),
              success: true,
              fail: false,
            );
          }
          final content = files[name];
          if (content == null) return busRefused;
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
          return busAccepted;
      }
    }
    if (service == ServiceType.script) {
      if (functionCid == 0) {
        return PacketResponse(
          payload: scriptSlotMap(loaded, slots: 8),
          success: true,
          fail: false,
        );
      }
      return busAccepted;
    }
    return busAccepted;
  }
}

/// A Save All policy with test-sized windows.
///
/// The production policy waits 25 s for a real save and 2 s for a node to settle (flash page
/// erases outlast any ordinary request); a unit test cannot spend that, so it installs this and
/// the policy is restored when the test ends.
void installFastSavePolicy() {
  final previous = restoreSavePolicy;
  restoreSavePolicy = const RestoreSavePolicy(
    window: Duration(milliseconds: 20),
    settle: Duration(milliseconds: 5),
    probeWindow: Duration(milliseconds: 20),
    resendWindow: Duration(milliseconds: 20),
  );
  addTearDown(() => restoreSavePolicy = previous);
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

/// The address of the panel's pending field: the BLE Settings block's own `Enabled`.
///
/// The settings panels write a persistent field and hold it in `core/unsaved_settings.dart` until a
/// Save All confirms it; this is the same write the Bluetooth panel makes.
int get _enabledAddress => makeBlockInfo(BlockType.bleSettings.value, 0, 0, 0);

PendingSetting _pendingEnabled() => PendingSetting(
  deviceId: idFromString('1.1')!,
  blockName: 'Bluetooth',
  blockType: BlockType.bleSettings.value,
  inst: 0,
  field: 0,
  fieldName: 'Enabled',
  meta: ValueInfo(type: DataType.bool_.value, size: 1),
  bytes: const [1],
);

/// How many times the run READ the panel's field: a field read's payload is its 4-byte BlockInfo.
///
/// This is the difference between "the app read the field back" and "the app assumed it", which is
/// the whole point of the confirmation.
int _readsOf(_RefusingBus bus, int address) => bus.sent
    .where(
      (r) =>
          r.service == ServiceType.register &&
          r.cid == RegisterCid.read &&
          r.payload.length == 4 &&
          bytesToInt(r.payload) == address,
    )
    .length;

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

  test(
    'a Save All the device never answers is re-sent once the node answers again',
    () async {
      // Save All is what puts the run's writes in flash: a save the device never confirms used to
      // be dropped at the call site, so a restore that stayed in RAM read as applied (measured on
      // the rig over BLE: three device saves in one run ran out their timeout and the report still
      // said every item landed).
      //
      // A timeout is not proof the save was lost, so the re-send waits for the node to answer a
      // light read first: this bus answers everything except Save All, so the re-send is allowed.
      final bus = _RefusingBus()..silent.add('register/5');
      installRequestChannel(bus);
      addTearDown(() => installRequestChannel(null));
      installFastSavePolicy();

      final report = await applyRestorePlan(_plan());

      expect(
        report.written,
        greaterThan(0),
        reason: 'the items themselves landed',
      );
      final save = report.reasons.singleWhere((r) => r.kindWord == 'save all');
      expect(save.subject, contains('Save All'));
      expect(save.target, 'Tamu (1.1)');
      expect(save.code, contains('no reply'));
      expect(save.meaning, contains('RAM only'));
      expect(
        save.meaning,
        contains('re-sent'),
        reason: 'the node answered a status read, so the save was asked again',
      );
      expect(
        bus.sent
            .where(
              (r) =>
                  r.service == ServiceType.register &&
                  r.cid == RegisterCid.saveAll,
            )
            .length,
        2,
        reason: 'one re-send, and only after the node proved it was answering',
      );
    },
  );

  test('a Save All against a silent node is NOT re-sent blindly', () async {
    // The old policy was "25 s, then 250 ms later 5 s more": a second Save All launched on top of
    // a run that was very likely still going - two saves through one link, and 250 ms is far too
    // soon to know anything. Now the timeout buys ONE light read: a node that answers it is not
    // busy (so its save is not still running, and a re-send is safe); a node that does not answer
    // is either still saving or gone, and in both cases a second save is the wrong thing to send.
    final bus = _RefusingBus()..silent.addAll({'register/5', 'register/2'});
    installRequestChannel(bus);
    addTearDown(() => installRequestChannel(null));
    installFastSavePolicy();

    final report = await applyRestorePlan(_plan());

    final save = report.reasons.singleWhere((r) => r.kindWord == 'save all');
    expect(save.meaning, contains('RAM only'));
    expect(
      save.meaning,
      contains('not re-sent'),
      reason: 'the report says the save may still be running on the node',
    );
    expect(
      bus.sent
          .where(
            (r) =>
                r.service == ServiceType.register &&
                r.cid == RegisterCid.saveAll,
          )
          .length,
      1,
      reason:
          'one Save All, and no second burst at the node that never answered',
    );
    expect(
      bus.sent.where(
        (r) => r.service == ServiceType.register && r.cid == RegisterCid.read,
      ),
      hasLength(1),
      reason: 'the status read is one send, not a retry policy worth of them',
    );
  });

  test(
    'a Save All whose first reply is lost is recovered, and nothing is reported',
    () async {
      final bus = _RefusingBus()..silentOnce.add('register/5');
      installRequestChannel(bus);
      addTearDown(() => installRequestChannel(null));
      installFastSavePolicy();

      final report = await applyRestorePlan(_plan());

      expect(
        report.reasons.where((r) => r.kindWord == 'save all'),
        isEmpty,
        reason: report.failures.join(' | '),
      );
      expect(
        bus.sent
            .where(
              (r) =>
                  r.service == ServiceType.register &&
                  r.cid == RegisterCid.saveAll,
            )
            .length,
        2,
      );
    },
  );

  test('a Save All that lands confirms the panel\'s pending write', () async {
    // The run's own Save All is the same CID 5 a settings panel's "Save all" sends, and the
    // restore writes the same fields - so the panel's write can be durable now while its entry
    // still says "Not saved". The run confirms it the way the register view's button does: the
    // field is READ BACK, and the entry leaves the store only because the device holds it.
    final bus = _RefusingBus()..fieldValues[_enabledAddress] = const [1];
    installRequestChannel(bus);
    addTearDown(() => installRequestChannel(null));
    final store = UnsavedSettings.forTest()..mark(_pendingEnabled());
    final device = idFromString('1.1')!;

    final report = await applyRestorePlan(_plan(), unsavedStore: store);

    expect(
      report.reasons.where((r) => r.kindWord == 'save all'),
      isEmpty,
      reason: report.failures.join(' | '),
    );
    expect(
      store.isDirty(device),
      isFalse,
      reason: 'the save made it durable: ${store.describe(device)}',
    );
    expect(store.of(device), isEmpty);
    expect(_readsOf(bus, _enabledAddress), 1);
  });

  test('a refused Save All keeps the panel\'s write in place', () async {
    // A save that does not land is not a save: the entry must survive it, and the run must not
    // read the field back at all (the confirmation is not a guess that the write stuck).
    final bus = _RefusingBus()
      ..refuse.add('register/5')
      ..fieldValues[_enabledAddress] = const [1];
    installRequestChannel(bus);
    addTearDown(() => installRequestChannel(null));
    installFastSavePolicy();
    final store = UnsavedSettings.forTest()..mark(_pendingEnabled());
    final device = idFromString('1.1')!;

    final report = await applyRestorePlan(_plan(), unsavedStore: store);

    expect(report.reasons.where((r) => r.kindWord == 'save all'), hasLength(1));
    expect(store.isDirty(device), isTrue);
    expect(store.describe(device), 'Enabled');
    expect(_readsOf(bus, _enabledAddress), 0);
  });

  test('a landed save whose field reads back different keeps the entry', () async {
    // Save All answered, but the device holds another value for the panel's field: the entry is
    // exactly the thing that must not be dropped, because the value it names is not on the device.
    final bus = _RefusingBus()..fieldValues[_enabledAddress] = const [0];
    installRequestChannel(bus);
    addTearDown(() => installRequestChannel(null));
    final store = UnsavedSettings.forTest()..mark(_pendingEnabled());
    final device = idFromString('1.1')!;

    final report = await applyRestorePlan(_plan(), unsavedStore: store);

    expect(report.reasons.where((r) => r.kindWord == 'save all'), isEmpty);
    expect(store.isDirty(device), isTrue);
    expect(store.describe(device), 'Enabled');
    expect(_readsOf(bus, _enabledAddress), 1);
  });

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
