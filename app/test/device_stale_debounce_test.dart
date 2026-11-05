/// A single lost ping must not mark a live DAS stale.
///
/// The owner's report (2026-11-05): a DAS dropped out of the device list for a moment and came
/// back, reading as "not discovered properly" while its subscriptions and buttons kept working -
/// the node had never been gone. The roster read was one half of it (`sndb_roster_test.dart`);
/// the other half was `DeviceDatabase._doRefresh`'s known-device branch, which sent ONE
/// unretried ping and marked the node stale if that frame was lost.
///
/// The rule now: a known device's ping is re-sent inside the same sweep (three sends, the roster
/// read's own 200/600 ms backoff), every lost reply counts one *consecutive* miss, any whole
/// answer clears the count, and only `deviceMissThreshold` misses in a row mark the node stale.
/// Because the re-sends happen inside the sweep, a device that is really gone still reaches the
/// threshold in that same refresh - the stale bound is unchanged, and a sweep where nothing is
/// lost still costs one send per device.
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/fragment_reassembler.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/types.dart';

/// The three DAS serials the rig's core reports for ids 2, 3 and 4 (captured 2026-11-05).
const List<String> _dasSerials = [
  'CDABA26400000000000000000000',
  'CDABBD6400000000000000000000',
  'CDABA06400000000000000000000',
];

/// The core's own serial (System field 1), so the sweep sees a known core.
const String _coreSerialHex = 'E4B063C820700000000000000000';

List<int> _serialBytes(String hex) => [
  for (var i = 0; i < hex.length ~/ 2; i++)
    int.parse(hex.substring(i * 2, i * 2 + 2), radix: 16),
];

List<int> _record(String serial, int id) => [
  ..._serialBytes(serial),
  id & 0xFF,
  (id >> 8) & 0xFF,
];

List<int> _at(int i) => _record(_dasSerials[i], i + 2);

/// The rig's roster: ids 2, 3, 4 (the core is never in its own SNDB).
final List<int> _rigRoster = [
  for (var i = 0; i < _dasSerials.length; i++) ..._at(i),
];

/// The rig's whole reply: one fragment that is also the last one.
PacketResponse _whole(List<int> body) => PacketResponse(
  payload: body,
  success: true,
  fail: false,
  chunks: [FragmentChunk(0, body)],
  stopped: true,
  total: 1,
);

/// A good ping answer: Device CID 1 acknowledges with an empty payload.
PacketResponse _pingOk() => _whole(const [0]);

/// A ping reply that lost a fragment and never saw its STOP frame - the shape the link's stall
/// watchdog hands back over BLE. It is NOT an answer.
PacketResponse _pingHoled() => PacketResponse(
  payload: const [0, 0],
  success: true,
  fail: false,
  missing: const [1],
  stopped: false,
  total: 2,
);

/// A fake App Interface channel one sweep can run against: the roster read, the ping (with a
/// per-device script of lost/answered replies), and the System register reads the core's identity
/// needs. No bus, no radio.
class _SweepChannel implements TolerantRequestChannel {
  _SweepChannel({this.pingScript = const {}, List<int>? roster})
    : roster = roster ?? _rigRoster;

  /// Per device id: the replies to its pings in order; the LAST entry repeats forever, so a
  /// single `[null]` script is "this device never answers". A null entry is a lost frame -
  /// thrown, exactly as a request timeout reaches the client.
  final Map<int, List<PacketResponse?>> pingScript;

  /// The body the core answers SNDB Read All (CID 0x13) with.
  final List<int> roster;

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
      return _whole(roster);
    }
    if (service == ServiceType.device && functionCid == 1) {
      final n = (pingsSent[targetId] ?? 0) + 1;
      pingsSent[targetId] = n;
      final script = pingScript[targetId];
      final reply = script == null
          ? _pingOk()
          : script[n - 1 < script.length ? n - 1 : script.length - 1];
      log.add('dev $targetId ping $n: ${reply == null ? 'lost' : 'answered'}');
      if (reply == null) {
        throw StateError('device CID 1 request timed out');
      }
      return reply;
    }
    if (service == ServiceType.register) {
      return _systemField(payload);
    }
    throw StateError('unexpected $service/$functionCid to dev $targetId');
  }

  /// The System block reads `readDeviceIdentity` makes: field 1 is the serial, so it is answered
  /// with the core's own serial; every other field answers empty (the identity reader treats
  /// that as "the device reported nothing", which keeps this fake to the parts under test).
  PacketResponse _systemField(List<int> payload) {
    final info = payload.length >= 4 ? uint32FromBytes(payload) : 0;
    if (blockInfoField(info) != 1) {
      return const PacketResponse(payload: [], success: true, fail: false);
    }
    const meta = ValueInfo(type: 0, size: 14);
    return _whole([
      ...uint32ToBytes(info),
      ...meta.toBytes(),
      ..._serialBytes(_coreSerialHex),
    ]);
  }
}

/// A database with ids 2, 3 and 4 already known (serials set), so a sweep takes the ping branch
/// for each of them instead of re-reading their identity.
DeviceDatabase _knownNetwork() {
  final db = DeviceDatabase.forTest()..linkIsUpOverride = true;
  for (var i = 0; i < _dasSerials.length; i++) {
    db.seedDeviceForTest(
      DeviceEntry(id: i + 2)
        ..serialNumber = _dasSerials[i]
        ..name = 'Sensors ${i + 1}',
    );
  }
  return db;
}

void main() {
  // The sweep raises in-app events ("Device discovered" for the core), and the notification path
  // reads a GlobalKey messenger, which needs the binding to exist.
  TestWidgetsFlutterBinding.ensureInitialized();

  tearDown(() => installRequestChannel(null));

  test('one lost reply is re-sent, the node stays present', () async {
    // The owner's case: the first ping frame to the DAS is lost, the re-send answers.
    final channel = _SweepChannel(
      pingScript: {
        2: [null, _pingOk()],
      },
    );
    installRequestChannel(channel);
    final db = _knownNetwork();

    await db.refreshNetwork();

    expect(db.lastError, isNull);
    final node = db.byId(2)!;
    expect(node.stale, isFalse, reason: 'one lost frame cannot demote');
    expect(
      node.consecutiveMisses,
      0,
      reason: 'the answer cleared the miss count',
    );
    expect(channel.pingsSent[2], 2, reason: 'lost, then re-sent once');
    expect(
      channel.pingsSent[3],
      1,
      reason: 'an answering device costs one send',
    );
    expect(channel.log, contains('dev 2 ping 1: lost'));
    expect(channel.log, contains('dev 2 ping 2: answered'));
  });

  test('two lost replies are still re-sent: the node stays', () async {
    // deviceMissThreshold - 1 losses, i.e. one short of the demotion.
    final channel = _SweepChannel(
      pingScript: {
        2: [null, null, _pingOk()],
      },
    );
    installRequestChannel(channel);
    final db = _knownNetwork();

    await db.refreshNetwork();

    final node = db.byId(2)!;
    expect(deviceMissThreshold, 3);
    expect(node.stale, isFalse);
    expect(node.consecutiveMisses, 0);
    expect(channel.pingsSent[2], deviceMissThreshold);
  });

  test('three consecutive losses mark the node stale', () async {
    final channel = _SweepChannel(
      pingScript: {
        2: [null],
      },
    );
    installRequestChannel(channel);
    final db = _knownNetwork();

    await db.refreshNetwork();

    final node = db.byId(2)!;
    expect(
      channel.pingsSent[2],
      deviceMissThreshold,
      reason: 'the sweep used every send',
    );
    expect(node.consecutiveMisses, deviceMissThreshold);
    expect(node.stale, isTrue, reason: 'it never answered');
    expect(db.byId(3)!.stale, isFalse, reason: 'its neighbours are untouched');
  });

  test('a reply clears the count, so a later streak starts over', () async {
    // Sweep 1: two losses, then the third send answers. Sweep 2: every send is lost.
    final channel = _SweepChannel(
      pingScript: {
        2: [null, null, _pingOk(), null],
      },
    );
    installRequestChannel(channel);
    final db = _knownNetwork();

    await db.refreshNetwork();
    expect(
      db.byId(2)!.consecutiveMisses,
      0,
      reason: 'the answer reset a 2-miss streak',
    );
    expect(db.byId(2)!.stale, isFalse);

    await db.refreshNetwork();

    final node = db.byId(2)!;
    expect(
      channel.pingsSent[2],
      2 * devicePingAttempts,
      reason: 'a fresh streak: the count did not carry over',
    );
    expect(node.consecutiveMisses, deviceMissThreshold);
    expect(node.stale, isTrue);
  });

  test('a holed reply is not an answer, so it is re-sent', () async {
    // The roster read's own policy (`PacketResponse.gap`), applied to the ping: a stream that
    // stopped without its STOP frame is not a whole answer.
    final channel = _SweepChannel(
      pingScript: {
        2: [_pingHoled(), _pingOk()],
      },
    );
    installRequestChannel(channel);
    final db = _knownNetwork();

    await db.refreshNetwork();

    final node = db.byId(2)!;
    expect(channel.pingsSent[2], 2);
    expect(node.stale, isFalse);
    expect(node.consecutiveMisses, 0);
  });

  test('a roster without the node marks it stale at once', () async {
    // A whole roster read is the one place an absence is real; it is not debounced.
    final channel = _SweepChannel(
      roster: [for (var i = 0; i < 2; i++) ..._at(i)],
    );
    installRequestChannel(channel);
    final db = _knownNetwork();

    await db.refreshNetwork();

    expect(db.byId(4)!.stale, isTrue);
    expect(db.byId(4)!.consecutiveMisses, 0);
    expect(channel.pingsSent[4], isNull, reason: 'it is not in the roster');
    expect(db.byId(2)!.stale, isFalse);
    expect(db.byId(3)!.stale, isFalse);
  });
}
