/// The two shapes every fake channel in this suite was copying.
///
/// A `TolerantRequestChannel` has three entry points (`request`, `requestWithFlags`,
/// `requestTolerant`) and each fake answered them the same way: `request` awaits the tolerant
/// path and keeps its payload, `requestWithFlags` returns it whole. That is forty lines of
/// delegation per fake, and it was pasted into nine test files - where a fake that got one
/// signature wrong compiled fine and quietly answered nothing. [FakeTolerantChannel] is that
/// shape once: a fake writes only [answer].
///
/// [wholeReply] is the one-fragment reply (`stopped`, one chunk) that a device's entire answer
/// takes when it fits a single frame; five files built it by hand.
library;

import 'package:tamuapp/core/fragment_reassembler.dart' show FragmentChunk;
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';

/// The reply a device gives for a body it sent whole: one fragment, which is also the last one.
PacketResponse wholeReply(List<int> body) => PacketResponse(
  payload: body,
  success: true,
  fail: false,
  chunks: [FragmentChunk(0, body)],
  stopped: true,
  total: 1,
);

/// A channel that answers every request through one tolerant method.
///
/// A fake overrides [answer] only. Throwing from it is how a fake models a lost frame or a
/// timeout - exactly the shape the request layer sees - and the channel's other entry points then
/// behave as the app's own connection does.
abstract class FakeTolerantChannel implements TolerantRequestChannel {
  /// The transaction id this fake hands out. Nothing in the suite depends on its value; it is
  /// here because [RequestChannel.takeTxId] must answer something.
  @override
  int takeTxId() => 0xF700;

  @override
  bool get isConnected => true;

  @override
  void setSubscriptionListener(Function(List<int>)? listener) {}

  /// The reply to one request. The only method a fake writes.
  Future<PacketResponse> answer(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
  });

  @override
  Future<List<int>> request(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
    bool requestFrag = false,
    int? transactionId,
  }) async => (await requestTolerant(
    targetId,
    service,
    functionCid,
    payload: payload,
    timeout: timeout,
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
  }) => requestTolerant(
    targetId,
    service,
    functionCid,
    payload: payload,
    timeout: timeout,
  );

  @override
  Future<PacketResponse> requestTolerant(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
  }) => answer(
    targetId,
    service,
    functionCid,
    payload: payload,
    timeout: timeout,
  );
}
