/// Register service client (Docs/Services/Register.md).
///
/// Uses BlockInfo (Type10|Inst6|Field8|Key8) format instead of BlockIndex.
library;

import 'dart:typed_data';

import 'connection.dart';
import 'diagnostics.dart';
import 'protocol.dart';
import 'types.dart';

/// Builds a 32-bit BlockInfo: Type(10)|Instance(6)|Field(8)|Key(8).
Uint8List makeBlockInfo(int type, int inst, int field, int key) {
  final bi = ((type & 0x3FF) << 22) | ((inst & 0x3F) << 16) | ((field & 0xFF) << 8) | (key & 0xFF);
  return Uint8List(4)
    ..[0] = bi & 0xFF
    ..[1] = (bi >> 8) & 0xFF
    ..[2] = (bi >> 16) & 0xFF
    ..[3] = (bi >> 24) & 0xFF;
}

/// One entry of a dynamic block (dynamic/keyed memory is part of the Register service).
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

/// Register service client (Docs/Services/Register.md).
///
/// Uses BlockInfo (Type10|Inst6|Field8|Key8) format instead of BlockIndex.
class RegisterClient {
  final int deviceId;

  RegisterClient({required this.deviceId});

  ServiceType get service => ServiceType.register;
  String get logTag => 'register';

  Duration get _requestTimeout => const Duration(seconds: 2);

  /// Sends a request to the Register service and returns the reply payload.
  Future<List<int>?> request(int cid, {List<int> payload = const [], Duration? timeout}) async {
    try {
      return await ConnectionManager.instance.request(deviceId, ServiceType.register, cid,
          payload: payload, timeout: timeout ?? _requestTimeout);
    } catch (error) {
      AppDiagnostics.log('register', 'request failed: $error');
      return null;
    }
  }

  /// Slices the value bytes after a BlockMeta header, clamped to the declared size
  /// (the wire payload is padded to 4 bytes).
  static List<int> valueSlice(List<int> reply, int size) {
    if (size <= 0) return <int>[];
    final avail = reply.length - 8;
    return reply.sublist(8, 8 + ((size > avail) ? avail : size));
  }

  /// Enumerate block types (CID 0, Enum 0).
  Future<List<int>?> enumerateBlockTypes() async {
    // Enum 0 for block types - firmware expects enum_level + bi_req (5 bytes)
    // Response: BlockInfo echo (4 bytes) + types array, padded to a 4-byte multiple.
    final reply = await request(0, payload: [0, 0, 0, 0, 0]);
    if (reply == null || reply.length < 5) return null;
    // The type list is the reply after the 4-byte BlockInfo echo; trailing padding
    // bytes (0) must not be counted as block types.
    var count = reply.length - 4;
    while (count > 0 && reply[3 + count] == 0) {
      count--;
    }
    return reply.sublist(4, 4 + count);
  }

/// Enumerate instances of a block type (CID 0, field=0xFF).
  Future<int?> getInstanceCount(int blockType) async {
    // Enum 1 for instances - firmware expects enum_level + bi_req (5 bytes)
    // bi_req: type=blockType, instance=0x3F (all), field=0xFF (all), key=0
    // Instance is in bits 16-21 (6 bits), field in bits 8-15.
    final bi = ((blockType & 0x3FF) << 22) | ((0x3F & 0x3F) << 16) | (0xFF << 8) | 0;
    final payload = [
      1, // enum_level = 1
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF
    ];
    final reply = await request(0, payload: payload);
    if (reply == null || reply.length < 5) return null;
    return reply[4];
  }

  /// Enumerate fields in a block (CID 0, key=0xFF).
  Future<int?> getFieldCount(int blockType, int instance) async {
    // Enum 2 for fields - firmware expects enum_level + bi_req (5 bytes)
    // Response: BlockInfo echo (4 bytes) + count (2 bytes, little-endian)
    final bi = ((blockType & 0x3FF) << 22) | ((instance & 0x3F) << 16) | (0xFF << 8);
    final payload = [
      2, // enum_level = 2
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF
    ];
    final reply = await request(0, payload: payload);
    if (reply == null || reply.length < 6) return null;
    return (reply[4] | (reply[5] << 8));
  }

  /// Read block meta + name (CID 1).
  Future<({BlockMeta meta, String name})?> readBlockMeta(int blockType, int instance) async {
    // For block meta, we need type|inst|field=0xFF|key=0
    final bi = ((blockType & 0x3FF) << 22) | ((instance & 0x3F) << 16) | (0xFF << 8) | 0;
    final payload = [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF
    ];
    final reply = await request(1, payload: payload);
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    final name = reply.length > 8
        ? String.fromCharCodes(reply.sublist(8)).replaceAll('\x00', '')
        : '';
    return (meta: meta, name: name);
  }

  /// Read field value (CID 1) for system block (type=0, inst=0).
  Future<({BlockMeta meta, List<int> value})?> readField(int field, int key) async {
    final bi = (0 << 22) | (0 << 16) | ((field & 0xFF) << 8) | (key & 0xFF);
    final payload = [bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF];
    final reply = await request(1, payload: payload);
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    return (meta: meta, value: valueSlice(reply, meta.size));
  }

  /// Read field value (CID 1) for a specific block type and instance.
  Future<({BlockMeta meta, List<int> value})?> readBlockField(int blockType, int instance, int field, int key) async {
    final bi = ((blockType & 0x3FF) << 22) | ((instance & 0x3F) << 16) | ((field & 0xFF) << 8) | (key & 0xFF);
    final payload = [bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF];
    final reply = await request(1, payload: payload);
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    return (meta: meta, value: valueSlice(reply, meta.size));
  }

  /// Write field value (CID 2) for a specific block type and instance (static/dynamic blocks).
  /// Payload: BlockInfo (4) + BlockMeta (4) + value
  Future<List<int>?> writeBlockField(int blockType, int instance, int field, int key, BlockMeta meta, List<int> value) async {
    final bi = ((blockType & 0x3FF) << 22) | ((instance & 0x3F) << 16) | ((field & 0xFF) << 8) | (key & 0xFF);
    final payload = [bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF, ...meta.toBytes(), ...value];
    final reply = await request(2, payload: payload);
    if (reply == null || reply.length < 8) return null;
    final echoMeta = BlockMeta.fromBytes(reply, 4);
    return valueSlice(reply, echoMeta.size);
  }

  /// Reads all blocks (static + dynamic) by enumerating types and instances.
  /// The System block (type 0, inst 0) is a virtual block not in the static registry.
  /// Dynamic blocks use type BlockType.dynamic and are not returned by enumerateBlockTypes.
  Future<List<({int type, int inst, BlockMeta meta, String name})?>?> readBlocks() async {
    final types = await enumerateBlockTypes();
    if (types == null) return null;
    final blocks = <({int type, int inst, BlockMeta meta, String name})?>[];
    
    // Add System block (type 0, inst 0) explicitly - it's a virtual block
    // not present in the static block registry.
    final sysBlock = await readBlockMeta(0, 0);
    if (sysBlock != null) {
      blocks.add((type: 0, inst: 0, meta: sysBlock.meta, name: sysBlock.name));
    }
    
    for (final type in types) {
      final count = await getInstanceCount(type);
      if (count == null) continue;
      for (var inst = 0; inst < count; inst++) {
        final block = await readBlockMeta(type, inst);
        if (block != null) {
          blocks.add((type: type, inst: inst, meta: block.meta, name: block.name));
        }
      }
    }
    
    // Add Dynamic blocks (type BlockType.dynamic) - not returned by enumerateBlockTypes
    final dynCount = await getInstanceCount(BlockType.dynamic.value);
    if (dynCount != null && dynCount > 0) {
      for (var inst = 0; inst < dynCount; inst++) {
        final block = await readBlockMeta(BlockType.dynamic.value, inst);
        if (block != null) {
          blocks.add((type: BlockType.dynamic.value, inst: inst, meta: block.meta, name: block.name));
        }
      }
    }
    
    return blocks;
  }

  // ===========================================================================
  // Dynamic (and keyed) memory - fully part of the Register service
  // (Docs/Services/Register.md "Dynamic blocks", CIDs 0x10-0x15).
  // ===========================================================================

  static Uint8List _dynBi(int inst, int field, [int key = 0]) =>
      makeBlockInfo(BlockType.dynamic.value, inst, field, key);

  /// Reads the dynamic block list (CID 0 Enum 1 for instances). None/Deleted
  /// blocks are hidden (their index stays reserved until a save compacts).
  Future<List<DynBlock>?> readDynamicBlocks() async {
    final count = await getInstanceCount(BlockType.dynamic.value);
    if (count == null) return null;
    final blocks = <DynBlock>[];
    for (var i = 0; i < count; i++) {
      final block = await readDynamicBlockMeta(i);
      if (block != null &&
          block.blockType != BlockType.deleted &&
          block.blockType != BlockType.none) {
        blocks.add(block);
      }
    }
    return blocks;
  }

  /// Reads one dynamic block's meta + name (CID 1).
  Future<DynBlock?> readDynamicBlockMeta(int block) async {
    final reply = await request(1, payload: _dynBi(block, 0xFF));
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    final name = reply.length > 8
        ? String.fromCharCodes(reply.sublist(8)).replaceAll('\x00', '')
        : '';
    return DynBlock(index: block, meta: meta, name: name);
  }

  /// Reads one dynamic entry's current value (CID 1).
  Future<DynField?> readDynamicField(DynBlock block, int field) async {
    final reply = await request(1, payload: _dynBi(block.index, field));
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    return DynField(index: field, meta: meta, value: valueSlice(reply, meta.size));
  }

  /// Reads one dynamic entry's BACKUP value (CID 0x15).
  Future<DynField?> readDynamicBackupField(DynBlock block, int field) async {
    final reply = await request(0x15, payload: _dynBi(block.index, field));
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    return DynField(index: field, meta: meta, value: valueSlice(reply, meta.size));
  }

  /// Writes a dynamic entry (CID 2). Writing type Deleted marks it for deletion.
  Future<List<int>?> writeDynamicField(
      DynBlock block, DynField field, List<int> newValue,
      {DataType? newType}) async {
    final meta = BlockMeta(
      flagsAndType: newType != null
          ? (field.meta.flags & FieldFlags.mask) | newType.value
          : field.meta.flagsAndType,
      key: field.meta.key,
      size: newValue.length,
    );
    return _writeDynamicValue(_dynBi(block.index, field.index), meta, newValue);
  }

  /// Creates a new dynamic block (CID 0x10). Returns the assigned block index.
  Future<int?> createDynamicBlock(BlockType type, String name, {int? index}) async {
    final nameBytes = name.codeUnits.take(12).toList();
    while (nameBytes.length < 4) {
      nameBytes.add(0x20); // pad with spaces
    }
    final typeBits = type.value & 0x3FF;
    final bi = index != null
        ? (typeBits << 22) | ((index & 0x3F) << 16) | (0xFF << 8) | 0xFF
        : (typeBits << 22) | (0xFF << 8) | 0xFF;
    final reply = await request(0x10, timeout: const Duration(seconds: 5), payload: [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF,
      ...nameBytes,
    ]);
    if (reply == null || reply.length < 5) return null;
    return reply[0]; // BlockIndex echo, block byte
  }

  /// Sets a dynamic block's name and/or type (CID 2, field 0xFF = block meta).
  Future<bool> writeDynamicBlockMeta(DynBlock block, String name, BlockType? type) async {
    final nameBytes = name.codeUnits.take(12).toList();
    while (nameBytes.length < 4) {
      nameBytes.add(0x20);
    }
    final meta = BlockMeta(
      flagsAndType: (type ?? block.blockType).value,
      size: nameBytes.length,
    );
    final reply = await request(2, payload: [
      ..._dynBi(block.index, 0xFF),
      ...meta.toBytes(),
      ...nameBytes,
    ]);
    return reply != null && reply.length >= 5 && reply[4] != 0;
  }

  /// Re-reads a block's meta so callers get fresh map counts.
  Future<DynBlock?> refreshDynamicBlockMeta(DynBlock block) async =>
      readDynamicBlockMeta(block.index);

  /// Appends an entry to a dynamic block (CID 2), or fills the slot at `index`
  /// when given (a None placeholder keeps indexes stable).
  Future<List<int>?> appendDynamicEntry(DynBlock block, BlockMeta meta, List<int> value,
      {int? index}) async {
    final sizedMeta = BlockMeta(
      flagsAndType: meta.flagsAndType,
      key: meta.key,
      size: value.length,
    );
    final fieldIdx = index ?? block.fieldCount;
    return _writeDynamicValue(_dynBi(block.index, fieldIdx), sizedMeta, value,
        timeout: const Duration(seconds: 4));
  }

  Future<List<int>?> _writeDynamicValue(Uint8List bi, BlockMeta meta, List<int> value,
      {Duration? timeout}) async {
    final reply = await request(2, payload: [...bi, ...meta.toBytes(), ...value],
        timeout: timeout ?? const Duration(seconds: 4));
    if (reply == null || reply.length < 8) return null;
    final echoMeta = BlockMeta.fromBytes(reply, 4);
    return valueSlice(reply, echoMeta.size);
  }

  /// Deletes a dynamic block / entry (marked Deleted; deallocated on save) - CID 0x11.
  Future<bool> deleteDynamic({required int block, int? field}) async {
    final reply = await request(0x11, payload: _dynBi(block, field ?? 0xFF));
    return reply != null;
  }

  /// Saves the dynamic registry (CID 3; instance 0x3F = everything).
  Future<bool> saveDynamic({int? block}) async {
    final inst = block ?? 0x3F;
    final reply = await request(3, payload: _dynBi(inst, 0xFF));
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  /// Recalls the dynamic registry (CID 4; instance 0x3F = everything).
  Future<bool> recallDynamic({int? block}) async {
    final inst = block ?? 0x3F;
    final reply = await request(4, payload: _dynBi(inst, 0xFF));
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }
}