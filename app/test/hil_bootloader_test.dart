/// Flashes the whole main binary through the new bootloader (Docs/Services/Bootloader.md).
///
/// Requires the target node to be sitting in bootloader mode (the DAS flashed with
/// `-D BOOTLOADER_FORCE`, since the button cannot be held from a test). The image defaults
/// to the DAS app build; override with `TAMU_HIL_BIN`.
///
///   TAMU_HIL=/dev/ttyACM1 TAMU_HIL_BIN=../firmware/.pio/build/DAS_v0_1/firmware.bin \
///       bash test/run_hil_tests.sh test/hil_bootloader_test.dart
@Tags(['hil'])
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader_client.dart';
import 'package:tamuapp/core/device_db.dart';
import 'hil_helpers.dart';

void main() {
  final skipReason = hilSetup();

  late BootloaderClient boot;

  setUpAll(() async {
    if (skipReason != null) return;
    final db = DeviceDatabase.instance;
    await db.refreshNetwork();
    await Future<void>.delayed(const Duration(seconds: 2));
    await db.refreshNetwork();
    // The core relays the raw frames onto its RSBus, so we address the connected core.
    final tamu = findTamu(db) ?? (throw StateError('Tamu not found'));
    boot = BootloaderClient(coreId: tamu.id);
  });

  test('flashes and verifies the entire main binary', skip: skipReason,
      timeout: const Timeout(Duration(minutes: 10)), () async {
    final path = Platform.environment['TAMU_HIL_BIN'] ??
        '../firmware/.pio/build/DAS_v0_1/firmware.bin';
    final file = File(path);
    if (!file.existsSync()) {
      markTestSkipped('main binary not found at $path (build it, or set TAMU_HIL_BIN)');
      return;
    }
    final image = file.readAsBytesSync();
    expect(image, isNotEmpty);

    // The node answers a read only while in bootloader mode; the running app waits for 0xAA
    // and ignores the raw 0xCA frame.
    final probe = await boot.readChunk(0);
    expect(probe, isNotNull,
        reason: 'no bootloader reply - is the node in bootloader mode (BOOTLOADER_FORCE)?');

    final phases = <FlashPhase>[];
    final result = await boot.flash(image, onProgress: (p) {
      if (phases.isEmpty || phases.last != p.phase) {
        phases.add(p.phase);
        // ignore: avoid_print
        print('phase ${p.phase.name} round=${p.round} '
            'total=${p.total} completed=${p.completed}');
      }
    });

    expect(result.verified, isTrue,
        reason: 'image not verified after ${result.passes} passes '
            '(${result.corrections} corrections)');
    expect(phases, contains(FlashPhase.writing));
    expect(phases, contains(FlashPhase.verifying));
    expect(phases.last, FlashPhase.done);

    // ignore: avoid_print
    print('flashed ${image.length} bytes via bootloader: '
        'passes=${result.passes} corrections=${result.corrections} '
        'writes=${result.writes} reads=${result.reads}');
  });
}
