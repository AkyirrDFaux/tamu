import 'package:flutter/foundation.dart';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/device_db.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  debugDefaultTargetPlatformOverride = TargetPlatform.linux;

  test('usb -> ble switch', () async {
    final mgr = ConnectionManager.instance;
    await mgr.setAutoRefresh(false);
    final port = Platform.environment['TAMU_HIL']!;
    final db = DeviceDatabase.instance;

    // 1. USB connect + ping
    final usbErr = await mgr.connectTo(
        DiscoveredLink(id: port, type: LinkType.usb, name: 'Tamu'));
    // ignore: avoid_print
    print('[S] usb connect -> $usbErr');
    var up = false;
    for (var i = 0; i < 10 && !up; i++) {
      try {
        up = await db.pingCore();
      } catch (_) {}
      await Future<void>.delayed(const Duration(milliseconds: 250));
    }
    // ignore: avoid_print
    print('[S] usb ping ok=$up');

    // 2. manual disconnect (as the connection page does)
    await mgr.disconnect(manual: true);
    await Future<void>.delayed(const Duration(milliseconds: 500));
    // ignore: avoid_print
    print('[S] usb disconnected, isConnected=${mgr.isConnected}');

    // 3. BLE scan + connect (switch)
    mgr.source = LinkSource.ble;
    DiscoveredLink? ble;
    for (var round = 0; round < 3 && ble == null; round++) {
      await mgr.refresh();
      final deadline = DateTime.now().add(const Duration(seconds: 10));
      while (ble == null && DateTime.now().isBefore(deadline)) {
        await Future<void>.delayed(const Duration(milliseconds: 400));
        ble = mgr.discoveredLinks
            .where((l) => l.type == LinkType.ble && l.id == 'E4:B0:63:C8:20:72')
            .firstOrNull;
      }
      await mgr.stopScan();
    }
    if (ble == null) {
      // ignore: avoid_print
      print('[S] BLE device not found');
      return;
    }
    final bleErr = await mgr.connectTo(ble);
    // ignore: avoid_print
    print('[S] ble connect -> $bleErr connected=${mgr.isConnected}');

    // 4. ping over BLE after switch
    for (var i = 0; i < 6; i++) {
      try {
        final ok = await db.pingCore().timeout(const Duration(seconds: 5));
        // ignore: avoid_print
        print('[S] ble ping $i ok=$ok connected=${mgr.isConnected}');
      } catch (e) {
        // ignore: avoid_print
        print('[S] ble ping $i FAILED: $e connected=${mgr.isConnected}');
      }
    }
    await mgr.disconnect();
  }, timeout: const Timeout(Duration(minutes: 2)));
}