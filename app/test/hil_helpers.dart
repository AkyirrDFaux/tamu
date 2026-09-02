/// Shared helpers for hardware-in-the-loop tests.
///
/// Usage:
/// ```dart
/// import 'hil_helpers.dart';
/// void main() => runHilTests('my test file', [hilTest('name', () async { ... })]);
/// ```
library;

import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/device_db.dart';

/// Connects to the core via USB or BLE based on [TAMU_HIL] env var, waits for
/// the core to answer pings. Returns `null` if connected, or a skip-reason
/// string if skipped.
///
/// Call this from `setUpAll`. The connection stays alive for all tests in the
/// file; `tearDownAll` disconnects automatically.
Future<String?> connectHil() async {
  TestWidgetsFlutterBinding.ensureInitialized();
  debugDefaultTargetPlatformOverride = TargetPlatform.linux;

  final target = Platform.environment['TAMU_HIL'];
  if (target == null) return 'TAMU_HIL not set';

  final mgr = ConnectionManager.instance;
  if (target == 'ble') {
    mgr.source = LinkSource.ble;
    await mgr.setAutoRefresh(false);
    DiscoveredLink? link;
    for (var round = 0; round < 3 && link == null; round++) {
      await mgr.refresh();
      final deadline = DateTime.now().add(const Duration(seconds: 12));
      while (link == null && DateTime.now().isBefore(deadline)) {
        await Future<void>.delayed(const Duration(milliseconds: 500));
        link = mgr.discoveredLinks
            .where((l) =>
                l.type == LinkType.ble &&
                l.id.toUpperCase() == 'E4:B0:63:C8:20:72')
            .firstOrNull;
      }
      await mgr.stopScan();
    }
    if (link == null) fail('Tamu not found during BLE scan');
    final err = await mgr.connectTo(link);
    if (err != null) fail('BLE connect failed: $err');
  } else {
    await mgr.setAutoRefresh(false);
    final err = await mgr.connectTo(
        DiscoveredLink(id: target, type: LinkType.usb, name: 'Tamu core'));
    if (err != null) fail('connect failed: $err');
  }

  // Wait for the core to answer pings (boot settle).
  for (var i = 0; i < 20; i++) {
    if (await DeviceDatabase.instance.pingCore()) return null;
    await Future<void>.delayed(const Duration(milliseconds: 250));
  }
  fail('core did not answer ping within settle window');
}

/// Disconnects from the core. Call from `tearDownAll`.
Future<void> disconnectHil() => ConnectionManager.instance.disconnect();
