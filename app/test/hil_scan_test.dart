/// Hardware test to scan the bus and see what nodes are present.
@Tags(['hil'])
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/device_db.dart';

import 'hil/hil_env.dart';

void main() {
  final env = HilEnv();
  setUpAll(env.connectAndScan);
  tearDownAll(env.finish);

  test('scan bus and list devices', () async {
    expect(env.hilRequested, isTrue, reason: "TAMU_HIL must be set");
    expect(env.rig.isEmpty, isFalse, reason: 'no devices found');

    print('=== Rig Inventory ===');
    for (final device in env.rig.devices) {
      print('  ${device.label}');
      print('    caps: 0x${device.capabilities.toRadixString(16)}');
      print('    blocks: ${device.blocks.map((b) => b.label).join(", ")}');
      print('    bootloader: ${device.bootloader}');
    }

    // Ping the core
    final db = DeviceDatabase.instance;
    final corePing = await db.pingCore();
    print('  Ping core: ${corePing ? "OK" : "FAIL"}');
  }, timeout: Timeout.none);
}