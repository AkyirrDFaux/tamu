@Tags(['ble'])
library;

import 'package:flutter/foundation.dart';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  debugDefaultTargetPlatformOverride = TargetPlatform.linux;

  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  test('ble scan probe', () async {
    final mgr = ConnectionManager.instance;
    mgr.source = LinkSource.ble;
    await mgr.setAutoRefresh(false);
    await mgr.refresh();
    final deadline = DateTime.now().add(const Duration(seconds: 15));
    while (DateTime.now().isBefore(deadline)) {
      await Future<void>.delayed(const Duration(milliseconds: 500));
      final links = mgr.discoveredLinks
          .where((l) => l.type == LinkType.ble)
          .toList();
      if (links.isNotEmpty) {
        // ignore: avoid_print
        print('[S] BLE links: '
            '${links.map((l) => 'id=${l.id} name=${l.name}').toList()}');
      }
    }
    await mgr.stopScan();
    // ignore: avoid_print
    print('[S] scan done');
  }, timeout: const Timeout(Duration(seconds: 40)), skip: skipReason);
}