/// "Device discovered" is for a node that appears WHILE the app is connected, not for the nodes
/// the initial roster load found.
///
/// The owner (2026-11-05): "limit 'device discovered' to new devices that appear only after
/// connecting, not in the initial load." Before this, `DeviceDatabase._doRefresh` announced every
/// id that was not in its map at the start of the sweep, so a normal connect with four nodes read
/// as four "discovered" lines on top of the connect itself.
///
/// The rule now: the first whole sweep of a session is the BASELINE (`_discoveryBaseline` is null
/// until it completes) and announces nothing; later sweeps announce only ids the baseline does not
/// hold, one notice each. A node leaves the baseline only when it was announced "Device lost", so a
/// node that misses a ping or two - inside `deviceMissThreshold` - announces nothing when it
/// answers again. A failed first sweep never reaches the baseline capture, so the next whole sweep
/// becomes it: the announcements are delayed, never suppressed for the session.
library;

import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/notice.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/transport.dart';

import 'support/sweep_channel.dart';

/// A fifth node that is not on the rig, for "a device appeared after the connect".
const String fifthSerialHex = 'CDAB0F0500000000000000000000';

/// The four names the initial load reports: the core reports none, so it keeps its fallback.
Map<int, String> _rigNames() => {
  2: 'Sensors 1',
  3: 'Sensors 2',
  4: 'Sensors 3',
};

/// The roster with [fifthSerialHex] added, so the next sweep finds a node that was not there.
List<int> _rosterWithFifthNode() => [
  ...rigRoster,
  ...rosterRecord(fifthSerialHex, 5),
];

/// Every notice the app published while the test ran, in order.
///
/// A notice replaces the one on screen (there is no queue to read back), so the events are caught
/// as they are raised - which is also how the app experiences them.
class _NoticeLog {
  final List<AppNotice> _seen = [];

  void start() => NoticeCenter.instance.addListener(_onChange);

  void stop() => NoticeCenter.instance.removeListener(_onChange);

  void _onChange() {
    final notice = NoticeCenter.instance.current;
    if (notice == null) return;
    if (_seen.isNotEmpty && _seen.last.id == notice.id) return;
    _seen.add(notice);
  }

  List<AppNotice> _of(String event) => [
    for (final notice in _seen)
      if (notice.event == event) notice,
  ];

  List<AppNotice> get discovered => _of('Device discovered');

  List<AppNotice> get lost => _of('Device lost');

  List<String> get discoveredMessages => [
    for (final notice in discovered) notice.message,
  ];

  List<String> get lostMessages => [for (final notice in lost) notice.message];
}

/// A session link for the real connect path: it comes up at once and never sends a byte.
class _SessionTransport implements Transport {
  final _packets = StreamController<Uint8List>.broadcast();

  @override
  String get id => 'AA:BB:CC:DD:EE:02';

  @override
  String get displayName => 'BLE $id';

  @override
  Stream<Uint8List> get linkBytes => _packets.stream;

  @override
  Stream<Uint8List> get packetStream => _packets.stream;

  @override
  Future<void> connect() async {}

  @override
  Future<void> send(List<int> streamBytes) async {}

  @override
  Future<void> close() async {}
}

void main() {
  // The sweep's notice path reads the app's messenger GlobalKey.
  TestWidgetsFlutterBinding.ensureInitialized();

  late _NoticeLog notices;

  setUp(() {
    // A host has to be mounted or `NoticeCenter.raise` publishes nothing (it will not keep a
    // notice for a strip that does not exist); the occurrence numbers restart with each test.
    NoticeCenter.instance
      ..reset()
      ..attachHost();
    notices = _NoticeLog()..start();
  });

  tearDown(() {
    notices.stop();
    NoticeCenter.instance
      ..detachHost()
      ..reset();
    installRequestChannel(null);
  });

  test('the initial sweep with four nodes announces nothing', () async {
    final channel = SweepChannel(names: _rigNames());
    installRequestChannel(channel);
    final db = DeviceDatabase.forTest()..linkIsUpOverride = true;

    await db.refreshNetwork();

    expect(db.lastError, isNull);
    expect(
      db.all.length,
      4,
      reason: 'the core plus the three DAS nodes the roster names',
    );
    expect(
      notices.discovered,
      isEmpty,
      reason:
          'nothing was discovered: this IS the network the app connected to',
    );
    expect(
      db.discoveryBaselineForTest,
      {coreId, 2, 3, 4},
      reason: 'the first whole sweep captured the baseline',
    );
  });

  test('a node that appears later is announced exactly once', () async {
    final channel = SweepChannel(names: _rigNames());
    installRequestChannel(channel);
    final db = DeviceDatabase.forTest()..linkIsUpOverride = true;
    await db.refreshNetwork();
    expect(notices.discovered, isEmpty);

    // A DAS joins the network while the app is connected.
    channel.roster = _rosterWithFifthNode();
    channel.names[5] = 'Sensors 4';
    await db.refreshNetwork();

    expect(
      notices.discoveredMessages,
      ['Sensors 4 discovered'],
      reason: 'one notice, naming the device that arrived',
    );

    // The next sweeps find the same five nodes: the notice is not replayed.
    await db.refreshNetwork();
    await db.refreshNetwork();
    expect(notices.discovered.length, 1);
    expect(db.discoveryBaselineForTest, {coreId, 2, 3, 4, 5});
  });

  test('a node that flaps inside the debounce window announces nothing', () async {
    final channel = SweepChannel(names: _rigNames());
    installRequestChannel(channel);
    final db = DeviceDatabase.forTest()..linkIsUpOverride = true;
    await db.refreshNetwork();

    // Two lost pings in a row, then an answer: inside deviceMissThreshold, so the node never
    // reads stale. The old debounce test proves the presence half; this is the notice half.
    channel.pingScript[2] = [null, null, pingOk()];
    await db.refreshNetwork();
    await db.refreshNetwork();

    expect(db.byId(2)!.stale, isFalse, reason: 'it was never gone');
    expect(
      notices.discovered,
      isEmpty,
      reason: 'a node that flaked out and back is not a discovery',
    );
    expect(notices.lost, isEmpty);
  });

  test('a node announces again only after it was announced lost', () async {
    final channel = SweepChannel(names: _rigNames());
    installRequestChannel(channel);
    final db = DeviceDatabase.forTest()..linkIsUpOverride = true;
    await db.refreshNetwork();

    // Three sends, none answered: the node is stale, so it IS announced lost - and leaves the
    // baseline, which is the one way it can be discovered a second time.
    channel.pingScript[2] = [null];
    await db.refreshNetwork();
    expect(db.byId(2)!.stale, isTrue);
    expect(notices.lostMessages, ['Sensors 1 lost']);
    expect(notices.discovered, isEmpty);

    // It answers again: one discovery, because it had been announced gone.
    channel.pingScript[2] = [pingOk()];
    await db.refreshNetwork();

    expect(notices.discoveredMessages, ['Sensors 1 discovered']);
    expect(db.byId(2)!.stale, isFalse);
  });

  test('a reconnect starts a new baseline and announces no known node', () async {
    final channel = SweepChannel(names: _rigNames());
    installRequestChannel(channel);
    final db = DeviceDatabase.forTest()..linkIsUpOverride = true;

    await db.refreshNetwork(); // session 1: the four-node baseline
    channel.roster = _rosterWithFifthNode();
    channel.names[5] = 'Sensors 4';
    await db.refreshNetwork(); // one real discovery
    expect(notices.discovered.length, 1);

    // What `ConnectionManager.connectTo` does when the link comes back up (its own test is below).
    db.resetDiscoveryBaseline();
    await db.refreshNetwork();

    expect(
      db.discoveryBaselineForTest,
      {coreId, 2, 3, 4, 5},
      reason: 'the loaded network became the new baseline',
    );
    expect(
      notices.discovered.length,
      1,
      reason: 'the nodes that were already known are not re-announced',
    );
  });

  test('a failed first sweep leaves the baseline for the next one', () async {
    // The roster read cannot be completed: the sweep throws before it can capture a baseline.
    // A session must not go silent for that - the next whole sweep is the baseline.
    final channel = SweepChannel(names: _rigNames())..rosterReadFails = true;
    installRequestChannel(channel);
    final db = DeviceDatabase.forTest()..linkIsUpOverride = true;

    await db.refreshNetwork();

    expect(db.lastError, contains('SNDB read failed'));
    expect(
      db.discoveryBaselineForTest,
      isNull,
      reason: 'a sweep that failed is not a baseline',
    );
    expect(notices.discovered, isEmpty);

    // The link recovers: this sweep is the baseline, so it announces nothing...
    channel.rosterReadFails = false;
    await db.refreshNetwork();
    expect(db.discoveryBaselineForTest, {coreId, 2, 3, 4});
    expect(notices.discovered, isEmpty);

    // ...and the session is not silenced: the node that arrives next is still announced.
    channel.roster = _rosterWithFifthNode();
    channel.names[5] = 'Sensors 4';
    await db.refreshNetwork();
    expect(notices.discoveredMessages, ['Sensors 4 discovered']);
  });

  test('a connect starts a fresh baseline', () async {
    // The real path, on the app's own manager and database: `connectTo` is where a session comes
    // up. The first connect's sweep makes a baseline; the second connect's sweep FAILS its roster
    // read, so an unset baseline afterwards proves the connect cleared it rather than carrying
    // the previous session's over.
    const link = DiscoveredLink(
      id: 'AA:BB:CC:DD:EE:02',
      type: LinkType.ble,
      name: 'Tamu core',
    );
    final manager = ConnectionManager.instance;
    final db = DeviceDatabase.instance;
    final channel = SweepChannel(names: _rigNames());
    installRequestChannel(channel);
    manager.transportFactory = (_) => _SessionTransport();
    addTearDown(manager.disconnect);

    expect(await manager.connectTo(link), isNull);
    await db.refreshNetwork(); // joins the sweep connectTo started
    expect(db.discoveryBaselineForTest, {coreId, 2, 3, 4});
    expect(notices.discovered, isEmpty);

    await manager.disconnect();
    channel.rosterReadFails = true;
    expect(await manager.connectTo(link), isNull);
    await db.refreshNetwork();

    expect(db.lastError, contains('SNDB read failed'));
    expect(
      db.discoveryBaselineForTest,
      isNull,
      reason: 'the new session began with no baseline at all',
    );
  });
}
