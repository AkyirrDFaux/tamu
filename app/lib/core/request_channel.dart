/// The App Interface request channel the service clients talk through.
///
/// The per-service clients (Register, Scripts, Storage, Subscriptions) are pure Dart: they
/// encode a request, hand it to the channel installed here, and decode the reply. Keeping the
/// wire access behind this one interface is what lets the SAME clients drive the app's live
/// connection and a plain `dart run` tool - the tool installs its own channel and never imports
/// the app's Flutter-coupled connection stack (`connection.dart`, `transport.dart`).
library;

import 'protocol.dart';

/// A completed service reply: its reassembled payload plus the packet-level
/// SUCCESS/FAIL flags. Most callers only need the payload ([RequestChannel.request]);
/// the subscription client checks the flags to tell an accepted write from a refusal.
class PacketResponse {
  final List<int> payload;
  final bool success;
  final bool fail;

  const PacketResponse({
    required this.payload,
    required this.success,
    required this.fail,
  });

  /// A reply is accepted only when it is explicitly successful. A FAIL response (or a
  /// response with neither flag) is not a success.
  bool get ok => success && !fail;
}

/// One link's request/response surface, as the service clients use it.
abstract class RequestChannel {
  /// True while a link is up. Callers that must not fire requests at nothing check it.
  bool get isConnected;

  /// Allocates a free transaction ID for manual use (e.g. sharing one TRID across a paired
  /// pair of requests). The caller owns the slot until it is used by [request].
  int takeTxId();

  /// Sends a single-packet request and waits for its response payload (REQACK is
  /// always set). Throws on timeout.
  Future<List<int>> request(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
    bool requestFrag = false,
    int? transactionId,
  });

  /// Like [request], but returns the packet-level SUCCESS/FAIL flags alongside the
  /// payload so callers can tell an accepted write from a FAIL reply.
  Future<PacketResponse> requestWithFlags(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
    bool requestFrag = false,
    int? transactionId,
  });

  /// Registers the receiver of firmware-pushed subscription value updates
  /// (service Subscriptions CID 0), or clears it with null.
  void setSubscriptionListener(Function(List<int>)? listener);
}

/// How a request ended, when the caller must report WHY a write did not land.
///
/// The service clients answer `null`/`false` for every kind of failure, which is the right answer
/// for a page that only needs "did it work" - and useless for a restore, which reports per item and
/// has to say what the device said (Akyirr, 2026-10-25: 17 failures with no reason). One of these is
/// recorded on a [RequestTrace] by the layer that knows the difference.
enum RequestFaultKind {
  /// Nothing is installed, or the link is down: the request never reached a device.
  noLink('no link'),

  /// The request was sent and nothing came back within its timeout.
  noReply('no reply'),

  /// The device answered FAIL (packet flag `FLAG_FAIL`); [RequestFault.payload] carries the reason
  /// bytes it sent with it, which for every command the app uses is empty.
  refused('FAIL'),

  /// The device answered, but with nothing the command could use: an empty or short reply where a
  /// value is echoed on success. That is how a FAIL looks on the payload-only commands, whose flag
  /// the client never sees - so it is reported as "answered nothing", not as a lost reply.
  unusable('no usable reply'),

  /// The app refused the request itself and sent nothing: a write whose value has no bytes (see
  /// `ValueUpdateApplicable` in the firmware - a zero-length value poisons the row it targets), or
  /// one the client can see has nowhere to go. Separate from [noLink] because the bus was fine and
  /// separate from [unusable] because nothing was ever sent.
  notSent('not sent');

  const RequestFaultKind(this.word);

  /// The word the report uses for this kind.
  final String word;
}

/// Why one request did not produce an answer, as far as the app can see it.
///
/// Small and plain on purpose: it travels from the request layer to a restore's per-item reason,
/// and the wording of that reason belongs to the restore (`RestoreFailure`), not here.
class RequestFault {
  final RequestFaultKind kind;

  /// The device the request was addressed to.
  final int deviceId;

  /// The service and command, when the layer that recorded it knows them.
  final ServiceType? service;
  final int? cid;

  /// The bytes the device answered with, when the fault is [RequestFaultKind.refused] or
  /// [RequestFaultKind.unusable]: the FAIL reason payload (empty on this firmware), or the reply
  /// that could not be used.
  final List<int> payload;

  /// The transport error behind a [RequestFaultKind.noReply], for the report to quote.
  final String? detail;

  const RequestFault({
    required this.kind,
    required this.deviceId,
    this.service,
    this.cid,
    this.payload = const [],
    this.detail,
  });

  /// The FAIL flag a refused request carries (protocol.dart `flagFail`), for the report.
  static const int failFlag = 0x40;

  @override
  String toString() =>
      'RequestFault(${kind.word}, dev $deviceId, '
      '${service?.name}/$cid, ${payload.length} byte(s)'
      '${detail == null ? '' : ', $detail'})';
}

/// The last failure of one client's requests, kept until the caller clears it.
///
/// A client instance is per device and drives one request at a time, so a trace belongs to that
/// client: two pages reading the same device through different instances cannot overwrite each
/// other's reason. The FIRST fault after a [clear] is the one kept - the root cause of a compound
/// operation such as a staged file write (create, fragments, rename), not whichever step happened
/// last.
///
/// Nothing here changes what a request returns: an untraced caller (every page that only needs
/// "did it work") is untouched, and a traced one gets the same answer plus the reason.
class RequestTrace {
  RequestFault? _first;

  /// The first fault recorded since the last [clear], or null when nothing has failed.
  RequestFault? get fault => _first;

  /// Forgets the faults recorded so far, so the next operation's reason is its own.
  void clear() => _first = null;

  /// Records [fault] unless one is already kept.
  void record(RequestFault fault) => _first ??= fault;

  /// Records that the device refused the request (FAIL), with the reason bytes it sent.
  void refused(
    int deviceId, {
    ServiceType? service,
    int? cid,
    List<int> payload = const [],
    String? detail,
  }) {
    if (_first != null) return;
    _first = RequestFault(
      kind: RequestFaultKind.refused,
      deviceId: deviceId,
      service: service,
      cid: cid,
      payload: payload,
      detail: detail,
    );
  }

  /// Records that the device answered nothing usable where the command expects a value.
  void unusable(
    int deviceId, {
    ServiceType? service,
    int? cid,
    List<int> payload = const [],
    String? detail,
  }) {
    if (_first != null) return;
    _first = RequestFault(
      kind: RequestFaultKind.unusable,
      deviceId: deviceId,
      service: service,
      cid: cid,
      payload: payload,
      detail: detail,
    );
  }

  /// Records that the app refused the request and never sent it, with [detail] saying why.
  void notSent(int deviceId, {ServiceType? service, int? cid, String? detail}) {
    if (_first != null) return;
    _first = RequestFault(
      kind: RequestFaultKind.notSent,
      deviceId: deviceId,
      service: service,
      cid: cid,
      detail: detail,
    );
  }
}

/// The installed channel, or null when nothing has installed one.
///
/// The app installs its connection (`connection.dart`); a CLI tool installs its own serial
/// channel. With no channel installed a request has nowhere to go, and the clients report the
/// same "no reply" they report for an unreachable device.
RequestChannel? get requestChannel => _active;

RequestChannel? _active;

/// Installs [channel] as the one the service clients send through.
void installRequestChannel(RequestChannel? channel) => _active = channel;
