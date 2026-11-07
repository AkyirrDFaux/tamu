/// The request layer survives a platform call that never returns, and one lost fragment does not
/// turn into three more bursts at the resource that lost it.
///
/// The two mechanisms this file pins, both of them from the owner's "unreliable and slower than
/// it used to be":
///
///  * **The deadline covers the SEND.** `requestWithFlags` used to await `transport.send(...)` and
///    only then arm `.timeout`, so a write that never returned parked the request before any
///    deadline existed - and the request's slot with it. Eight of those wedged the link for good:
///    every later request waited in `acquire()` with no error, no log, and no way out but a
///    restart. `BleTransport.send` awaits `UniversalBle.write(withoutResponse: false)`, which is
///    exactly a platform call that can hang (added with the SNDB burst, commit 7cd66ef).
///  * **A truncated reply is reported, not re-burst.** Storage CID 5 asks a file BY NAME and has
///    no offset form, so a second read is the whole file again, into the ring that just dropped
///    fragments. The stall watchdog also started at the first fragment, so a reply whose FIRST
///    fragment was lost armed nothing and waited out the 10 s read timeout - three times.
library;

import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/request_gate.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/transport.dart';

const _link = DiscoveredLink(
  id: 'AA:BB:CC:DD:EE:02',
  type: LinkType.ble,
  name: 'Tamu core',
);

/// A link whose `send` can be held open forever: the platform write that never returns.
///
/// Nothing is written into [linkBytes]/[packetStream] unless a test asks for it, so a request sent
/// through this transport is a device that answers nothing at all.
class _StallingTransport implements Transport {
  /// When true, [send] returns a future that never completes.
  bool stall = false;

  /// Every frame handed to [send].
  final frames = <List<int>>[];

  final _packets = StreamController<Uint8List>.broadcast();

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
  Future<void> send(List<int> streamBytes) {
    frames.add(streamBytes);
    if (stall) return Completer<void>().future; // never completes
    return Future<void>.value();
  }

  @override
  Future<void> close() async {
    if (!_packets.isClosed) await _packets.close();
  }
}

/// The outcome of a request that is expected to fail, without letting the failure escape.
Future<Object?> _failure(Future<void> future) async {
  try {
    await future;
    return null;
  } catch (error) {
    return error;
  }
}

Future<ConnectionManager> _connected(_StallingTransport transport) async {
  final manager = ConnectionManager.forTest();
  manager.transportFactory = (_) => transport;
  await manager.connectTo(_link);
  return manager;
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('a platform write that never returns', () {
    test('fails on the request deadline instead of parking forever', () async {
      final transport = _StallingTransport();
      final manager = await _connected(transport);
      transport.stall = true;

      final started = DateTime.now();
      final error = await _failure(
        manager.request(
          1,
          ServiceType.register,
          0,
          timeout: const Duration(milliseconds: 300),
        ),
      );
      final elapsed = DateTime.now().difference(started);

      expect(error, isA<TransportException>());
      expect('$error', contains('request timed out'));
      expect(
        elapsed,
        lessThan(const Duration(seconds: 2)),
        reason: 'the deadline covers the send, so it fires at 300 ms',
      );
      expect(
        transport.frames,
        hasLength(1),
        reason: 'the frame really was handed to the platform write',
      );
      await manager.disconnect();
    });

    test('eight of them do not wedge the link for the next request', () async {
      final transport = _StallingTransport();
      final manager = await _connected(transport);
      transport.stall = true;

      // The gate's whole budget (ConnectionManager.maxInFlightRequests), every one of them
      // parked inside a send that never returns.
      final parked = [
        for (var i = 0; i < ConnectionManager.maxInFlightRequests; i++)
          _failure(
            manager.request(
              1,
              ServiceType.register,
              i,
              timeout: const Duration(milliseconds: 200),
            ),
          ),
      ];
      final errors = await Future.wait(parked);
      expect(errors, everyElement(isA<TransportException>()));
      expect(
        errors.map((error) => '$error'),
        everyElement(contains('request timed out')),
        reason: 'none of them was refused for want of a slot',
      );

      // The link answers again - and the whole budget is available: a second round of eight
      // parked requests plus a ninth that queues behind them must behave exactly like the first
      // round (each fails on its own deadline). A leaked slot shows up here as the ninth waiting
      // out the queue bound and failing with "the link is busy" 5 s later instead.
      final started = DateTime.now();
      final second = await Future.wait([
        for (var i = 0; i < ConnectionManager.maxInFlightRequests + 1; i++)
          _failure(
            manager.request(
              1,
              ServiceType.register,
              i,
              timeout: const Duration(milliseconds: 200),
            ),
          ),
      ]);
      expect(
        DateTime.now().difference(started),
        lessThan(const Duration(seconds: 3)),
        reason: 'no request waited for a slot that never came back',
      );
      expect(
        second.map((error) => '$error'),
        everyElement(contains('request timed out')),
        reason: 'the ninth was admitted, not refused by the gate',
      );
      await manager.disconnect();
    });

    test('a disconnect takes the slots back from requests still in a send', () async {
      final transport = _StallingTransport();
      final manager = await _connected(transport);
      transport.stall = true;

      // Requests with a long deadline: without the teardown they would hold every slot for it.
      final parked = [
        for (var i = 0; i < ConnectionManager.maxInFlightRequests; i++)
          _failure(
            manager.request(
              1,
              ServiceType.register,
              i,
              timeout: const Duration(seconds: 5),
            ),
          ),
      ];
      await Future<void>.delayed(const Duration(milliseconds: 50));
      expect(
        transport.frames,
        hasLength(ConnectionManager.maxInFlightRequests),
      );

      await manager.disconnect();

      // A fresh session must get the whole budget back at once, not wait out the old deadlines.
      final again = _StallingTransport();
      final manager2 = await _connected(again);
      final started = DateTime.now();
      final error = await _failure(
        manager2.request(
          1,
          ServiceType.register,
          0,
          timeout: const Duration(milliseconds: 200),
        ),
      );
      expect(
        DateTime.now().difference(started),
        lessThan(const Duration(milliseconds: 500)),
        reason: 'disconnect.finally released the parked slots',
      );
      expect('$error', contains('request timed out'));

      // The requests still parked in their sends were failed by the teardown itself, not left
      // waiting for their own 5 s deadlines.
      final abandoned = await Future.wait(parked);
      expect(abandoned, everyElement(isA<TransportException>()));
      expect(
        abandoned.map((error) => '$error'),
        everyElement(contains('Disconnected')),
        reason: 'the teardown answers the requests it abandons',
      );
      await manager2.disconnect();
    });
  });

  group('a reply that never starts arriving', () {
    test(
      'a tolerant read is closed by the stall watchdog armed at the send',
      () async {
        final transport = _StallingTransport();
        final manager = await _connected(transport);

        final started = DateTime.now();
        final reply = await manager.requestTolerant(
          1,
          ServiceType.storage,
          5,
          payload: StorageClient.padName('.TABLE  '),
          timeout: const Duration(seconds: 10),
        );
        final elapsed = DateTime.now().difference(started);

        // The watchdog starts with the send, so the "no fragment at all" case is reportable in the
        // stall window rather than at the request's own 10 s deadline.
        expect(
          elapsed,
          lessThan(ConnectionManager.fragmentStallTimeout * 2),
          reason: 'closed by the stall watchdog, not by the 10 s read timeout',
        );
        expect(
          reply.partial,
          isTrue,
          reason: 'not whole: nothing of it arrived',
        );
        expect(reply.chunks, isEmpty);
        expect(reply.stopped, isFalse, reason: 'no STOP frame ever came');
        expect(reply.gap!.message, contains('no part of the reply'));
        await manager.disconnect();
      },
    );

    test(
      'a whole-file read sends ONCE and names the hole and the count',
      () async {
        final transport = _StallingTransport();
        final manager = await _connected(transport);
        installRequestChannel(manager);
        addTearDown(() => installRequestChannel(null));

        final started = DateTime.now();
        final read = await StorageClient(deviceId: 1).readFileTableResult();
        final elapsed = DateTime.now().difference(started);

        expect(read.records, isNull, reason: 'a partial read is never data');
        expect(
          transport.frames,
          hasLength(1),
          reason: 'one send: a reply that arrived incomplete is not re-burst',
        );
        expect(read.attempts, 1);
        expect(read.detail, contains('no part of the reply'));
        expect(read.detail, contains('asked 1 time(s)'));
        expect(
          elapsed,
          lessThan(ConnectionManager.fragmentStallTimeout * 2),
          reason:
              'the read failed in the stall window, not in three 10 s timeouts',
        );
        await manager.disconnect();
      },
    );
  });

  test('the gate hands its budget to the next session', () async {
    // The pure counterpart of the disconnect test above: a slot taken before a teardown owns
    // nothing afterwards, so its late release cannot eat a live request's slot.
    final gate = RequestGate(1);
    final stale = await gate.acquire();
    gate.releaseAll(const TransportException('Disconnected'));
    expect(gate.generation, 1);
    expect(gate.inFlight, 0);

    final live = await gate.acquire();
    expect(gate.inFlight, 1);
    stale.release();
    expect(gate.inFlight, 1, reason: 'the stale slot gave nothing back');
    live.release();
    expect(gate.inFlight, 0);
  });
}
