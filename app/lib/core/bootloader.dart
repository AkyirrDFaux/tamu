/// Bootloader packet codec (Docs/Services/Bootloader.md, `c3aabab`), protocol v2.
///
/// The bootloader replaces the main binary; it ignores the standard packet format and speaks
/// this raw, byte-packed frame on USB (cores) or RSBus (nodes):
///
///   [0]      0xCA start marker
///   [1]      control: padding(3)=0 | parity(1, even) | command(4)
///   [2..5]   offset, u32 little-endian, relative to the main binary space start
///   [6..37]  32-byte payload  (write, read-response)
///   [38]     0xBC end marker  (write, read-response)
///
/// Header-only frames (the offset slot only) are start | control | offset | end = 7 bytes.
///
/// The command is a 4-bit field: bit 0 is the direction (0 request, 1 response) and bits 1-3 the
/// operation, so `0b000x` identify, `0b001x` read, `0b010x` erase, `0b011x` write, `0b100x`
/// CRC. `0b1010`-`0b1111` are unassigned. The parity bit sits at bit 4 because the command
/// needs four bits, and that is what makes a v2 frame distinguishable from a v1 one: the frame
/// LENGTHS are the same (both identify responses are 39 bytes), the control byte is not.
///
/// The identify response carries the app-space size (u32) in the offset slot, then the flash
/// PAGE SIZE (u32, the erase unit), then the device name as a 28-byte null-padded field:
/// start | control | size | page | name | end = 39 bytes. A bootloader that reports no usable
/// page size there is a pre-v2 one and is refused rather than guessed at - see
/// [Bootloader.pageSizePlausible].
///
/// The CRC request carries nothing (start | control | end, 3 bytes) because the CRC always covers
/// the WHOLE app space; the response is start | control | crc32 | end = 7 bytes. CRC-32 is the
/// reflected 0xEDB88320 polynomial, init 0xFFFFFFFF, final XOR - the value `zlib.crc32`
/// computes (CRC-32/ISO-HDLC).
///
/// The erase request names one page in the offset slot and must be page-aligned; any other offset
/// is refused, never rounded. Its response carries the page it blanked, with bit 0 set when that
/// page is not blank - bit 0 is the protocol's failure flag on an erase ack and on a write ack,
/// because every real page/chunk offset is 32-byte aligned.
///
/// Parity is even over the "rest of the packet" after the control byte's padding/parity: the
/// command (4 bits), the offset and the payload (when present). The markers and the padding bits
/// are excluded. This mirrors the firmware's `Core/Functions/Bootloader.h` exactly; the
/// known-answer vectors are shared between the two test suites so the codecs cannot drift.
library;

import 'dart:typed_data';

import 'types.dart';

class Bootloader {
  static const int start = 0xCA;
  static const int end = 0xBC;

  // Bit 0 is the direction (0 request, 1 response), bits 1-3 the operation.
  static const int cmdIdentifyRequest = 0x0; // 0b0000
  static const int cmdIdentifyResponse = 0x1; // 0b0001
  static const int cmdReadRequest = 0x2; // 0b0010
  static const int cmdReadResponse = 0x3; // 0b0011
  static const int cmdEraseRequest =
      0x4; // 0b0100 - one page, its own base offset
  static const int cmdEraseResponse =
      0x5; // 0b0101 - the page, bit 0 set when not blank
  static const int cmdWrite = 0x6; // 0b0110 - one 32-byte chunk
  static const int cmdWriteResponse =
      0x7; // 0b0111 - the last successfully written offset
  static const int cmdCrcRequest =
      0x8; // 0b1000 - no body: the CRC covers the app space
  static const int cmdCrcResponse =
      0x9; // 0b1001 - the CRC32 of the whole app space

  /// [decode]'s result for a bad frame. 0 is a valid command (identify-request), so it cannot be
  /// used as the "invalid" sentinel.
  static const int invalid = 0xFF;

  /// Nothing-written sentinel carried in a write-response's offset slot.
  static const int nothingWritten = 0xFFFFFFFF;

  static const int payloadSize = 32;
  static const int headerSize = 6; // start + control + offset
  static const int readRequestSize = 7; // header + end
  static const int dataSize = 39; // header + payload + end
  static const int crcRequestSize = 3; // start + control + end
  static const int crcResponseSize = 7; // start + control + crc32 + end
  static const int nameSize = 28;

  /// The identify response's geometry field, right after the app size.
  static const int identifyPageAt = headerSize; // 6

  /// The name that follows the app size and the page size.
  static const int identifyNameAt = headerSize + 4; // 10
  static const int identifyResponseSize = identifyNameAt + nameSize + 1; // 39

  /// True when an erase or write ack carries the failure flag: bit 0 of the offset slot, which no
  /// real page or chunk offset ever sets.
  static bool ackFailed(int offset) => (offset & 1) != 0;

  /// Even parity over the command (4 bits), the offset and the payload (bytes 2..len-2).
  ///
  /// Parity of a XOR is the XOR of the parities, so the whole body collapses to one byte with a
  /// single XOR per byte and one fold at the end - the same fold the firmware runs.
  static int parity(List<int> frame) {
    var acc = frame[1] & 0x0F;
    for (var i = 2; i + 1 < frame.length; i++) {
      acc ^= frame[i];
    }
    acc ^= acc >> 4;
    acc ^= acc >> 2;
    acc ^= acc >> 1;
    return acc & 1;
  }

  static void _writeControl(Uint8List frame, int cmd) {
    // The command is part of the parity, so it has to sit in the control byte BEFORE the parity is
    // folded - otherwise `parity` reads a zero command and every odd-parity command (read, erase,
    // CRC, identify-response) goes out one bit wrong.
    frame[1] = cmd & 0x0F;
    frame[1] |= parity(frame) << 4;
  }

  /// A header-only frame (`readRequestSize` bytes): start marker, [offset] slot, end marker, and
  /// the command with its parity in the control byte. One builder serves every request and ack
  /// of that shape - identify/read/erase requests, write and erase acknowledgements.
  static Uint8List _headerFrame(int offset, int cmd) {
    final f = Uint8List(readRequestSize);
    f[0] = start;
    f.setRange(2, 6, uint32ToBytes(offset));
    f[readRequestSize - 1] = end;
    _writeControl(f, cmd);
    return f;
  }

  /// A frame carrying a `payloadSize`-byte body (`dataSize` bytes): the header above plus the
  /// padded body before the end marker.
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

  /// Write acknowledgement: header-only, carrying the offset of the last successfully written
  /// chunk, or the page with bit 0 set when a chunk in it could not be written.
  static Uint8List encodeWriteResponse(int offset) =>
      _headerFrame(offset, cmdWriteResponse);

  static Uint8List encodeReadRequest(int offset) =>
      _headerFrame(offset, cmdReadRequest);

  static Uint8List encodeReadResponse(int offset, List<int> payload) =>
      _payloadFrame(offset, payload, cmdReadResponse);

  /// Identify request: header-only, offset slot zero.
  static Uint8List encodeIdentifyRequest() =>
      _headerFrame(0, cmdIdentifyRequest);

  /// Identify response: app-space size (u32) in the offset slot, the flash page size (u32), then
  /// the name as a 28-byte, null-padded field - constants first, name last.
  static Uint8List encodeIdentifyResponse(
    int appSize,
    int pageSize,
    List<int> name,
  ) {
    final f = Uint8List(identifyResponseSize);
    f[0] = start;
    f.setRange(2, 6, uint32ToBytes(appSize));
    f.setRange(identifyPageAt, identifyPageAt + 4, uint32ToBytes(pageSize));
    final n = name.length > nameSize ? nameSize : name.length;
    f.setRange(identifyNameAt, identifyNameAt + n, name.sublist(0, n));
    f[identifyResponseSize - 1] = end;
    _writeControl(f, cmdIdentifyResponse);
    return f;
  }

  /// Erase request: header-only, naming the page by its own base offset.
  static Uint8List encodeEraseRequest(int offset) =>
      _headerFrame(offset, cmdEraseRequest);

  /// Erase acknowledgement: the page blanked, with bit 0 set when it is not blank.
  static Uint8List encodeEraseResponse(int offset) =>
      _headerFrame(offset, cmdEraseResponse);

  /// CRC request: the shortest frame there is, because the CRC covers the whole app space.
  static Uint8List encodeCrcRequest() {
    final f = Uint8List(crcRequestSize);
    f[0] = start;
    f[crcRequestSize - 1] = end;
    _writeControl(f, cmdCrcRequest);
    return f;
  }

  /// CRC response: the CRC32 of the whole app space between the control byte and the end marker.
  static Uint8List encodeCrcResponse(int crc) {
    final f = Uint8List(crcResponseSize);
    f[0] = start;
    f.setRange(2, 6, uint32ToBytes(crc));
    f[crcResponseSize - 1] = end;
    _writeControl(f, cmdCrcResponse);
    return f;
  }

  /// Left-aligns [payload] in a `payloadSize`-byte buffer, truncating any excess and
  /// zero-padding the tail.
  static Uint8List padPayload(List<int> payload) {
    final out = Uint8List(payloadSize);
    final n = payload.length > payloadSize ? payloadSize : payload.length;
    out.setRange(0, n, payload);
    return out;
  }

  static int offset(List<int> frame) => uint32FromBytes(frame, 2);

  /// The app-space size carried in an identify response's offset slot.
  static int appSize(List<int> frame) => uint32FromBytes(frame, 2);

  /// The flash page size carried in an identify response - the erase unit.
  static int pageSize(List<int> frame) =>
      uint32FromBytes(frame, identifyPageAt);

  static List<int> payload(List<int> frame) =>
      frame.sublist(headerSize, headerSize + payloadSize);

  /// The CRC32 carried by a CRC response.
  static int crc(List<int> frame) => uint32FromBytes(frame, 2);

  /// Whether a page size is one a v2 bootloader could have reported: a power of two that is at
  /// least one chunk and no larger than a flash sector. A pre-v2 identify response has the name
  /// where the page size belongs, so its four bytes read as text and fail this.
  static bool pageSizePlausible(int pageSize) =>
      pageSize >= payloadSize &&
      pageSize <= 65536 &&
      (pageSize & (pageSize - 1)) == 0;

  /// The padded length of an image: the whole number of 32-byte chunks the image occupies.
  static int paddedLength(int length) =>
      ((length + payloadSize - 1) ~/ payloadSize) * payloadSize;

  /// One step of CRC-32/ISO-HDLC (reflected poly 0xEDB88320).
  static int _crcStep(int crc, int byte) {
    var c = crc ^ (byte & 0xFF);
    for (var bit = 0; bit < 8; bit++) {
      c = (c >> 1) ^ ((c & 1) != 0 ? 0xEDB88320 : 0);
    }
    return c;
  }

  /// One byte of CRC-32/ISO-HDLC from a RAW state - no init, no final XOR - so a caller can walk
  /// two digests byte by byte ([firmware_package.dart] localises a single differing byte with it).
  static int crc32Step(int crc, int byte) => _crcStep(crc, byte);

  /// CRC32 over [data].
  static int crc32(List<int> data) {
    var crc = 0xFFFFFFFF;
    for (final b in data) {
      crc = _crcStep(crc, b);
    }
    return (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF;
  }

  /// CRC32 over [image] padded with 0xFF to [paddedLength] - the manifest's `crc32_padded`, the
  /// digest of the image's own chunks.
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

  /// CRC32 over [image] padded with 0xFF to [space] bytes - the value a device holding exactly
  /// this image answers its whole-space CRC with, once the tail past the image is blank.
  static int crc32Space(List<int> image, int space) {
    var crc = 0xFFFFFFFF;
    for (var i = 0; i < space; i++) {
      crc = _crcStep(crc, i < image.length ? image[i] : 0xFF);
    }
    return (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF;
  }

  /// The device name carried in an identify response, with the trailing null padding trimmed.
  static String name(List<int> frame) {
    final bytes = frame.sublist(identifyNameAt, identifyNameAt + nameSize);
    var n = bytes.length;
    while (n > 0 && bytes[n - 1] == 0) {
      n--;
    }
    return String.fromCharCodes(bytes.sublist(0, n));
  }

  /// The frame length for a command, or 0 when the command is unassigned.
  static int frameSize(int cmd) => switch (cmd) {
    cmdIdentifyRequest || cmdReadRequest || cmdEraseRequest => readRequestSize,
    cmdWriteResponse || cmdEraseResponse => readRequestSize,
    cmdIdentifyResponse || cmdReadResponse || cmdWrite => dataSize,
    cmdCrcRequest => crcRequestSize,
    cmdCrcResponse => crcResponseSize,
    _ => 0,
  };

  /// Validates a complete frame; returns the command, or [invalid] when the frame is bad.
  static int decode(List<int> frame) {
    if (frame.length < crcRequestSize || frame[0] != start) return invalid;
    if ((frame[1] & 0xE0) != 0) {
      return invalid; // padding (bits 5-7) must be zero
    }
    final cmd = frame[1] & 0x0F;
    final expect = frameSize(cmd);
    if (expect == 0 ||
        frame.length != expect ||
        frame[frame.length - 1] != end) {
      return invalid;
    }
    final got = (frame[1] >> 4) & 1;
    return got == parity(frame) ? cmd : invalid;
  }

  /// The pre-v2 command field's identify-response value (three-bit command, parity at bit 3).
  ///
  /// Only used to say WHAT an undecodable identify reply is: the deprecated encoding's frame is
  /// the same length, so a "not a v2 identify" reply is checked against these rules to report it
  /// as an old bootloader instead of as silence.
  static const int v1IdentifyResponse = 0x1;

  /// Decodes a frame under the deprecated three-bit command rules, or returns [invalid].
  static int decodeV1(List<int> frame) {
    if (frame.length < readRequestSize || frame[0] != start) return invalid;
    if ((frame[1] & 0xF0) != 0) {
      return invalid; // 3 padding bits + the bit-3 parity
    }
    final cmd = frame[1] & 0x07;
    final expect = switch (cmd) {
      0x0 || 0x2 || 0x4 => readRequestSize,
      0x1 || 0x3 => dataSize,
      _ => 0,
    };
    if (expect == 0 ||
        frame.length != expect ||
        frame[frame.length - 1] != end) {
      return invalid;
    }
    var p = 0;
    for (var bit = 0; bit < 3; bit++) {
      p ^= (cmd >> bit) & 1;
    }
    for (var i = 2; i + 1 < frame.length; i++) {
      var b = frame[i];
      b ^= b >> 4;
      b ^= b >> 2;
      b ^= b >> 1;
      p ^= b & 1;
    }
    return ((frame[1] >> 3) & 1) == (p & 1) ? cmd : invalid;
  }
}
