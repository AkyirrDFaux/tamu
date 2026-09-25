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
// Capture
// ---------------------------------------------------------------------------

/// Collects all of one device's data semantically.
