import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader.dart';
import 'package:tamuapp/core/bootloader_client.dart';

import 'support/bootloader_transport.dart';

/// A client over [t] whose waits are microseconds: the backoff shape is asserted separately, so
/// the cases here measure what the client DOES, not how long it waits.
BootloaderClient _client(FakeBootloaderTransport t) => BootloaderClient(
  transport: t,
  busyBackoff: const Duration(milliseconds: 1),
  busyBackoffCap: const Duration(milliseconds: 2),
);

/// A 1 KiB device with 64-byte pages: 16 pages, 32 chunks - the DAS geometry scaled down.
FakeBootloaderTransport _device() =>
    FakeBootloaderTransport(appSize: 1024, pageSize: 64);

void main() {
  group('a whole image into a blank device', () {
    test('identify, compare, write, blank, then both measures', () async {
      final t = _device();
      final image = testImage(1024);
      final phases = <FlashPhase>[];
      final result = await _client(t).flash(
        image,
        onProgress: (p) {
          if (phases.isEmpty || phases.last != p.phase) phases.add(p.phase);
        },
      );

      expect(result.outcome, FlashVerifyOutcome.confirmed);
      expect(result.clean, isTrue);
      expect(t.identifies, 1, reason: 'identify comes first, once');
      expect(result.pagesCompared, 16);
      expect(result.pagesKept, 0, reason: 'a blank device holds none of it');
      expect(result.pagesWritten, 16);
      expect(result.pagesBlanked, 0, reason: 'the image fills the app space');
      expect(result.chunksWritten, 32);
      expect(result.rounds, 1, reason: 'a clean read-back needs no correction');
      expect(result.verify.crc, Bootloader.crc32Space(image, 1024));
      expect(result.verify.expectedCrc, result.verify.crc);
      expect(t.holds(image), isTrue);
      expect(
        phases,
        containsAllInOrder([
          FlashPhase.comparing,
          FlashPhase.writing,
          FlashPhase.verifying,
          FlashPhase.done,
        ]),
      );
      expect(result.note, contains('CRC'));
      expect(result.note, contains('all identical'));
    });

    test('a page that already matches is never written', () async {
      final t = _device();
      final image = testImage(1024);
      await _client(t).flash(image);
      final writesAfterFirst = t.writes;
      final erasesAfterFirst = t.erases;
      final crcsAfterFirst = t.crcs;

      final again = await _client(t).flash(image);

      expect(again.clean, isTrue);
      expect(again.pagesKept, 16);
      expect(again.pagesWritten, 0);
      expect(again.chunksWritten, 0);
      expect(t.writes, writesAfterFirst, reason: 'not one write the second time');
      expect(t.erases, erasesAfterFirst);
      expect(
        t.crcs,
        crcsAfterFirst + 1,
        reason: 'verification still runs both measures',
      );
      expect(again.correctionRounds, 0);
    });
  });

  group('delta upload', () {
    test('only the pages that differ are erased and written', () async {
      final t = _device();
      final image = testImage(1024);
      await _client(t).flash(image);

      // ~200 bytes changed across two pages: 0x100 and 0x80.
      final changed = Uint8List.fromList(image);
      for (var i = 0; i < 100; i++) {
        changed[0x100 + i % 64] = changed[0x100 + i % 64] ^ 0x5A;
      }
      for (var i = 0; i < 64; i++) {
        changed[0x80 + i] = changed[0x80 + i] ^ 0xA5;
      }

      final writesBefore = t.writes;
      final erasesBefore = t.erases;
      final result = await _client(t).flash(changed);

      expect(result.clean, isTrue);
      expect(result.pagesKept, 14);
      expect(result.pagesWritten, 2, reason: '0x80 and 0x100, nothing else');
      expect(t.erases - erasesBefore, 2);
      expect(
        result.chunksWritten,
        lessThanOrEqualTo(4),
        reason: 'two pages are four 32-byte chunks',
      );
      expect(
        t.writes - writesBefore,
        lessThanOrEqualTo(4),
        reason: 'the whole point: 32 chunks became 4',
      );
      expect(t.holds(changed), isTrue);
      expect(result.chunksSkipped, greaterThanOrEqualTo(0));
    });

    test('a changed byte inside one chunk writes that page only', () async {
      final t = _device();
      final image = testImage(1024);
      await _client(t).flash(image);

      final changed = Uint8List.fromList(image);
      changed[0x20] = changed[0x20] ^ 0xFF;

      final result = await _client(t).flash(changed);
      expect(result.clean, isTrue);
      expect(result.pagesWritten, 1);
      expect(result.chunksWritten, 2, reason: 'the page is two chunks');
      expect(t.holds(changed), isTrue);
    });
  });

  group('the tail', () {
    test('the pages after a short image are blanked', () async {
      final t = _device();
      final image = testImage(512);

      final result = await _client(t).flash(image);

      expect(result.clean, isTrue);
      expect(result.pagesCompared, 8, reason: '512 B of the 1024 B space');
      expect(
        result.pagesBlanked,
        0,
        reason:
            'the device was already blank, so nothing had to be erased - a page '
            'that already holds 0xFF is as good as blanked',
      );
      expect(
        result.verify.expectedCrc,
        Bootloader.crc32Space(image, 1024),
        reason: 'the image padded to the app space with 0xFF',
      );
      expect(result.verify.crc, result.verify.expectedCrc);
      for (var i = 512; i < 1024; i++) {
        expect(t.space[i], 0xFF, reason: 'tail byte $i');
      }
    });

    test('an earlier image in the tail is blanked, not kept', () async {
      final t = _device();
      await _client(t).flash(testImage(1024));
      // Now upload a shorter image: the bytes of the longer one must go.
      final short = testImage(256);

      final result = await _client(t).flash(short);

      expect(result.clean, isTrue);
      expect(result.pagesBlanked, 12);
      for (var i = 256; i < 1024; i++) {
        expect(t.space[i], 0xFF, reason: 'tail byte $i');
      }
    });

    test('a page the node refuses to blank is part of the model', () async {
      final t = _device();
      // The DAS keeps its 512-byte storage at the top of its app space and its erase stops short
      // of it: those bytes cannot be blanked, so both measures have to expect them.
      t.protectedPages.add(960);
      t.space.setRange(960, 1024, List<int>.filled(64, 0x5A));

      final result = await _client(t).flash(testImage(512));

      expect(result.clean, isTrue);
      expect(result.pagesProtected, 1);
      expect(
        result.verify.expectedCrc,
        Bootloader.crc32(t.expectedSpace(testImage(512))),
        reason: 'the CRC covers what the device holds where it will not blank',
      );
      expect(t.space.sublist(960, 1024), List<int>.filled(64, 0x5A));
      expect(
        result.note,
        contains('1 tail page(s) not blankable'),
      );
    });

    test('a protected page the image needs is a node refusal', () async {
      final t = _device();
      t.protectedPages.add(0);
      // A page cannot be erased, so the image cannot be written there.
      await expectLater(
        _client(t).flash(testImage(1024)),
        throwsA(
          isA<FlashAborted>()
              .having((a) => a.fault, 'fault', FlashFault.nodeRefused)
              .having((a) => a.message, 'message', contains('refused')),
        ),
      );
      expect(t.writes, 0, reason: 'nothing was programmed');
    });
  });

  group('the two measures', () {
    test('a CRC that matches while a byte differs is its own outcome', () async {
      // The stored bytes and the CRC are right; the READ path is not. One correction round cannot
      // settle that, so five run and the run stops naming both measures.
      final t = _device()..readLies = true;
      await expectLater(
        _client(t).flash(testImage(1024)),
        throwsA(
          isA<FlashAborted>()
              .having((a) => a.fault, 'fault', FlashFault.verifyMismatch)
              .having(
                (a) => a.message,
                'message',
                contains('CRC matches, read-back differs'),
              )
              .having((a) => a.mismatches, 'mismatches', isNotNull),
        ),
      );
    });

    test('the read-back is the only measure when the node has no CRC', () async {
      final t = _device()..noCrc = true;
      final result = await _client(t).flash(testImage(1024));

      expect(result.outcome, FlashVerifyOutcome.readBackOnly);
      expect(result.clean, isTrue);
      expect(result.verify.hasCrc, isFalse);
      expect(result.note, contains('CRC none'));
      expect(result.note, contains('all identical'));
    });

    test('a write the device took and lost is corrected once', () async {
      final t = _device()..dropWriteOnce.add(0x140);
      final result = await _client(t).flash(testImage(1024));

      expect(result.clean, isTrue);
      expect(result.correctionRounds, 1);
      expect(result.rounds, 2);
      expect(result.chunksWritten, greaterThan(32), reason: 'the page was rewritten');
      expect(t.holds(testImage(1024)), isTrue);
    });
  });

  group('correction rounds', () {
    test('one corrupt word mid-verify is named, then fixed', () async {
      final t = _device();
      var corrupted = false;
      t.beforeReadBack = (device, round) {
        if (round != 1 || corrupted) return;
        corrupted = true;
        // A word damaged between the CRC exchange and the read-back, the way an SWD write does.
        device.space[0x140] = 0xDE;
        device.space[0x141] = 0xAD;
      };

      final result = await _client(t).flash(testImage(1024));

      expect(result.clean, isTrue);
      expect(result.correctionRounds, 1);
      expect(result.corrections, greaterThan(0));
      expect(t.holds(testImage(1024)), isTrue);
    });

    test('five corrupt rounds, and the sixth failure names the offsets', () async {
      final t = _device();
      // Everything the run programs is broken again before every read-back: a flash that will not
      // hold its data, or a bench hook that keeps damaging the same page.
      t.beforeReadBack = (device, round) {
        device.space[0x140] = 0xDE;
        device.space[0x141] = 0xAD;
      };

      await expectLater(
        _client(t).flash(testImage(1024)),
        throwsA(
          isA<FlashAborted>()
              .having((a) => a.fault, 'fault', FlashFault.verifyMismatch)
              .having(
                (a) => a.message,
                'message',
                allOf(contains('five correction rounds'), contains('0x140')),
              )
              .having((a) => a.mismatches?.pages, 'pages', [0x140]),
        ),
      );
      // Five corrections ran, i.e. six verifications, and then it stopped.
      expect(t.rounds, 6);
    });

    test('a clean read-back rewrites nothing', () async {
      final t = _device();
      await _client(t).flash(testImage(1024));
      final writes = t.writes;
      final erases = t.erases;

      await _client(t).flash(testImage(1024));

      expect(t.writes, writes);
      expect(t.erases, erases);
    });
  });

  group('a v1 bootloader is refused', () {
    test('nothing is written and the message says why', () async {
      final t = _device()..oldBootloader = true;
      await expectLater(
        _client(t).flash(testImage(1024)),
        throwsA(
          isA<FlashAborted>()
              .having((a) => a.fault, 'fault', FlashFault.oldBootloader)
              .having(
                (a) => a.message,
                'message',
                allOf(contains('pre-v2'), contains('nothing was written')),
              )
              .having((a) => a.remedy, 'remedy', contains('protocol v2')),
        ),
      );
      expect(t.writes, 0);
      expect(t.erases, 0);
      expect(t.reads, 0);
    });

    test('identify() refuses it too, so a probe says so', () async {
      final t = _device()..oldBootloader = true;
      await expectLater(
        _client(t).identify(),
        throwsA(
          isA<FlashAborted>().having(
            (a) => a.fault,
            'fault',
            FlashFault.oldBootloader,
          ),
        ),
      );
    });
  });

  group('the four passthrough reasons are kept apart', () {
    test('busy is waited out, not treated as a failure', () async {
      final t = _device()..busyLeft = 3;
      final result = await _client(t).flash(testImage(1024));

      expect(result.clean, isTrue);
      expect(t.busyLeft, 0);
    });

    test('a relay that stays busy fails as busy', () async {
      final t = _device()..busyAlways = true;
      await expectLater(
        _client(t).flash(testImage(1024)),
        throwsA(
          isA<FlashAborted>()
              .having((a) => a.fault, 'fault', FlashFault.busy)
              .having((a) => a.message, 'message', contains('outstanding')),
        ),
      );
      expect(t.writes, 0);
    });

    test('the busy backoff is bounded, so it cannot livelock', () async {
      // Five waits across six attempts: 250 + 500 + 1000 + 2000 + 2000 = 5.75 s; nothing grows
      // past the cap.
      final client = BootloaderClient();
      expect(client.busyBackoff, const Duration(milliseconds: 250));
      expect(client.busyBackoffCap, const Duration(seconds: 2));
      expect(client.busyAttempts, 6);
      var total = 0;
      var wait = client.busyBackoff;
      for (var i = 1; i < client.busyAttempts; i++) {
        total += wait.inMilliseconds;
        final next = wait.inMilliseconds * 2;
        wait = Duration(
          milliseconds: next > client.busyBackoffCap.inMilliseconds
              ? client.busyBackoffCap.inMilliseconds
              : next,
        );
      }
      expect(total, 5750, reason: 'the whole bounded busy wait for one request');
    });

    test('malformed is a client bug: it stops at once', () async {
      final t = _device()..malformed = true;
      await expectLater(
        _client(t).flash(testImage(1024)),
        throwsA(
          isA<FlashAborted>()
              .having((a) => a.fault, 'fault', FlashFault.malformed)
              .having((a) => a.remedy, 'remedy', contains('bug in the app')),
        ),
      );
      expect(t.writes, 0);
      expect(t.reads, 0);
    });

    test('node silent is not retried: it stops the run', () async {
      final t = _device();
      t.beforeReadBack = (device, round) {
        // Silence the node for the read-back, and zero the counter so the assertion below counts
        // the read-back's own attempts and not the compare pass's reads.
        device.silent = true;
        device.reads = 0;
      };
      await expectLater(
        _client(t).flash(testImage(1024)),
        throwsA(
          isA<FlashAborted>().having(
            (a) => a.fault,
            'fault',
            FlashFault.nodeSilent,
          ),
        ),
      );
      // One read attempt, not three: the core's 0x03 is a verdict, not our link.
      expect(t.reads, 1);
    });

    test('a FAIL with no reason byte is its own fault', () async {
      final t = _device()..unreported = true;
      await expectLater(
        _client(t).flash(testImage(1024)),
        throwsA(
          isA<FlashAborted>()
              .having((a) => a.fault, 'fault', FlashFault.unreportedRefusal)
              .having((a) => a.remedy, 'remedy', contains('reason')),
        ),
      );
    });

    test('a node that is not there answers identify with nothing', () async {
      final t = _device()..silent = true;
      expect(await _client(t).identify(), isNull);
      await expectLater(
        _client(t).flash(testImage(1024)),
        throwsA(
          isA<FlashAborted>().having(
            (a) => a.fault,
            'fault',
            FlashFault.noAnswer,
          ),
        ),
      );
    });
  });

  group('the run is bounded', () {
    test('an expired deadline stops it before the first chunk', () async {
      final t = _device();
      await expectLater(
        _client(t).flash(
          testImage(1024),
          deadline: const Duration(milliseconds: -1),
        ),
        throwsA(
          isA<FlashAborted>()
              .having((a) => a.fault, 'fault', FlashFault.deadline)
              .having((a) => a.message, 'message', contains('deadline')),
        ),
      );
      expect(t.writes, 0);
    });

    test('the user can cancel between pages', () async {
      final t = _device();
      await expectLater(
        _client(t).flash(
          testImage(1024),
          isCancelled: () => t.writes >= 2,
        ),
        throwsA(
          isA<FlashAborted>().having(
            (a) => a.fault,
            'fault',
            FlashFault.cancelled,
          ),
        ),
      );
      expect(t.writes, lessThan(32), reason: 'it stopped early');
    });

    test('the deadline covers five rounds plus corrections', () {
      // Six verifications of the DAS space (448 chunk reads each, 5.9 s measured) plus the write
      // pass and five correction rounds. 180 s is the firmware probe's floor; this is well past it.
      expect(
        BootloaderClient.flashDeadline.inSeconds,
        greaterThanOrEqualTo(180),
      );
      expect(BootloaderClient.flashDeadline.inMinutes, 10);
    });

    test('an image larger than the app space is refused before anything', () async {
      final t = _device();
      await expectLater(
        _client(t).flash(testImage(2048), maxImageSize: 1024),
        throwsA(isA<ArgumentError>()),
      );
      expect(t.identifies, 0);
      expect(t.writes, 0);
    });

    test('an image needing more than the last page is refused', () async {
      final t = _device();
      await expectLater(
        _client(t).flash(testImage(1025)),
        throwsA(isA<ArgumentError>()),
      );
      expect(t.writes, 0);
    });

    test('a caller CRC that is not the space\'s is refused', () async {
      final t = _device();
      await expectLater(
        _client(t).flash(testImage(512), expectedCrc: 0x12345678),
        throwsA(isA<ArgumentError>()),
      );
    });

    test('the caller CRC is accepted when it is the space\'s', () async {
      final t = _device();
      final result = await _client(t).flash(
        testImage(512),
        expectedCrc: Bootloader.crc32Space(testImage(512), 1024),
      );
      expect(result.clean, isTrue);
    });
  });

  group('verify() on its own writes nothing', () {
    test('reports both measures and leaves the device alone', () async {
      final t = _device();
      final client = _client(t);
      await client.flash(testImage(512));
      final writes = t.writes;
      final erases = t.erases;

      final clean = await client.verify(testImage(512));
      expect(clean.clean, isTrue);
      expect(clean.outcome, FlashVerifyOutcome.confirmed);

      final other = await client.verify(testImage(1024));
      expect(other.clean, isFalse);
      expect(other.differing, greaterThan(0));
      expect(t.writes, writes, reason: 'a verify is read-only');
      expect(t.erases, erases);
    });
  });

  test('identify exposes the app size, the page size and the name', () async {
    final t = FakeBootloaderTransport(appSize: 14336, pageSize: 64);
    final id = await _client(t).identify();

    expect(id, isNotNull);
    expect(id!.appSize, 14336);
    expect(id.pageSize, 64);
    expect(id.name, 'DAS v0.1');
    expect(id.pages, 224);
    expect(id.toString(), contains('14336'));
  });

  test('identify returns null when the device does not answer', () async {
    final t = _device()..silent = true;
    expect(await _client(t).identify(), isNull);
  });

  // The passthrough CIDs are wire values shared with the firmware's Device.h; a remap on either
  // side would silently break every passthrough operation. Docs/Services/Bootloader.md "Core
  // Bootloader Passthrough Commands (002x)": they follow the bootloader command order.
  group('passthrough command IDs follow the command order', () {
    test('0x20 identify, 0x21 read, 0x22 erase, 0x23 write, 0x24 CRC', () {
      expect(coreBootIdentify, 0x20);
      expect(coreBootRead, 0x21);
      expect(coreBootErase, 0x22);
      expect(coreBootWrite, 0x23);
      expect(coreBootCrc, 0x24);
    });

    test('each CID is its frame\'s operation, in order', () {
      final ids = [
        coreBootIdentify,
        coreBootRead,
        coreBootErase,
        coreBootWrite,
        coreBootCrc,
      ];
      expect(ids, List<int>.generate(5, (i) => 0x20 + i));
      // The bootloader operation bits 1-3 of each pair's request: 0 identify, 1 read, 2 erase,
      // 3 write, 4 CRC.
      for (var rank = 0; rank < 5; rank++) {
        expect((ids[rank] - 0x20), rank);
        expect(Bootloader.cmdIdentifyRequest + (rank << 1), rank << 1);
      }
    });
  });
}
