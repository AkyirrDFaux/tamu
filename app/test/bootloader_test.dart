/// Host test for the bootloader packet codec (app/lib/core/bootloader.dart), protocol v2.
///
/// The known-answer vectors are the exact bytes printed by the firmware's
/// `firmware/test/native/bootloader_test.cpp`, so the two codecs are pinned to the same bytes.
/// Keep them in sync when the format changes.
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader.dart';

String hex(List<int> b) =>
    b.map((x) => x.toRadixString(16).padLeft(2, '0').toUpperCase()).join();

/// 100 bytes of `i & 0xFF`: 4 chunks once padded, i.e. 28 bytes of 0xFF tail.
final List<int> testImage100 = List<int>.generate(100, (i) => i);

void main() {
  test('the command field is (operation, direction) in four bits', () {
    // Docs/Services/Bootloader.md "Bootloader Packets": bit 0 direction, bits 1-3 operation.
    expect(Bootloader.cmdIdentifyRequest, 0x0); // 0b0000
    expect(Bootloader.cmdIdentifyResponse, 0x1); // 0b0001
    expect(Bootloader.cmdReadRequest, 0x2); // 0b0010
    expect(Bootloader.cmdReadResponse, 0x3); // 0b0011
    expect(Bootloader.cmdEraseRequest, 0x4); // 0b0100
    expect(Bootloader.cmdEraseResponse, 0x5); // 0b0101
    expect(Bootloader.cmdWrite, 0x6); // 0b0110
    expect(Bootloader.cmdWriteResponse, 0x7); // 0b0111
    expect(Bootloader.cmdCrcRequest, 0x8); // 0b1000
    expect(Bootloader.cmdCrcResponse, 0x9); // 0b1001
    // The pairs are one operation apart and the direction is bit 0 of each.
    for (var op = 0; op < 5; op++) {
      expect(Bootloader.cmdIdentifyRequest + (op << 1), op << 1);
      expect(Bootloader.cmdIdentifyResponse + (op << 1), (op << 1) | 1);
    }
    // 0b1010-0b1111 are unassigned and are not frame sizes.
    for (var cmd = 0x0A; cmd < 0x10; cmd++) {
      expect(Bootloader.frameSize(cmd), 0, reason: 'command 0x$cmd');
    }
    expect(Bootloader.readRequestSize, 7);
    expect(Bootloader.dataSize, 39);
    expect(Bootloader.identifyResponseSize, 39);
    expect(Bootloader.crcRequestSize, 3);
    expect(Bootloader.crcResponseSize, 7);
  });

  test('write frame matches the firmware KAT', () {
    final payload = List<int>.generate(Bootloader.payloadSize, (i) => i);
    final f = Bootloader.encodeWrite(0, payload);
    expect(f.length, Bootloader.dataSize);
    expect(
      hex(f),
      'CA0600000000000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1FBC',
    );
    expect(Bootloader.decode(f), Bootloader.cmdWrite);
    expect(Bootloader.offset(f), 0);
    expect(Bootloader.payload(f), payload);
  });

  test('write response matches the firmware KAT', () {
    final f = Bootloader.encodeWriteResponse(0x1040);
    expect(f.length, Bootloader.readRequestSize);
    expect(hex(f), 'CA1740100000BC');
    expect(Bootloader.decode(f), Bootloader.cmdWriteResponse);
    expect(Bootloader.offset(f), 0x1040);
    expect(Bootloader.ackFailed(0x1040), isFalse);
  });

  test('write response nothing-written sentinel round-trips', () {
    final f = Bootloader.encodeWriteResponse(Bootloader.nothingWritten);
    expect(hex(f), 'CA17FFFFFFFFBC');
    expect(Bootloader.decode(f), Bootloader.cmdWriteResponse);
    expect(Bootloader.offset(f), Bootloader.nothingWritten);
  });

  test('a page failure is flagged in bit 0 of the ack', () {
    // The failure flag: a real page offset is 64-byte aligned and a real chunk 32-byte aligned, so
    // bit 0 is never set in a plain ack.
    final f = Bootloader.encodeWriteResponse(0x3441);
    expect(Bootloader.decode(f), Bootloader.cmdWriteResponse);
    expect(Bootloader.ackFailed(Bootloader.offset(f)), isTrue);
    expect(Bootloader.ackFailed(0x3440), isFalse);
  });

  test('read request matches the firmware KAT', () {
    final f = Bootloader.encodeReadRequest(0x100);
    expect(f.length, Bootloader.readRequestSize);
    expect(hex(f), 'CA0200010000BC');
    expect(Bootloader.decode(f), Bootloader.cmdReadRequest);
    expect(Bootloader.offset(f), 0x100);
  });

  test('read response matches the firmware KAT', () {
    final payload = List<int>.generate(Bootloader.payloadSize, (i) => 0xA0 + i);
    final f = Bootloader.encodeReadResponse(0x40, payload);
    expect(
      hex(f),
      'CA1340000000A0A1A2A3A4A5A6A7A8A9AAABACADAEAFB0B1B2B3B4B5B6B7B8B9BABBBCBDBEBFBC',
    );
    expect(Bootloader.decode(f), Bootloader.cmdReadResponse);
    expect(Bootloader.offset(f), 0x40);
    expect(Bootloader.payload(f), payload);
  });

  test('identify request matches the firmware KAT', () {
    final f = Bootloader.encodeIdentifyRequest();
    expect(f.length, Bootloader.readRequestSize);
    expect(hex(f), 'CA0000000000BC');
    // Command 0b0000 is VALID, so decode must return it - not an "invalid" sentinel.
    expect(Bootloader.decode(f), Bootloader.cmdIdentifyRequest);
    expect(Bootloader.decode(f), 0);
    expect(Bootloader.offset(f), 0);
  });

  test('identify response carries size, page size and name', () {
    // The firmware KAT for (0x3600, 64, "DAS v0.1"): app size in the offset slot, the page size
    // right after it, then the 28-byte null-padded name.
    final das = Bootloader.encodeIdentifyResponse(
      0x3600,
      64,
      'DAS v0.1'.codeUnits,
    );
    expect(das.length, Bootloader.identifyResponseSize);
    expect(
      hex(das),
      'CA1100360000400000004441532076302E310000000000000000000000000000000000000000BC',
    );
    expect(Bootloader.decode(das), Bootloader.cmdIdentifyResponse);
    expect(Bootloader.appSize(das), 0x3600);
    expect(Bootloader.pageSize(das), 64);
    expect(Bootloader.identifyPageAt, 6);
    expect(Bootloader.identifyNameAt, 10);
    expect(Bootloader.name(das), 'DAS v0.1');

    // A 4 KB sector is expressible because the page size is a full u32.
    final core = Bootloader.encodeIdentifyResponse(
      0x283000,
      4096,
      'Tamu v2.0A'.codeUnits,
    );
    expect(
      hex(core),
      'CA11003028000010000054616D752076322E3041'
      '000000000000000000000000000000000000BC',
    );
    expect(Bootloader.decode(core), Bootloader.cmdIdentifyResponse);
    expect(Bootloader.appSize(core), 0x283000);
    expect(Bootloader.pageSize(core), 4096);
    expect(Bootloader.name(core), 'Tamu v2.0A');
  });

  test('identify response parities differ with the body (0x01 vs 0x11)', () {
    // The DAS's own geometry (app size 0x3800) gives control 0x01; 0x3600 gives 0x11. The parity
    // covers the app size, the page size and the name, so a wrong field cannot pass.
    final f = Bootloader.encodeIdentifyResponse(
      0x3800,
      64,
      'DAS v0.1'.codeUnits,
    );
    expect(
      hex(f),
      'CA0100380000400000004441532076302E31'
      '0000000000000000000000000000000000000000BC',
    );
    expect(Bootloader.decode(f), Bootloader.cmdIdentifyResponse);
    expect(Bootloader.pageSize(f), 64);
  });

  test('a name longer than 28 bytes is truncated, never spilling', () {
    final f = Bootloader.encodeIdentifyResponse(
      0,
      64,
      List<int>.filled(40, 0x41),
    );
    expect(f[Bootloader.identifyNameAt + 27], 0x41);
    expect(f[Bootloader.identifyResponseSize - 1], Bootloader.end);
    expect(Bootloader.name(f).length, Bootloader.nameSize);
    expect(Bootloader.decode(f), Bootloader.cmdIdentifyResponse);
  });

  test('erase request and response match the firmware KAT', () {
    final req = Bootloader.encodeEraseRequest(0x3440);
    expect(req.length, Bootloader.readRequestSize);
    expect(hex(req), 'CA1440340000BC');
    expect(Bootloader.decode(req), Bootloader.cmdEraseRequest);
    expect(Bootloader.offset(req), 0x3440);

    final ok = Bootloader.encodeEraseResponse(0x3440);
    expect(hex(ok), 'CA0540340000BC');
    expect(Bootloader.decode(ok), Bootloader.cmdEraseResponse);
    expect(Bootloader.ackFailed(Bootloader.offset(ok)), isFalse);

    // A page that is not blank is answered with the page and the flag.
    final failed = Bootloader.encodeEraseResponse(0x3440 | 1);
    expect(hex(failed), 'CA1541340000BC');
    expect(Bootloader.decode(failed), Bootloader.cmdEraseResponse);
    expect(Bootloader.ackFailed(Bootloader.offset(failed)), isTrue);
  });

  test('CRC request is three bytes: the CRC covers the whole app space', () {
    final f = Bootloader.encodeCrcRequest();
    expect(f.length, Bootloader.crcRequestSize);
    expect(Bootloader.crcRequestSize, 3);
    expect(hex(f), 'CA18BC');
    expect(Bootloader.decode(f), Bootloader.cmdCrcRequest);
  });

  test('CRC response matches the firmware KAT', () {
    final f = Bootloader.encodeCrcResponse(0xDEADBEEF);
    expect(f.length, Bootloader.crcResponseSize);
    expect(hex(f), 'CA09EFBEADDEBC');
    expect(Bootloader.decode(f), Bootloader.cmdCrcResponse);
    expect(Bootloader.crc(f), 0xDEADBEEF);

    // Little-endian, right after the control byte.
    final das = Bootloader.encodeCrcResponse(0xB674DAED);
    expect(hex(das), 'CA09EDDA74B6BC');
    expect(Bootloader.crc(das), 0xB674DAED);
  });

  test('a v1 control byte does not decode as v2', () {
    // The deprecated encoding put the parity at bit 3 over a three-bit command; v2 moved it to
    // bit 4 over four. A frame whose body parity is 1 therefore carries control 0x09 under the old
    // rules and 0x11 under the new: the old byte is not a v2 frame, and the deprecated decoder
    // recognizes it for what it is. (With body parity 0 the two bytes coincide - which is why the
    // app refuses a pre-v2 node by its PAGE FIELD, not by its control byte.)
    final v2 = Bootloader.encodeIdentifyResponse(0x3700, 64, 'DAS'.codeUnits);
    final v1 = List<int>.from(v2)..[1] = 0x09;
    expect(Bootloader.decode(v1), Bootloader.invalid);
    // ... and the deprecated decoder recognizes it for what it is.
    expect(Bootloader.decodeV1(v1), Bootloader.v1IdentifyResponse);
  });

  test('pageSizePlausible rejects the pre-v2 body', () {
    // The old layout has the name where the page size belongs: its first four characters read as
    // a 32-bit number. "DAS " is 0x20415344 - not a power of two.
    expect(Bootloader.pageSizePlausible(0x20415344), isFalse);
    expect(Bootloader.pageSizePlausible(0), isFalse);
    expect(Bootloader.pageSizePlausible(16), isFalse);
    expect(Bootloader.pageSizePlausible(48), isFalse);
    expect(Bootloader.pageSizePlausible(32), isTrue);
    expect(Bootloader.pageSizePlausible(64), isTrue);
    expect(Bootloader.pageSizePlausible(4096), isTrue);
    expect(Bootloader.pageSizePlausible(1 << 20), isFalse);
  });

  test('crc32 is CRC-32/ISO-HDLC (zlib.crc32)', () {
    // The value the device's CRC command returns and the manifest records. "123456789" is the
    // standard check string: 0xCBF43926.
    expect(Bootloader.crc32('123456789'.codeUnits), 0xCBF43926);
    expect(Bootloader.crc32(<int>[]), 0);
    expect(Bootloader.paddedLength(100), 128);
    expect(Bootloader.paddedLength(128), 128);
    expect(Bootloader.paddedLength(0), 0);
    expect(
      Bootloader.crc32Padded(testImage100),
      Bootloader.crc32([...testImage100, ...List.filled(28, 0xFF)]),
    );
    // The whole-space CRC: the image, then 0xFF to the app size.
    const space = 14336;
    expect(
      Bootloader.crc32Space(testImage100, space),
      Bootloader.crc32([
        ...testImage100,
        ...List.filled(space - testImage100.length, 0xFF),
      ]),
    );
    // Padding to the app space is not padding to a chunk boundary: 13812 -> 13824 is the image's
    // own chunk padding, 14336 is the space the device digests.
    expect(
      Bootloader.crc32Space(testImage100, 128),
      Bootloader.crc32Padded(testImage100),
    );
  });

  test('every single-bit flip is rejected', () {
    final payload = List<int>.generate(
      Bootloader.payloadSize,
      (i) => i * 3 + 1,
    );
    final frames = [
      Bootloader.encodeWrite(0x100, payload),
      Bootloader.encodeIdentifyResponse(0x3600, 64, 'DAS v0.1'.codeUnits),
      Bootloader.encodeEraseRequest(0x3400),
      Bootloader.encodeEraseResponse(0x3400),
      Bootloader.encodeCrcRequest(),
      Bootloader.encodeCrcResponse(0xDEADBEEF),
      Bootloader.encodeWriteResponse(0x3400),
    ];
    for (final f in frames) {
      for (var i = 0; i < f.length; i++) {
        for (var bit = 0; bit < 8; bit++) {
          final bad = List<int>.from(f);
          bad[i] ^= 1 << bit;
          expect(
            Bootloader.decode(bad),
            Bootloader.invalid,
            reason: '${hex(f)} byte $i bit $bit',
          );
        }
      }
    }
  });

  test('structural rejections', () {
    final f = Bootloader.encodeWrite(
      0,
      List<int>.filled(Bootloader.payloadSize, 0),
    );
    final noPad = List<int>.from(f)..[1] |= 0x20; // padding bit set (bit 5)
    expect(Bootloader.decode(noPad), Bootloader.invalid);
    final noStart = List<int>.from(f)..[0] = 0;
    expect(Bootloader.decode(noStart), Bootloader.invalid);
    final noEnd = List<int>.from(f)..[Bootloader.dataSize - 1] = 0;
    expect(Bootloader.decode(noEnd), Bootloader.invalid);
    expect(
      Bootloader.decode(f.sublist(0, Bootloader.readRequestSize)),
      Bootloader.invalid,
    );
    expect(Bootloader.decode(const []), Bootloader.invalid);
    // A header-only frame is 7 bytes like a read request: the command tells them apart.
    final read = Bootloader.encodeReadRequest(0x40);
    expect(Bootloader.decode(read), Bootloader.cmdReadRequest);
    expect(
      Bootloader.decode(Bootloader.encodeEraseRequest(0x40)),
      Bootloader.cmdEraseRequest,
    );
    // The CRC request is three bytes, so a shorter frame is not a frame at all.
    expect(Bootloader.decode(const [0xCA]), Bootloader.invalid);
    expect(Bootloader.decode(const [0xCA, 0x18]), Bootloader.invalid);
  });
}
