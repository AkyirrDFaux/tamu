import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader.dart';
import 'package:tamuapp/core/bootloader_client.dart';

import 'support/bootloader_transport.dart';

BootloaderClient _client(FakeBootloaderTransport t) =>
    BootloaderClient(transport: t, writePacing: Duration.zero);

void main() {
  group('two measures, reported separately', () {
    test('a clean write: the digest and the read-back both agree', () async {
      final t = FakeBootloaderTransport();
      final result = await _client(t).flash(testImage(100));

      expect(result.outcome, FlashVerifyOutcome.confirmed);
      expect(result.clean, isTrue);
      // 100 bytes -> 4 chunks (the tail padded).
      expect(result.writes, 4);
      expect(t.digests, 1, reason: 'one digest request');
      expect(
        result.reads,
        4,
        reason: 'the full read-back pass still runs: it is the second measure',
      );
      expect(result.verify.digest, Bootloader.crc32Padded(testImage(100)));
      expect(result.verify.digestOk, isTrue);
      expect(result.note, allOf(contains('match'), contains('all identical')));
    });

    test('the expected digest comes from the caller when it has one', () async {
      // The package manifest's crc32_padded is what the app compares against. A value that does
      // not match the device's image is the digest's verdict on its own: the read-back still ran
      // and found every byte identical, which is a named disagreement, not one side winning.
      final t = FakeBootloaderTransport();
      final image = testImage(100);

      await expectLater(
        _client(t).flash(image, expectedDigest: 0x1234ABCD),
        throwsA(
          isA<FlashAborted>()
              .having((a) => a.fault, 'fault', FlashFault.verifyMismatch)
              .having(
                (a) => a.message,
                'message',
                allOf(
                  contains('0x1234abcd'),
                  contains('digest differs, read-back matches'),
                ),
              ),
        ),
      );
      expect(t.writes, 4, reason: 'nothing differed in the read-back: no rewrite');
      expect(t.reads, 4);
    });

    test('a digest that passes while a byte differs is its own outcome', () async {
      // The node's own CRC and its own bytes contradict each other. The read-back wins nothing:
      // the outcome is named, the acknowledged chunk is rewritten once, and the re-read decides.
      final t = FakeBootloaderTransport()..readBackLies = true;
      final image = testImage(100);

      await expectLater(
        _client(t).flash(image),
        throwsA(
          isA<FlashAborted>()
              .having((a) => a.fault, 'fault', FlashFault.verifyMismatch)
              .having(
                (a) => a.message,
                'message',
                contains('digest matches, read-back differs'),
              )
              .having((a) => a.mismatches?.count, 'count', greaterThan(0)),
        ),
      );
      expect(
        t.writes,
        greaterThan(4),
        reason: 'one correction round for the acknowledged chunks that differ',
      );
    });
  });

  test(
    'the write response lets a rejected chunk be retried immediately',
    () async {
      // A rejected write is reported with a stale last-written offset, so the client retries it
      // inside the write loop instead of waiting for the verification.
      final t = FakeBootloaderTransport()..rejectOnce.add(64);
      final result = await _client(t).flash(testImage(100));

      expect(result.outcome, FlashVerifyOutcome.confirmed);
      // 4 chunks issued, plus one retry of the rejected chunk.
      expect(t.writes, 5);
      expect(t.flash[64], isNotNull);
    },
  );

  group('a disagreement costs one correction round, acknowledged chunks only', () {
    test('a lost byte on an acknowledged chunk is rewritten once', () async {
      // The device took the data and lost it. Both measures say so; one correction round rewrites
      // the chunk (the write pass was acknowledged for it), and the one re-read clears it.
      final t = FakeBootloaderTransport()
        ..corruptAll = true
        ..corruptFirstWriteOnly = true;
      final result = await _client(t).flash(testImage(100));

      expect(result.corrections, 4);
      expect(result.writes, 8, reason: 'one write pass plus one correction round');
      expect(result.reads, 8, reason: 'the read-back pass plus the re-read');
      expect(result.note, contains('correction round rewrote 4 chunk(s)'));
    });

    test('a chunk the node never acknowledged is not rewritten', () async {
      // The passthrough ACKs without a last-written offset, so a dropped write is unconfirmed:
      // rewriting it into a node that is not taking data is what the card forbids.
      final t = FakeBootloaderTransport()
        ..nullOffsetAcks = true
        ..dropOnce.addAll({0, 64});

      await expectLater(
        _client(t).flash(testImage(100)),
        throwsA(
          isA<FlashAborted>()
              .having((a) => a.fault, 'fault', FlashFault.verifyMismatch)
              .having((a) => a.mismatches?.count, 'count', 2)
              .having((a) => a.mismatches?.unconfirmed, 'unconfirmed', 2),
        ),
      );
      expect(t.writes, 4, reason: 'one write pass, and no rewrite of unacknowledged chunks');
      expect(t.reads, 4, reason: 'one read-back pass, then stop');
    });

    test('a corrected chunk that still reads back wrong ends the run', () async {
      // The correction is acknowledged and the bytes still do not hold: one round, then a stop.
      final t = FakeBootloaderTransport()..corruptAll = true;

      await expectLater(
        _client(t).flash(testImage(100)),
        throwsA(
          isA<FlashAborted>()
              .having((a) => a.fault, 'fault', FlashFault.verifyMismatch)
              .having(
                (a) => a.message,
                'message',
                contains('corrected chunk still does not read back'),
              ),
        ),
      );
      expect(t.writes, 8, reason: 'exactly one correction round, never a second');
    });
  });

  group('a flash that cannot finish stops in bounded time', () {
    // A node that leaves bootloader mode after identify() succeeded: every write (or every read)
    // goes nowhere. The pass used to walk all 13,500 chunks and then repeat it up to 20 times.
    const chunks = 100; // 3,200 bytes: the same shapes, a test-sized walk

    test('the first write the node never takes ends the write pass', () async {
      final t = FakeBootloaderTransport()..alwaysReject = true;

      await expectLater(
        _client(t).flash(testImage(chunks * Bootloader.payloadSize)),
        throwsA(
          isA<FlashAborted>()
              .having((a) => a.fault, 'fault', FlashFault.writeRefused)
              .having((a) => a.chunk, 'chunk', 1)
              .having((a) => a.total, 'total', chunks)
              .having((a) => a.message, 'message', contains('never confirmed'))
              .having((a) => a.remedy, 'remedy', contains('Power-cycle')),
        ),
      );
      expect(
        t.writes,
        3,
        reason: 'three attempts on ONE chunk, not one attempt on every chunk',
      );
      expect(t.reads, 0, reason: 'there is nothing to read back');
      expect(t.digests, 0, reason: 'nothing was verified');
    });

    test('a node that stops answering ends the read-back pass', () async {
      final t = FakeBootloaderTransport()..alwaysNullRead = true;

      await expectLater(
        _client(t).flash(testImage(chunks * Bootloader.payloadSize)),
        throwsA(
          isA<FlashAborted>()
              .having((a) => a.fault, 'fault', FlashFault.nodeSilent)
              .having((a) => a.chunk, 'chunk', 1)
              .having(
                (a) => a.message,
                'message',
                contains('stopped answering'),
              )
              .having((a) => a.remedy, 'remedy', contains('Power-cycle')),
        ),
      );
      expect(
        t.reads,
        5,
        reason:
            "the first chunk's five read attempts, then stop - not 100 chunks "
            'x 20 passes',
      );
      expect(t.writes, chunks, reason: 'the write pass had already run');
    });

    test('the deadline stops a run before its first chunk', () async {
      final t = FakeBootloaderTransport();

      await expectLater(
        _client(t).flash(
          testImage(chunks * Bootloader.payloadSize),
          // An already-expired deadline is what a test can do to prove the check runs at all;
          // the real one is BootloaderClient.flashDeadline.
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

    test('the user can cancel between chunks', () async {
      final t = FakeBootloaderTransport();

      await expectLater(
        _client(t).flash(
          testImage(chunks * Bootloader.payloadSize),
          // Two chunks land, then the flag goes up.
          isCancelled: () => t.writes >= 2,
        ),
        throwsA(
          isA<FlashAborted>()
              .having((a) => a.fault, 'fault', FlashFault.cancelled)
              .having((a) => a.message, 'message', contains('cancelled'))
              .having((a) => a.chunk, 'chunk', 3),
        ),
      );
      expect(
        t.writes,
        2,
        reason: 'the chunk in flight was not abandoned mid-write',
      );
    });

    test('the real deadline leaves room for a full image', () {
      // 13,500 chunks at 50 ms of write pacing each is the worst legitimate run.
      const realChunks = 13500;
      final paced = BootloaderClient().writePacing * realChunks;
      expect(
        BootloaderClient.flashDeadline,
        greaterThan(paced),
        reason: 'paced writes alone are ${paced.inMinutes} min',
      );
    });
  });

  group('a bootloader without the digest command', () {
    test('the read-back pass is the only measure, and the result says so', () async {
      final t = FakeBootloaderTransport()..noDigest = true;
      final result = await _client(t).flash(testImage(100));

      expect(result.outcome, FlashVerifyOutcome.readBackOnly);
      expect(result.clean, isTrue);
      expect(result.verify.hasDigest, isFalse);
      expect(t.digests, 2, reason: 'the digest is retried once, then given up on');
      expect(result.reads, 4, reason: 'one read-back pass over the image');
      expect(
        result.note,
        allOf(contains('digest none'), contains('read-back')),
      );
    });

    test('a difference found by that pass is named with its offset', () async {
      // The passthrough ACKs without a last-written offset, so the dropped chunk is unconfirmed -
      // and the correction round may only touch acknowledged chunks, so it is reported instead.
      final t = FakeBootloaderTransport()
        ..noDigest = true
        ..nullOffsetAcks = true
        ..dropOnce.add(32);

      await expectLater(
        _client(t).flash(testImage(100)),
        throwsA(
          isA<FlashAborted>()
              .having((a) => a.fault, 'fault', FlashFault.verifyMismatch)
              .having((a) => a.mismatches?.summary, 'summary', contains('0x20'))
              .having((a) => a.chunk, 'chunk', 2),
        ),
      );
      expect(t.reads, 4, reason: 'the pass reads the whole image once');
    });
  });

  group('verify() on its own writes nothing', () {
    test('reports both measures and leaves the device alone', () async {
      final t = FakeBootloaderTransport();
      final client = _client(t);
      final held = Uint8List.fromList(List.generate(100, (i) => (i * 3) & 0xFF));
      await client.flash(held);
      final writesBefore = t.writes;

      final result = await client.verify(testImage(100));

      expect(result.clean, isFalse);
      expect(result.outcome, FlashVerifyOutcome.bothDiffer);
      expect(result.differing, 4);
      expect(t.writes, writesBefore, reason: 'a verify is read-only');
      expect(result.reads, 4);
    });

    test('a clean verify of the image the device holds', () async {
      final t = FakeBootloaderTransport();
      final client = _client(t);
      final image = testImage(100);
      await client.flash(image);

      final result = await client.verify(image);

      expect(result.clean, isTrue);
      expect(result.outcome, FlashVerifyOutcome.confirmed);
      expect(result.differing, 0);
    });

    test('an older bootloader falls back to the read-back pass alone', () async {
      final t = FakeBootloaderTransport()..noDigest = true;
      final result = await _client(t).verify(testImage(100));

      expect(result.outcome, FlashVerifyOutcome.readBackOnly);
      expect(result.readBacks, 1);
      expect(result.reads, 4);
      expect(t.writes, 0);
    });
  });

  test('identify exposes the app size and name', () async {
    final t = FakeBootloaderTransport()
      ..identity = const BootloaderIdentity(
        appSize: 0x2A0000,
        name: 'Tamu v2.0A',
      );
    final id = await _client(t).identify();

    expect(id, isNotNull);
    expect(id!.appSize, 0x2A0000);
    expect(id.name, 'Tamu v2.0A');
  });

  test('identify returns null when the device does not answer', () async {
    final t = FakeBootloaderTransport();
    expect(await _client(t).identify(), isNull);
  });

  test(
    'an image larger than the device app space is refused before writing',
    () async {
      final t = FakeBootloaderTransport();
      await expectLater(
        _client(t).flash(testImage(100), maxImageSize: 64),
        throwsA(isA<ArgumentError>()),
      );
      expect(t.writes, 0); // nothing was written

      // The same image fits when the app space is large enough.
      final ok = await _client(t).flash(testImage(100), maxImageSize: 100);
      expect(ok.outcome, FlashVerifyOutcome.confirmed);
    },
  );

  // The passthrough CIDs are wire values shared with the firmware's Device.h; a remap on either
  // side would silently break every passthrough operation (the codec KATs pin the frame bytes,
  // but nothing pinned these). Docs/Services/Bootloader.md "Core Bootloader Passthrough
  // Commands (002x)".
  group('passthrough command IDs match the firmware', () {
    test('0x20 identification, 0x21 write, 0x22 read, 0x23 digest', () {
      expect(coreBootIdentify, 0x20);
      expect(coreBootWrite, 0x21);
      expect(coreBootRead, 0x22);
      expect(coreBootDigest, 0x23);
    });

    test('the four IDs are distinct and inside the 002x range', () {
      final ids = {coreBootIdentify, coreBootWrite, coreBootRead, coreBootDigest};
      expect(ids.length, 4);
      for (final id in ids) {
        expect(id, inInclusiveRange(0x20, 0x23));
      }
    });
  });
}
