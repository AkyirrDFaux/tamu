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
///
/// What that means for the in-app notices is `device_discovery_notice_test.dart`'s subject; both
/// files drive the same sweep through the same fake (`support/sweep_channel.dart`).
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/request_channel.dart';

import 'support/sweep_channel.dart';

/// A database with ids 2, 3 and 4 already known (serials set), so a sweep takes the ping branch
/// for each of them instead of re-reading their identity.
DeviceDatabase _knownNetwork() {
  final db = DeviceDatabase.forTest()..linkIsUpOverride = true;
  for (var i = 0; i < dasSerials.length; i++) {
    db.seedDeviceForTest(
      DeviceEntry(id: i + 2)
        ..serialNumber = dasSerials[i]
        ..name = 'Sensors ${i + 1}',
    );
  }
  return db;
}

void main() {
  // The sweep raises in-app events (the session's baseline is silent, but a later discovery is
  // not), and the notification path reads a GlobalKey messenger, which needs the binding.
  TestWidgetsFlutterBinding.ensureInitialized();

  tearDown(() => installRequestChannel(null));

  test('one lost reply is re-sent, the node stays present', () async {
    // The owner's case: the first ping frame to the DAS is lost, the re-send answers.
    final channel = SweepChannel(
      pingScript: {
        2: [null, pingOk()],
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
    final channel = SweepChannel(
      pingScript: {
        2: [null, null, pingOk()],
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
    final channel = SweepChannel(
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
    final channel = SweepChannel(
      pingScript: {
        2: [null, null, pingOk(), null],
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
    final channel = SweepChannel(
      pingScript: {
        2: [pingHoled(), pingOk()],
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
    final channel = SweepChannel(
      roster: [
        for (var i = 0; i < 2; i++) ...rosterRecord(dasSerials[i], i + 2),
      ],
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
