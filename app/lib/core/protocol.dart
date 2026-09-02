/// Generic packet protocol (Docs/Data Formats.md).
///
/// Wire layout: CRC8 | Flags | Priority | PayloadLen | ID TGT | ID SRC | SRV TGT | SRV SRC | Payload
/// PayloadLen is in 4-byte units (max 69 = 276 bytes); the payload is padded to 4 on the wire.
library;

import 'dart:typed_data';

const int maxPayloadSize = 276;

// Flag bitmasks.
const int flagReqAck = 1 << 0;
const int flagStart = 1 << 1;
const int flagStop = 1 << 2;
const int flagType = 1 << 3; // 0 = request, 1 = response
const int flagFrag = 1 << 4; // first 4 payload bytes = fragmentation info (u16 current + u16 total)

/// Default priority byte (0 = highest, default 128).
const int defaultPriority = 128;

/// Legacy placeholder address. The firmware rewrites id_src on app frames (the core
/// proxies the app), so this value is inert - kept only as a safe default.
const int appSourceId = 0xFFFE;

/// Service types (Docs/Service ID table.md, matches the firmware enum).
enum ServiceType {
  device(0x00),
  logHandler(0x01),
  storage(0x02),
  systemMemory(0x04),
  dynamicMemory(0x05),
  keyedMemory(0x06),
  script(0x08),
  scriptInstructions(0x09),
  router(0x10),
  app(0x11),
  cli(0x12);

  final int value;
  const ServiceType(this.value);

  static ServiceType? fromValue(int value) {
    for (final type in ServiceType.values) {
      if (type.value == value) return type;
    }
    return null;
  }
}

int makeService(ServiceType type, int cid) => (type.value << 8) | cid;
ServiceType? serviceTypeOf(int srv) => ServiceType.fromValue(srv >> 8);
int serviceCidOf(int srv) => srv & 0xFF;

/// CRC8, polynomial 0x07, init 0x00 (matches Core/Functions/Packet.h).
int crc8(List<int> data) {
  var crc = 0;
  for (final byte in data) {
    crc ^= byte;
    for (var i = 0; i < 8; i++) {
      crc = (crc & 0x80) != 0 ? ((crc << 1) ^ 0x07) & 0xFF : (crc << 1) & 0xFF;
    }
  }
  return crc;
}

/// Builds the 4-byte fragmentation info (u16 current fragment + u16 total fragments)
/// that prefixes every FRAG-flagged packet's payload (Data Formats.md).
Uint8List writeFragInfo(int current, int total) =>
    Uint8List.fromList([current & 0xFF, (current >> 8) & 0xFF, total & 0xFF, (total >> 8) & 0xFF]);

/// Parses the fragmentation info from the first 4 payload bytes of a FRAG packet.
({int current, int total}) fragInfoOf(List<int> payload) => (
      current: payload[0] | (payload[1] << 8),
      total: payload[2] | (payload[3] << 8),
    );

/// A single parsed packet frame.
class PacketFrame {
  final int flags;
  final int priority;
  final int idTarget;
  final int idSource;
  final int srvTarget;
  final int srvSource;
  final Uint8List payload;

  PacketFrame({
    required this.flags,
    required this.priority,
    required this.idTarget,
    required this.idSource,
    required this.srvTarget,
    required this.srvSource,
    required this.payload,
  });

  bool get isResponse => (flags & flagType) != 0;
  bool get isStart => (flags & flagStart) != 0;
  bool get isStop => (flags & flagStop) != 0;
  bool get isFrag => (flags & flagFrag) != 0;
  bool get isSingle => isStart && isStop;

  /// Serialises the frame including the CRC8 header byte. The payload is padded to a
  /// multiple of 4 and PayloadLen carries the padded size in 4-byte units.
  Uint8List toBytes() {
    assert(payload.length <= maxPayloadSize,
        'payload ${payload.length} exceeds max $maxPayloadSize');
    final padded = (payload.length + 3) & ~3;
    final bytes = ByteData(12 + padded);
    bytes.setUint8(0, 0); // CRC placeholder, patched below
    bytes.setUint8(1, flags);
    bytes.setUint8(2, priority);
    bytes.setUint8(3, padded ~/ 4);
    bytes.setUint16(4, idTarget, Endian.little);
    bytes.setUint16(6, idSource, Endian.little);
    bytes.setUint16(8, srvTarget, Endian.little);
    bytes.setUint16(10, srvSource, Endian.little);
    final out = bytes.buffer.asUint8List();
    out.setAll(12, payload);
    for (var i = payload.length; i < padded; i++) {
      out[12 + i] = 0;
    }
    bytes.setUint8(0, crc8(out.sublist(1)));
    return out;
  }

  /// Parses a frame from `data` starting at `offset`. Returns null if there is
  /// not enough data yet. Throws FormatException on a CRC mismatch.
  static PacketFrame? tryParse(List<int> data, int offset) {
    if (data.length - offset < 12) return null;
    final units = data[offset + 3];
    final len = units * 4;
    if (data.length - offset < 12 + len) return null;
    final body = data.sublist(offset + 1, offset + 12 + len);
    if (crc8(body) != data[offset]) {
      throw const FormatException('CRC mismatch');
    }
    final payload =
        len > 0 ? Uint8List.fromList(data.sublist(offset + 12, offset + 12 + len)) : Uint8List(0);
    return PacketFrame(
      flags: data[offset + 1],
      priority: data[offset + 2],
      idTarget: data[offset + 4] | (data[offset + 5] << 8),
      idSource: data[offset + 6] | (data[offset + 7] << 8),
      srvTarget: data[offset + 8] | (data[offset + 9] << 8),
      srvSource: data[offset + 10] | (data[offset + 11] << 8),
      payload: payload,
    );
  }

  /// Builds a single-packet frame (START|STOP set, default priority). Requests carry
  /// REQACK: several services (System/Dynamic/Keyed Memory) respond only when it is set.
  /// Set [frag] to flag the payload as carrying 4 bytes of fragmentation info first.
  factory PacketFrame.single({
    required int targetId,
    required int srvTarget,
    required int srvSource,
    required bool response,
    List<int> payload = const [],
    bool frag = false,
  }) {
    return PacketFrame(
      flags: flagStart |
          flagStop |
          flagReqAck |
          (response ? flagType : 0) |
          (frag ? flagFrag : 0),
      priority: defaultPriority,
      idTarget: targetId,
      idSource: appSourceId,
      srvTarget: srvTarget,
      srvSource: srvSource,
      payload: Uint8List.fromList(payload),
    );
  }
}

/// Incremental parser turning a continuous packet byte stream into frames.
class PacketStreamParser {
  final List<int> _buffer = [];

  /// Feeds raw stream bytes and returns all complete frames found.
  /// Bytes before the first valid frame are discarded on CRC errors (resync).
  List<PacketFrame> feed(List<int> chunk) {
    _buffer.addAll(chunk);
    final frames = <PacketFrame>[];
    var offset = 0;
    while (true) {
      final remaining = _buffer.length - offset;
      if (remaining < 12) break;
      final units = _buffer[offset + 3];
      final len = units * 4;
      if (len > maxPayloadSize) {
        // Corrupt length (would stall waiting for ~1kB that will never arrive) — resync.
        offset++;
        continue;
      }
      if (remaining < 12 + len) break;
      final PacketFrame frame;
      try {
        frame = PacketFrame.tryParse(_buffer, offset)!;
      } on FormatException {
        offset++; // resync past the corrupt byte
        continue;
      }
      frames.add(frame);
      offset += 12 + len;
    }
    if (offset > 0) _buffer.removeRange(0, offset);
    return frames;
  }
}