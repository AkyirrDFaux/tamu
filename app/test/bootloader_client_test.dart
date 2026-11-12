import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader.dart';
import 'package:tamuapp/core/bootloader_client.dart';

import 'support/bootloader_transport.dart';

BootloaderClient _client(FakeBootloaderTransport t) =>
    BootloaderClient(transport: t, writePacing: Duration.zero);

void main() {
  test('a clean write is confirmed by one read-back, no corrections', () async {
    final t = FakeBootloaderTransport();
    final result = await _client(t).flash(testImage(100));

    expect(result.readBacks, 1);
    expect(result.corrections, 0);
    // 100 bytes -> 4 chunks (the tail padded).
    expect(result.writes, 4);
    expect(result.reads, 4);
  });

  test(
    'the write response lets a rejected chunk be retried immediately',
    () async {
      // A rejected write is reported with a stale last-written offset, so the client retries it
      // inside the write loop instead of waiting for the read-back.
      final t = FakeBootloaderTransport()..rejectOnce.add(64);
      final result = await _client(t).flash(testImage(100));

      expect(result.readBacks, 1);
      expect(result.corrections, 0);
      // 4 chunks issued, plus one retry of the rejected chunk.
      expect(t.writes, 5);
      expect(t.flash[64], isNotNull);
    },
  );

  test('a dropped write the channel did not confirm is rewritten once', () async {
    // The passthrough ACKs without a last-written offset, so a dropped write only shows up as a
    // read-back mismatch - and only that kind of chunk may be rewritten.
    final t = FakeBootloaderTransport()
      ..nullOffsetAcks = true
      ..dropOnce.addAll({0, 64});
    final result = await _client(t).flash(testImage(100));

    expect(result.readBacks, 2, reason: 'the faults plus one re-read of them');
    expect(result.corrections, 2);
    expect(t.writes, 6, reason: '4 chunks written, 2 of them rewritten once');
    expect(t.reads, 6, reason: '4 chunks read back, then the 2 corrected ones');
  });

  test('a read timeout is retried, not treated as a mismatch', () async {
    final t = FakeBootloaderTransport()..timeoutOnce.add(32);
    final result = await _client(t).flash(testImage(100));

    expect(result.corrections, 0);
    // The timed-out chunk was re-read by the transport within the same read-back.
    expect(t.reads, greaterThan(t.writes));
  });

  group('a verification read is read-only and never repeats', () {
    // The card's case: the transport acknowledges every write (with the chunk's own offset) but
    // returns wrong data on read. The old loop read the image back, rewrote every chunk that did
    // not match and verified again, up to 20 passes. The run must now stop after the first
    // read-back, having written the image exactly once.
    test('an acknowledged chunk that reads back wrong stops the run', () async {
      final t = FakeBootloaderTransport()..corruptAll = true;
      const total = 4; // 100 bytes at the 32-byte payload

      await expectLater(
        _client(t).flash(testImage(100)),
        throwsA(
          isA<FlashAborted>()
              .having((a) => a.fault, 'fault', FlashFault.verifyMismatch)
              .having((a) => a.fault.label, 'label', 'image does not read back')
              .having((a) => a.mismatches?.count, 'count', total)
              .having((a) => a.mismatches?.confirmed, 'confirmed', total)
              .having((a) => a.mismatches?.unconfirmed, 'unconfirmed', 0)
              .having(
                (a) => a.mismatches?.summary,
                'summary',
                allOf(
                  contains('$total of $total chunks differ'),
                  contains('first: 0x0 (byte 0: wrote 0x00, read 0xff)'),
                ),
              )
              .having(
                (a) => a.message,
                'message',
                contains('acknowledged $total of the differing chunks'),
              )
              .having((a) => a.remedy, 'remedy', contains('Power-cycle')),
        ),
      );
      expect(
        t.writes,
        total,
        reason:
            'the image written exactly once: a chunk the device confirmed is not '
            'rewritten while it is failing to hold the data',
      );
      expect(t.reads, total, reason: 'one read-back of the image, then stop');
    });

    test('wrong and unconfirmed everywhere: one correction, then stop', () async {
      // Every write is ACKed without an offset and every read comes back wrong, so every chunk is
      // a correction candidate. The old bound was 20 read-verify-correct passes; the run gets one
      // correction round and one re-read, then aborts.
      final t = FakeBootloaderTransport()
        ..nullOffsetAcks = true
        ..corruptAll = true;
      const total = 4;

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
      expect(
        t.writes,
        total * 2,
        reason: 'one write pass, one correction round',
      );
      expect(
        t.reads,
        total + 1,
        reason:
            'one read-back, then the re-read stops at the first chunk still '
            'wrong',
      );
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
      expect(ok.readBacks, 1);
    },
  );

  // The passthrough CIDs are wire values shared with the firmware's Device.h; a remap on either
  // side would silently break every passthrough operation (the codec KATs pin the frame bytes,
  // but nothing pinned these). Docs/Services/Bootloader.md "Core Bootloader Passthrough
  // Commands (002x)".
  group('passthrough command IDs match the firmware', () {
    test('0x20 identification, 0x21 write, 0x22 read', () {
      expect(coreBootIdentify, 0x20);
      expect(coreBootWrite, 0x21);
      expect(coreBootRead, 0x22);
    });

    test('the three IDs are distinct and inside the 002x range', () {
      final ids = {coreBootIdentify, coreBootWrite, coreBootRead};
      expect(ids.length, 3);
      for (final id in ids) {
        expect(id, inInclusiveRange(0x20, 0x22));
      }
    });
  });
}
