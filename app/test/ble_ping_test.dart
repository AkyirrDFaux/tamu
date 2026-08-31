@Tags(['ble'])
library;

import 'package:flutter/foundation.dart';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/diagnostics.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  debugDefaultTargetPlatformOverride = TargetPlatform.linux;

  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  test('ble ping x10', () async {
    final mgr = ConnectionManager.instance;
    mgr.source = LinkSource.ble;
    await mgr.setAutoRefresh(false);
    DiscoveredLink? link;
    for (var round = 0; round < 3 && link == null; round++) {
      await mgr.refresh();
      final deadline = DateTime.now().add(const Duration(seconds: 10));
      while (link == null && DateTime.now().isBefore(deadline)) {
        await Future<void>.delayed(const Duration(milliseconds: 400));
        link = mgr.discoveredLinks
            .where((l) => l.type == LinkType.ble && l.id == 'E4:B0:63:C8:20:72')
            .firstOrNull;
      }
      await mgr.stopScan();
    }
    if (link == null) {
      // ignore: avoid_print
      print('[B] device not found');
      return;
    }
    final err = await mgr.connectTo(link);
    // ignore: avoid_print
    print('[B] connectTo -> $err');
    addTearDown(() => mgr.disconnect());
    final db = DeviceDatabase.instance;
    for (var i = 0; i < 10; i++) {
      final started = DateTime.now();
      try {
        final ok = await db.pingCore()
            .timeout(const Duration(seconds: 5));
        // ignore: avoid_print
        print('[B] ping $i ok=$ok connected=${mgr.isConnected} '
            '${DateTime.now().difference(started).inMilliseconds} ms');
      } catch (e) {
        // ignore: avoid_print
        print('[B] ping $i FAILED: $e connected=${mgr.isConnected} '
            '${DateTime.now().difference(started).inMilliseconds} ms');
      }
    }
    // ignore: avoid_print
    print('[B] diagnostics:\n${AppDiagnostics.dump()}');

    // HIL-style: immediate refreshNetwork after connect (races the unawaited one)
    await db.refreshNetwork();
    final core = db.byId(1);
    // ignore: avoid_print
    print('[B] HIL-style immediate: core type=${core?.type} '
        'cap=${core?.capabilities.toRadixString(16)}');

    // wait a bit for the unawaited refresh to finish, then re-check
    await Future<void>.delayed(const Duration(seconds: 3));
    final core2 = db.byId(1);
    // ignore: avoid_print
    print('[B] after 3s settle: core type=${core2?.type} '
        'sn=${core2?.serialNumber} devices=${db.all.map((d) => d.id).toList()}');

    // concurrent pings
    final concurrent = await Future.wait(
        List.generate(8, (_) => db.pingCore().catchError((e) => false)));
    // ignore: avoid_print
    print('[B] concurrent 8 pings: ok=${concurrent.where((x) => x).length}/8');
  }, timeout: const Timeout(Duration(minutes: 2)), skip: skipReason);
}