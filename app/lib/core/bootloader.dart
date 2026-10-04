/// Bootloader packet codec (Docs/Services/Bootloader.md).
///
/// The bootloader replaces the main binary; it deliberately ignores the standard packet
/// format and speaks this raw, byte-packed frame on USB (cores) or RSBus (nodes):
///
///   [0]      0xCA start marker
///   [1]      control: padding(5)=0 | parity(1, even) | command(2)
///   [2..5]   offset, u32 little-endian, relative to the main binary space start
///   [6..37]  32-byte payload  (write and read-response only)
///   [38]     0xBC end marker  (write and read-response)
///
/// A read-request is header-only (7 bytes). Commands: 01 write, 10 read-request, 11
/// read-response. Parity is even over every bit of the frame except the parity bit itself,
/// so a valid frame has an even total number of set bits. This mirrors the firmware's
/// `Core/Functions/Bootloader.h` exactly; the known-answer vectors are shared between the
/// two test suites so the codecs cannot drift.
library;

import 'dart:typed_data';

import 'types.dart';

class Bootloader {
  static const int start = 0xCA;
  static const int end = 0xBC;

  static const int cmdWrite = 0x1; // 0b01
  static const int cmdReadRequest = 0x2; // 0b10
  static const int cmdReadResponse = 0x3; // 0b11

  static const int payloadSize = 32;
  static const int headerSize = 6; // start + control + offset
  static const int readRequestSize = 7; // header + end
  static const int dataSize = 39; // header + payload + end
  static const int maxFrameSize = dataSize;

  /// Even parity over the whole frame, excluding the parity bit (byte 1, bit 2).
  static int parity(List<int> frame) {
    var p = 0;
    for (var i = 0; i < frame.length; i++) {
      var b = frame[i];
      if (i == 1) b &= ~0x04;
      b ^= b >> 4;
      b ^= b >> 2;
      b ^= b >> 1;
      p ^= b & 1;
    }
    return p & 1;
  }

  static void _writeControl(Uint8List frame, int cmd) {
    frame[1] = cmd & 0x03;
    frame[1] |= parity(frame) << 2;
  }

  static Uint8List encodeWrite(int offset, List<int> payload) {
    final f = Uint8List(dataSize);
    f[0] = start;
    f.setRange(2, 6, uint32ToBytes(offset));
    f.setRange(6, 6 + payloadSize, _pad(payload));
    f[dataSize - 1] = end;
    _writeControl(f, cmdWrite);
    return f;
  }

  static Uint8List encodeReadRequest(int offset) {
    final f = Uint8List(readRequestSize);
    f[0] = start;
    f.setRange(2, 6, uint32ToBytes(offset));
    f[readRequestSize - 1] = end;
    _writeControl(f, cmdReadRequest);
    return f;
  }

  static Uint8List encodeReadResponse(int offset, List<int> payload) {
    final f = Uint8List(dataSize);
    f[0] = start;
    f.setRange(2, 6, uint32ToBytes(offset));
    f.setRange(6, 6 + payloadSize, _pad(payload));
    f[dataSize - 1] = end;
    _writeControl(f, cmdReadResponse);
    return f;
  }

  static Uint8List _pad(List<int> payload) {
    final out = Uint8List(payloadSize);
    final n = payload.length > payloadSize ? payloadSize : payload.length;
    out.setRange(0, n, payload);
    return out;
  }

  static int offset(List<int> frame) => uint32FromBytes(frame, 2);

  static List<int> payload(List<int> frame) =>
      frame.sublist(headerSize, headerSize + payloadSize);

  /// The frame length for a command, or 0 when the command is unknown.
  static int frameSize(int cmd) {
    if (cmd == cmdReadRequest) return readRequestSize;
    if (cmd == cmdWrite || cmd == cmdReadResponse) return dataSize;
    return 0;
  }

  /// Validates a complete frame; returns the command, or 0 when invalid.
  static int decode(List<int> frame) {
    if (frame.length < readRequestSize || frame[0] != start) return 0;
    if ((frame[1] & 0xF8) != 0) return 0; // padding must be zero
    final cmd = frame[1] & 0x03;
    final expect = frameSize(cmd);
    if (expect == 0 || frame.length != expect || frame[frame.length - 1] != end) {
      return 0;
    }
    final got = (frame[1] >> 2) & 1;
    return got == parity(frame) ? cmd : 0;
  }
}
