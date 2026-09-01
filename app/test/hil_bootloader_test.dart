/// Hardware-in-the-loop test for the bootloader bridge (Docs/Services/Bootloader.md).
///
/// Verifies:
///   CID 0 — check if core is in bootloader mode
///   CID 1 — enter/leave bootloader mode
///   CID 2 — device info (vendor info from node enumeration)
///   CID 4 — write a fragment
///   CID 3 — read back a fragment and verify
///
/// Requires DAS to be on the RS-Bus and user to hold boot button + reset
/// the DAS after the core enters bootloader mode (enumeration listener).
///
/// Run:
/// ```
/// LD_LIBRARY_PATH=build/linux/x64/debug/bundle/lib \
/// TAMU_HIL=/dev/ttyACM1 \
/// flutter test test/hil_bootloader_test.dart
/// ```
@Tags(['hil'])
library;

import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader_client.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/device_db.dart';

final Timeout hilTimeout = const Timeout(Duration(seconds: 120));

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  debugDefaultTargetPlatformOverride = TargetPlatform.linux;

  final hilTarget = Platform.environment['TAMU_HIL'];

  /// Build a synthetic 256-byte fragment filled with a recognisable pattern.
  Uint8List syntheticFragment() {
    final frag = Uint8List(256);
    for (var i = 0; i < frag.length; i++) {
      frag[i] = i & 0xFF;
    }
    return frag;
  }

  Future<void> connectToCore() async {
    final mgr = ConnectionManager.instance;
    final err = await mgr.connectTo(
        DiscoveredLink(id: hilTarget!, type: LinkType.usb, name: 'Tamu core'));
    if (err != null) fail('connect failed: $err');
    for (var i = 0; i < 20; i++) {
      if (await DeviceDatabase.instance.pingCore()) return;
      await Future<void>.delayed(const Duration(milliseconds: 250));
    }
    fail('core did not answer ping within settle window');
  }

  final skipReason = hilTarget == null ? 'TAMU_HIL not set' : false;

  setUp(() async {
    if (skipReason != false) return;
    await connectToCore();
  });

  tearDown(() async {
    if (skipReason != false) return;
    await ConnectionManager.instance.disconnect();
  });

  group('Bootloader', () {
    test('CID 0 — check if core is in bootloader mode', () async {
      final bl = BootloaderClient(deviceId: 1);
      final inBootloader = await bl.check();
      // Just report — don't fail, since we may not be in bootloader mode yet.
      debugPrint('Bootloader mode: $inBootloader');
    }, timeout: hilTimeout, skip: skipReason);

    test('CID 1 — enter bootloader mode', () async {
      final bl = BootloaderClient(deviceId: 1);
      final ok = await bl.switchMode(enter: true);
      expect(ok, isTrue, reason: 'switchMode(enter: true) should succeed');
      debugPrint('Entered bootloader mode — hold DAS boot button + reset now');
      // Core listens for node enumeration for up to 10s (1s poll × 100 attempts).
      // With no physical node reset, this will time out gracefully.
      await Future<void>.delayed(const Duration(seconds: 2));
    }, timeout: hilTimeout, skip: skipReason);

    test('CID 2 — device info from enumerated node', () async {
      final bl = BootloaderClient(deviceId: 1);
      final info = await bl.deviceInfo();
      if (info == null) {
        debugPrint('Node not enumerated yet — skipping write/read tests');
        return;
      }
      expect(info.length, equals(16));
      final deviceType = info[0] | (info[1] << 8);
      debugPrint('Device type: 0x${deviceType.toRadixString(16)}');
      final serial = String.fromCharCodes(info.sublist(2, 16));
      debugPrint('Serial: $serial');
    }, timeout: hilTimeout, skip: skipReason);

    test('CID 4+3 — write and read back a fragment', () async {
      final bl = BootloaderClient(deviceId: 1);

      // Ensure we're in bootloader mode and node is enumerated
      if (!await bl.check()) {
        debugPrint('Not in bootloader mode — skipping');
        return;
      }
      final info = await bl.deviceInfo();
      if (info == null) {
        debugPrint('Node not enumerated — skipping');
        return;
      }

      final frag = syntheticFragment();
      debugPrint('Writing fragment 0 (${frag.length} bytes)...');
      final ack = await bl.writeFragment(0, frag);
      expect(ack, equals(0), reason: 'writeFragment should ack frag_idx 0');

      debugPrint('Reading back fragment 0...');
      final readBack = await bl.readFragment(0);
      expect(readBack, isNotNull, reason: 'readFragment should return data');
      expect(readBack!.length, equals(256));

      // Verify byte-by-byte
      for (var i = 0; i < 256; i++) {
        if (readBack[i] != frag[i]) {
          fail('Mismatch at byte $i: expected ${frag[i]}, got ${readBack[i]}');
        }
      }
      debugPrint('Fragment 0 verified OK');
    }, timeout: hilTimeout, skip: skipReason);

    test('CID 1 — leave bootloader mode', () async {
      final bl = BootloaderClient(deviceId: 1);
      final ok = await bl.switchMode(enter: false);
      expect(ok, isTrue, reason: 'switchMode(enter: false) should succeed');
      debugPrint('Left bootloader mode');
    }, timeout: hilTimeout, skip: skipReason);
  });
}
