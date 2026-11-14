/// The app's verify pass against the rig: two measures, reported separately (card t_5e16d932).
///
/// Read-only: it never writes a byte, so it can be run against a node in any state. The image it
/// compares against is named by `TAMU_DIGEST_IMAGE`; `TAMU_EXPECT_MISMATCH=1` turns the run into
/// the corrupt-image case, which must report the differing offset and both measures.
///
///   TAMU_HIL=/dev/ttyACM0 TAMU_DIGEST_IMAGE=binaries/DAS_v0_1.bin \
///     LD_LIBRARY_PATH=build/linux/x64/debug/bundle/lib \
///     flutter test --concurrency=1 test/hil_digest_verify_test.dart
///
/// Tagged `hil` so `./test.sh` (which excludes that tag) never runs it. A bare `flutter test`
/// skips: without `TAMU_DIGEST_IMAGE` there is nothing to compare against.
@Tags(['hil'])
library;

import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader.dart';
import 'package:tamuapp/core/bootloader_client.dart';

import 'hil/hil_api.dart';
import 'hil/hil_env.dart';

/// The core whose RSBus relays the raw bootloader frames onto the bus.
const HilPrereq _relayCore = HilPrereq(isCore: true);

void main() {
  final env = HilEnv();
  setUpAll(env.connectAndScan);
  tearDownAll(env.finish);

  test('the app verifies the node image with two measures, and never writes', () async {
    final imagePath = Platform.environment['TAMU_DIGEST_IMAGE'];
    if (imagePath == null) {
      markTestSkipped('set TAMU_DIGEST_IMAGE to the image the node should hold');
      return;
    }
    expect(
      env.hilRequested,
      isTrue,
      reason: 'TAMU_HIL must name the relay core port',
    );

    final image = File(imagePath);
    expect(image.existsSync(), isTrue, reason: 'no image at $imagePath');
    final expectMismatch = Platform.environment['TAMU_EXPECT_MISMATCH'] == '1';

    final cores = env.rig.satisfying(_relayCore);
    final coreId = cores.isEmpty ? 1 : cores.first.id;
    final client = BootloaderClient(coreId: coreId);

    final identity = await client.identify();
    expect(
      identity,
      isNotNull,
      reason: 'no node answered the identify; is it in bootloader mode?',
    );
    // ignore: avoid_print
    print('IDENTIFY core $coreId -> $identity');

    final bytes = Uint8List.fromList(image.readAsBytesSync());
    final watch = Stopwatch()..start();
    final result = await client.verify(bytes);
    watch.stop();

    final crc = result.crc;
    // ignore: avoid_print
    print(
      'VERIFY ${image.path}: ${bytes.length} B '
      '(padded ${bytes.length + (32 - bytes.length % 32) % 32}) in '
      '${watch.elapsedMilliseconds} ms - outcome=${result.outcome.label} '
      'CRC=0x${(crc ?? 0).toRadixString(16).padLeft(8, '0')} '
      'expected=0x${result.expectedCrc.toRadixString(16).padLeft(8, '0')} '
      'readBacks=${result.readBacks} reads=${result.reads} '
      'differ=${result.differing} clean=${result.clean}',
    );
    // ignore: avoid_print
    print('NOTE ${result.note}');
    if (result.differences != null) {
      // ignore: avoid_print
      print('DIFFERENCES ${result.differences!.summary}');
    }

    if (expectMismatch) {
      expect(result.clean, isFalse, reason: 'the node holds a word that differs');
      expect(
        result.differences?.firstDifference,
        isNotNull,
        reason: 'the read-back pass has to name the offset',
      );
      expect(result.reads, greaterThan(0), reason: 'the full read-back pass ran');
    } else {
      // Two measures, reported separately. The read path is flaky on this rig (Issues.md: a few
      // chunks out of 432 come back with a shifted payload), so a clean image may legitimately
      // end as "CRC matches, read-back differs" - the disagreement is the finding, and neither
      // side is preferred. What must never happen on a clean image is a CRC MISMATCH.
      expect(
        result.hasCrc ? result.crcOk : true,
        isTrue,
        reason:
            'the node must hold this image; its whole-space CRC is compared with the '
            'space the host models',
      );
      expect(
        result.reads,
        greaterThanOrEqualTo(
          (identity!.appSize + Bootloader.payloadSize - 1) ~/
              Bootloader.payloadSize,
        ),
        reason: 'the read-back pass covers the whole app space, CRC or not',
      );
      expect(
        result.outcome,
        anyOf(
          FlashVerifyOutcome.confirmed,
          FlashVerifyOutcome.readBackOnly,
          FlashVerifyOutcome.crcAgreesReadBackDiffers,
        ),
      );
      if (!result.clean) {
        // ignore: avoid_print
        print(
          'DISAGREEMENT named, not resolved: ${result.outcome.label} - '
          'the read path is flaky on this rig (Issues.md)',
        );
      }
    }
  }, timeout: Timeout.none);
}
