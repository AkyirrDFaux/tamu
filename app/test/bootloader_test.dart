/// Host test for the bootloader packet codec (app/lib/core/bootloader.dart).
///
/// The known-answer vectors are the exact strings printed by the firmware's
/// `firmware/test/native/bootloader_test.cpp`, so the two codecs are pinned to the same
/// bytes. Keep them in sync when the format changes.
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader.dart';

String hex(List<int> b) =>
    b.map((x) => x.toRadixString(16).padLeft(2, '0').toUpperCase()).join();

void main() {
  test('write frame matches the firmware KAT', () {
    final payload = List<int>.generate(Bootloader.payloadSize, (i) => i);
    final f = Bootloader.encodeWrite(0, payload);
    expect(f.length, Bootloader.dataSize);
    expect(
      hex(f),
      'CA0500000000000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1FBC',
    );
    expect(Bootloader.decode(f), Bootloader.cmdWrite);
    expect(Bootloader.offset(f), 0);
    expect(Bootloader.payload(f), payload);
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
      'CA0740000000A0A1A2A3A4A5A6A7A8A9AAABACADAEAFB0B1B2B3B4B5B6B7B8B9BABBBCBDBEBFBC',
    );
    expect(Bootloader.decode(f), Bootloader.cmdReadResponse);
    expect(Bootloader.offset(f), 0x40);
    expect(Bootloader.payload(f), payload);
  });

  test('every single-bit flip is rejected', () {
    final payload = List<int>.generate(Bootloader.payloadSize, (i) => i * 3 + 1);
    final f = Bootloader.encodeWrite(0x100, payload);
    for (var i = 0; i < f.length; i++) {
      for (var bit = 0; bit < 8; bit++) {
        final bad = List<int>.from(f);
        bad[i] ^= 1 << bit;
        expect(Bootloader.decode(bad), 0, reason: 'byte $i bit $bit');
      }
    }
  });

  test('structural rejections', () {
    final f = Bootloader.encodeWrite(0, List<int>.filled(Bootloader.payloadSize, 0));
    final noPad = List<int>.from(f)..[1] |= 0x08;
    expect(Bootloader.decode(noPad), 0);
    final noStart = List<int>.from(f)..[0] = 0;
    expect(Bootloader.decode(noStart), 0);
    final noEnd = List<int>.from(f)..[Bootloader.dataSize - 1] = 0;
    expect(Bootloader.decode(noEnd), 0);
    expect(Bootloader.decode(f.sublist(0, Bootloader.readRequestSize)), 0);
    expect(Bootloader.decode(const []), 0);
  });
}
