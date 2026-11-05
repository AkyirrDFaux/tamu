/// The fake App Interface link one `DeviceDatabase` sweep can run against.
///
/// A sweep is three kinds of request - the SNDB roster (Device CID 0x13), one ping per known
/// device (Device CID 1) and the System register reads an identity needs - and nothing else, so
/// this fake answers exactly those and throws for the rest. No bus, no radio, no timers unless the
/// test scripts a lost reply (which pays the roster read's own 200/600 ms backoff).
///
/// Shared by `device_stale_debounce_test.dart` (what a lost ping does to presence) and
/// `device_discovery_notice_test.dart` (what a new node does to the notices), because both drive
/// the same sweep and must not disagree about its answers.
library;

import 'package:tamuapp/core/fragment_reassembler.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/types.dart';

/// The three DAS serials the rig's core reports for ids 2, 3 and 4 (captured 2026-11-05).
const List<String> dasSerials = [
  'CDABA26400000000000000000000',
  'CDABBD6400000000000000000000',
  'CDABA06400000000000000000000',
];

/// The core's own serial (System field 1), so the sweep sees a known core.
const String coreSerialHex = 'E4B063C820700000000000000000';

/// One serial number as the 14 bytes the wire carries.
List<int> serialBytes(String hex) => [
  for (var i = 0; i < hex.length ~/ 2; i++)
    int.parse(hex.substring(i * 2, i * 2 + 2), radix: 16),
];

/// One SNDB record: the 14-byte serial and the node's id.
List<int> rosterRecord(String serial, int id) => [
  ...serialBytes(serial),
  id & 0xFF,
  (id >> 8) & 0xFF,
];

/// The rig's roster: ids 2, 3, 4 (the core is never in its own SNDB).
final List<int> rigRoster = [
  for (var i = 0; i < dasSerials.length; i++)
    ...rosterRecord(dasSerials[i], i + 2),
];

/// The rig's whole reply: one fragment that is also the last one.
PacketResponse wholeReply(List<int> body) => PacketResponse(
  payload: body,
  success: true,
  fail: false,
  chunks: [FragmentChunk(0, body)],
  stopped: true,
  total: 1,
);

/// A good ping answer: Device CID 1 acknowledges with an empty payload.
PacketResponse pingOk() => wholeReply(const [0]);

/// A reply that lost a fragment and never saw its STOP frame - the shape the link's stall
/// watchdog hands back over BLE. It is NOT an answer.
PacketResponse pingHoled() => PacketResponse(
  payload: const [0, 0],
  success: true,
  fail: false,
  missing: const [1],
  stopped: false,
  total: 2,
);

/// A fake App Interface channel one sweep can run against.
class SweepChannel implements TolerantRequestChannel {
  SweepChannel({
    Map<int, List<PacketResponse?>>? pingScript,
    List<int>? roster,
    Map<int, String>? names,
  }) : pingScript = pingScript ?? <int, List<PacketResponse?>>{},
       roster = roster ?? rigRoster,
       names = names ?? <int, String>{};

  /// Per device id: the replies to its pings in order; the LAST entry repeats forever, so a
  /// single `[null]` script is "this device never answers". A null entry is a lost frame -
  /// thrown, exactly as a request timeout reaches the client.
  final Map<int, List<PacketResponse?>> pingScript;

  /// The body the core answers SNDB Read All (CID 0x13) with. Mutable: a test makes a device
  /// appear between sweeps by replacing it.
  List<int> roster;

  /// When true the roster read comes back as a stream that lost a fragment and never saw its
  /// STOP frame - the read policy re-requests it (200/600 ms apart) and finally reports a fault.
  bool rosterReadFails = false;

  /// The System Name (field 6) each device reports. A device with no entry answers nothing,
  /// which the identity reader turns into its per-id fallback name (`Device 5`).
  final Map<int, String> names;

  /// One line per request, in the order they were sent, for the test to quote.
  final List<String> log = [];

  /// Ping requests received per device id.
  final Map<int, int> pingsSent = {};

  @override
  bool get isConnected => true;

  @override
  int takeTxId() => 0xF900;

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
  }) async {
    if (service == ServiceType.device && functionCid == 0x13) {
      log.add('roster read');
      return rosterReadFails ? pingHoled() : wholeReply(roster);
    }
    if (service == ServiceType.device && functionCid == 1) {
      final n = (pingsSent[targetId] ?? 0) + 1;
      pingsSent[targetId] = n;
      final script = pingScript[targetId];
      final reply = script == null
          ? pingOk()
          : script[n - 1 < script.length ? n - 1 : script.length - 1];
      log.add('dev $targetId ping $n: ${reply == null ? 'lost' : 'answered'}');
      if (reply == null) {
        throw StateError('device CID 1 request timed out');
      }
      return reply;
    }
    if (service == ServiceType.register) {
      return _systemField(targetId, payload);
    }
    throw StateError('unexpected $service/$functionCid to dev $targetId');
  }

  /// A system serial for [id]: the core's own, one of the rig's DAS serials, or a synthetic one
  /// for a node the rig does not have.
  String _serialFor(int id) {
    if (id == coreId) return coreSerialHex;
    final i = id - 2;
    if (i >= 0 && i < dasSerials.length) return dasSerials[i];
    return 'CDAB00${id.toRadixString(16).padLeft(2, '0').toUpperCase()}'
        '00000000000000000000';
  }

  /// The System block reads `readDeviceIdentity` makes: field 1 is the serial, field 6 the name
  /// (when the test gave one), and every other field answers empty - the identity reader treats
  /// that as "the device reported nothing", which keeps this fake to the parts under test.
  PacketResponse _systemField(int targetId, List<int> payload) {
    final info = payload.length >= 4 ? uint32FromBytes(payload) : 0;
    switch (blockInfoField(info)) {
      case 1:
        final bytes = serialBytes(_serialFor(targetId));
        return wholeReply([
          ...uint32ToBytes(info),
          ...ValueInfo(type: 0, size: bytes.length).toBytes(),
          ...bytes,
        ]);
      case 6:
        final name = names[targetId];
        if (name == null) {
          return const PacketResponse(payload: [], success: true, fail: false);
        }
        final bytes = name.codeUnits;
        return wholeReply([
          ...uint32ToBytes(info),
          ...ValueInfo(type: DataType.name.value, size: bytes.length).toBytes(),
          ...bytes,
        ]);
      default:
        return const PacketResponse(payload: [], success: true, fail: false);
    }
  }
}
