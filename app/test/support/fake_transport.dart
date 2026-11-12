/// A device at the other end of a `Transport`, parsed frame by frame.
///
/// The two suites that drive a restore through the app's REAL request layer
/// (`restore_over_gate_test`) or that need every gate slot pinned (`request_fault_busy_test`) each
/// wrote the same `Transport`: a packet controller, a `PacketStreamParser`, the request log, the
/// six `Transport` members and the send loop that turns the app's stream bytes back into frames.
/// That block is not test logic - it is the wire - and a copy that drifted would model a different
/// link than the app under test talks to.
///
/// A fake now writes [onFrame] and nothing else. [sent] is filled before the hook runs, so a fake
/// that drops a request still records that it was sent, which is what "the frame reached the link"
/// means in both suites.
library;

import 'dart:async';
import 'dart:typed_data';

import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/transport.dart';

/// The one link the transport fakes ride on: a BLE peer, named the way the app names it.
const DiscoveredLink fakeBleLink = DiscoveredLink(
  id: 'AA:BB:CC:DD:EE:02',
  type: LinkType.ble,
  name: 'Tamu core',
);

/// One request frame the app got as far as sending, as the link saw it.
typedef SentFrame = ({int deviceId, ServiceType service, int cid});

/// A `Transport` whose requests a test answers frame by frame.
abstract class FrameTransport implements Transport {
  final StreamController<Uint8List> _packets =
      StreamController<Uint8List>.broadcast();
  final PacketStreamParser _parser = PacketStreamParser();

  /// Every request frame the app sent, in order.
  final List<SentFrame> sent = [];

  /// What the fake does with one request: answer it, drop it, or hold it.
  void onFrame(PacketFrame frame, ServiceType service, int cid);

  /// One reply, as the device would put it on the wire.
  void emit(List<int> bytes) {
    if (!_packets.isClosed) _packets.add(Uint8List.fromList(bytes));
  }

  /// Whether the link is closed - a held reply checks this before it answers.
  bool get isClosed => _packets.isClosed;

  @override
  String get id => fakeBleLink.id;

  @override
  String get displayName => 'BLE ${fakeBleLink.id}';

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
      onFrame(frame, service, cid);
    }
  }
}
