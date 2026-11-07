/// A restore driven through the app's REAL request layer - `ConnectionManager` and its
/// `RequestGate` - instead of a channel installed by a test.
///
/// Every other restore test installs a fake channel with `installRequestChannel`, which is the
/// layer BELOW the ones this card changed: the gate's bound, the connection's queue-timeout type
/// and the order the saves go out in are exactly what a fake channel cannot show. Here the device
/// is a transport: the test frames come in over `packetStream`, go through the connection's
/// transaction layer (TRID routing, the gate, the deadline) and come back as real reply frames.
///
/// Two findings are pinned:
///
///  * **The saves go one device at a time.** The device forwards ONE request at a time, so four
///    Save Alls fired together spend three of their four 25 s windows queued; with the fake
///    holding each save open for [_saveHold], a concurrent phase shows FOUR saves on the wire and
///    a sequential one shows exactly one.
///  * **A request that never got a slot is a busy link, not an unanswered device.** With every
///    gate slot pinned by earlier requests, the restore's request expires on the queue bound; the
///    report must say the app never got its turn (`RequestFaultKind.busy`) instead of reporting a
///    node that did not answer.
library;

import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/transport.dart';
import 'package:tamuapp/core/types.dart';

const _link = DiscoveredLink(
  id: 'AA:BB:CC:DD:EE:02',
  type: LinkType.ble,
  name: 'Tamu core',
);

/// How long the fake device holds a Save All before answering it: long enough that two saves
/// issued together overlap on the wire, short enough for a unit test.
const _saveHold = Duration(milliseconds: 40);

/// A device at the other end of a transport: it parses the app's frames and answers them.
///
/// It answers the way the firmware does for the two commands a restore needs - a register write
/// echoes the ValueInfo it stored, Save All is answered after [_saveHold] - and counts how many
/// Save Alls are on the wire at once, which is what tells a concurrent save phase from a serial
/// one.
class _FakeDevice implements Transport {
  final _packets = StreamController<Uint8List>.broadcast();
  final _parser = PacketStreamParser();

  /// Every request frame the app sent, in order.
  final sent = <({int deviceId, ServiceType service, int cid})>[];

  /// Commands answered with nothing at all, as `service/cid`: a request that sits in the link for
  /// the whole of its own deadline.
  final silent = <String>{};

  int savesOnWire = 0;
  int mostSavesOnWire = 0;

  @override
  String get id => _link.id;

  @override
  String get displayName => 'BLE ${_link.id}';

  @override
  Stream<Uint8List> get linkBytes => _packets.stream;

  @override
  Stream<Uint8List> get packetStream => _packets.stream;

  @override
  Future<void> connect() async {}

  @override
  Future<void> close() async {
    if (!_packets.isClosed) await _packets.close();
  }

  @override
  Future<void> send(List<int> streamBytes) async {
    for (final frame in _parser.feed(Uint8List.fromList(streamBytes))) {
      if (frame.isResponse) continue;
      final service = serviceTypeOf(frame.srvTarget);
      if (service == null) continue;
      final cid = serviceCidOf(frame.srvTarget);
      sent.add((deviceId: frame.idTarget, service: service, cid: cid));
      if (silent.contains('${service.name}/$cid')) continue;
      final isSave = service == ServiceType.register && cid == 5;
      if (isSave) {
        savesOnWire++;
        if (savesOnWire > mostSavesOnWire) mostSavesOnWire = savesOnWire;
      }
      unawaited(_answer(frame, isSave: isSave));
    }
  }

  Future<void> _answer(PacketFrame request, {required bool isSave}) async {
    if (isSave) await Future<void>.delayed(_saveHold);
    if (isSave) savesOnWire--;
    if (_packets.isClosed) return;
    final service = serviceTypeOf(request.srvTarget);
    final cid = serviceCidOf(request.srvTarget);
    var payload = const <int>[];
    if (service == ServiceType.register &&
        cid == 3 &&
        request.payload.length >= 8) {
      // An accepted write echoes the ValueInfo of the row it stored.
      payload = request.payload.sublist(0, 8);
    }
    final reply = PacketFrame(
      flags: flagType | flagSuccess | flagStart | flagStop,
      priority: defaultPriority,
      idTarget: request.idSource,
      idSource: request.idTarget,
      srvTarget: request.srvTarget,
      srvSource: request.srvSource,
      payload: Uint8List.fromList(payload),
    );
    _packets.add(reply.toBytes());
  }

  /// The Save All frames the device was sent, in wire order.
  List<int> get saveOrder => [
    for (final request in sent)
      if (request.service == ServiceType.register && request.cid == 5)
        request.deviceId,
  ];
}

BackupDevice _archive(String address, String name) => BackupDevice(
  type: 'Tamu v2.0A',
  typeId: 1,
  id: idFromString(address)!,
  name: name,
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
  scripts: const [],
  files: const [],
  layouts: const [],
  requesterSubscriptions: const [],
);

LiveDevice _live(String address, String name) => LiveDevice(
  id: idFromString(address)!,
  name: name,
  blocks: [
    LiveBlock(
      type: BlockType.pwm.value,
      instance: 0,
      name: 'Fan',
      isDynamic: false,
      entries: [
        LiveEntry(
          field: 1,
          key: 0,
          fieldName: 'Duty',
          keyName: 'Key 0',
          meta: ValueInfo(type: DataType.number.value, size: 4),
          value: const [0, 0, 0, 0],
        ),
      ],
    ),
  ],
);

/// A restore plan for [addresses], each with one archive and one live node.
RestorePlan _planFor(List<(String, String)> devices) {
  final nodes = [for (final (address, name) in devices) _live(address, name)];
  return planFromSnapshot(
    devices: [for (final (address, name) in devices) _archive(address, name)],
    live: nodes,
    inventories: {
      for (final node in nodes)
        node.id: const LiveInventory(
          fileNames: {},
          loadedSlots: {},
          requesters: [],
          sndb: [],
        ),
    },
  );
}

Future<ConnectionManager> _connected(_FakeDevice device) async {
  final manager = ConnectionManager.forTest();
  manager.transportFactory = (_) => device;
  await manager.connectTo(_link);
  return manager;
}

/// A request expected to fail, without letting the failure escape.
Future<Object?> _failure(Future<void> future) async {
  try {
    await future;
    return null;
  } catch (error) {
    return error;
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  tearDown(RestoreSession.clear);

  test(
    'the Save Alls of a four-device restore go one at a time, on the wire',
    () async {
      final device = _FakeDevice();
      final manager = await _connected(device);
      installRequestChannel(manager);
      addTearDown(() => installRequestChannel(null));

      final report = await applyRestorePlan(
        _planFor([
          ('1.1', 'Tamu'),
          ('2.1', 'DAS A'),
          ('3.1', 'DAS B'),
          ('4.1', 'DAS C'),
        ]),
      );

      // The device forwards ONE request at a time, so a save phase that fires them together spends
      // every window but the first one queued (which is how a finished save was reported as
      // "RAM only"). Sequential: exactly one save is ever on the wire.
      expect(
        device.mostSavesOnWire,
        1,
        reason: 'four saves were outstanding at once before this fix',
      );
      expect(device.saveOrder, hasLength(4), reason: 'one save per device');
      expect(device.saveOrder, [
        for (final address in ['1.1', '2.1', '3.1', '4.1'])
          idFromString(address),
      ], reason: 'saved in address order, one after the other');
      expect(
        report.failures.where((line) => line.contains('RAM only')),
        isEmpty,
        reason: report.failures.join(' | '),
      );
      expect(report.failed, 0, reason: report.failures.join(' | '));
      await manager.disconnect();
    },
  );

  test(
    'a request that never got a gate slot reports a busy link, not a dead node',
    () async {
      // Every slot is held by earlier requests that the device never answers, so anything the
      // restore sends waits out `ConnectionManager.requestQueueTimeout` and is refused - a device
      // verdict the app never measured, which used to reach the report as `RequestFaultKind.noReply`
      // ("the device did not answer").
      final device = _FakeDevice()..silent.add('register/0');
      final manager = await _connected(device);
      installRequestChannel(manager);
      addTearDown(() => installRequestChannel(null));

      final pinned = [
        for (var i = 0; i < ConnectionManager.maxInFlightRequests; i++)
          _failure(
            manager.request(
              1,
              ServiceType.register,
              0,
              timeout: const Duration(seconds: 30),
            ),
          ),
      ];
      await Future<void>.delayed(const Duration(milliseconds: 50));
      expect(
        device.sent.where((r) => r.cid == 0),
        hasLength(ConnectionManager.maxInFlightRequests),
        reason: 'the whole budget is parked in the link',
      );

      final report = await applyRestorePlan(_planFor([('1.1', 'Tamu')]));

      expect(report.reasons, isNotEmpty);
      final failure = report.reasons.first;
      // The code names the LINK: the request never left the host.
      expect(failure.code, contains('never got a slot on the link'));
      expect(
        failure.code,
        isNot(contains('no reply')),
        reason: 'the device was never asked, so it cannot be called silent',
      );
      expect(failure.code, contains('were still held by earlier requests'));

      await manager.disconnect();
      // The parked requests are answered by the teardown, not left pending past the test.
      expect(await Future.wait(pinned), everyElement(isNotNull));
    },
  );
}
