/// The three reporting sites that used to flatten a queue expiry into "the device did not answer".
///
/// `RequestQueueTimeout` is the link's own verdict: every gate slot was held for the whole queue
/// bound, so not one frame went out. Each site below reports why an operation failed by recording a
/// `RequestFault` on its client's trace, and the restore report renders that fault's KIND
/// (`restoreCodeText`) - so recording `noReply` there printed "no reply: the link was busy holding
/// other requests ... so this read was never sent", a sentence that contradicts its own label.
///
/// One test per site, each FEEDING a `RequestQueueTimeout`: the storage status path (create /
/// delete / rename / resize / format), a whole-file read, and a subscriptions request. The read one
/// goes through the app's real request layer (`ConnectionManager` + `RequestGate`) with every slot
/// pinned, because a fake channel cannot produce the gate's expiry - the same trick
/// `restore_over_gate_test.dart` uses.
library;

import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/request_gate.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/subscription_client.dart';
import 'package:tamuapp/core/transport.dart';
import 'package:tamuapp/core/types.dart';

import 'support/fake_bus.dart';

const _link = DiscoveredLink(
  id: 'AA:BB:CC:DD:EE:02',
  type: LinkType.ble,
  name: 'Tamu core',
);

/// A link whose requests never got a slot: it throws the gate's own expiry, or - once
/// [queueBound] is off - the ordinary transport failure that keeps meaning [noReply].
class _QueueBoundChannel extends FakeBus {
  bool queueBound = true;

  RequestQueueTimeout _expiry() => const RequestQueueTimeout(
    waited: Duration(seconds: 5),
    inFlight: 8,
    limit: 8,
  );

  Future<T> _refuse<T>() async {
    if (queueBound) throw _expiry();
    throw StateError('the frame was never acknowledged');
  }

  @override
  Future<List<int>> plain(
    int targetId,
    ServiceType service,
    int functionCid,
    List<int> payload,
  ) => _refuse();

  @override
  Future<PacketResponse> flagged(
    int targetId,
    ServiceType service,
    int functionCid,
    List<int> payload,
  ) => _refuse();
}

/// A device at the other end of a transport that answers NOTHING: every request it is sent stays
/// on the wire for its own deadline. That is what pins the gate's slots for the read test.
class _SilentDevice implements Transport {
  final _packets = StreamController<Uint8List>.broadcast();
  final _parser = PacketStreamParser();

  /// Every request frame the app got as far as sending, in order.
  final sent = <({int deviceId, ServiceType service, int cid})>[];

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
      sent.add((
        deviceId: frame.idTarget,
        service: service,
        cid: serviceCidOf(frame.srvTarget),
      ));
    }
  }
}

/// A request expected to fail, without letting the failure escape.
Future<Object?> _failure(Future<Object?> future) async {
  try {
    return await future;
  } catch (error) {
    return error;
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test(
    'a storage status command that never got a slot is a busy link, not a reply',
    () async {
      final previous = requestChannel;
      final channel = _QueueBoundChannel();
      installRequestChannel(channel);
      final client = StorageClient(deviceId: idFromString('1.1')!);
      addTearDown(() => installRequestChannel(previous));

      // create/delete/rename/resize/format all report through `_status`; delete is the shortest.
      expect(await client.deleteFile('A'), isFalse);
      expect(client.trace.fault, isNotNull);
      expect(
        client.trace.fault!.kind,
        RequestFaultKind.busy,
        reason: 'the command never left the host',
      );
      final code = restoreCodeText(fault: client.trace.fault);
      expect(code, contains('never got a slot on the link'), reason: code);
      expect(
        code,
        isNot(contains('no reply')),
        reason: 'the device was never asked, so it cannot be called silent',
      );

      // Every other throw keeps the unanswered-device label.
      client.trace.clear();
      channel.queueBound = false;
      expect(await client.deleteFile('A'), isFalse);
      expect(
        client.trace.fault!.kind,
        RequestFaultKind.noReply,
        reason: 'only the queue bound is not a device verdict',
      );
    },
  );

  test(
    'a whole-file read that never got a slot records a busy link, not a reply',
    () async {
      final device = _SilentDevice();
      final manager = ConnectionManager.forTest();
      manager.transportFactory = (_) => device;
      await manager.connectTo(_link);
      final previous = requestChannel;
      installRequestChannel(manager);
      final client = StorageClient(deviceId: idFromString('1.1')!);
      addTearDown(() => installRequestChannel(previous));

      // Pin every gate slot with a request the device never answers, so the read below waits out
      // `ConnectionManager.requestQueueTimeout` and is never sent.
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
        device.sent.where((request) => request.cid == 0),
        hasLength(ConnectionManager.maxInFlightRequests),
        reason: 'the whole budget is parked in the link',
      );

      final read = await client.readFileWhole('.TABLE');
      expect(read.failed, isTrue);
      // The read's own sentence already said it was never sent; the recorded kind must agree.
      expect(read.detail, contains('was never sent'), reason: read.detail);
      expect(client.trace.fault, isNotNull);
      expect(
        client.trace.fault!.kind,
        RequestFaultKind.busy,
        reason: read.detail,
      );
      final code = restoreCodeText(fault: client.trace.fault);
      expect(code, contains('never got a slot on the link'), reason: code);
      expect(
        code,
        isNot(contains('no reply')),
        reason:
            'the busy sentence and the label must not contradict each other',
      );

      await manager.disconnect();
      // The parked requests are failed by the teardown, not left pending past the test.
      expect(await Future.wait(pinned), everyElement(isNotNull));
    },
  );

  test(
    'a subscriptions request that never got a slot is a busy link, not a reply',
    () async {
      final previous = requestChannel;
      final channel = _QueueBoundChannel();
      installRequestChannel(channel);
      final client = SubscriptionClient(deviceId: idFromString('39.1')!);
      addTearDown(() {
        installRequestChannel(previous);
        client.dispose();
      });

      expect(await client.readRequesterSubscriptions(), isNull);
      expect(client.trace.fault, isNotNull);
      expect(
        client.trace.fault!.kind,
        RequestFaultKind.busy,
        reason: 'the table read never left the host',
      );
      final code = restoreCodeText(fault: client.trace.fault);
      expect(code, contains('never got a slot on the link'), reason: code);
      expect(
        code,
        isNot(contains('no reply')),
        reason: 'the device was never asked, so it cannot be called silent',
      );

      // Every other throw keeps the unanswered-device label.
      client.trace.clear();
      channel.queueBound = false;
      expect(await client.readRequesterSubscriptions(), isNull);
      expect(client.trace.fault!.kind, RequestFaultKind.noReply);
    },
  );
}
