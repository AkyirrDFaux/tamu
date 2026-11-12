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
/// 0b011 write-response, 0b100 read-request, 0b101 read-response, 0b110 digest-request,
/// 0b111 digest-response. The write-response carries the offset of the last successfully
/// written chunk (0xFFFFFFFF = nothing written yet); the identify-response carries the
/// app-space size (u32) in the offset slot and the device name as the 32-byte, null-padded
/// payload. [decode] returns [invalid] (not 0) for a bad frame, because 0 is the valid
/// identify-request command.
///
/// The digest request carries the range in place of a payload - the offset in the header's own
/// slot followed by a u16 length - and its 9-byte frame is start | control | offset | length |
/// end; the 11-byte response is start | control | offset | crc32 | end, the offset echoed. Bytes
/// past the end of the app region read as 0xFF, so a padded length still matches a correct
/// image, and the CRC is the same CRC-32/ISO-HDLC the package manifest records.
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
  static const int cmdDigestRequest = 0x6; // 0b110 - CRC32 of a range of the app region
  static const int cmdDigestResponse = 0x7; // 0b111 - the CRC, offset echoed

  /// [decode]'s result for a bad frame. 0 is a valid command (identify-request), so it
  /// cannot be used as the "invalid" sentinel.
  static const int invalid = 0xFF;

  /// Nothing-written sentinel carried in a write-response's offset slot.
  static const int nothingWritten = 0xFFFFFFFF;

  static const int payloadSize = 32;
  static const int headerSize = 6; // start + control + offset
  static const int readRequestSize = 7; // header + end
  static const int dataSize = 39; // header + payload + end
  static const int digestRequestSize = 9; // header + length(u16) + end
  static const int digestResponseSize = 11; // header + crc32(u32) + end

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

  /// A header-only frame (`readRequestSize` bytes): start marker, [offset] slot, end marker, and
  /// the command with its parity in the control byte. Every header-only command is this frame -
  /// a write acknowledgement, a read request, an identify request.
  static Uint8List _headerFrame(int offset, int cmd) {
    final f = Uint8List(readRequestSize);
    f[0] = start;
    f.setRange(2, 6, uint32ToBytes(offset));
    f[readRequestSize - 1] = end;
    _writeControl(f, cmd);
    return f;
  }

  /// A frame carrying a `payloadSize`-byte body (`dataSize` bytes): the header above plus the
  /// padded body before the end marker. The identify response's 32-byte name is the same width,
  /// so it rides here too.
  static Uint8List _payloadFrame(int offset, List<int> payload, int cmd) {
    final f = Uint8List(dataSize);
    f[0] = start;
    f.setRange(2, 6, uint32ToBytes(offset));
    f.setRange(6, 6 + payloadSize, padPayload(payload));
    f[dataSize - 1] = end;
    _writeControl(f, cmd);
    return f;
  }

  static Uint8List encodeWrite(int offset, List<int> payload) =>
      _payloadFrame(offset, payload, cmdWrite);

  /// Write acknowledgement: header-only, carrying the offset of the last successfully
  /// written chunk ([nothingWritten] when nothing has been written yet).
  static Uint8List encodeWriteResponse(int offset) =>
      _headerFrame(offset, cmdWriteResponse);

  static Uint8List encodeReadRequest(int offset) =>
      _headerFrame(offset, cmdReadRequest);

  static Uint8List encodeReadResponse(int offset, List<int> payload) =>
      _payloadFrame(offset, payload, cmdReadResponse);

  /// Identify request: header-only, offset slot zero.
  static Uint8List encodeIdentifyRequest() =>
      _headerFrame(0, cmdIdentifyRequest);

  /// Digest request: the range's offset in the header's own slot and its length as a u16 after
  /// it. The node answers with the CRC32 of `[offset, offset + length)`; bytes past the end of
  /// the app region read as 0xFF, so [paddedLength] is the length to ask with.
  static Uint8List encodeDigestRequest(int offset, int length) {
    final f = Uint8List(digestRequestSize);
    f[0] = start;
    f.setRange(2, 6, uint32ToBytes(offset));
    f[6] = length & 0xFF;
    f[7] = (length >> 8) & 0xFF;
    f[digestRequestSize - 1] = end;
    _writeControl(f, cmdDigestRequest);
    return f;
  }

  /// Digest response: the offset echoed in the header's slot, then the CRC32 (u32).
  static Uint8List encodeDigestResponse(int offset, int crc) {
    final f = Uint8List(digestResponseSize);
    f[0] = start;
    f.setRange(2, 6, uint32ToBytes(offset));
    f.setRange(6, 10, uint32ToBytes(crc));
    f[digestResponseSize - 1] = end;
    _writeControl(f, cmdDigestResponse);
    return f;
  }

  /// Identify response: app-space size (u32) in the offset slot, device name as the 32-byte,
  /// null-padded payload.
  static Uint8List encodeIdentifyResponse(int appSize, List<int> name) =>
      // padPayload zero-fills the tail, which is exactly the null-padding the name wants.
      _payloadFrame(appSize, name, cmdIdentifyResponse);

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

  /// The length carried by a digest request.
  static int digestLength(List<int> frame) => frame[6] | (frame[7] << 8);

  /// The CRC32 carried by a digest response.
  static int digestCrc(List<int> frame) => uint32FromBytes(frame, 6);

  /// The padded length of an image: the whole number of 32-byte chunks the bootloader covers,
  /// which is the range a digest request asks about (the tail reads back as erased 0xFF).
  static int paddedLength(int length) =>
      ((length + payloadSize - 1) ~/ payloadSize) * payloadSize;

  /// One step of CRC-32/ISO-HDLC (reflected poly 0xEDB88320) - the value the device's digest
  /// command returns, how zlib.crc32 computes, and what the package manifest carries.
  static int _crcStep(int crc, int byte) {
    var c = crc ^ (byte & 0xFF);
    for (var bit = 0; bit < 8; bit++) {
      c = (c >> 1) ^ ((c & 1) != 0 ? 0xEDB88320 : 0);
    }
    return c;
  }

  /// CRC32 over [data].
  static int crc32(List<int> data) {
    var crc = 0xFFFFFFFF;
    for (final b in data) {
      crc = _crcStep(crc, b);
    }
    return (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF;
  }

  /// CRC32 over [image] padded with 0xFF to [paddedLength] - the digest a device holding
  /// exactly these bytes answers with, and the manifest's `crc32_padded`.
  static int crc32Padded(List<int> image) {
    var crc = 0xFFFFFFFF;
    for (final b in image) {
      crc = _crcStep(crc, b);
    }
    for (var i = image.length; i < paddedLength(image.length); i++) {
      crc = _crcStep(crc, 0xFF);
    }
    return (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF;
  }

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
    if (cmd == cmdDigestRequest) return digestRequestSize;
    if (cmd == cmdDigestResponse) return digestResponseSize;
    return 0;
  }

  /// Validates a complete frame; returns the command, or [invalid] when the frame is bad.
  static int decode(List<int> frame) {
    if (frame.length < readRequestSize || frame[0] != start) return invalid;
    if ((frame[1] & 0xF0) != 0) return invalid; // padding must be zero
    final cmd = frame[1] & 0x07;
    final expect = frameSize(cmd);
    if (expect == 0 ||
        frame.length != expect ||
        frame[frame.length - 1] != end) {
      return invalid;
    }
    final got = (frame[1] >> 3) & 1;
    return got == parity(frame) ? cmd : invalid;
  }
}
