/// Decoders (and the two field writers) for the device's own backup registries
/// (Docs/Services/Register.md "Save").
///
/// The device persists its Save targets as storage files:
///   `STATLOG`   static-block + System persistent fields (firmware `StaticMemory.h`)
///   `SUBREQ`    the requester-subscription table (firmware `Subscriptions.h`)
///   `DT_<xx>`   a dynamic block's entry table (firmware `Memory.h`)
///   `DV_<xx>`   that block's packed persistent values (firmware `Memory.h`)
///
/// The decoders serve the storage file viewer and the Register view's Current/Backup
/// toggle from one parse. The writers exist because the wire only carries `Save All` /
/// `Recall All`: "partial saving/recall is handled by app with direct file writes / direct
/// register writes" (Docs/Services/Register.md), so saving one field writes the file here
/// and recalling one is a register write.
library;

import 'dart:typed_data';

import 'types.dart';

/// STATLOG `BlockIndex.block`: 0xFF ends the log.
const int statlogEnd = 0xFF;
/// STATLOG `BlockIndex.block`: 0xFE marks a System-block entry.
const int statlogSystemBlock = 0xFE;

/// System-block persistent field indexes (Register.md): 6 = Name, 7 = NetID.
const int systemNameField = 6;
const int systemNetIdField = 7;

/// The firmware's SUBREQ entry cap (Subscriptions.h).
const int maxSubreqEntries = 16;

/// Rounds `v` up to the next 4-byte boundary (the wire/backup alignment).
int align4(int v) => (v + 3) & ~3;

int _u16(List<int> b, int o) => o + 1 < b.length ? (b[o] | (b[o + 1] << 8)) : 0;

/// One stored value, addressed the way the Register addresses fields.
class BackupEntry {
  /// Register block type (0 = the System block).
  final int blockType;
  final int inst;
  final int field;
  final int key;
  final BlockMeta meta;

  /// The stored value, exactly [BlockMeta.size] bytes long.
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

/// One raw STATLOG record. Blocks are addressed by *registry index*, which only the
/// caller can resolve (see [DeviceBackup.decode]'s `staticRegistry`).
class StatlogRecord {
  final int blockIdx;
  final int field;
  final BlockMeta meta;
  final Uint8List value;

  const StatlogRecord({
    required this.blockIdx,
    required this.field,
    required this.meta,
    required this.value,
  });

  bool get isSystem => blockIdx == statlogSystemBlock;
}

/// Decodes the STATLOG registry backup. Stops at the 0xFF terminator or the end, and
/// stops early rather than reading past a truncated trailing record.
List<StatlogRecord> decodeStatlog(List<int> bytes) {
  final out = <StatlogRecord>[];
  var c = 0;
  while (c + 8 <= bytes.length) {
    final blockIdx = bytes[c];
    if (blockIdx == statlogEnd) break;
    final field = bytes[c + 1];
    final meta = BlockMeta.fromPacked(bytes, c + 4);
    final size = meta.size;
    if (c + 8 + size > bytes.length) break;
    out.add(StatlogRecord(
      blockIdx: blockIdx,
      field: field,
      meta: meta,
      value: Uint8List.fromList(bytes.sublist(c + 8, c + 8 + size)),
    ));
    c += 8 + align4(size);
  }
  return out;
}

/// Returns [statlog] with the entry for (`blockIdx`, `field`) replaced by [value], or with a
/// fresh entry appended when there was none, followed by a 0xFF terminator.
///
/// The other entries are copied byte-for-byte rather than re-encoded, so an entry this app
/// does not understand survives a partial save. [value] must be the field's full stored size
/// (its descriptor's `size`), which is what a live register read returns.
///
/// This is the "direct file write" half of the per-field Save: the device only offers
/// Save All / Recall All, so a single field's save is built here and written with
/// `StorageClient.writeFile`.
List<int> statlogSaveField(
    List<int> statlog, int blockIdx, int field, BlockMeta meta, List<int> value) {
  final out = <int>[];
  var c = 0;
  while (c + 8 <= statlog.length) {
    if (statlog[c] == statlogEnd) break;
    final len = 8 + align4(statlog[c + 7]); // BlockMeta.Size is the header's last byte
    if (c + len > statlog.length) break; // truncated tail: dropped, like the decoder
    if (!(statlog[c] == blockIdx && statlog[c + 1] == field)) {
      out.addAll(statlog.sublist(c, c + len));
    }
    c += len;
  }
  out.addAll(_statlogEntry(blockIdx, field, meta, value));
  out.add(statlogEnd); // everything after the terminator is ignored on read
  return out;
}

/// One STATLOG entry: `BlockIndex[4] + BlockMeta[4] + value`, padded to 4 bytes.
List<int> _statlogEntry(int blockIdx, int field, BlockMeta meta, List<int> value) {
  final out = <int>[
    blockIdx & 0xFF, field & 0xFF, 0xFF, 0,
    meta.flagsAndType & 0xFF, (meta.flagsAndType >> 8) & 0xFF, meta.key & 0xFF,
    value.length & 0xFF,
    ...value,
  ];
  while (out.length % 4 != 0) {
    out.add(0);
  }
  return out;
}

/// Returns [values] (a block's `DV_<xx>` bytes) with the (`field`, `key`) entry's bytes
/// replaced by [value], or null when that entry is not persistent or [value] is not the size
/// the table declares.
///
/// The persistent entries are concatenated in table order, so an entry's offset is the sum of
/// the preceding persistent entries' sizes - the same rule `decodeDynamicValues` uses.
List<int>? dvSaveField(
    DynamicTable table, List<int> values, int field, int key, List<int> value) {
  var offset = 0;
  for (final e in table.entries) {
    if (!e.persistent) continue;
    if (e.field == field && e.key == key) {
      if (value.length != e.size || offset + e.size > values.length) return null;
      final out = List<int>.from(values);
      out.setRange(offset, offset + e.size, value);
      return out;
    }
    offset += e.size;
  }
  return null; // not a persistent entry (Save is only offered for persistent fields)
}

/// One stored requester subscription (SUBREQ layout, Subscriptions.h).
class SubreqEntry {
  final int targetReg;
  final int sourceReg;
  final int providerAddr;
  final int trigger;
  final int periodMs;
  final int minTimeMs;
  final double deadzone;

  const SubreqEntry({
    required this.targetReg,
    required this.sourceReg,
    required this.providerAddr,
    required this.trigger,
    required this.periodMs,
    required this.minTimeMs,
    required this.deadzone,
  });
}

/// Decodes the SUBREQ requester table: u8 count, then 26 bytes per entry
/// (target u32, source u32, provider u16, trigger u8 + 3 pad, period u32, min u32,
/// deadzone Number 16.16).
List<SubreqEntry> decodeSubreq(List<int> bytes) {
  if (bytes.isEmpty) return const [];
  final count = bytes[0];
  if (count > maxSubreqEntries) return const [];
  final out = <SubreqEntry>[];
  var c = 1;
  for (var i = 0; i < count && c + 26 <= bytes.length; i++) {
    final targetReg = uint32FromBytes(bytes, c);
    c += 4;
    final sourceReg = uint32FromBytes(bytes, c);
    c += 4;
    final providerAddr = _u16(bytes, c);
    c += 2;
    final trigger = bytes[c];
    c += 4; // trigger + 3 pad
    final periodMs = uint32FromBytes(bytes, c);
    c += 4;
    final minTimeMs = uint32FromBytes(bytes, c);
    c += 4;
    final deadzone = numberFromBytes(bytes, c);
    c += 4;
    out.add(SubreqEntry(
      targetReg: targetReg,
      sourceReg: sourceReg,
      providerAddr: providerAddr,
      trigger: trigger,
      periodMs: periodMs,
      minTimeMs: minTimeMs,
      deadzone: deadzone,
    ));
  }
  return out;
}

/// One field/key entry of a dynamic block's saved table.
class DynamicTableEntry {
  final int field;
  final int key;
  final int flagsAndType;
  final int size;

  const DynamicTableEntry({
    required this.field,
    required this.key,
    required this.flagsAndType,
    required this.size,
  });

  BlockMeta get meta => BlockMeta(flagsAndType: flagsAndType, key: key, size: size);
  bool get persistent => flagsAndType & FieldFlags.persistent != 0;
}

/// A dynamic block's saved table (`DT_<xx>`).
class DynamicTable {
  final String name;
  final int typeValue;
  final List<DynamicTableEntry> entries;

  const DynamicTable({required this.name, required this.typeValue, required this.entries});

  /// Total bytes the persistent entries occupy in the sibling `DV_` file.
  int get persistentSize {
    var total = 0;
    for (final e in entries) {
      if (e.persistent) total += e.size;
    }
    return total;
  }
}

/// Decodes a dynamic block's table. Returns null when the bytes are too short to hold
/// the header or the declared entries (a truncated or corrupt table).
DynamicTable? decodeDynamicTable(List<int> bytes) {
  if (bytes.isEmpty) return null;
  var c = 0;
  final nameLen = bytes[c++];
  if (c + nameLen + 4 > bytes.length) return null;
  final name = String.fromCharCodes(bytes.sublist(c, c + nameLen));
  c += nameLen;
  final typeValue = _u16(bytes, c);
  final entryCount = _u16(bytes, c + 2);
  c += 4;
  if (c + entryCount * 6 > bytes.length) return null;

  final entries = <DynamicTableEntry>[];
  for (var i = 0; i < entryCount; i++) {
    final fieldKey = _u16(bytes, c);
    entries.add(DynamicTableEntry(
      field: fieldKey >> 8,
      key: fieldKey & 0xFF,
      flagsAndType: _u16(bytes, c + 2),
      size: bytes[c + 4],
    ));
    c += 6;
  }
  return DynamicTable(name: name, typeValue: typeValue, entries: entries);
}

/// Pairs a decoded table with its `DV_` bytes.
///
/// The values are the persistent entries' bytes concatenated in *table order* (the
/// firmware requires the DV length to be exactly [DynamicTable.persistentSize]); a
/// mismatch means the two files do not belong together, so nothing is returned.
List<BackupEntry> decodeDynamicValues(int inst, DynamicTable table, List<int> values) {
  if (values.length != table.persistentSize) return const [];
  final out = <BackupEntry>[];
  var offset = 0;
  for (final e in table.entries) {
    if (!e.persistent) continue;
    out.add(BackupEntry(
      blockType: BlockType.dynamic.value,
      inst: inst,
      field: e.field,
      key: e.key,
      meta: e.meta,
      value: Uint8List.fromList(values.sublist(offset, offset + e.size)),
    ));
    offset += e.size;
  }
  return out;
}

/// Key for the static/System map. These fields are addressed by *field* only: the app
/// always reads a static field at key 0xFF, and the stored descriptor's key is not
/// meaningful per field.
typedef _StaticKey = ({int blockType, int inst, int field});

/// Key for the dynamic map (real field/key pairs).
typedef _DynamicKey = ({int inst, int field, int key});

/// The device's decoded backup, ready for lookup by Register address.
class DeviceBackup {
  final Map<_StaticKey, BackupEntry> _static;
  final Map<_DynamicKey, BackupEntry> _dynamic;
  /// slot -> decoded DT_ table (retains volatile entries, which have no stored value).
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
  /// [staticRegistry] must be the device's static blocks in **registry order** with the
  /// System block excluded (the storage page builds it exactly that way) - STATLOG
  /// addresses blocks by that index. [dynamic] maps a dynamic slot index to its
  /// (`DT_`, `DV_`) file bytes.
  factory DeviceBackup.decode({
    List<int>? statlog,
    List<({int type, int inst})> staticRegistry = const [],
    Map<int, ({List<int> table, List<int> values})> dynamic = const {},
  }) {
    final statics = <_StaticKey, BackupEntry>{};
    if (statlog != null) {
      for (final r in decodeStatlog(statlog)) {
        if (r.isSystem) {
          // The System block is virtual: type 0, inst 0.
          statics[(blockType: 0, inst: 0, field: r.field)] = BackupEntry(
              blockType: 0,
              inst: 0,
              field: r.field,
              key: r.meta.key,
              meta: r.meta,
              value: r.value);
          continue;
        }
        if (r.blockIdx >= staticRegistry.length) continue; // unmapped index
        final b = staticRegistry[r.blockIdx];
        statics[(blockType: b.type, inst: b.inst, field: r.field)] = BackupEntry(
            blockType: b.type,
            inst: b.inst,
            field: r.field,
            key: r.meta.key,
            meta: r.meta,
            value: r.value);
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
