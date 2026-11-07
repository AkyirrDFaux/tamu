/// The request layer survives a platform call that never returns, and a reply's head is not
/// mistaken for silence.
///
/// The mechanisms this file pins, all of them from the owner's "unreliable and slower than it
/// used to be" and the second-pass review of the fix that followed:
///
///  * **The deadline covers the SEND.** `requestWithFlags` used to await `transport.send(...)` and
///    only then arm `.timeout`, so a write that never returned parked the request before any
///    deadline existed - and the request's slot with it. Eight of those wedged the link for good:
///    every later request waited in `acquire()` with no error, no log, and no way out but a
///    restart. `BleTransport.send` awaits `UniversalBle.write(withoutResponse: false)`, which is
///    exactly a platform call that can hang (added with the SNDB burst, commit 7cd66ef).
///  * **A head is not a hole.** The stall watchdog is a STREAM watchdog: it is armed by the first
///    fragment that lands. It used to be armed at the send, which made every queueing delay look
///    like a device that sent nothing: a GATT write slower than the window closed the read as a
///    "successful" zero-fragment partial while the frame was still in the platform, and the read
///    policy reports that shape once, as a hole, instead of asking again. The wait for a reply's
///    first fragment is the device's queue (`fragmentStallTimeout` has the arithmetic), and the
///    request's own deadline owns it.
///  * **A truncated reply is reported, not re-burst.** Storage CID 5 asks a file BY NAME and has
///    no offset form, so a second read is the whole file again, into the ring that just dropped
///    fragments - which is why a stream that STARTED and stopped is one send with its hole named.
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

/// A link whose `send` can be held open: the platform write that never returns, or one that is
/// merely slow.
///
/// Nothing is written into [_packets] unless a test asks for it, so a request sent through this
/// transport is a device that answers nothing at all.
class _StallingTransport implements Transport {
  /// When true, [send] returns a future that never completes.
  bool stall = false;

  /// How long [send] takes when it is not stalled - the slow GATT write.
  Duration sendDelay = Duration.zero;

  /// When set, every request is answered with this many fragments of a [fragTotal]-fragment
  /// reply and then silence: a stream that starts and stops.
  int? fragments;

  /// The fragment count a partial stream declares.
  int fragTotal = 4;

  /// Every frame handed to [send].
  final frames = <List<int>>[];

  final _packets = StreamController<Uint8List>.broadcast();
  final _parser = PacketStreamParser();

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
  Future<void> send(List<int> streamBytes) async {
    frames.add(streamBytes);
    if (stall) return Completer<void>().future; // never completes
    if (sendDelay > Duration.zero) await Future<void>.delayed(sendDelay);
    final count = fragments;
    if (count == null) return;
    for (final frame in _parser.feed(Uint8List.fromList(streamBytes))) {
      _deliverFragments(frame.srvSource, count);
    }
  }

  @override
  Future<void> close() async {
    if (!_packets.isClosed) await _packets.close();
  }

  /// Delivers the first [count] fragments of a [fragTotal]-fragment reply for [txId], then stops:
  /// the stream the app must recognise as truncated.
  ///
  /// The fragmentation info the wire format carries (u16 index + u16 total) is prefixed to each
  /// body; the STOP flag is set only on the last fragment of a WHOLE stream, so a stream that
  /// stops early never carries it.
  void _deliverFragments(int txId, int count) {
    for (var index = 0; index < count; index++) {
      _packets.add(
        PacketFrame(
          flags:
              flagType |
              flagSuccess |
              flagFrag |
              (index == 0 ? flagStart : 0) |
              (index == fragTotal - 1 ? flagStop : 0),
          priority: defaultPriority,
          idTarget: appSourceId,
          idSource: 1,
          srvTarget: makeService(ServiceType.storage, 5),
          srvSource: txId,
          payload: Uint8List.fromList([
            ...writeFragInfo(index, fragTotal),
            ...List<int>.filled(8, index + 1),
          ]),
        ).toBytes(),
      );
    }
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

  group('the head of a reply is not a hole', () {
    test(
      'a send slower than the stream window is failed by the deadline, not reported as silence',
      () async {
        // A GATT write with response that takes longer than the stream window. The watchdog used
        // to be armed BEFORE this send, so at 2 s it closed the read as a "successful"
        // zero-fragment partial - a device verdict about a frame the app had not written yet, and
        // one the read policy reports rather than re-sends. The head belongs to the deadline, so
        // this request must end in a TIMEOUT error (a failed attempt, retried by the reader).
        final transport = _StallingTransport()
          ..sendDelay =
              ConnectionManager.fragmentStallTimeout +
              const Duration(milliseconds: 500);
        final manager = await _connected(transport);

        final started = DateTime.now();
        final error = await _failure(
          manager.requestTolerant(
            1,
            ServiceType.storage,
            5,
            payload: StorageClient.padName('.TABLE  '),
            timeout: const Duration(seconds: 4),
          ),
        );
        final elapsed = DateTime.now().difference(started);

        expect(
          error,
          isA<TransportException>(),
          reason:
              'a slow platform write is not a reply: it must not come back as a partial reply '
              'that the read policy would report as a hole',
        );
        expect('$error', contains('timed out'));
        expect(
          elapsed,
          greaterThanOrEqualTo(transport.sendDelay),
          reason:
              'nothing was concluded about the device before its frame was written',
        );
        await manager.disconnect();
      },
    );

    test(
      'a device that answers nothing ends on the deadline, not in the stall window',
      () async {
        final transport = _StallingTransport();
        final manager = await _connected(transport);

        // The deadline is longer than the stream window on purpose: before this fix the watchdog
        // closed the read at 2 s as a "successful" zero-fragment partial, which is the shape the read
        // policy reports instead of re-sending. Nothing of the reply has arrived, so there is no
        // stream to watch: the deadline is the only clock that owns this wait.
        final started = DateTime.now();
        final error = await _failure(
          manager.requestTolerant(
            1,
            ServiceType.storage,
            5,
            payload: StorageClient.padName('.TABLE  '),
            timeout: const Duration(milliseconds: 2500),
          ),
        );
        final elapsed = DateTime.now().difference(started);

        expect(
          error,
          isA<TransportException>(),
          reason:
              'a head that never started is the deadline\'s, not a zero-fragment reply',
        );
        expect('$error', contains('timed out'));
        expect(
          elapsed,
          greaterThanOrEqualTo(const Duration(milliseconds: 2500)),
          reason: 'the whole deadline is given to the head',
        );
        await manager.disconnect();
      },
    );
  });

  group('a reply that stops mid-stream', () {
    test(
      'is closed by the stream watchdog and reported ONCE, naming the hole',
      () async {
        final transport = _StallingTransport()..fragments = 2;
        final manager = await _connected(transport);
        installRequestChannel(manager);
        addTearDown(() => installRequestChannel(null));

        // Two of four fragments, then the stream goes quiet: this is the shape the stream
        // watchdog exists for.
        final started = DateTime.now();
        final result = await StorageClient(deviceId: 1).readFileTableResult();
        final elapsed = DateTime.now().difference(started);

        expect(result.records, isNull, reason: 'a partial read is never data');
        expect(
          transport.frames,
          hasLength(1),
          reason: 'one send: a stream that started is not re-burst',
        );
        expect(result.attempts, 1);
        expect(result.detail, contains('fragment 2 of 4 never arrived'));
        expect(
          result.detail,
          contains('the stream stopped without its STOP frame'),
        );
        expect(
          elapsed,
          lessThan(ConnectionManager.fragmentStallTimeout * 2),
          reason:
              'closed by the stream watchdog, not by the 10 s read deadline',
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
