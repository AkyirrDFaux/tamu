/// Programmatic RSBus bootloader upload (not part of the routine HIL suite).
///
/// Pushes a host-built node application image into a device sitting in bootloader mode,
/// over the relay core's passthrough - Docs/Services/Bootloader.md, "Core Bootloader
/// Passthrough Commands (002x)". The passthrough reaches whichever node is in bootloader
/// mode without addressing it, so no device id is needed.
///
///   TAMU_HIL=/dev/ttyACM1 TAMU_DAS_IMAGE=firmware/.pio/build/DAS_v0_1/firmware.bin \
///     flutter test --concurrency=1 test/hil_bootloader_upload_test.dart
///
/// Driven by `./upload.sh rsbus das [core-port]`, which builds the image and sets both
/// variables. Destructive: it replaces the target node's application image, and the node
/// must already be in bootloader mode (the DAS runs a `-D BOOTLOADER_FORCE` build).
///
/// Tagged `hil` so `./test.sh` (which excludes that tag) never runs it. Once the tool has
/// opted in via TAMU_DAS_IMAGE this file FAILS on a missing rig instead of skipping - a
/// silent skip would be reported as a successful upload. A bare `flutter test` skips.
@Tags(['hil'])
library;

import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader_client.dart';

import 'hil/hil_api.dart';
import 'hil/hil_env.dart';

/// The core whose RSBus relays the raw bootloader frames onto the bus.
const HilPrereq _relayCore = HilPrereq(isCore: true);

void main() {
  final env = HilEnv();
  setUpAll(env.connectAndScan);
  tearDownAll(env.finish);

  test('upload a node application image over the RSBus passthrough', () async {
    // Driven by `./upload.sh rsbus das`. A bare `flutter test` must not fail for want of a
    // rig, so it skips unless the tool opted in; the `hil` tag keeps it out of ./test.sh.
    // The flash itself paces 429 chunks and then verifies by read-back, so the per-test
    // timeout is disabled below (Timeout.none) rather than guessed at.
    final imagePath = Platform.environment['TAMU_DAS_IMAGE'];
    if (imagePath == null) {
      markTestSkipped('not an upload run (use ./upload.sh rsbus das)');
      return;
    }

    expect(
      env.hilRequested,
      isTrue,
      reason: "TAMU_HIL must name the relay core's port (upload.sh sets it)",
    );
    expect(env.rig.isEmpty, isFalse, reason: 'the rig reported no devices');

    final image = File(imagePath);
    expect(image.existsSync(), isTrue, reason: 'no image at $imagePath');

    final cores = env.rig.satisfying(_relayCore);
    final coreId = cores.isEmpty ? 1 : cores.first.id;

    final boot = BootloaderClient(coreId: coreId);
    final identity = await boot.identify();
    // ignore: avoid_print
    print('IDENTIFY core $coreId -> ${identity ?? 'no response'}');
    expect(
      identity,
      isNotNull,
      reason: 'no node answered the identify; is it in bootloader mode?',
    );
    final appSize = identity!.appSize;

    final bytes = Uint8List.fromList(image.readAsBytesSync());
    // ignore: avoid_print
    print('UPLOAD ${image.path}: ${bytes.length} B into a $appSize B app space');

    final result = await boot.flash(
      bytes,
      maxImageSize: appSize,
      onProgress: (p) {
        // ignore: avoid_print
        print('  ${p.phase.name} ${p.completed}/${p.total}');
      },
    );

    // ignore: avoid_print
    print(
      'RESULT verified=${result.verified} passes=${result.passes} '
      'corrections=${result.corrections} writes=${result.writes} '
      'reads=${result.reads}',
    );
    expect(
      result.verified,
      isTrue,
      reason: 'read-back verification did not converge',
    );
  }, timeout: Timeout.none);
}
