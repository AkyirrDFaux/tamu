/// Bootloader packet codec (Docs/Services/Bootloader.md).
///
/// The bootloader replaces the main binary; it deliberately ignores the standard packet
/// format and speaks this raw, byte-packed frame on USB (cores) or RSBus (nodes):
///
///   [0]      0xCA start marker
///   [1]      control: padding(4)=0 | parity(1, even) | command(3)
///   [2..5]   offset, u32 little-endian, relative to the main binary space start
///   [6..37]  32-byte payload  (write, read-response and identify-response)
///   [38]     0xBC end marker  (write, read-response and identify-response)
///
/// Header-only frames (no payload) are start + control + offset + end = 7 bytes.
/// Commands: 0b000 identify-request, 0b001 identify-response, 0b010 write-request,
/// 0b011 write-response, 0b100 read-request, 0b101 read-response. The write-response carries
/// the offset of the last successfully written chunk (0xFFFFFFFF = nothing written yet); the
/// identify-response carries the app-space size (u32) in the offset slot and the device name
/// as the 32-byte, null-padded payload. [decode] returns [_invalid] (not 0) for a bad frame,
/// because 0 is the valid identify-request command.
///
/// Parity is even over the "rest of the packet" after the control byte's padding/parity: the
/// command (3 bits), the offset and the payload (when present); the markers and the padding
/// bits are excluded. This mirrors the firmware's `Core/Functions/Bootloader.h` exactly; the
/// known-answer vectors are shared between the two test suites so the codecs cannot drift.
library;

import 'dart:typed_data';

import 'types.dart';

class Bootloader {
  static const int start = 0xCA;
  static const int end = 0xBC;

  static const int cmdIdentifyRequest = 0x0; // 0b000
  static const int cmdIdentifyResponse = 0x1; // 0b001
  static const int cmdWrite = 0x2; // 0b010
  static const int cmdWriteResponse = 0x3; // 0b011
  static const int cmdReadRequest = 0x4; // 0b100
  static const int cmdReadResponse = 0x5; // 0b101

  /// [decode]'s result for a bad frame. 0 is a valid command (identify-request), so it
  /// cannot be used as the "invalid" sentinel.
  static const int invalid = 0xFF;

  /// Nothing-written sentinel carried in a write-response's offset slot.
  static const int nothingWritten = 0xFFFFFFFF;

  static const int payloadSize = 32;
  static const int nameSize = 32; // identify-response name (null-padded)
  static const int headerSize = 6; // start + control + offset
  static const int readRequestSize = 7; // header + end
  static const int dataSize = 39; // header + payload + end
  static const int maxFrameSize = dataSize;

  /// Even parity over the command (3 bits), the offset and the payload (bytes 2..len-2).
  static int parity(List<int> frame) {
    var p = 0;
    final cmd = frame[1] & 0x07;
    p ^= (cmd & 1) ^ ((cmd >> 1) & 1) ^ ((cmd >> 2) & 1);
    for (var i = 2; i + 1 < frame.length; i++) {
      var b = frame[i];
      b ^= b >> 4;
      b ^= b >> 2;
      b ^= b >> 1;
      p ^= b & 1;
    }
    return p & 1;
  }

  static void _writeControl(Uint8List frame, int cmd) {
    frame[1] = cmd & 0x07;
    frame[1] |= parity(frame) << 3;
  }

  static Uint8List encodeWrite(int offset, List<int> payload) {
    final f = Uint8List(dataSize);
    f[0] = start;
    f.setRange(2, 6, uint32ToBytes(offset));
    f.setRange(6, 6 + payloadSize, padPayload(payload));
    f[dataSize - 1] = end;
    _writeControl(f, cmdWrite);
    return f;
  }

  /// Write acknowledgement: header-only, carrying the offset of the last successfully
  /// written chunk ([nothingWritten] when nothing has been written yet).
  static Uint8List encodeWriteResponse(int offset) {
    final f = Uint8List(readRequestSize);
    f[0] = start;
    f.setRange(2, 6, uint32ToBytes(offset));
    f[readRequestSize - 1] = end;
    _writeControl(f, cmdWriteResponse);
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
    f.setRange(6, 6 + payloadSize, padPayload(payload));
    f[dataSize - 1] = end;
    _writeControl(f, cmdReadResponse);
    return f;
  }

  /// Identify request: header-only, offset slot zero.
  static Uint8List encodeIdentifyRequest() {
    final f = Uint8List(readRequestSize);
    f[0] = start;
    // offset slot stays zero.
    f[readRequestSize - 1] = end;
    _writeControl(f, cmdIdentifyRequest);
    return f;
  }

  /// Identify response: app-space size (u32) in the offset slot, device name as the 32-byte,
  /// null-padded payload.
  static Uint8List encodeIdentifyResponse(int appSize, List<int> name) {
    final f = Uint8List(dataSize);
    f[0] = start;
    f.setRange(2, 6, uint32ToBytes(appSize));
    // padPayload zero-fills the tail, which is exactly the null-padding the name wants.
    f.setRange(6, 6 + nameSize, padPayload(name));
    f[dataSize - 1] = end;
    _writeControl(f, cmdIdentifyResponse);
    return f;
  }

  /// Left-aligns [payload] in a `payloadSize`-byte buffer, truncating any excess and
  /// zero-padding the tail. Shared by the encoders here and by the flashing client, which
  /// pads a chunk before sending it.
  static Uint8List padPayload(List<int> payload) {
    final out = Uint8List(payloadSize);
    final n = payload.length > payloadSize ? payloadSize : payload.length;
    out.setRange(0, n, payload);
    return out;
  }

  static int offset(List<int> frame) => uint32FromBytes(frame, 2);

  /// The app-space size carried in an identify-response's offset slot.
  static int appSize(List<int> frame) => uint32FromBytes(frame, 2);

  static List<int> payload(List<int> frame) =>
      frame.sublist(headerSize, headerSize + payloadSize);

  /// The device name carried in an identify-response's payload, with the trailing
  /// null padding trimmed.
  static String name(List<int> frame) {
    final bytes = payload(frame);
    var n = bytes.length;
    while (n > 0 && bytes[n - 1] == 0) {
      n--;
    }
    return String.fromCharCodes(bytes.sublist(0, n));
  }

  /// The frame length for a command, or 0 when the command is unknown.
  static int frameSize(int cmd) {
    if (cmd == cmdIdentifyRequest ||
        cmd == cmdWriteResponse ||
        cmd == cmdReadRequest) {
      return readRequestSize;
    }
    if (cmd == cmdIdentifyResponse ||
        cmd == cmdWrite ||
        cmd == cmdReadResponse) {
      return dataSize;
    }
    return 0;
  }

  /// Validates a complete frame; returns the command, or [invalid] when the frame is bad.
  static int decode(List<int> frame) {
    if (frame.length < readRequestSize || frame[0] != start) return invalid;
    if ((frame[1] & 0xF0) != 0) return invalid; // padding must be zero
    final cmd = frame[1] & 0x07;
    final expect = frameSize(cmd);
    if (expect == 0 || frame.length != expect || frame[frame.length - 1] != end) {
      return invalid;
    }
    final got = (frame[1] >> 3) & 1;
    return got == parity(frame) ? cmd : invalid;
  }
}
