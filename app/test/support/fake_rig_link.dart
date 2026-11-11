/// A rig link a test scripts.
///
/// `RigLink` is a `RequestChannel` plus [close]. The two CLI fakes in this suite carried the same
/// closed flag, the same close counter, the same `tridAppBase` transaction ids and the same
/// delegation (`request` = the flagged path's payload), because that is what a rig does: every
/// request it answers arrived with the packet flags. A fake writes [answer] and nothing else.
library;

import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import '../../tool/rig_link.dart' show RigLink;

/// A link whose replies a test scripts.
abstract class ScriptedRigLink implements RigLink {
  /// How many times the tool closed the link - a run closes it once, on its way out.
  int closeCount = 0;

  bool _closed = false;

  @override
  bool get isConnected => !_closed;

  @override
  Future<void> close() async {
    closeCount++;
    _closed = true;
  }

  @override
  int takeTxId() => tridAppBase;

  @override
  void setSubscriptionListener(Function(List<int>)? listener) {}

  /// The reply to one request. The only method a fake writes; throwing from it is how a fake
  /// models a rig that did not answer.
  Future<PacketResponse> answer(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
    bool requestFrag = false,
    int? transactionId,
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
  }) async => (await answer(
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
  }) => answer(
    targetId,
    service,
    functionCid,
    payload: payload,
    timeout: timeout,
    requestFrag: requestFrag,
    transactionId: transactionId,
  );
}
