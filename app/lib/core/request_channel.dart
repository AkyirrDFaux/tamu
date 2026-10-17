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

/// The installed channel, or null when nothing has installed one.
///
/// The app installs its connection (`connection.dart`); a CLI tool installs its own serial
/// channel. With no channel installed a request has nowhere to go, and the clients report the
/// same "no reply" they report for an unreachable device.
RequestChannel? get requestChannel => _active;

RequestChannel? _active;

/// Installs [channel] as the one the service clients send through.
void installRequestChannel(RequestChannel? channel) => _active = channel;
