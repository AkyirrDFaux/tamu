/// Bootloader tests: exclusive, one device in bootloader mode (Docs/HIL Tests.md).
///
/// The operator declares the devices sitting in bootloader mode with
/// `TAMU_HIL_BOOTLOADER=<id>[,<id>]` (a test cannot hold the boot button, so the
/// DAS runs a `-D BOOTLOADER_FORCE` build); with no device declared every test
/// here skips. A bootloader test is `exclusive`: it takes its device off the bus
/// and runs alone (Docs/HIL Tests.md, "Independence and Ordering").
///
/// The core passthrough reaches whichever node is in bootloader mode without
/// addressing it (Docs/Services/Bootloader.md), so these tests drive the declared
/// device through the relay core. The legacy `hil_bootloader_test.dart` flashes a
/// host-built `firmware.bin`, which need not match the image already on the
/// device; to stay non-destructive the flash test here reads the device's own app
/// image first and writes that same image back, so the device is never left
/// running a different binary. See the run report for what is deferred.
library;

import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader.dart';
import 'package:tamuapp/core/bootloader_client.dart';

import 'hil_api.dart';

/// A device the operator declared to be in bootloader mode.
const HilPrereq _inBootloader = HilPrereq(bootloader: true);

/// The core that relays the raw bootloader frames onto its RSBus.
const HilPrereq _relayCore = HilPrereq(isCore: true);

/// Above this app-space size a whole-image read/write is skipped: it cannot fit
/// the runner's fixed two-minute per-test timeout. A node's app space is far
/// smaller (the DAS is 13824 B); a core's is ~2.75 MB and is not scanned while
/// it sits in bootloader, so this only ever guards an unexpected rig.
const int _wholeImageLimit = 256 * 1024;

/// The relay core's id; the identify/read/write passthrough is addressed to it.
int _relayCoreId(HilContext ctx) {
  final cores = ctx.rig.satisfying(_relayCore);
  return cores.isEmpty ? 1 : cores.first.id;
}

/// The number of 32-byte chunks in an [appSize]-byte app space.
int _chunkCount(int appSize) =>
    (appSize + Bootloader.payloadSize - 1) ~/ Bootloader.payloadSize;

/// Reads [appSize] bytes of the main binary in 32-byte chunks. Returns null when
/// any chunk did not read back (a timeout), so a partial image is never written.
Future<Uint8List?> _readImage(BootloaderClient boot, int appSize) async {
  final out = BytesBuilder();
  for (var i = 0; i < _chunkCount(appSize); i++) {
    final data = await boot.readChunk(i * Bootloader.payloadSize);
    if (data == null) return null;
    out.add(data);
  }
  return out.toBytes();
}

final List<HilTest> bootloaderTests = <HilTest>[
  HilTest(
    'bootloader identifies its device',
    category: HilCategory.bootloader,
    runClass: HilRunClass.exclusive,
    prereq: _inBootloader,
    body: (ctx) async {
      final boot = BootloaderClient(coreId: _relayCoreId(ctx));
      for (final _ in ctx.devices(_inBootloader)) {
        final id = await boot.identify();
        expect(id, isNotNull,
            reason: 'the device did not answer identify - '
                'is it in bootloader mode (BOOTLOADER_FORCE)?');
        if (id == null) return;
        ctx.note('the device: identified ${id.name} '
            '(${id.appSize} byte app space)');
        expect(id.name, isNotEmpty, reason: 'the device reports a name');
        expect(id.appSize, greaterThan(0),
            reason: 'the device reports a non-zero app space');
      }
    },
  ),
  HilTest(
    'bootloader reads its main binary',
    category: HilCategory.bootloader,
    runClass: HilRunClass.exclusive,
    prereq: _inBootloader,
    body: (ctx) async {
      final boot = BootloaderClient(coreId: _relayCoreId(ctx));
      for (final _ in ctx.devices(_inBootloader)) {
        final id = await boot.identify();
        expect(id, isNotNull, reason: 'the device did not answer identify');
        if (id == null) return;
        if (id.appSize > _wholeImageLimit) {
          markTestSkipped(
              'the device app space ${id.appSize} B exceeds the whole-image limit');
          return;
        }
        final image = await _readImage(boot, id.appSize);
        expect(image, isNotNull,
            reason: 'the device app image did not read back completely');
        if (image == null) return;
        expect(image.length, _chunkCount(id.appSize) * Bootloader.payloadSize,
            reason: 'the device app image has the expected length');
        // A programmed image: erased flash reads back all 0xFF.
        expect(image.any((b) => b != 0xFF), isTrue,
            reason: 'the device app space holds a programmed image');
        ctx.note('the device: read ${image.length} bytes of a '
            '${id.appSize} byte app space');
      }
    },
  ),
  HilTest(
    'bootloader rewrites and verifies its app image',
    category: HilCategory.bootloader,
    runClass: HilRunClass.exclusive,
    prereq: _inBootloader,
    body: (ctx) async {
      final boot = BootloaderClient(coreId: _relayCoreId(ctx));
      for (final _ in ctx.devices(_inBootloader)) {
        final id = await boot.identify();
        expect(id, isNotNull, reason: 'the device did not answer identify');
        if (id == null) return;
        if (id.appSize > _wholeImageLimit) {
          markTestSkipped(
              'the device app space ${id.appSize} B exceeds the whole-image limit');
          return;
        }
        // Read the device's own image first: the round trip only ever writes
        // back the bytes the device already holds, so it cannot swap in a
        // different binary. A partial read aborts before any write.
        final image = await _readImage(boot, id.appSize);
        if (image == null) {
          markTestSkipped(
              'the device app image did not read back; refusing to write');
          return;
        }
        final phases = <FlashPhase>[];
        final result = await boot.flash(image,
            maxImageSize: id.appSize,
            onProgress: (p) {
              if (phases.isEmpty || phases.last != p.phase) {
                phases.add(p.phase);
              }
            });
        expect(result.verified, isTrue,
            reason: 'the device image not verified after ${result.passes} '
                'passes (${result.corrections} corrections)');
        expect(phases, contains(FlashPhase.writing));
        expect(phases, contains(FlashPhase.verifying));
        expect(phases.last, FlashPhase.done);
        ctx.note('the device: rewrote ${image.length} bytes, '
            'passes=${result.passes} corrections=${result.corrections} '
            'writes=${result.writes} reads=${result.reads}');
      }
    },
  ),
];
