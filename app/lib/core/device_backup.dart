/// Decoders (and the two field writers) for the device's own backup registries
/// (Docs/Services/Register.md "Save").
///
/// The device persists its Save targets as storage files:
///   `.SV`       the static memory's persistent space (firmware `StaticMemory.h` + the board's
///               `StaticPersistent`): a raw 1:1 mirror with no per-record headers
///   `SUBREQ`    the requester-subscription table (firmware `Subscriptions.h`)
///   `.DT_<xx>`   a dynamic block's entry table (firmware `Memory.h`)
///   `.DV_<xx>`   that block's packed persistent values (firmware `Memory.h`)
///
/// `.SV` carries no offsets, so the app recomputes the static persistent layout from the
/// per-type field sizes + the 32-bit alignment rule (see [StaticSpaceLayout]). The decoders
/// serve the storage file viewer and the Register view's Current/Backup toggle from one parse.
/// The writers exist because the wire only carries `Save All` / `Recall All`: "partial
/// saving/recall is handled by app with direct file writes / direct register writes"
/// (Docs/Services/Register.md), so saving one field writes the file here and recalling one is a
/// register write.
library;

import 'dart:typed_data';

import 'types.dart';

/// System-block persistent field indexes (Register.md): 6 = Name, 7 = NetID.
const int systemNameField = 6;
const int systemNetIdField = 7;

/// The System block's persistent segment: Name (16 bytes, fixed and space-padded, no NUL) then
/// NetID (1 byte, core only). The firmware pads the segment up to the 4-byte alignment of the
/// first static block, so it is 16 B on a node and 20 B on a core.
const int systemNameOffset = 0;
const int systemNameSize = 16;
const int systemNetIdOffset = 16;
const int systemNetIdSize = 1;

/// The System segment size in the `.SV` space for a device with/without a NetID field.
int systemSegmentSize(bool hasNetId) => align4(systemNameSize + (hasNetId ? systemNetIdSize : 0));

/// The firmware's SUBREQ entry cap (Subscriptions.h).
const int maxSubreqEntries = 16;

/// Rounds `v` up to the next 4-byte boundary (the wire/backup alignment).
int align4(int v) => (v + 3) & ~3;

int _u16(List<int> b, int o) => o + 1 < b.length ? (b[o] | (b[o + 1] << 8)) : 0;

/// Aligns a static-space offset to a value of `size` bytes (Docs/Services/Register.md "Memory
/// with 32-bit alignment"): `1 -> 1`, `2 -> 2`, `>= 3 -> 4`.
int _alignValue(int offset, int size) {
  final a = size >= 3 ? 4 : (size == 2 ? 2 : 1);
  return (offset + a - 1) & ~(a - 1);
}

/// A static block type's persistent fields, in field order, as read from the device
/// (Register CID 1 field list + CID 2 per-field ValueInfo). `type` is the wire DataType
/// value. The `.SV` decoder recomputes each field's offset from these sizes + the 32-bit
/// alignment rule, so the app no longer mirrors the firmware's compile-time layout.
/// (See `types.dart`.)

/// One static block type's resolved persistent segment: the base offset of its first instance,
/// the per-instance stride, and each persistent field's offset/size within the instance.
class StaticTypeLayout {
  final int base;
  final int stride;
  final Map<int, int> fieldOffset;
  final Map<int, int> fieldSize;
  final Map<int, DataType> fieldType;

  const StaticTypeLayout({
    required this.base,
    required this.stride,
    required this.fieldOffset,
    required this.fieldSize,
    required this.fieldType,
  });
}

/// The static persistent space's resolved layout, computed from the device's static registry
/// (the block types + their instances, in enumeration order).
class StaticSpaceLayout {
  final Map<int, StaticTypeLayout> types;

  const StaticSpaceLayout(this.types);

  StaticTypeLayout? type(int blockType) => types[blockType];

  /// The byte offset of `(blockType, inst, field)` in `.SV`, or null when the field is not a
  /// persistent static field.
  int? offsetOf(int blockType, int inst, int field) {
    final t = types[blockType];
    final off = t?.fieldOffset[field];
    return (t == null || off == null) ? null : t.base + inst * t.stride + off;
  }

  /// The stored size of a persistent static field, or null when it is not one.
  int? sizeOf(int blockType, int field) => types[blockType]?.fieldSize[field];

  /// Builds the layout from the device's static registry (`(type, inst)` pairs). Types are laid
  /// out in ascending block-type order with their instances contiguous (Docs/Services/Register.md
  /// "System + Static memory blocks"); the System block (type 0) is the first segment.
  factory StaticSpaceLayout.fromRegistry(
      List<({int type, int inst})> registry, StaticFieldLayout fields,
      {bool hasNetId = true}) {
    final counts = <int, int>{};
    for (final r in registry) {
      counts[r.type] = (counts[r.type] ?? 0) + 1;
    }
    final ordered = counts.keys.toList()..sort();
    final sysSize = systemSegmentSize(hasNetId);
    final types = <int, StaticTypeLayout>{
      0: StaticTypeLayout(
        base: 0,
        stride: sysSize,
        fieldOffset: {
          systemNameField: systemNameOffset,
          if (hasNetId) systemNetIdField: systemNetIdOffset,
        },
        fieldSize: {
          systemNameField: systemNameSize,
          if (hasNetId) systemNetIdField: systemNetIdSize,
        },
        fieldType: {
          systemNameField: DataType.name,
          if (hasNetId) systemNetIdField: DataType.id,
        },
      ),
    };
    var cursor = sysSize;
    for (final t in ordered) {
      final flds = fields[t] ?? const [];
      final fieldOffset = <int, int>{};
      final fieldSize = <int, int>{};
      final fieldType = <int, DataType>{};
      var inner = 0;
      for (final f in flds) {
        inner = _alignValue(inner, f.size);
        fieldOffset[f.field] = inner;
        fieldSize[f.field] = f.size;
        fieldType[f.field] = DataType.fromValue(f.type);
        inner += f.size;
      }
      // The struct stride is 4-byte aligned too (the 32-bit alignment rule applies to
      // the instance stride, not just each field).
      final stride = align4(inner);
      types[t] = StaticTypeLayout(
        base: cursor,
        stride: stride,
        fieldOffset: fieldOffset,
        fieldSize: fieldSize,
        fieldType: fieldType,
      );
      cursor += stride * counts[t]!;
    }
    return StaticSpaceLayout(types);
  }
}

/// One stored value, addressed the way the Register addresses fields.
class BackupEntry {
  /// Register block type (0 = the System block).
  final int blockType;
  final int inst;
  final int field;
  final int key;
  final ValueInfo meta;

  /// The stored value, exactly [ValueInfo.size] bytes long.
  final Uint8List value;

  const BackupEntry({
    required this.blockType,
    required this.inst,
    required this.field,
    required this.key,
    required this.meta,
    required this.value,
  });
}

/// Decodes the static persistent space (`.SV`): the System block's fields, then every static
/// block's persistent fields. [registry] is the device's static blocks in enumeration order.
List<BackupEntry> decodeSv(
    List<int> bytes, StaticSpaceLayout layout, List<({int type, int inst})> registry) {
  final out = <BackupEntry>[];
  // The System block is virtual (type 0, inst 0) and has no registry entry.
  final instances = <({int type, int inst})>[(type: 0, inst: 0), ...registry];
  for (final r in instances) {
    final t = layout.type(r.type);
    if (t == null) continue;
    for (final f in t.fieldOffset.entries) {
      final size = t.fieldSize[f.key]!;
      final offset = t.base + r.inst * t.stride + f.value;
      if (offset + size > bytes.length) continue; // truncated space
      final key = (r.type == 0 && f.key == systemNetIdField) ? 0 : 0xFF;
      out.add(BackupEntry(
        blockType: r.type,
        inst: r.inst,
        field: f.key,
        key: key,
        meta: ValueInfo(
            type: t.fieldType[f.key]!.value,
            flags: ValueFlags.persistent,
            key: key,
            size: size),
        value: Uint8List.fromList(bytes.sublist(offset, offset + size)),
      ));
    }
  }
  return out;
}

/// Returns [sv] with the persistent static field's bytes replaced by [value] (the field's stored
/// size). This is the "direct file write" half of a per-field Save; null when the field is not a
/// persistent static field or [value] is the wrong size.
List<int>? svSaveField(
    List<int> sv, StaticSpaceLayout layout, int blockType, int inst, int field, List<int> value) {
  final offset = layout.offsetOf(blockType, inst, field);
  final size = layout.sizeOf(blockType, field);
  if (offset == null || size == null || value.length != size) return null;
  final out = List<int>.from(sv);
  while (out.length < offset + size) {
    out.add(0);
  }
  out.setRange(offset, offset + size, value);
  return out;
}

/// Returns [values] (a block's `.DV_<xx>` bytes) with the (`field`, `key`) entry's bytes
/// replaced by [value], or null when that entry is not persistent or [value] is not the size
/// the table declares.
///
/// Each persistent entry's bytes sit at its stored MemoryOffset in the DV space.
List<int>? dvSaveField(
    DynamicTable table, List<int> values, int field, int key, List<int> value) {
  for (final e in table.entries) {
    if (!e.persistent) continue;
    if (e.field == field && e.key == key) {
      if (value.length != e.size || e.offset + e.size > values.length) return null;
      final out = List<int>.from(values);
      out.setRange(e.offset, e.offset + e.size, value);
      return out;
    }
  }
  return null; // not a persistent entry (Save is only offered for persistent fields)
}

/// One stored requester subscription (`.SUBREQ` layout, Subscriptions.h).
class SubreqEntry {
  final int providerAddr;
  final int trid;
  final int sourceReg;
  final int trigger;
  final int minTimeMs;
  final int periodMs;
  final double deadzone;
  final int targetReg;

  const SubreqEntry({
    required this.providerAddr,
    required this.trid,
    required this.sourceReg,
    required this.trigger,
    required this.minTimeMs,
    required this.periodMs,
    required this.deadzone,
    required this.targetReg,
  });
}

/// Decodes the `.SUBREQ` requester table: u8 count, then 24 bytes per entry
/// (provider u16, trid u16, subscription table [source u32, trigger u8, min u24, period u32,
/// deadzone Number 16.16], target u32). The timeout is not persisted.
List<SubreqEntry> decodeSubreq(List<int> bytes) {
  if (bytes.isEmpty) return const [];
  final count = bytes[0];
  if (count > maxSubreqEntries) return const [];
  final out = <SubreqEntry>[];
  var c = 1;
  for (var i = 0; i < count && c + 24 <= bytes.length; i++) {
    final providerAddr = _u16(bytes, c);
    c += 2;
    final trid = _u16(bytes, c);
    c += 2;
    final table = SubscriptionTable.fromBytes(bytes, c);
    c += 16;
    final targetReg = uint32FromBytes(bytes, c);
    c += 4;
    out.add(SubreqEntry(
      providerAddr: providerAddr,
      trid: trid,
      sourceReg: table.sourceReg,
      trigger: table.trigger.value,
      minTimeMs: table.minTimeMs,
      periodMs: table.periodMs,
      deadzone: table.deadzone,
      targetReg: targetReg,
    ));
  }
  return out;
}

/// One field/key entry of a dynamic block's saved table.
class DynamicTableEntry {
  final int field;
  final int key;
  final int offset; // MemoryOffset into the entry's value space
  final int type;
  final int flags;
  final int size;

  const DynamicTableEntry({
    required this.field,
    required this.key,
    required this.offset,
    required this.type,
    required this.flags,
    required this.size,
  });

  ValueInfo get meta => ValueInfo(type: type, flags: flags, key: key, size: size);
  bool get persistent => flags & ValueFlags.persistent != 0;
}

/// A dynamic block's saved table (`.DT_<xx>`).
class DynamicTable {
  final String name;
  final List<DynamicTableEntry> entries;

  const DynamicTable({required this.name, required this.entries});

  /// Total bytes the persistent entries occupy in the sibling `.DV_` file (the space is
  /// compacted, so the highest persistent MemoryOffset + size is its length).
  int get persistentSize {
    var total = 0;
    for (final e in entries) {
      if (e.persistent && e.offset + e.size > total) total = e.offset + e.size;
    }
    return total;
  }
}

/// Decodes a dynamic block's table. Layout (Docs/Services/Register.md "Dynamic Block
/// Table"): Name (16 chars), entry count (uint16), 16-bit reserved padding, then the
/// entries (Field&Key uint16, MemoryOffset uint16, ValueInfo). Returns null when the bytes
/// are too short to hold the header or the declared entries (a truncated or corrupt table).
/// The block's bank type is not stored: it is derived from the file's global index.
DynamicTable? decodeDynamicTable(List<int> bytes) {
  const nameLen = 16;
  if (bytes.length < nameLen + 4) return null;
  final name = decodePaddedString(bytes.sublist(0, nameLen));
  final entryCount = _u16(bytes, nameLen);
  var c = nameLen + 4; // entry count (2) + reserved padding (2)
  if (c + entryCount * 8 > bytes.length) return null;

  final entries = <DynamicTableEntry>[];
  for (var i = 0; i < entryCount; i++) {
    final fieldKey = _u16(bytes, c);
    final offset = _u16(bytes, c + 2);
    final info = ValueInfo.fromBytes(bytes, c + 4);
    entries.add(DynamicTableEntry(
      field: fieldKey >> 8,
      key: fieldKey & 0xFF,
      offset: offset,
      type: info.type,
      flags: info.flags,
      size: info.size,
    ));
    c += 8;
  }
  return DynamicTable(name: name, entries: entries);
}

/// Pairs a decoded table with its `.DV_` bytes.
///
/// Each persistent entry's bytes sit at its stored MemoryOffset in the DV space (the
/// firmware requires the DV length to be exactly [DynamicTable.persistentSize]); a mismatch
/// means the two files do not belong together, so nothing is returned.
List<BackupEntry> decodeDynamicValues(int inst, DynamicTable table, List<int> values) {
  if (values.length != table.persistentSize) return const [];
  final out = <BackupEntry>[];
  for (final e in table.entries) {
    if (!e.persistent) continue;
    if (e.offset + e.size > values.length) continue;
    out.add(BackupEntry(
      blockType: dynamicTypeForIndex(inst),
      inst: inst,
      field: e.field,
      key: e.key,
      meta: e.meta,
      value: Uint8List.fromList(values.sublist(e.offset, e.offset + e.size)),
    ));
  }
  return out;
}

/// Key for the static/System map. These fields are addressed by *field* only: the wire key is
/// 0 for the System schema and 0xFF for a single-key static field (the firmware normalises
/// 0xFF to 0), so the map key ignores it.
typedef _StaticKey = ({int blockType, int inst, int field});

/// Key for the dynamic map (real field/key pairs).
typedef _DynamicKey = ({int inst, int field, int key});

/// The device's decoded backup, ready for lookup by Register address.
class DeviceBackup {
  final Map<_StaticKey, BackupEntry> _static;
  final Map<_DynamicKey, BackupEntry> _dynamic;
  /// slot -> decoded .DT_ table (retains volatile entries, which have no stored value).
  final Map<int, DynamicTable> _tables;

  const DeviceBackup._(this._static, this._dynamic, this._tables);

  /// An empty backup (nothing saved yet, or the files could not be read).
  static const DeviceBackup empty = DeviceBackup._({}, {}, {});

  bool get hasAny => _static.isNotEmpty || _dynamic.isNotEmpty || _tables.isNotEmpty;

  /// The stored value of a static-block or System field, or null when not backed up.
  BackupEntry? staticField(int blockType, int inst, int field) =>
      _static[(blockType: blockType, inst: inst, field: field)];

  /// The stored value of a dynamic (field, key) entry, or null when not backed up.
  BackupEntry? dynamicField(int inst, int field, int key) =>
      _dynamic[(inst: inst, field: field, key: key)];

  /// The stored table for a dynamic slot, or null when the slot has none.
  DynamicTable? dynamicTableFor(int inst) => _tables[inst];

  /// Distinct fields in a slot's stored table, ascending (empty when there is none).
  List<int> dynamicFieldsFor(int inst) {
    final t = _tables[inst];
    if (t == null) return const [];
    return (<int>{for (final e in t.entries) e.field}.toList())..sort();
  }

  /// Keys of one field in a slot's stored table, ascending.
  List<int> dynamicKeysFor(int inst, int field) {
    final t = _tables[inst];
    if (t == null) return const [];
    return (<int>{for (final e in t.entries) if (e.field == field) e.key}.toList())..sort();
  }

  /// The table entry for a dynamic (field, key) - present for volatile entries too, which
  /// have no stored value. Null when the field/key is not in the stored table.
  DynamicTableEntry? dynamicTableEntry(int inst, int field, int key) {
    final t = _tables[inst];
    if (t == null) return null;
    for (final e in t.entries) {
      if (e.field == field && e.key == key) return e;
    }
    return null;
  }

  /// Decodes every available part.
  ///
  /// [staticRegistry] is the device's static blocks (type + per-type instance) in enumeration
  /// order; the `.SV` decoder recomputes each field's offset from it. [dynamic] maps a dynamic
  /// slot index to its (`.DT_`, `.DV_`) file bytes.
  factory DeviceBackup.decode({
    List<int>? sv,
    List<({int type, int inst})> staticRegistry = const [],
    StaticFieldLayout staticFields = const {},
    Map<int, ({List<int> table, List<int> values})> dynamic = const {},
    bool hasNetId = true,
  }) {
    final statics = <_StaticKey, BackupEntry>{};
    if (sv != null) {
      final layout =
          StaticSpaceLayout.fromRegistry(staticRegistry, staticFields, hasNetId: hasNetId);
      for (final e in decodeSv(sv, layout, staticRegistry)) {
        statics[(blockType: e.blockType, inst: e.inst, field: e.field)] = e;
      }
    }

    final dynamics = <_DynamicKey, BackupEntry>{};
    final tables = <int, DynamicTable>{};
    for (final entry in dynamic.entries) {
      final table = decodeDynamicTable(entry.value.table);
      if (table == null) continue;
      tables[entry.key] = table;
      for (final v in decodeDynamicValues(entry.key, table, entry.value.values)) {
        dynamics[(inst: v.inst, field: v.field, key: v.key)] = v;
      }
    }

    return DeviceBackup._(statics, dynamics, tables);
  }
}
