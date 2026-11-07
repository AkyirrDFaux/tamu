import 'diagnostics.dart';
import 'protocol.dart';
import 'request_channel.dart';
import 'request_gate.dart';
import 'types.dart';

/// Records one request that never got a turn, and logs it under [tag].
///
/// The link's queue bound (`RequestQueueTimeout`) is not the device: not one frame went out.
/// Every caller that reports why a request failed takes its kind from the trace, so this is the
/// only place the two can stay apart.
void _recordBusy(
  RequestTrace? trace,
  String tag,
  int targetId,
  ServiceType service,
  int cid,
  RequestQueueTimeout error,
) {
  AppDiagnostics.log(
    tag,
    'dev ${idToString(targetId)} $service/$cid not sent - the link is busy: $error',
  );
  trace?.record(
    RequestFault(
      kind: RequestFaultKind.busy,
      deviceId: targetId,
      service: service,
      cid: cid,
      detail: '$error',
    ),
  );
}

/// Sends one service request, turning a transport failure into `null` and logging it under [tag].
///
/// Every per-service client wrapper is this call with its own service, timeout and tag - the
/// only things that ever differed between them. Centralising it keeps the failure handling
/// (swallow to null, log with the address and the service/CID) identical everywhere instead of
/// four slightly different copies.
///
/// The request goes through the installed [requestChannel]: the app installs its connection, a
/// CLI tool installs its own serial channel.
///
/// [trace] is for a caller that has to report WHY a request did not land (a restore reports per
/// item). It is only told about the failures this layer can see - no link, no reply - and the
/// client records the rest (a FAIL flag, a reply that carries nothing usable), because only the
/// client knows what a usable reply for its command looks like. Passing no trace changes nothing.
Future<List<int>?> serviceRequest(
  int targetId,
  ServiceType service,
  int cid, {
  List<int> payload = const [],
  Duration? timeout,
  Duration defaultTimeout = const Duration(seconds: 3),
  bool requestFrag = false,
  required String tag,
  RequestTrace? trace,
}) async {
  final channel = requestChannel;
  if (channel == null) {
    AppDiagnostics.log(
      tag,
      'dev ${idToString(targetId)} $service/$cid: no link',
    );
    trace?.record(
      RequestFault(
        kind: RequestFaultKind.noLink,
        deviceId: targetId,
        service: service,
        cid: cid,
      ),
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
  } on RequestQueueTimeout catch (error) {
    // The request never got a turn: the link was busy, the device was never asked. Its own kind,
    // because every other throw here is "the device did not answer" - and a queue expiry reported
    // as silence is a device verdict nobody measured.
    _recordBusy(trace, tag, targetId, service, cid, error);
    return null;
  } catch (error) {
    AppDiagnostics.log(
      tag,
      'dev ${idToString(targetId)} $service/$cid failed: $error',
    );
    trace?.record(
      RequestFault(
        kind: RequestFaultKind.noReply,
        deviceId: targetId,
        service: service,
        cid: cid,
        detail: '$error',
      ),
    );
    return null;
  }
}

/// Sibling of [serviceRequest] that keeps the packet-level SUCCESS/FAIL flags, for the commands
/// that report their status in the frame rather than in a payload byte.
///
/// A FAIL answer is recorded on [trace] as [RequestFaultKind.refused] - the one place that flag is
/// visible - so a client's caller can report the code the device answered with instead of only
/// that the command returned false.
Future<PacketResponse?> serviceRequestWithFlags(
  int targetId,
  ServiceType service,
  int cid, {
  List<int> payload = const [],
  Duration? timeout,
  Duration defaultTimeout = const Duration(seconds: 3),
  bool requestFrag = false,
  required String tag,
  RequestTrace? trace,
}) async {
  final channel = requestChannel;
  if (channel == null) {
    AppDiagnostics.log(
      tag,
      'dev ${idToString(targetId)} $service/$cid: no link',
    );
    trace?.record(
      RequestFault(
        kind: RequestFaultKind.noLink,
        deviceId: targetId,
        service: service,
        cid: cid,
      ),
    );
    return null;
  }
  try {
    final response = await channel.requestWithFlags(
      targetId,
      service,
      cid,
      payload: payload,
      timeout: timeout ?? defaultTimeout,
      requestFrag: requestFrag,
    );
    if (response.fail) {
      trace?.refused(
        targetId,
        service: service,
        cid: cid,
        payload: response.payload,
      );
    }
    return response;
  } on RequestQueueTimeout catch (error) {
    _recordBusy(trace, tag, targetId, service, cid, error);
    return null;
  } catch (error) {
    AppDiagnostics.log(
      tag,
      'dev ${idToString(targetId)} $service/$cid failed: $error',
    );
    trace?.record(
      RequestFault(
        kind: RequestFaultKind.noReply,
        deviceId: targetId,
        service: service,
        cid: cid,
        detail: '$error',
      ),
    );
    return null;
  }
}
