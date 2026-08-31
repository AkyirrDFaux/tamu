@Tags(['hil', 'ble'])
library;

import 'package:flutter/foundation.dart';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/device_db.dart';

const String kCoreBleMacAddress = 'E4:B0:63:C8:20:72';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  debugDefaultTargetPlatformOverride = TargetPlatform.linux;

  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  test('ping RTT probe', () async {
    final mode = Platform.environment['TAMU_HIL']!;

    final mgr = ConnectionManager.instance;
    await mgr.setAutoRefresh(false);
    late final DiscoveredLink link;
    if (mode == 'ble') {
      mgr.source = LinkSource.ble;
      DiscoveredLink? found;
      for (var round = 0; round < 3 && found == null; round++) {
        await mgr.refresh();
        final deadline = DateTime.now().add(const Duration(seconds: 12));
        while (found == null && DateTime.now().isBefore(deadline)) {
          await Future<void>.delayed(const Duration(milliseconds: 500));
          found = mgr.discoveredLinks
              .where((l) => l.type == LinkType.ble)
              .where((l) => l.id.toUpperCase() == kCoreBleMacAddress)
              .firstOrNull;
        }
      }
      expect(found, isNotNull, reason: 'Tamu not advertising');
      await mgr.stopScan();
      link = found!;
    } else {
      link =
          DiscoveredLink(id: mode, type: LinkType.usb, name: 'Tamu core');
    }

    final db = DeviceDatabase.instance;
    final err = await mgr.connectTo(link);
    if (err != null) fail('connect failed: $err');
    addTearDown(mgr.disconnect);

    bool up = false;
    for (var i = 0; i < 10 && !up; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 400));
      up = await db.pingCore();
    }
    expect(up, isTrue, reason: 'device did not answer pings after connect');

    final rtts = <int>[];
    for (var i = 0; i < 25; i++) {
      final sw = Stopwatch()..start();
      final ok = await db.pingCore();
      sw.stop();
      expect(ok, isTrue);
      if (i >= 5) rtts.add(sw.elapsedMilliseconds);
      await Future.delayed(const Duration(milliseconds: 30));
    }
    rtts.sort();
    // ignore: avoid_print
    print('[RTT] min=${rtts.first} median=${rtts[rtts.length ~/ 2]} '
        'max=${rtts.last} '
        'avg=${rtts.reduce((a, b) => a + b) ~/ rtts.length} ms');
  }, timeout: const Timeout(Duration(minutes: 3)), skip: skipReason);
}
