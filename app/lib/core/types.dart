/// Shared data types (Docs/Data Formats.md) and the common block model used by
/// the memory services (Docs/Services/Register.md).
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

/// Parses a "net.device" address (both fields hex) into a 16-bit ID, or null.
int? idFromString(String text) {
  final parts = text.split('.');
  if (parts.length != 2) return null;
  final net = int.tryParse(parts[0], radix: 16);
  final dev = int.tryParse(parts[1], radix: 16);
  if (net == null || dev == null) return null;
  return ((net & 0x3F) << 10) | (dev & 0x3FF);
}

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

int blockInfoType(int blockInfo) => (blockInfo >> 22) & 0x3FF;
int blockInfoInstance(int blockInfo) => (blockInfo >> 16) & 0x3F;
int blockInfoField(int blockInfo) => (blockInfo >> 8) & 0xFF;
int blockInfoKey(int blockInfo) => blockInfo & 0xFF;

/// The BlockInfo as 4 little-endian bytes (wire payload prefix).
Uint8List blockInfoBytes(int type, int inst, int field, int key) =>
    uint32ToBytes(makeBlockInfo(type, inst, field, key));

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
  uint32(0x0E), // Unsigned 32-bit (firmware DataType::Uint32)
  devType(0x0F), // alias to enum (firmware DataType::DevType)
  blockInfo(0x10), // register pointer type|inst|field|key (firmware DataType::BlockInfo)
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

/// Canonical, human-readable name of a data type (Docs/App/Backup.md: the storage
/// format describes types "in words"). Used by the semantic backup format.
String dataTypeWord(DataType type) => switch (type) {
      DataType.none => 'None',
      DataType.undefined => 'Undefined',
      DataType.sn => 'Serial number',
      DataType.id || DataType.netAddr => 'ID',
      DataType.bool_ => 'Bool',
      DataType.integer => 'Index',
      DataType.number => 'Number',
      DataType.vector => 'Vector',
      DataType.matrix => 'Matrix',
      DataType.colour => 'Colour',
      DataType.string => 'String',
      DataType.filename => 'Filename',
      DataType.enum_ => 'Enum',
      DataType.deleted => 'Deleted',
      DataType.uint32 => 'Uint32',
      DataType.devType => 'Device type',
      DataType.blockInfo => 'BlockInfo',
      DataType.geometry => 'Geometry dict',
      DataType.texture => 'Texture dict',
    };

/// Parses a [dataTypeWord] back to its data type (null when unknown).
DataType? dataTypeFromWord(String word) {
  const words = <String, DataType>{
    'None': DataType.none,
    'Undefined': DataType.undefined,
    'Serial number': DataType.sn,
    'ID': DataType.id,
    'Bool': DataType.bool_,
    'Index': DataType.integer,
    'Number': DataType.number,
    'Vector': DataType.vector,
    'Matrix': DataType.matrix,
    'Colour': DataType.colour,
    'String': DataType.string,
    'Filename': DataType.filename,
    'Enum': DataType.enum_,
    'Deleted': DataType.deleted,
    'Uint32': DataType.uint32,
    'Device type': DataType.devType,
    'BlockInfo': DataType.blockInfo,
    'Geometry dict': DataType.geometry,
    'Texture dict': DataType.texture,
  };
  return words[word];
}

/// The System block is type 0 instance 0 in the Register service; it shares the numeric
/// value with the dynamic "None" tombstone, so it is not a [BlockType] member (that would
/// duplicate the enum value). Use this constant for system-block comparisons.
const int systemBlockTypeValue = 0x00;

/// Block type label for a raw type value, with the System block and the banked dynamic types
/// resolved explicitly ([BlockType.fromValue] maps 0 to the "None" tombstone and does not know
/// the 0x3F0-0x3F3 banks).
String blockTypeLabel(int type) {
  if (type == systemBlockTypeValue) return 'System';
  if (isDynamicType(type)) return 'Dynamic';
  if (isScriptType(type)) return 'Script';
  return BlockType.fromValue(type).label;
}

/// Whether a Register block slot should be hidden as a dynamic tombstone. Dynamic tombstone
/// slots carry no block (their meta type is the "None" tombstone value), but the System block
/// reports meta type 0x00 too - the same numeric value - so the slot type must be checked as
/// well. `type` is the slot's block type (System = 0x00, dynamic = the 0x3F0-0x3F3 banks).
bool isHiddenRegisterSlot(int type, ValueInfo meta) =>
    type != systemBlockTypeValue && meta.type == BlockType.none.value;

enum BlockType {
  none(0x00), // tombstone: no block here; stable until save compacts
  undefined(0x01), // valid block, type not yet specified
  ledButton(0x03),
  pwm(0x04),
  accGyr(0x05),
  vysiDisplay(0x06),
  deleted(0x07),
  resistiveMeasure(0x08),
  button(0x09),
  led(0x0A),
  script(0x3FE), // app marker for the Scripts service (wire range 0x3F4-0x3F7)
  dynamic(0x3FF), // app marker for the Dynamic service (wire range 0x3F0-0x3F3)
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

// ---------------------------------------------------------------------------
// Banked dynamic memory (Docs/Services/Register.md "Block types")
// ---------------------------------------------------------------------------

/// The dynamic memory is four banked block types (0x3F0-0x3F3) of 64 instances each, addressed
/// by one **global** index `0..255`: `bank = index >> 6`, `instance = index & 63`. The storage
/// files carry that global index in hex (`.DT_XX` / `.DV_XX`).
const int dynamicTypeBase = 0x3F0;
const int dynamicTypeCount = 4;
const int dynamicInstancesPerType = 64;

bool isDynamicType(int type) =>
    type >= dynamicTypeBase && type < dynamicTypeBase + dynamicTypeCount;

/// The banked block type that owns global index `global`.
int dynamicTypeForIndex(int global) => dynamicTypeBase + (global >> 6);

/// The BlockInstance (0..63) of global index `global`.
int dynamicInstanceForIndex(int global) => global & 0x3F;

// ---------------------------------------------------------------------------
// Banked script memory (Docs/Services/Register.md "Block types")
// ---------------------------------------------------------------------------

/// Loaded scripts are four banked block types (0x3F4-0x3F7) of 64 instances each, addressed by
/// one **global** index `0..255` (the Script service owns the file↔slot mapping).
const int scriptTypeBase = 0x3F4;
const int scriptTypeCount = 4;

bool isScriptType(int type) =>
    type >= scriptTypeBase && type < scriptTypeBase + scriptTypeCount;

/// The banked block type that owns global script index `global`.
int scriptTypeForIndex(int global) => scriptTypeBase + (global >> 6);

/// The BlockInstance (0..63) of global script index `global`.
int scriptInstanceForIndex(int global) => global & 0x3F;

/// True for the block types that live in the firmware's `static_block_registry[]` - the device's
/// static memory, persisted 1:1 in `.SV` (Docs/Services/Register.md "System + Static memory
/// blocks"). The types are stacked in ascending block-type order with their instances contiguous,
/// which is the order this registry must keep.
///
/// The virtual System block (type 0) and the Script (0x3F4-0x3F7) / Dynamic (0x3F0-0x3F3)
/// memories are not static blocks. `RegisterClient.readBlocks()` appends those two *after* the
/// statics, so an unfiltered list happens to match the static layout order - but only by that
/// ordering. Filter with this helper wherever the static registry is built so the mapping cannot
/// silently drift.
bool isStaticRegistryType(int type) =>
    type != systemBlockTypeValue && !isScriptType(type) && !isDynamicType(type);

/// The ValueInfo flag bits (Docs/Services/Register.md "ValueInfo Flags", in that table's order).
/// Every flag is **passive**: a read reports them verbatim and a write carries the same
/// specification back.
class ValueFlags {
  static const readOnly = 0x01;
  static const persistent = 0x02;
  static const trigger = 0x04;

  static List<String> describe(int flags) {
    final names = <String>[];
    if (flags & readOnly != 0) names.add('RO');
    if (flags & persistent != 0) names.add('P');
    if (flags & trigger != 0) names.add('TR');
    return names;
  }
}

/// Capability bitfield (matches firmware Enums.h).
class Capability {
  static const core = 1 << 0;
  static const router = 1 << 1;
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

/// The descriptor (Docs/Services/Register.md "Map entry"): `Type(16) | Size(8) | Flags(8)`.
/// One type, on the wire and internally. The key is not part of it - it travels in the
/// request's/reply's BlockInfo, or in a table entry's Field&Key.
class ValueInfo {
  final int type;   // DataType (10 bits)
  final int size;   // value length in bytes
  final int flags;  // ValueFlags bits
  final int key;    // carried separately, not on the wire

  const ValueInfo({required this.type, this.size = 0, this.flags = 0, this.key = 0});

  DataType get dataType => DataType.fromValue(type);
  BlockType get blockType => BlockType.fromValue(type);

  bool get readOnly => flags & ValueFlags.readOnly != 0;
  bool get persistent => flags & ValueFlags.persistent != 0;

  Uint8List toBytes() => Uint8List(4)
    ..[0] = type & 0xFF
    ..[1] = (type >> 8) & 0xFF
    ..[2] = size
    ..[3] = flags;

  static ValueInfo fromBytes(List<int> bytes, [int offset = 0, int key = 0]) => ValueInfo(
        type: bytes[offset] | (bytes[offset + 1] << 8),
        size: bytes[offset + 2],
        flags: bytes[offset + 3],
        key: key,
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

  static TriggerType? fromWord(String word) {
    for (final t in TriggerType.values) {
      if (t.label == word || t.name == word) return t;
    }
    return null;
  }
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
