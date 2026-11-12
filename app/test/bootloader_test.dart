/// Host test for the bootloader packet codec (app/lib/core/bootloader.dart).
///
/// The known-answer vectors match the exact bytes printed by the firmware's
/// `firmware/test/native/bootloader_test.cpp`, so the two codecs are pinned to the same
/// bytes. Keep them in sync when the format changes.
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader.dart';

String hex(List<int> b) =>
    b.map((x) => x.toRadixString(16).padLeft(2, '0').toUpperCase()).join();

/// 100 bytes of `i & 0xFF`: 4 chunks once padded, i.e. 28 bytes of 0xFF tail.
final List<int> testImage100 = List<int>.generate(100, (i) => i);

void main() {
  test('write frame matches the firmware KAT', () {
    final payload = List<int>.generate(Bootloader.payloadSize, (i) => i);
    final f = Bootloader.encodeWrite(0, payload);
    expect(f.length, Bootloader.dataSize);
    expect(
      hex(f),
      'CA0A00000000000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1FBC',
    );
    expect(Bootloader.decode(f), Bootloader.cmdWrite);
    expect(Bootloader.offset(f), 0);
    expect(Bootloader.payload(f), payload);
  });

  test('write response matches the firmware KAT', () {
    final f = Bootloader.encodeWriteResponse(0x40);
    expect(f.length, Bootloader.readRequestSize);
    expect(hex(f), 'CA0B40000000BC');
    expect(Bootloader.decode(f), Bootloader.cmdWriteResponse);
    expect(Bootloader.offset(f), 0x40);
  });

  test('write response nothing-written sentinel round-trips', () {
    final f = Bootloader.encodeWriteResponse(Bootloader.nothingWritten);
    expect(hex(f), 'CA03FFFFFFFFBC');
    expect(Bootloader.decode(f), Bootloader.cmdWriteResponse);
    expect(Bootloader.offset(f), Bootloader.nothingWritten);
  });

  test('read request matches the firmware KAT', () {
    final f = Bootloader.encodeReadRequest(0x100);
    expect(f.length, Bootloader.readRequestSize);
    expect(hex(f), 'CA0400010000BC');
    expect(Bootloader.decode(f), Bootloader.cmdReadRequest);
    expect(Bootloader.offset(f), 0x100);
  });

  test('read response matches the firmware KAT', () {
    final payload = List<int>.generate(Bootloader.payloadSize, (i) => 0xA0 + i);
    final f = Bootloader.encodeReadResponse(0x40, payload);
    expect(
      hex(f),
      'CA0D40000000A0A1A2A3A4A5A6A7A8A9AAABACADAEAFB0B1B2B3B4B5B6B7B8B9BABBBCBDBEBFBC',
    );
    expect(Bootloader.decode(f), Bootloader.cmdReadResponse);
    expect(Bootloader.offset(f), 0x40);
    expect(Bootloader.payload(f), payload);
  });

  test('identify request matches the firmware KAT', () {
    final f = Bootloader.encodeIdentifyRequest();
    expect(f.length, Bootloader.readRequestSize);
    expect(hex(f), 'CA0000000000BC');
    // Command 0b000 is VALID, so decode must return it - not an "invalid" sentinel.
    expect(Bootloader.decode(f), Bootloader.cmdIdentifyRequest);
    expect(Bootloader.decode(f), 0);
    expect(Bootloader.offset(f), 0);
  });

  test('identify response matches the firmware KAT', () {
    final f = Bootloader.encodeIdentifyResponse(
      0x2A0000,
      'Tamu core v2.0A'.codeUnits,
    );
    expect(f.length, Bootloader.dataSize);
    expect(
      hex(f),
      'CA0100002A0054616D7520636F72652076322E3041'
      '0000000000000000000000000000000000BC',
    );
    expect(Bootloader.decode(f), Bootloader.cmdIdentifyResponse);
    expect(Bootloader.appSize(f), 0x2A0000);
    expect(Bootloader.name(f), 'Tamu core v2.0A');
  });

  test('digest request matches the firmware KAT', () {
    final f = Bootloader.encodeDigestRequest(0x1200, 0x1000);
    expect(f.length, Bootloader.digestRequestSize);
    expect(hex(f), 'CA0E001200000010BC');
    expect(Bootloader.decode(f), Bootloader.cmdDigestRequest);
    expect(Bootloader.offset(f), 0x1200);
    expect(Bootloader.digestLength(f), 0x1000);
  });

  test('digest response matches the firmware KAT', () {
    final f = Bootloader.encodeDigestResponse(0x1200, 0xDEADBEEF);
    expect(f.length, Bootloader.digestResponseSize);
    expect(hex(f), 'CA0F00120000EFBEADDEBC');
    expect(Bootloader.decode(f), Bootloader.cmdDigestResponse);
    expect(Bootloader.offset(f), 0x1200); // echoed
    expect(Bootloader.digestCrc(f), 0xDEADBEEF);
  });

  test('a digest frame of the other length is rejected', () {
    final req = Bootloader.encodeDigestRequest(0x1200, 0x1000);
    final resp = Bootloader.encodeDigestResponse(0x1200, 0xDEADBEEF);
    expect(Bootloader.decode(req), Bootloader.cmdDigestRequest);
    expect(Bootloader.decode(resp), Bootloader.cmdDigestResponse);
    // The two are 9 and 11 bytes: each must fail the other's size.
    expect(
      Bootloader.decode(resp.sublist(0, Bootloader.digestRequestSize)),
      Bootloader.invalid,
    );
    expect(
      Bootloader.decode([...req, 0x00]),
      Bootloader.invalid,
    );
  });

  test('crc32 is CRC-32/ISO-HDLC (zlib.crc32)', () {
    // The value the device's digest command returns and the manifest records. "123456789" is the
    // standard check string: 0xCBF43926.
    expect(Bootloader.crc32('123456789'.codeUnits), 0xCBF43926);
    expect(Bootloader.crc32(<int>[]), 0);
    // The app region's tail reads back as erased 0xFF, so the padded range is what is asked about.
    expect(Bootloader.paddedLength(100), 128);
    expect(Bootloader.paddedLength(128), 128);
    expect(Bootloader.paddedLength(0), 0);
    expect(
      Bootloader.crc32Padded(testImage100),
      Bootloader.crc32([...testImage100, ...List.filled(28, 0xFF)]),
    );
  });

  test('every single-bit flip is rejected', () {
    final payload = List<int>.generate(
      Bootloader.payloadSize,
      (i) => i * 3 + 1,
    );
    final f = Bootloader.encodeWrite(0x100, payload);
    for (var i = 0; i < f.length; i++) {
      for (var bit = 0; bit < 8; bit++) {
        final bad = List<int>.from(f);
        bad[i] ^= 1 << bit;
        expect(
          Bootloader.decode(bad),
          Bootloader.invalid,
          reason: 'byte $i bit $bit',
        );
      }
    }
  });

  test('structural rejections', () {
    final f = Bootloader.encodeWrite(
      0,
      List<int>.filled(Bootloader.payloadSize, 0),
    );
    final noPad = List<int>.from(f)..[1] |= 0x10; // padding bit set
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
  });
}
