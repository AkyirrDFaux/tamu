/// Dynamic Memory client using Register service (Docs/Services/Register.md).
///
/// Dynamic blocks are accessed via Register service (0x01) with BlockInfo:
/// Type=0x3FF (Dynamic), Instance=block index, Field=field index, Key=0.
/// CIDs: 0=Enumerate, 1=Read, 2=Write, 3=Save, 4=Recall, 0x10=Create, 0x11=Delete,
/// 0x12=GetName, 0x13=SetName, 0x14=GetMemoryUsage.
library;

import 'dart:typed_data';

import 'register_client.dart';
import 'types.dart';

/// One entry of a dynamic block.
class DynField {
  final int index;
  BlockMeta meta;
  List<int> value;

  DynField({required this.index, required this.meta, required this.value});

  bool get readOnly => meta.readOnly;
  bool get notSaved => meta.notSaved;
}

/// A user-created dynamic memory block.
class DynBlock {
  final int index;
  final BlockMeta meta;
  String name;

  /// Entries loaded lazily (one request per field).
  final Map<int, DynField> fields = {};

  DynBlock({required this.index, required this.meta, required this.name});

  BlockType get blockType => meta.blockType;
  int get fieldCount => meta.size;
}

class DynamicMemoryClient {
  final int deviceId;
  final RegisterClient _register;

  DynamicMemoryClient({required this.deviceId})
      : _register = RegisterClient(deviceId: deviceId);

  static const int kDynamicBlockType = 0x3FF;

  /// Builds a BlockInfo for dynamic blocks (Type=0x3FF).
  static Uint8List _makeBlockInfo(int inst, int field, [int key = 0]) {
    final bi = ((0x3FF & 0x3FF) << 22) | ((inst & 0x3F) << 16) | ((field & 0xFF) << 8) | (key & 0xFF);
    return Uint8List(4)
      ..[0] = bi & 0xFF
      ..[1] = (bi >> 8) & 0xFF
      ..[2] = (bi >> 16) & 0xFF
      ..[3] = (bi >> 24) & 0xFF;
  }

  /// Reads the block list (CID 0 Enum 1 for instances).
  Future<List<DynBlock>?> readBlocks() async {
    final count = await _register.getInstanceCount(0x3FF);
    if (count == null) return null;
    final blocks = <DynBlock>[];
    for (var i = 0; i < count; i++) {
      final block = await readBlockMeta(i);
      // None/Deleted (not yet saved) blocks still occupy their index slot;
      // hide them - their index stays reserved until save compacts.
      if (block != null &&
          block.blockType != BlockType.deleted &&
          block.blockType != BlockType.none) {
        blocks.add(block);
      }
    }
    return blocks;
  }

  /// Reads one block's meta + name (CID 1).
  Future<DynBlock?> readBlockMeta(int block) async {
    final bi = ((0x3FF & 0x3FF) << 22) | ((block & 0x3F) << 16) | (0xFF << 8) | 0;
    final payload = [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF
    ];
    final reply = await _register.request(1, payload: payload);
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    final name = reply.length > 8
        ? String.fromCharCodes(reply.sublist(8)).replaceAll('\x00', '')
        : '';
    return DynBlock(index: block, meta: meta, name: name);
  }

  /// Reads one entry's current value (CID 1).
  Future<DynField?> readField(DynBlock block, int field) async {
    final bi = ((0x3FF & 0x3FF) << 22) | ((block.index & 0x3F) << 16) | ((field & 0xFF) << 8) | 0;
    final payload = [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF
    ];
    final reply = await _register.request(1, payload: payload);
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    final value = RegisterClient.valueSlice(reply, meta.size);
    return DynField(index: field, meta: meta, value: value);
  }

  /// Reads one entry's BACKUP value (CID 0x15).
  Future<DynField?> readBackupField(DynBlock block, int field) async {
    final bi = ((0x3FF & 0x3FF) << 22) | ((block.index & 0x3F) << 16) | ((field & 0xFF) << 8);
    final payload = [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF
    ];
    final reply = await _register.request(0x15, payload: [bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF]);
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    return DynField(index: field, meta: meta, value: RegisterClient.valueSlice(reply, meta.size));
  }

  /// Writes an entry (CID 2). Writing type Deleted deletes it.
  Future<List<int>?> writeField(
      DynBlock block, DynField field, List<int> newValue,
      {DataType? newType}) async {
    var meta = BlockMeta(
      flagsAndType: newType != null
          ? (field.meta.flags & FieldFlags.mask) | newType.value
          : field.meta.flagsAndType,
      key: field.meta.key,
      size: newValue.length,
    );
    final bi = ((0x3FF & 0x3FF) << 22) | ((block.index & 0x3F) << 16) | ((field.index & 0xFF) << 8);
    final payload = [
      ..._makeBlockInfo(block.index, field.index),
      ...meta.toBytes(),
      ...newValue,
    ];
    final reply = await _register.request(2, payload: payload);
    if (reply == null || reply.length < 8) return null;
    final echoMeta = BlockMeta.fromBytes(reply, 4);
    return RegisterClient.valueSlice(reply, echoMeta.size);
  }

  /// Creates a new block (CID 0x10). Returns the assigned block index or null.
/// Firmware expects: BlockInfo (4 bytes) + name bytes (max 12 chars, no BlockMeta)
  Future<int?> createBlock(BlockType type, String name, {int? index}) async {
    final nameBytes = name.codeUnits.take(12).toList(); // 12 chars max per docs
    // Firmware requires minimum 8 bytes total (4 BlockInfo + 4 name bytes)
    while (nameBytes.length < 4) nameBytes.add(0x20); // pad with spaces
    final bi = index != null
        ? ((0x3FF & 0x3FF) << 22) | ((index & 0x3F) << 16) | (0xFF << 8) | 0xFF
        : ((0x3FF & 0x3FF) << 22) | (0xFF << 8) | 0xFF; // invalid instance for auto-assign
    final payload = [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF,
      ...nameBytes,
    ];
    final reply = await _register.request(0x10, payload: payload);
    if (reply == null || reply.length < 5) return null;
    return reply[0]; // BlockIndex echo, block byte
  }

  /// Sets a block's name and/or type (CID 0x13). Returns true on success.
/// Firmware expects: BlockInfo (4 bytes) + BlockMeta (4 bytes) + name bytes (up to 12 chars)
  Future<bool> writeBlockMeta(DynBlock block, String name, BlockType? type) async {
    final nameBytes = name.codeUnits.take(12).toList(); // 12 chars max per docs
    while (nameBytes.length < 4) nameBytes.add(0x20); // pad with spaces
    // Firmware expects: BlockInfo + BlockMeta + name
    final meta = BlockMeta(
      flagsAndType: (type ?? block.blockType).value,
      size: nameBytes.length,
    );
    final bi = ((0x3FF & 0x3FF) << 22) | ((block.index & 0x3F) << 16) | (0xFF << 8) | 0;
    final payload = [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF,
      ...meta.toBytes(),
      ...nameBytes,
    ];
    final reply = await _register.request(0x13, payload: payload);
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  /// Re-reads a single block's meta so callers get fresh map counts before
  /// index-sensitive operations.
  Future<DynBlock?> refreshBlockMeta(DynBlock block) async =>
      readBlockMeta(block.index);

  /// Appends an entry to a block (CID 2), or fills the slot at `index` when
  /// given (a None placeholder keeps indexes stable). `meta` carries the data
  /// type and flags; `value` the initial bytes.
  Future<List<int>?> appendEntry(DynBlock block, BlockMeta meta, List<int> value,
      {int? index}) async {
    final sizedMeta = BlockMeta(
      flagsAndType: meta.flagsAndType,
      key: meta.key,
      size: value.length,
    );
    final fieldIdx = index ?? block.fieldCount;
    final bi = ((0x3FF & 0x3FF) << 22) | ((block.index & 0x3F) << 16) | ((fieldIdx & 0xFF) << 8);
    final payload = [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF,
      ...sizedMeta.toBytes(),
      ...value,
    ];
    return writeValue(bi, sizedMeta, value, timeout: const Duration(seconds: 4));
  }

  /// Writes a value using CID 2.
  Future<List<int>?> writeValue(int bi, BlockMeta meta, List<int> value, {Duration? timeout}) async {
    final payload = [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF,
      ...meta.toBytes(),
      ...value,
    ];
    final reply = await _register.request(2, payload: payload, timeout: timeout ?? const Duration(seconds: 4));
    if (reply == null || reply.length < 8) return null;
    final echoMeta = BlockMeta.fromBytes(reply, 4);
    return RegisterClient.valueSlice(reply, echoMeta.size);
  }

  /// Deletes a block / entry (marked Deleted; deallocated on save) - CID 0x11.
  Future<bool> delete({required int block, int? field}) async {
    final bi = ((0x3FF & 0x3FF) << 22) | ((block & 0x3F) << 16) | ((field ?? 0xFF) << 8);
    final payload = [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF,
    ];
    final reply = await _register.request(0x11, payload: payload);
    return reply != null;
  }

  /// Save (CID 3): invalid block saves everything.
  Future<bool> save({int? block}) async {
    final bi = block != null
        ? ((0x3FF & 0x3FF) << 22) | ((block & 0x3F) << 16) | (0xFF << 8)
        : ((0x3FF & 0x3FF) << 22) | (0xFF << 8) | 0xFF; // invalid block
    final payload = [bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF];
    final reply = await _register.request(3, payload: payload);
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  /// Recall (CID 4): invalid block recalls everything.
  Future<bool> recall({int? block}) async {
    final bi = block != null
        ? ((0x3FF & 0x3FF) << 22) | ((block & 0x3F) << 16) | (0xFF << 8)
        : ((0x3FF & 0x3FF) << 22) | (0xFF << 8) | 0xFF; // invalid block
    final payload = [bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF];
    final reply = await _register.request(4, payload: payload);
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  /// Get block name (CID 0x12).
  Future<String?> getName(int block) async {
    final bi = ((0x3FF & 0x3FF) << 22) | ((block & 0x3F) << 16) | (0xFF << 8);
    final payload = [bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF];
    final reply = await _register.request(0x12, payload: payload);
    if (reply == null || reply.length < 16) return null;
    return String.fromCharCodes(reply).replaceAll('\x00', '').trimRight();
  }

  /// Set block name (CID 0x13).
/// Firmware expects: BlockInfo (4 bytes) + BlockMeta (4 bytes) + name bytes (up to 12 chars, min 4 bytes total)
  Future<bool> setName(int block, String name) async {
    final nameBytes = name.codeUnits.take(12).toList(); // 12 chars max per docs
    while (nameBytes.length < 4) nameBytes.add(0x20); // pad with spaces
    final meta = BlockMeta(flagsAndType: BlockType.undefined.value, size: nameBytes.length);
    final bi = ((0x3FF & 0x3FF) << 22) | ((block & 0x3F) << 16) | (0xFF << 8);
    final payload = [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF,
      ...meta.toBytes(),
      ...nameBytes,
    ];
    final reply = await _register.request(0x13, payload: payload);
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  /// Get memory usage (CID 0x14) - returns 6 uint32.
  Future<List<int>?> getMemoryUsage(int block) async {
    final bi = ((0x3FF & 0x3FF) << 22) | ((block & 0x3F) << 16) | (0xFF << 8);
    final payload = [bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF];
    final reply = await _register.request(0x14, payload: payload);
    if (reply == null || reply.length < 24) return null;
    return List<int>.from(reply);
  }

  }