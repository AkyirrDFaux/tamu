/// The app's verify pass against the rig: ONE digest request instead of 432 read-backs
/// (card t_5e16d932).
///
/// Read-only: it never writes a byte, so it can be run against a node in any state. The image it
/// compares against is named by `TAMU_DIGEST_IMAGE`; `TAMU_EXPECT_MISMATCH=1` turns the run into
/// the corrupt-image case, which must report the digest pair and the first differing offset.
///
///   TAMU_HIL=/dev/ttyACM0 TAMU_DIGEST_IMAGE=binaries/DAS_v0_1.bin \
///     flutter test --concurrency=1 test/hil_digest_verify_test.dart
///
/// Tagged `hil` so `./test.sh` (which excludes that tag) never runs it. A bare `flutter test`
/// skips: without `TAMU_DIGEST_IMAGE` there is nothing to compare against.
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

  test('the app verifies the node image with one digest, and never writes', () async {
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

    // ignore: avoid_print
    print(
      'VERIFY ${image.path}: ${bytes.length} B (padded ${bytes.length + (32 - bytes.length % 32) % 32}) '
      'in ${watch.elapsedMilliseconds} ms - method=${result.method.label} '
      'digest=0x${(result.digest ?? 0).toRadixString(16).padLeft(8, '0')} '
      'expected=0x${result.expectedDigest.toRadixString(16).padLeft(8, '0')} '
      'digests=${result.digests} readBacks=${result.readBacks} reads=${result.reads} '
      'clean=${result.clean}',
    );
    // ignore: avoid_print
    print('NOTE ${result.note}');
    if (result.mismatches != null) {
      // ignore: avoid_print
      print('MISMATCH ${result.mismatches!.summary}');
    }

    if (expectMismatch) {
      expect(result.clean, isFalse, reason: 'the node holds a word that differs');
      expect(
        result.mismatches?.firstDifference,
        isNotNull,
        reason: 'the read-back pass has to name the offset the digest cannot',
      );
      expect(result.reads, greaterThan(0), reason: 'one locating read-back pass');
    } else {
      expect(result.clean, isTrue, reason: 'the node must hold exactly this image');
      if (result.method == FlashVerify.digest) {
        expect(
          result.reads,
          0,
          reason: 'the digest answered: not one read-back of the image',
        );
      } else {
        // An older bootloader answers no digest: one read-back pass, and the result says why.
        expect(result.method, FlashVerify.readBackNoDigest);
        expect(
          result.reads,
          (bytes.length + 31) ~/ 32,
          reason: 'the fallback reads the image back once, chunk by chunk',
        );
        expect(
          result.note,
          allOf(contains('no digest'), contains('read-back')),
        );
      }
    }
  }, timeout: Timeout.none);
}
