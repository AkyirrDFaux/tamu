/// Generic packet protocol (Docs/Data Formats.md).
///
/// Wire layout: CRC8 | Flags | FragID | PayloadLen | ID TGT | ID SRC | SRV TGT | SRV SRC | Payload
library;

import 'dart:typed_data';

const int maxPayloadSize = 255;

// Flag bitmasks.
const int flagReqAck = 1 << 0;
const int flagStart = 1 << 1;
const int flagStop = 1 << 2;
const int flagType = 1 << 3; // 0 = request, 1 = response

// Address constants.
const int addrInvalid = 0x0000;
const int addrBroadcast = 0xFFFF;

/// Source ID used by the app. The app is not an assigned network device, so it
/// uses a fixed ID outside the normal assignment range (net 15).
const int appSourceId = 0xFFFE;

/// Service types (Docs/General architecture.md, matches the firmware enum).
enum ServiceType {
  device(0x01),
  logHandler(0x02),
  storage(0x03),
  systemMemory(0x04),
  dynamicMemory(0x05),
  keyedMemory(0x06),
  script(0x07),
  cli(0x09);

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

/// A single parsed packet frame.
class PacketFrame {
  final int flags;
  final int fragId;
  final int idTarget;
  final int idSource;
  final int srvTarget;
  final int srvSource;
  final Uint8List payload;

  PacketFrame({
    required this.flags,
    required this.fragId,
    required this.idTarget,
    required this.idSource,
    required this.srvTarget,
    required this.srvSource,
    required this.payload,
  });

  bool get isResponse => (flags & flagType) != 0;
  bool get isStart => (flags & flagStart) != 0;
  bool get isStop => (flags & flagStop) != 0;
  bool get isSingle => isStart && isStop;

  /// Serialises the frame including the CRC8 header byte.
  Uint8List toBytes() {
    final bytes = ByteData(12 + payload.length);
    bytes.setUint8(0, 0); // CRC placeholder, patched below
    bytes.setUint8(1, flags);
    bytes.setUint8(2, fragId);
    bytes.setUint8(3, payload.length);
    bytes.setUint16(4, idTarget, Endian.little);
    bytes.setUint16(6, idSource, Endian.little);
    bytes.setUint16(8, srvTarget, Endian.little);
    bytes.setUint16(10, srvSource, Endian.little);
    bytes.buffer.asUint8List().setAll(12, payload);
    bytes.setUint8(0, crc8(bytes.buffer.asUint8List(1)));
    return bytes.buffer.asUint8List();
  }

  /// Parses a frame from `data` starting at `offset`. Returns null if there is
  /// not enough data yet. Throws FormatException on a CRC mismatch.
  static PacketFrame? tryParse(List<int> data, int offset) {
    if (data.length - offset < 12) return null;
    final len = data[offset + 3];
    if (data.length - offset < 12 + len) return null;
    final body = data.sublist(offset + 1, offset + 12 + len);
    if (crc8(body) != data[offset]) {
      throw const FormatException('CRC mismatch');
    }
    final payload =
        len > 0 ? Uint8List.fromList(data.sublist(offset + 12, offset + 12 + len)) : Uint8List(0);
    return PacketFrame(
      flags: data[offset + 1],
      fragId: data[offset + 2],
      idTarget: data[offset + 4] | (data[offset + 5] << 8),
      idSource: data[offset + 6] | (data[offset + 7] << 8),
      srvTarget: data[offset + 8] | (data[offset + 9] << 8),
      srvSource: data[offset + 10] | (data[offset + 11] << 8),
      payload: payload,
    );
  }

  /// Builds a single-packet frame (START|STOP set, FragID 0).
  factory PacketFrame.single({
    required int targetId,
    required int srvTarget,
    required int srvSource,
    required bool response,
    List<int> payload = const [],
  }) {
    return PacketFrame(
      flags: flagStart | flagStop | (response ? flagType : 0),
      fragId: 0,
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
      final len = _buffer[offset + 3];
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
