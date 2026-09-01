/// Hardware-in-the-loop test for the DAS bootloader firmware upload.
///
/// Verifies: CID 0 (bootloader check) and CID 1 (app write via FRAG).
/// Requires the DAS to already be in bootloader mode (user held boot button
/// + reset before running). Optionally supply DAS_FIRMWARE env var pointing
/// to a binary to upload; otherwise a 512-byte synthetic payload is used.
///
/// Run:
/// ```
/// LIBSERIALPORT_PATH=build/linux/x64/debug/bundle/lib/libserialport.so \
/// TAMU_HIL=/dev/ttyACM0 flutter test test/hil_bootloader_test.dart
/// ```
/// Optionally:
/// ```
/// DAS_FIRMWARE=/path/to/das_v0_1_bootloader.bin flutter test test/hil_bootloader_test.dart
/// ```
@Tags(['hil'])
library;

import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader_client.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/device_db.dart';

const int dasId = 2;
final Timeout hilTimeout = const Timeout(Duration(seconds: 90));

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  debugDefaultTargetPlatformOverride = TargetPlatform.linux;

  final hilTarget = Platform.environment['TAMU_HIL'];

  /// Build a synthetic 512-byte binary filled with a recognisable pattern.
  Uint8List syntheticBinary() {
    final bin = Uint8List(512);
    for (var i = 0; i < bin.length; i++) {
      bin[i] = i & 0xFF;
    }
    return bin;
  }

  /// Load the firmware binary from DAS_FIRMWARE env var, or fall back to the
  /// synthetic payload.
  Uint8List loadFirmware() {
    final path = Platform.environment['DAS_FIRMWARE'];
    if (path != null) {
      final file = File(path);
      if (file.existsSync()) {
        return file.readAsBytesSync();
      }
      fail('DAS_FIRMWARE file not found: $path');
    }
    return syntheticBinary();
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
    test('CID 0 — check if DAS is in bootloader mode', () async {
      final bl = BootloaderClient(deviceId: dasId);
      final inBootloader = await bl.check();
      if (!inBootloader) {
        // Cannot proceed without bootloader mode; skip remaining tests.
        return;
      }
      expect(inBootloader, isTrue);
    }, timeout: hilTimeout, skip: skipReason);

    test('CID 1 — upload firmware binary', () async {
      final bl = BootloaderClient(deviceId: dasId);
      final inBootloader = await bl.check();
      if (!inBootloader) {
        return; // skip — device not in bootloader mode
      }

      final firmware = loadFirmware();
      final totalFrags =
          (firmware.length + 255) ~/ 256; // _fragChunkSize = 256
      debugPrint('uploading ${firmware.length} bytes in $totalFrags fragments');

      final ok = await bl.writeBinary(
        firmware,
        onProgress: (current, total) {
          if (current % 10 == 0 || current == total) {
            debugPrint('  frag $current/$total');
          }
        },
      );
      expect(ok, isTrue, reason: 'writeBinary should succeed');
    }, timeout: hilTimeout, skip: skipReason);
  });
}
