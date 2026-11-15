/// Device service CID 0x0004, "enter bootloader": the app's command that puts ONE node back into
/// its bootloader.
///
/// The inverse the owner asked for (2026-11-15). It is one existing Device-service request, not a
/// new protocol: an addressed, empty request to one node's address.
///
/// ## The ack is a complete send, and after it the node is not an app any more
///
/// The handler answers FIRST and the ack is a complete bus send, then the node leaves its own code
/// and jumps to the bootloader's published entry at `0x7FC` (firmware `Core/Services/Device.h`
/// `case 4:`, `Devices/DAS_v0.1/Main.h` `DeviceEnterBootloaderJump`); that call does not return.
/// So an ack means the node HAS left its app: nothing after it is an app answer, and every later
/// app request to that node (a System-block version read, a ping) is unanswered by design.
///
/// ## After the ack the node answers the BOOTLOADER's identify, not an app version
///
/// The one thing that answers for the node afterwards is the bootloader itself, reached through
/// the core's passthrough (`bootloader_client.dart` [PassthroughTransport], Device CID 0x20):
/// identify, carrying the app space and the erase page size - 64 bytes on a DAS
/// ([dasBootloaderPageSize]). That is the state change this client MODELS: from the ack on the
/// node is a [NodeMode.bootloader], not a node that went missing, and its app's silence is the
/// expected state rather than a fault. The identify is also what tells a lost ack from a node that
/// never left, because it is answered by the bootloader and not by the app.
///
/// ## A node that does not implement the command cannot refuse it in words
///
/// The handler exists only when the device defines `DEVICE_HAS_BOOTLOADER_ENTRY`
/// (`firmware/platformio.ini`, the DAS app). A node built without it does not answer Device
/// CID 4 at all: the service's `switch` falls through with no reply. So the refusal the app can
/// see comes in two shapes, and this client keeps them apart:
///
///  * a `FLAG_FAIL` reply - the node (or a core relaying for it) read the request and said no
///    ([EnterBootloaderOutcome.refused]);
///  * nothing at all, and no bootloader answering an identify for the node either: the node does
///    not implement the command, and its app is still running
///    ([EnterBootloaderOutcome.notImplemented]).
///
/// Nothing here retries: a re-send of CID 4 is harmless (a node already in its bootloader ignores
/// it) but it cannot repair a refusal, and an unanswered node that does implement the command is
/// indistinguishable from one that does not, so the caller is told which of the two the identify
/// probe proved instead of the client guessing.
library;

import 'bootloader_client.dart';
import 'diagnostics.dart';
import 'protocol.dart';
import 'request_channel.dart';
import 'service_request.dart';
import 'types.dart';

/// Device service command ID 0x0004, "enter bootloader".
///
/// Pinned to the firmware's `case 4:` in `firmware/src/Core/Services/Device.h` - which is compiled
/// only under `DEVICE_HAS_BOOTLOADER_ENTRY`, a define the DAS env carries in
/// `firmware/platformio.ini` - by `test/enter_bootloader_test.dart` and
/// `test/firmware_contract_test.dart`. `0x0004` is the first free CID after the four documented
/// Device commands: Discover (0), Ping (1), Identify (2), TimeSync (3).
const int deviceCidEnterBootloader = 0x0004;

/// The DAS bootloader's erase page size, as its identify reports it (`0x40`), and the page size
/// the rig answers for a node that has left its app. Recorded here so a handoff can quote the
/// figure the client expects instead of a number in the prose.
const int dasBootloaderPageSize = 64;

/// A target address Device CID 4 must never name: this command takes exactly ONE node, and a
/// broadcast or an invalid address is the app's own mistake, not a device answer.
const int broadcastAddress = 0xFFFF;
const int invalidAddress = 0x0000;

/// What a node IS after the command, as far as the app can model it.
///
/// The point of the type is that a node which has left its app is a BOOTLOADER, not an absent
/// device: a caller that keeps this apart does not report the following silence as a lost node.
enum NodeMode {
  /// The node's own app answers its Device and Register requests.
  app('app'),

  /// The node has left its app and answers the bootloader's identify (page size 64 on a DAS).
  bootloader('bootloader');

  const NodeMode(this.word);

  /// The word a handoff or a log uses.
  final String word;
}

/// How one enter-bootloader attempt ended.
enum EnterBootloaderOutcome {
  /// The node acked with a complete send and left its app.
  accepted('acked'),

  /// No COMPLETE ack arrived, but the node answers the bootloader's identify: it left its app and
  /// the ack - or the reply's framing - was lost on the way to the app.
  ackLost('no complete ack'),

  /// The node refused the command (`FLAG_FAIL`): it read the request and said no. Its app stays.
  refused('refused'),

  /// Nothing came back and no bootloader answers for the node: it does not implement CID 4 (or is
  /// not on the bus). Its app stays, which is the documented behaviour of a node built without
  /// `DEVICE_HAS_BOOTLOADER_ENTRY`.
  notImplemented('not implemented'),

  /// Nothing was sent: the app has no link, or the link's queue never gave the request a turn.
  /// Never a device verdict.
  notSent('not sent');

  const EnterBootloaderOutcome(this.word);

  /// The word a report uses for this outcome.
  final String word;
}

/// What one [enterNodeBootloader] attempt established about a node.
class EnterBootloaderResult {
  /// The node the request was addressed to.
  final int deviceId;

  final EnterBootloaderOutcome outcome;

  /// The state the client now models this node as: [NodeMode.bootloader] once the ack landed (or
  /// the node answered a bootloader identify), [NodeMode.app] otherwise.
  final NodeMode mode;

  /// The bootloader's own identify, when one answered for the node: its app space, its erase page
  /// size ([dasBootloaderPageSize] on a DAS) and its name. Null when no bootloader answered.
  final BootloaderIdentity? bootloader;

  /// The one thing this attempt could not put in [outcome] alone: why nothing was sent, or what
  /// the identify probe answered. Null when the outcome says everything.
  final String? detail;

  const EnterBootloaderResult({
    required this.deviceId,
    required this.outcome,
    required this.mode,
    this.bootloader,
    this.detail,
  });

  /// True when the node left its app - by its ack, or by answering as a bootloader after a lost
  /// ack.
  bool get entered => mode == NodeMode.bootloader;

  /// One sentence naming what the node did, for a log, a page or a test.
  String get message {
    final dev = 'dev ${idToString(deviceId)}';
    final pages = bootloader == null
        ? 'no page size'
        : '${bootloader!.pageSize} B pages';
    switch (outcome) {
      case EnterBootloaderOutcome.accepted:
        return '$dev left its app (Device CID 4 acked) and answers its '
            'bootloader: $pages';
      case EnterBootloaderOutcome.ackLost:
        return '$dev answers its bootloader ($pages), so it left its app; '
            'no complete ack came back';
      case EnterBootloaderOutcome.refused:
        return '$dev refused Device CID 4 (FLAG_FAIL): it stays in its app';
      case EnterBootloaderOutcome.notImplemented:
        return '$dev did not answer Device CID 4 and is not in its bootloader: '
            'it does not implement the command';
      case EnterBootloaderOutcome.notSent:
        return '$dev: ${detail ?? 'the request was not sent'}';
    }
  }

  @override
  String toString() => message;
}

/// Probes a node's bootloader; the default asks the core's passthrough for an identify.
typedef BootloaderIdentify =
    Future<BootloaderIdentity?> Function(Duration timeout);

/// Sends Device CID 4 to ONE node and reports the state change it makes.
///
/// The frame is the addressed form of every app request: a single packet to [deviceId]'s own
/// address carrying `Device/0x0004` with no payload. A broadcast or an invalid address is refused
/// before anything is sent - the command puts one node back, and the firmware's handler is
/// addressed for exactly that reason.
///
/// [ackTimeout] is the window for the ack. It is the app's request deadline, not the device's: the
/// node's own reply goes out with its complete send.
///
/// After the attempt, the node is asked for its bootloader's identify ([identify], the core's
/// passthrough by default) unless it refused the command. That answer is what the node's mode is
/// read from, and it is what tells a lost ack from a node that never left: the app's requests go
/// unanswered once the node is a bootloader, so silence alone is not evidence either way.
Future<EnterBootloaderResult> enterNodeBootloader(
  int deviceId, {
  Duration ackTimeout = const Duration(seconds: 2),
  Duration identifyTimeout = const Duration(milliseconds: 800),
  BootloaderIdentify? identify,
}) async {
  if (deviceId == invalidAddress || deviceId == broadcastAddress) {
    return EnterBootloaderResult(
      deviceId: deviceId,
      outcome: EnterBootloaderOutcome.notSent,
      mode: NodeMode.app,
      detail:
          'Device CID 4 names one node: ${idToString(deviceId)} is not a '
          'node address',
    );
  }

  // A request fired at a link that is not up is the app's own state, never a device verdict: the
  // app's connection reports it as a transport throw, which would otherwise read below as a node
  // that did not answer - i.e. as a node that does not implement the command.
  final channel = requestChannel;
  if (channel == null || !channel.isConnected) {
    return EnterBootloaderResult(
      deviceId: deviceId,
      outcome: EnterBootloaderOutcome.notSent,
      mode: NodeMode.app,
      detail: 'the app has no link, so nothing was sent to the node',
    );
  }

  final trace = RequestTrace();
  final reply = await serviceRequestWithFlags(
    deviceId,
    ServiceType.device,
    deviceCidEnterBootloader,
    timeout: ackTimeout,
    tag: 'enter-bootloader',
    trace: trace,
  );

  final fault = trace.fault;
  if (reply == null && _nothingWasSent(fault)) {
    return EnterBootloaderResult(
      deviceId: deviceId,
      outcome: EnterBootloaderOutcome.notSent,
      mode: NodeMode.app,
      detail: fault?.kind == RequestFaultKind.busy
          ? 'the request never got a turn on the link: ${fault?.detail}'
          : 'the app has no link, so nothing was sent to the node',
    );
  }

  final refused = reply != null && reply.fail;
  // A reply with neither SUCCESS nor FAIL is not an ack; it is treated as silence, and the
  // identify probe decides - the same path as a lost frame.
  final acked = reply != null && reply.ok && !reply.partial;

  // The node's mode is read from what answers FOR it afterwards. A refusal needs no probe: the
  // node said it stays an app.
  BootloaderIdentity? bootloader;
  if (!refused) {
    bootloader = await (identify ?? _passthroughIdentify)(identifyTimeout);
  }

  return enterBootloaderVerdict(
    deviceId: deviceId,
    acked: acked,
    refused: refused,
    bootloader: bootloader,
    detail: reply != null && reply.ok && reply.partial
        ? 'the ack arrived incomplete (${reply.gap})'
        : null,
  );
}

/// The verdict for one attempt, from the three things that can be known about it.
///
/// Pure on purpose: the ack's presence, the node's refusal and the bootloader's identify are the
/// whole input, so every combination is a unit test rather than a bus session.
EnterBootloaderResult enterBootloaderVerdict({
  required int deviceId,
  required bool acked,
  required bool refused,
  BootloaderIdentity? bootloader,
  String? detail,
}) {
  if (refused) {
    return EnterBootloaderResult(
      deviceId: deviceId,
      outcome: EnterBootloaderOutcome.refused,
      mode: NodeMode.app,
    );
  }
  if (acked) {
    return EnterBootloaderResult(
      deviceId: deviceId,
      outcome: EnterBootloaderOutcome.accepted,
      mode: NodeMode.bootloader,
      bootloader: bootloader,
      detail: bootloader == null
          ? 'the bootloader did not answer an identify'
          : detail,
    );
  }
  if (bootloader != null) {
    return EnterBootloaderResult(
      deviceId: deviceId,
      outcome: EnterBootloaderOutcome.ackLost,
      mode: NodeMode.bootloader,
      bootloader: bootloader,
      detail: detail,
    );
  }
  return EnterBootloaderResult(
    deviceId: deviceId,
    outcome: EnterBootloaderOutcome.notImplemented,
    mode: NodeMode.app,
    detail: detail,
  );
}

/// True when the request never reached a device: no link, or no slot on the link's queue. Both
/// are the app's own side, so neither may be reported as a node that did not answer.
bool _nothingWasSent(RequestFault? fault) =>
    fault?.kind == RequestFaultKind.noLink ||
    fault?.kind == RequestFaultKind.busy;

/// Asks the core's bootloader passthrough for the node's identify.
///
/// A passthrough refusal of any kind (busy core, a node that did not answer, a link that went
/// away) is "no bootloader answered for the node", which is a fact about the mode and not a fault
/// of this command: the device's own silence is what the passthrough reports as `nodeSilent`, and
/// every other reason is logged rather than raised.
Future<BootloaderIdentity?> _passthroughIdentify(Duration timeout) async {
  try {
    return await PassthroughTransport(coreId: coreId).identify(timeout);
  } on PassthroughFailure catch (failure) {
    AppDiagnostics.log('enter-bootloader', 'identify: ${failure.message}');
    return null;
  }
}
