import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader.dart';
import 'package:tamuapp/core/bootloader_client.dart';

import 'support/bootloader_transport.dart';

BootloaderClient _client(FakeBootloaderTransport t) =>
    BootloaderClient(transport: t, writePacing: Duration.zero);

void main() {
  test('a clean write is confirmed by ONE digest request', () async {
    final t = FakeBootloaderTransport();
    final result = await _client(t).flash(testImage(100));

    expect(result.method, FlashVerify.digest);
    // 100 bytes -> 4 chunks (the tail padded).
    expect(result.writes, 4);
    expect(t.digests, 1, reason: 'one digest request, not one read per chunk');
    expect(result.reads, 0, reason: 'the digest replaces every read-back');
    expect(result.digest, Bootloader.crc32Padded(testImage(100)));
    expect(result.digest, result.expectedDigest);
    expect(result.note, contains('one digest'));
  });

  test('the expected digest comes from the caller when it has one', () async {
    // The package manifest's crc32_padded is what the app compares against; a value that does not
    // match the device's image is a mismatch even when the bytes were written correctly.
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
              contains('0x1234abcd'),
            ),
      ),
    );
  });

  test(
    'the write response lets a rejected chunk be retried immediately',
    () async {
      // A rejected write is reported with a stale last-written offset, so the client retries it
      // inside the write loop instead of waiting for the verification.
      final t = FakeBootloaderTransport()..rejectOnce.add(64);
      final result = await _client(t).flash(testImage(100));

      expect(result.method, FlashVerify.digest);
      // 4 chunks issued, plus one retry of the rejected chunk.
      expect(t.writes, 5);
      expect(t.flash[64], isNotNull);
    },
  );

  test('a dropped write is caught by the digest and located by one read', () async {
    // The passthrough ACKs without a last-written offset, so a dropped write only shows up in the
    // digest. The locating read-back stops at the first difference: nothing is rewritten.
    final t = FakeBootloaderTransport()
      ..nullOffsetAcks = true
      ..dropOnce.addAll({0, 64});

    await expectLater(
      _client(t).flash(testImage(100)),
      throwsA(
        isA<FlashAborted>()
            .having((a) => a.fault, 'fault', FlashFault.verifyMismatch)
            .having(
              (a) => a.message,
              'message',
              allOf(contains('!='), contains('first differing chunk: 0x0')),
            )
            .having((a) => a.mismatches?.count, 'count', 1)
            .having((a) => a.mismatches?.unconfirmed, 'unconfirmed', 1)
            .having((a) => a.chunk, 'chunk', 1),
      ),
    );
    expect(t.writes, 4, reason: 'one write pass, and no correction round');
    expect(t.reads, 1, reason: 'the pass stops at the first differing chunk');
  });

  group('a verification never writes and never repeats', () {
    // The device takes every write and loses the data: the digest disagrees with the image, one
    // read-back names the first differing chunk, and the run stops.
    test('a lost chunk stops the run after one locating read', () async {
      final t = FakeBootloaderTransport()..corruptAll = true;

      await expectLater(
        _client(t).flash(testImage(100)),
        throwsA(
          isA<FlashAborted>()
              .having((a) => a.fault, 'fault', FlashFault.verifyMismatch)
              .having((a) => a.fault.label, 'label', 'image does not read back')
              .having(
                (a) => a.mismatches?.summary,
                'summary',
                'first differing chunk: 0x0 (byte 0: wrote 0x00, read 0xff)',
              )
              .having((a) => a.remedy, 'remedy', contains('Power-cycle')),
        ),
      );
      expect(
        t.writes,
        4,
        reason:
            'the image written exactly once: a chunk the device confirmed is not '
            'rewritten while it is failing to hold the data',
      );
      expect(t.reads, 1, reason: 'one read, at the first difference, then stop');
    });

    test('the digest pair is named in the message', () async {
      final t = FakeBootloaderTransport()..corruptAll = true;
      final image = testImage(100);

      await expectLater(
        _client(t).flash(image),
        throwsA(
          isA<FlashAborted>().having(
            (a) => a.message,
            'message',
            allOf(
              contains(
                Bootloader.crc32Padded(image).toRadixString(16).padLeft(8, '0'),
              ),
              contains('first differing chunk'),
            ),
          ),
        ),
      );
    });

    test('a verify on its own writes nothing', () async {
      // The device holds another image: verify() reports the first difference and programs no
      // byte - that is the read-only pass the Update page and the rig run.
      final t = FakeBootloaderTransport();
      final client = _client(t);
      final held = Uint8List.fromList(List.generate(100, (i) => (i * 3) & 0xFF));
      await client.flash(held);
      final writesBefore = t.writes;

      final result = await client.verify(testImage(100));

      expect(result.clean, isFalse);
      expect(result.method, FlashVerify.digest);
      expect(result.mismatches?.firstDifference?.offset, 0);
      // byte 0 of both images is 0, so the first differing byte is index 1
      expect(result.mismatches?.firstDifference?.at, 1);
      expect(t.writes, writesBefore, reason: 'a verify is read-only by construction');
      expect(result.digest, isNot(Bootloader.crc32Padded(testImage(100))));
    });

    test('a verify of the image the device holds is clean', () async {
      final t = FakeBootloaderTransport();
      final client = _client(t);
      final image = testImage(100);
      await client.flash(image);

      final readsBefore = t.reads;
      final result = await client.verify(image);

      expect(result.clean, isTrue);
      expect(result.reads, 0);
      expect(t.reads, readsBefore, reason: 'the digest answered: no read-back ran');
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
    test('falls back to ONE read-back pass and says why', () async {
      final t = FakeBootloaderTransport()..noDigest = true;
      final result = await _client(t).flash(testImage(100));

      expect(result.method, FlashVerify.readBackNoDigest);
      expect(result.digest, isNull);
      expect(t.digests, 2, reason: 'the digest is retried once, then given up on');
      expect(result.reads, 4, reason: 'one read-back pass over the image');
      expect(
        result.note,
        allOf(contains('no digest'), contains('read-back')),
      );
    });

    test('a difference found by the fallback pass is named with its offset', () async {
      // The passthrough ACKs without a last-written offset, so the dropped chunk is silent until
      // the read-back finds it.
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
      expect(t.reads, 4, reason: 'the fallback reads the whole image once');
    });

    test('a verify on its own also falls back, without writing', () async {
      final t = FakeBootloaderTransport()..noDigest = true;
      final result = await _client(t).verify(testImage(100));

      expect(result.method, FlashVerify.readBackNoDigest);
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
      expect(ok.method, FlashVerify.digest);
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
