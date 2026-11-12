/// The request-channel scaffolding every bus fake in this suite was copying.
///
/// A `RequestChannel` has four members a fake must answer - `isConnected`, `takeTxId`,
/// `setSubscriptionListener` and the two request entry points - and the two entry points carry
/// six named parameters each. Nine restore/bus fakes pasted that block verbatim, ~30 lines apiece,
/// where a fake that got one signature wrong still compiled and quietly answered nothing (the
/// failure mode `fake_channel.dart` documents for the tolerant path).
///
/// A fake now writes only the two methods it actually answers with: [flagged] for the path that
/// carries the reply's flags (a refused register write is only visible there), and [plain] for the
/// older payload-only path, which defaults to the flagged path's payload. Both are recorded in
/// [sent] once, in the order the app sent them, so a test reads the same log whichever path its
/// client used.
///
/// The two paths are NOT interchangeable: several fakes answer a file read (Storage CID 5) only on
/// the plain path and return an empty success on the flagged one, because that is what the older
/// fake buses do and what the app's `_readFileReply` fallback is written against. Keep that
/// distinction in [plain] instead of collapsing it.
library;

import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';

/// One request a fake bus answered, as the bus saw it.
///
/// [trid] is the transaction id the caller named (null when it let the channel allocate one); a
/// requester-table fake reads it back to name the rows a restore cancelled.
typedef BusRequest = ({
  ServiceType service,
  int cid,
  List<int> payload,
  int? trid,
});

/// A channel a test scripts, with the shared plumbing written once.
abstract class FakeBus implements RequestChannel {
  FakeBus({int firstTxId = 0x100}) : _txId = firstTxId;

  int _txId;

  /// Every request, in the order it was sent.
  final List<BusRequest> sent = [];

  /// Whether the link is up. A fake that models a dropped link sets this false.
  bool connected = true;

  @override
  bool get isConnected => connected;

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
  }) async {
    sent.add((
      service: service,
      cid: functionCid,
      payload: payload,
      trid: transactionId,
    ));
    return plain(targetId, service, functionCid, payload);
  }

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
    sent.add((
      service: service,
      cid: functionCid,
      payload: payload,
      trid: transactionId,
    ));
    return flagged(targetId, service, functionCid, payload);
  }

  /// The reply on the plain (`request`) path. Defaults to the flagged path's payload, which is
  /// what a device that does not tell the two paths apart answers.
  Future<List<int>> plain(
    int targetId,
    ServiceType service,
    int functionCid,
    List<int> payload,
  ) async => (await flagged(targetId, service, functionCid, payload)).payload;

  /// The reply on the path that carries the packet flags - the one a refused register write is
  /// visible on (`FLAG_FAIL` and no payload).
  Future<PacketResponse> flagged(
    int targetId,
    ServiceType service,
    int functionCid,
    List<int> payload,
  );
}

/// The firmware's refusal: `FLAG_FAIL` with no payload (`RespondStatus(Frame, false)`).
const PacketResponse busRefused = PacketResponse(
  payload: [],
  success: false,
  fail: true,
);

/// The firmware's bare acknowledgement: success, nothing to say.
const PacketResponse busAccepted = PacketResponse(
  payload: [],
  success: true,
  fail: false,
);

/// The reply an accepted register write gives: the request's own payload back (the address, the
/// `ValueInfo` it stored and the value bytes), carrying the success flag.
PacketResponse registerWriteEcho(List<int> payload) =>
    PacketResponse(payload: payload, success: true, fail: false);
