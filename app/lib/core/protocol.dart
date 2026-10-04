/// Generic packet protocol (Docs/Data Formats.md).
///
/// Wire layout (Docs/RSBus and Packets.md): CRC8 | Flags | Reserved(4)+Priority(4) |
/// PayloadLen (bytes) | SRC ID | TGT ID | CMD | TRID | Payload. The payload is exactly
/// PayloadLen bytes (max 116); the total is at most 128.
library;

import 'dart:typed_data';

const int maxPayloadSize = 116;

// Flag bitmasks.
const int flagReqAck = 1 << 0;
const int flagStart = 1 << 1;
const int flagStop = 1 << 2;
const int flagType = 1 << 3; // 0 = request, 1 = response
const int flagFrag = 1 << 4; // first 4 payload bytes = fragmentation info (u16 current + u16 total)
const int flagSuccess = 1 << 5; // response: success, no extra information
const int flagFail = 1 << 6; // response: error, no extra information

/// Default priority byte (docs: Reserved(4) | Priority(4), 0 = highest, default 8).
const int defaultPriority = 8;

/// Reserved TRID ranges (Docs/RSBus and Packets.md "Transaction IDs"). The App owns
/// 0xF000-0xFFFF: every app request draws its transaction ID from that range and a reply
/// echoes the same TRID, so the app routes on the full 16-bit value. (The System/Logs and
/// Script ranges are firmware-owned and unknown to the app, so they are not declared here.)
const int tridSubBase = 0x1000;
const int tridSubMax = 0x1FFF;
const int tridAppBase = 0xF000;
const int tridAppMax = 0xFFFF;

/// Legacy placeholder address. The firmware rewrites id_src on app frames (the core
/// proxies the app), so this value is inert - kept only as a safe default. It is the
/// app's own source only; it must never be used as a subscription address.
const int appSourceId = 0xFFFE;

/// Service types (matches the firmware enum in Core/Functions/Packet.h).
/// `router` (0x10) has no firmware service (Docs: "not to be implemented yet"); it is
/// kept because a packet test exercises the parser with it.
enum ServiceType {
  device(0x00),
  register(0x01),
  logHandler(0x02),
  storage(0x03),
  subscriptions(0x04),
  script(0x05),
  router(0x10);

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

  int get cmd => srvTarget;
  int get trid => srvSource;

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
  bool get isSuccess => (flags & flagSuccess) != 0;
  bool get isFail => (flags & flagFail) != 0;

  /// Serialises the frame including the CRC8 header byte. PayloadLen is the exact payload
  /// byte count (no padding).
  Uint8List toBytes() {
    assert(payload.length <= maxPayloadSize,
        'payload ${payload.length} exceeds max $maxPayloadSize');
    final bytes = ByteData(12 + payload.length);
    bytes.setUint8(0, 0); // CRC placeholder, patched below
    bytes.setUint8(1, flags);
    bytes.setUint8(2, priority);
    bytes.setUint8(3, payload.length); // Payload Length in bytes
    bytes.setUint16(4, idSource, Endian.little); // SRC ID
    bytes.setUint16(6, idTarget, Endian.little); // TGT ID
    bytes.setUint16(8, srvTarget, Endian.little); // CMD
    bytes.setUint16(10, srvSource, Endian.little); // TRID
    final out = bytes.buffer.asUint8List();
    out.setAll(12, payload);
    bytes.setUint8(0, crc8(out.sublist(1)));
    return out;
  }

  /// Parses a frame from `data` starting at `offset`. Returns null if there is
  /// not enough data yet. Throws FormatException on a CRC mismatch.
  static PacketFrame? tryParse(List<int> data, int offset) {
    if (data.length - offset < 12) return null;
    final len = data[offset + 3]; // Payload Length in bytes
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
      idSource: data[offset + 4] | (data[offset + 5] << 8),
      idTarget: data[offset + 6] | (data[offset + 7] << 8),
      srvTarget: data[offset + 8] | (data[offset + 9] << 8),
      srvSource: data[offset + 10] | (data[offset + 11] << 8),
      payload: payload,
    );
  }

  /// Builds a single-packet frame (START|STOP set, default priority). Requests carry
  /// REQACK: several services (System/Dynamic Memory) respond only when it is set.
  /// Set [requestFrag] if THIS REQUEST PACKET is a fragment (carries 4-byte frag info).
  factory PacketFrame.single({
    required int targetId,
    required int srvTarget,
    required int srvSource,
    required bool response,
    List<int> payload = const [],
    bool requestFrag = false,
  }) {
    return PacketFrame(
      flags: flagStart |
          flagStop |
          flagReqAck |
          (response ? flagType : 0) |
          (requestFrag ? flagFrag : 0),
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
      final len = _buffer[offset + 3]; // Payload Length in bytes
      if (len > maxPayloadSize) {
        // Corrupt length (would stall waiting for bytes that will never arrive) — resync.
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

/// Register service command IDs (Docs/Services/Register.md "Basic commands").
class RegisterCid {
  static const enumerateBlocks = 0;
  static const enumerateFields = 1;
  static const read = 2;
  static const write = 3;
  static const recallAll = 4;
  static const saveAll = 5;
}

/// Dynamic/keyed management command IDs (Docs/Services/Register.md "Dynamic commands").
class DynamicCid {
  static const create = 0x10;
  static const delete = 0x11;
  static const getName = 0x12;
  static const setName = 0x13;
}
