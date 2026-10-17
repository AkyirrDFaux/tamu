import 'diagnostics.dart';
import 'protocol.dart';
import 'request_channel.dart';
import 'types.dart';

/// Sends one service request, turning a transport failure into `null` and logging it under [tag].
///
/// Every per-service client wrapper is this call with its own service, timeout and tag - the
/// only things that ever differed between them. Centralising it keeps the failure handling
/// (swallow to null, log with the address and the service/CID) identical everywhere instead of
/// four slightly different copies.
///
/// The request goes through the installed [requestChannel]: the app installs its connection, a
/// CLI tool installs its own serial channel.
Future<List<int>?> serviceRequest(
  int targetId,
  ServiceType service,
  int cid, {
  List<int> payload = const [],
  Duration? timeout,
  Duration defaultTimeout = const Duration(seconds: 3),
  bool requestFrag = false,
  required String tag,
}) async {
  final channel = requestChannel;
  if (channel == null) {
    AppDiagnostics.log(
      tag,
      'dev ${idToString(targetId)} $service/$cid: no link',
    );
    return null;
  }
  try {
    return await channel.request(
      targetId,
      service,
      cid,
      payload: payload,
      timeout: timeout ?? defaultTimeout,
      requestFrag: requestFrag,
    );
  } catch (error) {
    AppDiagnostics.log(
      tag,
      'dev ${idToString(targetId)} $service/$cid failed: $error',
    );
    return null;
  }
}

/// Sibling of [serviceRequest] that keeps the packet-level SUCCESS/FAIL flags, for the commands
/// that report their status in the frame rather than in a payload byte.
Future<PacketResponse?> serviceRequestWithFlags(
  int targetId,
  ServiceType service,
  int cid, {
  List<int> payload = const [],
  Duration? timeout,
  Duration defaultTimeout = const Duration(seconds: 3),
  bool requestFrag = false,
  required String tag,
}) async {
  final channel = requestChannel;
  if (channel == null) {
    AppDiagnostics.log(
      tag,
      'dev ${idToString(targetId)} $service/$cid: no link',
    );
    return null;
  }
  try {
    return await channel.requestWithFlags(
      targetId,
      service,
      cid,
      payload: payload,
      timeout: timeout ?? defaultTimeout,
      requestFrag: requestFrag,
    );
  } catch (error) {
    AppDiagnostics.log(
      tag,
      'dev ${idToString(targetId)} $service/$cid failed: $error',
    );
    return null;
  }
}
