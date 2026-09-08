import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/transport.dart';

void main() {
  group('PacketFrame', () {
    test('round-trips a frame (payload padded to 4)', () {
      final frame = PacketFrame.single(
        targetId: 0x0002,
        srvTarget: makeService(ServiceType.device, 3),
        srvSource: makeService(ServiceType.device, 42),
        response: false,
        payload: [1, 2, 3],
      );
      final bytes = frame.toBytes();
      expect(bytes[0], crc8(bytes.sublist(1)));
      // Header: crc8 | flags | priority | payload_len(units) | ids/srvs.
      expect(bytes[2], defaultPriority);
      expect(bytes[3], 1, reason: '3-byte payload padded to 4 -> 1 unit');

      final parsed = PacketFrame.tryParse(bytes, 0)!;
      expect(parsed.flags, frame.flags);
      expect(parsed.priority, defaultPriority);
      expect(parsed.idTarget, 0x0002);
      expect(parsed.srvTarget, frame.srvTarget);
      expect(parsed.srvSource, frame.srvSource);
      expect(parsed.payload, [1, 2, 3, 0], reason: 'wire payload is padded to 4');
    });

    test('a 116-byte payload fits (max payload size)', () {
      final payload = List<int>.generate(116, (i) => i & 0xFF);
      final frame = PacketFrame.single(
        targetId: 1,
        srvTarget: makeService(ServiceType.storage, 6),
        srvSource: makeService(ServiceType.app, 1),
        response: false,
        payload: payload,
      );
      final bytes = frame.toBytes();
      expect(bytes[3], 29, reason: '116 bytes / 4 = 29 units');
      final parsed = PacketFrame.tryParse(bytes, 0)!;
      expect(parsed.payload, payload);
    });

    test('FRAG info helpers round-trip', () {
      final info = writeFragInfo(2, 17);
      expect(info, [2, 0, 17, 0]);
      final parsed = fragInfoOf(info);
      expect(parsed.current, 2);
      expect(parsed.total, 17);
    });

    test('detects CRC corruption', () {
      final bytes = PacketFrame.single(
        targetId: 1,
        srvTarget: makeService(ServiceType.device, 1),
        srvSource: makeService(ServiceType.device, 1),
        response: false,
      ).toBytes();
      bytes[bytes.length - 1] ^= 0xFF;
      expect(() => PacketFrame.tryParse(bytes, 0), throwsFormatException);
    });

    test('stream parser reassembles concatenated frames', () {
      final parser = PacketStreamParser();
      final a = PacketFrame.single(
              targetId: 2,
              srvTarget: makeService(ServiceType.systemMemory, 2),
              srvSource: makeService(ServiceType.systemMemory, 7),
              response: true,
              payload: [9, 9])
          .toBytes();
      final b = PacketFrame.single(
              targetId: 2,
              srvTarget: makeService(ServiceType.device, 8),
              srvSource: makeService(ServiceType.device, 8),
              response: true)
          .toBytes();

      // Feed split across chunk boundaries.
      final all = [...a, ...b];
      var frames = parser.feed(all.sublist(0, 10));
      expect(frames, isEmpty);
      frames = parser.feed(all.sublist(10));
      expect(frames.length, 2);
      expect(frames[0].payload, [9, 9, 0, 0], reason: 'wire payload is padded to 4');
      expect(frames[1].payload, isEmpty);
    });
  });

  group('USB framing', () {
    test('builds and parses link frames', () {
      const stream = [1, 2, 3, 4];
      final frame = buildUsbFrame(stream);
      expect(frame.first, usbFrameStart);
      expect(frame.last, usbFrameStop);
      expect(frame[2], stream.length);

      final parsed = UsbFrameParser().feed(frame);
      expect(parsed, stream);
    });

    test('resyncs on garbage between frames', () {
      final frame = buildUsbFrame([5, 6]);
      final parsed = UsbFrameParser().feed([0x00, 0xFF, ...frame]);
      expect(parsed, [5, 6]);
    });
  });

  group('BLE framing', () {
    test('length-prefixed chunks round-trip', () {
      final parser = BleLengthParser();
      final chunks = BleLengthParser.chunkOutgoing(
          [1, 2, 3, 4, 5, 6, 7, 8, 9, 10],
          mtu: 13);
      // MTU 13: total per write <= mtu - 3 -> payload <= 8 bytes per chunk.
      expect(chunks.length, 2);
      Uint8List assembled = Uint8List(0);
      for (final chunk in chunks) {
        final out = parser.feed(chunk);
        assembled = Uint8List.fromList([...assembled, ...out]);
      }
      expect(assembled, [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);
    });
  });
}
