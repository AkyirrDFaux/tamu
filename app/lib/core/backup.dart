/// Whole-network backup and restore (Docs/App/Backup.md).
///
/// "Unified tool for whole-network backup and restoration. Works with the device's
/// file systems (backup restoration), optional live restore. Creates a zipfile
/// containing per device JSON files if backing up the entire network."
///
/// Capture records the ENTIRE register (every block entry, volatile and read-only
/// included), the Subscriptions tables, the Scripts and the SNDB semantically, plus
/// every device file. The zip holds one JSON per device (no aggregate manifest).
/// Restore matches items by name so small firmware changes do not break an archive,
/// and items can be synced to a different device/block when compatible.
library;


import 'dart:convert';
import 'dart:typed_data';

import 'package:archive/archive_io.dart';

import 'backup_format.dart';
import 'backup_script.dart';
import 'backup_value.dart';
import 'block_registry.dart';
import 'device_db.dart';
import 'diagnostics.dart';
import 'register_client.dart';
import 'script_file.dart';
import 'storage_client.dart';
import 'subscription_client.dart';
import 'system_schema.dart';
import 'types.dart';

part 'backup_capture.dart'; // capture a device into the semantic backup model
part 'backup_restore.dart'; // live model, restore plan and apply

/// Files larger than this are not captured (the 64-byte Storage fragments make
/// large reads prohibitively slow); the value can be raised by the caller.
const int defaultMaxFileBytes = 128 * 1024;

// The System block schema (names/keys) is shared with the UI in core/system_schema.dart.

// ---------------------------------------------------------------------------
// Semantic naming helpers
// ---------------------------------------------------------------------------

/// Block type name in words (registry name when known, System block resolved explicitly).
String blockTypeWord(int typeValue) {
  if (typeValue == systemBlockTypeValue) return 'System';
  if (isDynamicType(typeValue)) return 'Dynamic';
  if (isScriptType(typeValue)) return 'Script';
  final type = BlockType.fromValue(typeValue);
  return blockInfoFor(type)?.typeName ?? type.label;
}

/// The registry field info for a static/system block field, if any.
FieldInfo? _staticFieldInfo(int typeValue, int field) =>
    blockInfoFor(BlockType.fromValue(typeValue))?.field(field);

/// Field name in words for a Register address (system/static names, else "Field N").
String registerFieldName(int typeIndex, int field) {
  if (typeIndex == systemBlockTypeValue) return systemFieldName(field);
  return _staticFieldInfo(typeIndex, field)?.name ?? 'Field $field';
}

String registerKeyName(int typeIndex, int field, int key) {
  if (typeIndex == systemBlockTypeValue) return systemKeyName(field, key);
  return 'Key $key';
}

/// A semantic address for a 32-bit BlockInfo.
BackupBlockRef _blockRef(int blockInfo) {
  final type = blockInfoType(blockInfo);
  final inst = blockInfoInstance(blockInfo);
  final field = blockInfoField(blockInfo);
  final key = blockInfoKey(blockInfo);
  return BackupBlockRef(
    block: blockTypeWord(type),
    typeIndex: type,
    instance: inst,
    field: registerFieldName(type, field),
    fieldIndex: field,
    key: registerKeyName(type, field, key),
    keyIndex: key,
  );
}

int? _scriptSlot(String fileName) {
  final upper = fileName.toUpperCase();
  if (!upper.startsWith('SCR_')) return null;
  return int.tryParse(upper.substring(4), radix: 16);
}

// ---------------------------------------------------------------------------
// Device walk (one read-order definition for capture and the live snapshot)
// ---------------------------------------------------------------------------

/// A register field visited by [walkDeviceBlocks]: its address, the descriptive
/// field/key names, the read metadata and the raw value bytes.
class VisitedField {
  final int field;
  final int key;
  final String fieldName;
  final String keyName;
  final ValueInfo meta;
  final FieldInfo? info;
  final List<int> value;

  VisitedField({
    required this.field,
    required this.key,
    required this.fieldName,
    required this.keyName,
    required this.meta,
    required this.value,
    this.info,
  });
}

/// A block and the fields visited in it.
class VisitedBlock {
  final int type;
  final int instance;
  final String name;
  final bool isDynamic;
  final List<VisitedField> fields;

  VisitedBlock({
    required this.type,
    required this.instance,
    required this.name,
    required this.isDynamic,
    required this.fields,
  });
}

/// Walks every restorable block of [reg] in one defined read order, skipping script
/// blocks and dynamic tombstones. Returns null when the block list cannot be read.
///
/// This is the single place that decides which fields/keys a device has and how they
/// are named, so backup capture and the live restore snapshot cannot drift apart.
Future<List<VisitedBlock>?> walkDeviceBlocks(RegisterClient reg) async {
  final blocks = await reg.readBlocks();
  if (blocks == null) return null;
  final out = <VisitedBlock>[];
  for (final b in blocks) {
    if (b == null) continue;
    final type = b.type;
    if (isScriptType(type)) continue; // scripts captured semantically
    // Dynamic tombstones (None/Deleted) carry no data; skip them.
    if (isDynamicType(type) &&
        (b.meta.type == BlockType.none.value ||
            b.meta.type == BlockType.deleted.value)) {
      continue;
    }
    if (isDynamicType(type)) {
      final dyn = DynBlock(index: b.inst, meta: b.meta, name: b.name);
      var fields = await reg.getDynamicFields(b.inst) ?? const <int>[];
      if (fields.isEmpty && b.meta.size > 0) {
        fields = [for (var i = 0; i < b.meta.size; i++) i];
      }
      final visited = <VisitedField>[];
      for (final field in fields) {
        final keys = await reg.getDynamicKeys(b.inst, field) ?? const <int>[0];
        for (final key in (keys.isEmpty ? const [0] : keys)) {
          final read = await reg.readDynamicField(dyn, field, key);
          if (read == null) continue;
          visited.add(VisitedField(
            field: field,
            key: key,
            fieldName: 'Field $field',
            keyName: 'Key $key',
            meta: read.meta,
            value: read.value,
          ));
        }
      }
      out.add(VisitedBlock(
        type: b.meta.type,
        instance: b.inst,
        name: b.name,
        isDynamic: true,
        fields: visited,
      ));
    } else if (type == systemBlockTypeValue) {
      final visited = <VisitedField>[];
      for (var field = 0; field < systemFieldCount; field++) {
        final keys = systemFieldKeys[field] ?? const {0: 'Key 0'};
        for (final key in keys.keys) {
          final read = await reg.readField(field, key);
          if (read == null) continue;
          visited.add(VisitedField(
            field: field,
            key: key,
            fieldName: systemFieldName(field),
            keyName: systemKeyName(field, key),
            meta: read.meta,
            value: read.value,
          ));
        }
      }
      out.add(VisitedBlock(
        type: systemBlockTypeValue,
        instance: 0,
        name: 'System',
        isDynamic: false,
        fields: visited,
      ));
    } else {
      final visited = <VisitedField>[];
      for (var field = 0; field < b.meta.size; field++) {
        final read = await reg.readBlockField(type, b.inst, field, 0);
        if (read == null) continue;
        final info = _staticFieldInfo(type, field);
        visited.add(VisitedField(
          field: field,
          key: 0,
          fieldName: info?.name ?? 'Field $field',
          keyName: 'Key 0',
          meta: read.meta,
          info: info,
          value: read.value,
        ));
      }
      out.add(VisitedBlock(
        type: type,
        instance: b.inst,
        name: b.name,
        isDynamic: false,
        fields: visited,
      ));
    }
  }
  return out;
}

/// Capture side of the shared walk: a walked block as the semantic backup model.
BackupBlock backupBlockFromVisited(VisitedBlock b) => BackupBlock(
      type: blockTypeWord(b.type),
      typeIndex: b.type,
      instance: b.instance,
      name: b.name,
      isDynamic: b.isDynamic,
      entries: [
        for (final f in b.fields)
          BackupEntry(
            field: f.fieldName,
            fieldIndex: f.field,
            key: f.keyName,
            keyIndex: f.key,
            type: dataTypeWord(f.meta.dataType),
            flags: flagWords(f.meta.flags),
            unit: f.info?.unit,
            value: encodeSemantic(f.meta.dataType, f.value, info: f.info),
            size: f.value.length,
          ),
      ],
    );

/// Restore side of the shared walk: a walked block as the live snapshot model. It agrees
/// with [backupBlockFromVisited] on every field/key address and name by construction.
LiveBlock liveBlockFromVisited(VisitedBlock b) => LiveBlock(
      type: b.type,
      instance: b.instance,
      name: b.name,
      isDynamic: b.isDynamic,
      entries: [
        for (final f in b.fields)
          LiveEntry(
            field: f.field,
            key: f.key,
            fieldName: f.fieldName,
            keyName: f.keyName,
            meta: f.meta,
            info: f.info,
          ),
      ],
    );

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

/// Collects all of one device's data semantically.
