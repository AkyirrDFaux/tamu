/// Shared data types (Docs/Data Formats.md) and the common block model used by
/// the memory services (Docs/Services/System Memory.md).
library;

import 'dart:typed_data';

// ---------------------------------------------------------------------------
// Basic types
// ---------------------------------------------------------------------------

/// Serial number: 14-byte UUID, displayed as 28 hex characters.
String serialNumberToHex(List<int> sn) =>
    sn.map((b) => b.toRadixString(16).padLeft(2, '0').toUpperCase()).join();

/// Device ID: 16 bit (6 bit net + 10 bit device) per Data Formats.md and Packet.h.
int idNet(int id) => (id >> 10) & 0x3F;
int idDevice(int id) => id & 0x3FF;
String idToString(int id) => '${idNet(id)}.${idDevice(id)}';

/// Sign-extends a 32 bit little-endian value (Dart ints are 64 bit, so the
/// sign bit must be expanded manually).
int _signExtend32(int raw) => (raw & 0x80000000) != 0 ? raw - 0x100000000 : raw;

/// Number: 16.16 SIGNED fixed point.
double numberFromBytes(List<int> bytes, [int offset = 0]) {
  final raw = bytes[offset] | (bytes[offset + 1] << 8) |
          (bytes[offset + 2] << 16) | (bytes[offset + 3] << 24);
  return _signExtend32(raw) / 65536.0;
}

Uint8List numberToBytes(double value) {
  final raw = (value * 65536.0).round() & 0xFFFFFFFF;
  return Uint8List(4)..[0] = raw & 0xFF..[1] = (raw >> 8) & 0xFF
    ..[2] = (raw >> 16) & 0xFF..[3] = (raw >> 24) & 0xFF;
}

int int32FromBytes(List<int> bytes, [int offset = 0]) =>
    _signExtend32(bytes[offset] | (bytes[offset + 1] << 8) |
        (bytes[offset + 2] << 16) | (bytes[offset + 3] << 24));

int uint32FromBytes(List<int> bytes, [int offset = 0]) =>
    bytes[offset] | (bytes[offset + 1] << 8) |
    (bytes[offset + 2] << 16) | (bytes[offset + 3] << 24);

Uint8List uint32ToBytes(int value) {
  final v = value & 0xFFFFFFFF;
  return Uint8List(4)
    ..[0] = v & 0xFF
    ..[1] = (v >> 8) & 0xFF
    ..[2] = (v >> 16) & 0xFF
    ..[3] = (v >> 24) & 0xFF;
}

// ---------------------------------------------------------------------------
// BlockInfo (mirror of firmware Core/Services/Register.h MakeBlockInfo)
// ---------------------------------------------------------------------------

/// Packs a BlockInfo into its 32-bit register index: type (10 bit) | instance
/// (6 bit) | field (8 bit) | key (8 bit).
int makeBlockInfo(int type, int inst, int field, int key) =>
    ((type & 0x3FF) << 22) | ((inst & 0x3F) << 16) | ((field & 0xFF) << 8) | (key & 0xFF);

/// The BlockInfo as 4 little-endian bytes (wire payload prefix).
Uint8List blockInfoBytes(int type, int inst, int field, int key) {
  final bi = makeBlockInfo(type, inst, field, key);
  return Uint8List(4)
    ..[0] = bi & 0xFF
    ..[1] = (bi >> 8) & 0xFF
    ..[2] = (bi >> 16) & 0xFF
    ..[3] = (bi >> 24) & 0xFF;
}

/// Decodes a little-endian byte string of any length into an int (null when empty).
int? bytesToInt(List<int> bytes) {
  if (bytes.isEmpty) return null;
  int value = 0;
  for (var i = 0; i < bytes.length; i++) {
    value |= bytes[i] << (8 * i);
  }
  return value;
}

/// Encodes a value as `size` little-endian bytes.
List<int> intToBytes(int value, int size) {
  final bytes = <int>[];
  for (var i = 0; i < size; i++) {
    bytes.add((value >> (8 * i)) & 0xFF);
  }
  return bytes;
}

// ---------------------------------------------------------------------------
// Enums (mirror Core/Types/Enums.h)
// ---------------------------------------------------------------------------

enum DeviceType {
  unknown(0x00),
  tamuV20A(0x01),
  dualAnalogSensor(0x03);

  final int value;
  const DeviceType(this.value);

  static DeviceType fromValue(int value) {
    for (final type in DeviceType.values) {
      if (type.value == value) return type;
    }
    return DeviceType.unknown;
  }

  String get label => switch (this) {
        DeviceType.unknown => 'Unknown',
        DeviceType.tamuV20A => 'Tamu v2.0A',
        DeviceType.dualAnalogSensor => 'DAS v0.1',
      };
}

enum DataType {
  none(0x00),
  undefined(0x01),
  sn(0x02),
  id(0x03),
  bool_(0x04),
  integer(0x05), // Index 32-bit signed (firmware DataType::Index)
  number(0x06),
  vector(0x07),
  matrix(0x08),
  colour(0x09),
  string(0x0A),
  filename(0x0B),
  enum_(0x0C),
  deleted(0x0D),
  idx(0x11), // alias for uint32 (legacy Index type at 0x0E)
  uint32(0x0E), // Unsigned 32-bit (firmware DataType::Uint32)
  devType(0x0F), // alias to enum (firmware DataType::DevType)
  netAddr(0x03), // alias to id
  geometry(0x101), // dictionary marker (firmware DataType::Geometry = 0x101)
  texture(0x102); // dictionary marker (firmware DataType::Texture = 0x102)

  final int value;
  const DataType(this.value);

  static DataType fromValue(int value) {
    for (final type in DataType.values) {
      if (type.value == value) return type;
    }
    return DataType.none;
  }
}

enum BlockType {
  none(0x00), // tombstone: no block here; stable until save compacts
  undefined(0x01), // valid block, type not yet specified
  system(0x00), // System block (type 0, inst 0 in Register service)
  ledButton(0x03),
  pwm(0x04),
  accGyr(0x05),
  vysiDisplay(0x06),
  deleted(0x07),
  resistiveMeasure(0x08),
  button(0x09),
  led(0x0A),
  script(0x3FE), // loaded-script blocks (Script service, exposed via the Register)
  dynamic(0x3FF), // Dynamic memory block type
  render(0x100);

  final int value;
  const BlockType(this.value);

  static BlockType fromValue(int value) {
    for (final type in BlockType.values) {
      if (type.value == value) return type;
    }
    return BlockType.undefined;
  }

  String get label => switch (this) {
        BlockType.none => 'None',
        BlockType.undefined => 'Undefined',
        BlockType.system => 'System',
        BlockType.ledButton => 'LED/Button',
        BlockType.pwm => 'PWM',
        BlockType.accGyr => 'Acc/Gyr',
        BlockType.vysiDisplay => 'LED Display',
        BlockType.deleted => 'Deleted',
        BlockType.resistiveMeasure => 'Resistive Measure',
        BlockType.button => 'Button',
        BlockType.led => 'LED',
        BlockType.script => 'Script',
        BlockType.dynamic => 'Dynamic',
        BlockType.render => 'Render',
      };
}

/// Field/block flag bits (bits 10-15 of BlockMeta.FlagsAndType per Register.md).
class FieldFlags {
  static const mask = 0xFC00; // flags occupy bits 10-15 of FlagsAndType
  static const readOnly = 0x0400;
  static const persistent = 0x0800;
  static const trigger = 0x1000;
  static const notSaved = 0x2000;
  static const scriptUpdated = 0x4000;
  static const external = 0x8000;
  static const valid = 0x0400; // alias to readOnly for INVAL check legacy (flash valid)

  static List<String> describe(int flags) {
    final names = <String>[];
    if (flags & readOnly != 0) names.add('RO');
    if (flags & persistent != 0) names.add('P');
    if (flags & trigger != 0) names.add('TR');
    if (flags & notSaved != 0) names.add('NS');
    if (flags & scriptUpdated != 0) names.add('SU');
    if (flags & external != 0) names.add('EXT');
    // Note: 'valid' (FlashValid) shares bit with readOnly (0x0400) in firmware.
    // We cannot distinguish "flash invalid" from "writable" so we don't display INVAL.
    return names;
  }
}

/// Capability bitfield (matches firmware Enums.h).
class Capability {
  static const core = 1 << 0;
  static const router = 1 << 1;
  static const cli = 1 << 2;
  static const dynamicMemory = 1 << 3;
  static const scripts = 1 << 4;
  static const storageFiles = 1 << 5;
  static const appInterface = 1 << 6;
  static const subscriptions = 1 << 7;
  static const node = 1 << 8;

  static List<String> describe(int caps) {
    final names = <String>[];
    if (caps & core != 0) names.add('Core');
    if (caps & router != 0) names.add('Router');
    if (caps & cli != 0) names.add('CLI');
    if (caps & dynamicMemory != 0) names.add('DynMem');
    if (caps & scripts != 0) names.add('Scripts');
    if (caps & storageFiles != 0) names.add('Files');
    if (caps & appInterface != 0) names.add('App');
    if (caps & subscriptions != 0) names.add('Subs');
    if (caps & node != 0) names.add('Node');
    return names;
  }
}

// ---------------------------------------------------------------------------
// Common structs
// ---------------------------------------------------------------------------

/// BlockMeta (6bit flags, 10bit type, 8bit key/padding, 8bit value length).
class BlockMeta {
  final int flagsAndType; // bits 10-15 flags, bits 0-9 type
  final int key;
  final int size;

  const BlockMeta({required this.flagsAndType, this.key = 0, this.size = 0});

  int get flags => flagsAndType & 0xFC00;
  int get typeValue => flagsAndType & 0x03FF;
  DataType get dataType => DataType.fromValue(typeValue);
  BlockType get blockType => BlockType.fromValue(typeValue);

  bool get readOnly => flags & FieldFlags.readOnly != 0;
  bool get notSaved => flags & FieldFlags.notSaved != 0;
  bool get persistent => flags & FieldFlags.persistent != 0;

  Uint8List toBytes() => Uint8List(4)
    ..[0] = flagsAndType & 0xFF
    ..[1] = (flagsAndType >> 8) & 0xFF
    ..[2] = key
    ..[3] = size;

  static BlockMeta fromBytes(List<int> bytes, [int offset = 0]) => BlockMeta(
        flagsAndType: bytes[offset] | (bytes[offset + 1] << 8),
        key: bytes[offset + 2],
        size: bytes[offset + 3],
      );
}

// ---------------------------------------------------------------------------
// Subscriptions (Docs/Services/Subscriptions.md)
// ---------------------------------------------------------------------------

enum TriggerType {
  periodic(0),
  onChangePeriodic(1),
  onChangeConfirm(2),
  edgeRising(3),
  edgeFalling(4),
  edgeAny(5),
  deltaPeriodic(6);

  final int value;
  const TriggerType(this.value);

  static TriggerType fromValue(int value) {
    for (final t in TriggerType.values) {
      if (t.value == value) return t;
    }
    return TriggerType.periodic;
  }

  String get label => switch (this) {
    TriggerType.periodic => 'Periodic',
    TriggerType.onChangePeriodic => 'On change + period',
    TriggerType.onChangeConfirm => 'On change + confirm',
    TriggerType.edgeRising => 'Edge rising',
    TriggerType.edgeFalling => 'Edge falling',
    TriggerType.edgeAny => 'Edge (any)',
    TriggerType.deltaPeriodic => 'Delta + period',
  };

  /// Edge triggers compare boolean values; the others compare hashes/values.
  bool get isEdge =>
      this == TriggerType.edgeRising ||
      this == TriggerType.edgeFalling ||
      this == TriggerType.edgeAny;
}

/// Provider-side subscription entry (what the device stores for incoming subscriptions)
class ProviderSubscription {
  final int index;
  final int requesterAddr;
  final int trid;
  final int sourceReg; // BlockInfo
  final TriggerType trigger;
  final int periodMs;
  final int minTimeMs;
  final int lastSentMs;
  final int hash;
  final double deadzone;

  const ProviderSubscription({
    required this.index,
    required this.requesterAddr,
    required this.trid,
    required this.sourceReg,
    required this.trigger,
    required this.periodMs,
    required this.minTimeMs,
    required this.lastSentMs,
    required this.hash,
    required this.deadzone,
  });

  static ProviderSubscription fromBytes(int index, List<int> bytes) {
    int offset = 0;
    final requesterAddr = bytes[offset] | (bytes[offset + 1] << 8); offset += 2;
    final trid = bytes[offset] | (bytes[offset + 1] << 8); offset += 2;
    final sourceReg = uint32FromBytes(bytes, offset); offset += 4;
    final trigger = TriggerType.fromValue(bytes[offset++]);
    offset += 3; // 24-bit padding
    final periodMs = uint32FromBytes(bytes, offset); offset += 4;
    final minTimeMs = uint32FromBytes(bytes, offset); offset += 4;
    final lastSentMs = uint32FromBytes(bytes, offset); offset += 4;
    final hash = uint32FromBytes(bytes, offset); offset += 4;
    final deadzone = numberFromBytes(bytes, offset);
    return ProviderSubscription(
      index: index,
      requesterAddr: requesterAddr,
      trid: trid,
      sourceReg: sourceReg,
      trigger: trigger,
      periodMs: periodMs,
      minTimeMs: minTimeMs,
      lastSentMs: lastSentMs,
      hash: hash,
      deadzone: deadzone,
    );
  }
}

/// Requester-side subscription entry (outgoing subscriptions from Tamu)
class RequesterSubscription {
  final int index;
  final int providerAddr;
  final int trid;
  final int targetReg; // BlockInfo
  final int sourceReg; // BlockInfo
  final TriggerType trigger;
  final int periodMs;
  final int minTimeMs;
  final double deadzone;

  const RequesterSubscription({
    required this.index,
    required this.providerAddr,
    required this.trid,
    required this.targetReg,
    required this.sourceReg,
    required this.trigger,
    required this.periodMs,
    required this.minTimeMs,
    this.deadzone = 0,
  });

  static RequesterSubscription fromBytes(int index, List<int> bytes) {
    int offset = 0;
    final providerAddr = bytes[offset] | (bytes[offset + 1] << 8); offset += 2;
    final trid = bytes[offset] | (bytes[offset + 1] << 8); offset += 2;
    final targetReg = uint32FromBytes(bytes, offset); offset += 4;
    final sourceReg = uint32FromBytes(bytes, offset); offset += 4;
    final trigger = TriggerType.fromValue(bytes[offset++]);
    offset += 3; // 24-bit padding
    final periodMs = uint32FromBytes(bytes, offset); offset += 4;
    final minTimeMs = uint32FromBytes(bytes, offset); offset += 4;
    final deadzone = numberFromBytes(bytes, offset);
    return RequesterSubscription(
      index: index,
      providerAddr: providerAddr,
      trid: trid,
      targetReg: targetReg,
      sourceReg: sourceReg,
      trigger: trigger,
      periodMs: periodMs,
      minTimeMs: minTimeMs,
      deadzone: deadzone,
    );
  }

  /// BlockInfo components of the target (local) register.
  int get blockType => (targetReg >> 22) & 0x3FF;
  int get blockInst => (targetReg >> 16) & 0x3F;
  int get blockField => (targetReg >> 8) & 0xFF;
  int get blockKey => targetReg & 0xFF;

  /// BlockInfo components of the source (provider) register.
  int get blockTypeS => (sourceReg >> 22) & 0x3FF;
  int get blockInstS => (sourceReg >> 16) & 0x3F;
  int get blockFieldS => (sourceReg >> 8) & 0xFF;
  int get blockKeyS => sourceReg & 0xFF;

  /// Wire layout for the CID 4 set request (providerAddr, trid, targetReg, sourceReg,
  /// trigger+24 pad, period, min). The TRID is also echoed in the packet header.
  List<int> toCreatePayload() {
    final buf = <int>[];
    buf.addAll([providerAddr & 0xFF, (providerAddr >> 8) & 0xFF]);
    buf.addAll([trid & 0xFF, (trid >> 8) & 0xFF]);
    buf.addAll(uint32ToBytes(targetReg));
    buf.addAll(uint32ToBytes(sourceReg));
    buf.add(trigger.value);
    buf.addAll([0, 0, 0]); // 24-bit padding
    buf.addAll(uint32ToBytes(periodMs));
    buf.addAll(uint32ToBytes(minTimeMs));
    buf.addAll(numberToBytes(deadzone));
    return buf;
  }
}
