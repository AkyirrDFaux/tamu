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

  /// Internal alias for backwards compatibility.
  Future<List<int>?> _request(int cid, {List<int> payload = const [], Duration? timeout}) => request(cid, payload: payload, timeout: timeout);

  /// Slices the value bytes after a BlockMeta header, clamped to the declared size
  /// (the wire payload is padded to 4 bytes).
  static List<int> _valueSlice(List<int> reply, int size) {
    if (size <= 0) return <int>[];
    final avail = reply.length - 8;
    return reply.sublist(8, 8 + ((size > avail) ? avail : size));
  }

  /// Public version for external use
  static List<int> valueSlice(List<int> reply, int size) => _valueSlice(reply, size);

  /// Block-list summary (CID 0): returns number of block types.
  Future<int?> getBlockTypeCount() async {
    final payload = makeBlockInfo(0, 0, 0xFF, 0); // Enum 0, invalid block
    final reply = await _request(0, payload: payload);
    if (reply == null || reply.length < 5) return null;
    return reply[4];
  }

  /// Enumerate block types (CID 0, Enum 0).
  Future<List<int>?> enumerateBlockTypes() async {
    // Enum 0 for block types - firmware expects enum_level + bi_req (5 bytes)
    // Response: BlockInfo echo (4 bytes) + types array
    final reply = await _request(0, payload: [0, 0, 0, 0, 0]);
    if (reply == null || reply.length < 5) return null;
    // Count is reply.length - 4 (the BlockInfo echo)
    final count = reply.length - 4;
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
    final reply = await _request(0, payload: payload);
    print('[REG] getInstanceCount($blockType) reply: $reply');
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
    final reply = await _request(0, payload: payload);
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
    final reply = await _request(1, payload: payload);
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
    final reply = await _request(1, payload: payload);
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    return (meta: meta, value: _valueSlice(reply, meta.size));
  }

  /// Read field value (CID 1) for a specific block type and instance.
  Future<({BlockMeta meta, List<int> value})?> readBlockField(int blockType, int instance, int field, int key) async {
    final bi = ((blockType & 0x3FF) << 22) | ((instance & 0x3F) << 16) | ((field & 0xFF) << 8) | (key & 0xFF);
    final payload = [bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF];
    final reply = await _request(1, payload: payload);
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    return (meta: meta, value: _valueSlice(reply, meta.size));
  }

  /// Write field value (CID 2) for a specific block type and instance (static/dynamic blocks).
  /// Payload: BlockInfo (4) + BlockMeta (4) + value
  Future<List<int>?> writeBlockField(int blockType, int instance, int field, int key, BlockMeta meta, List<int> value) async {
    final bi = ((blockType & 0x3FF) << 22) | ((instance & 0x3F) << 16) | ((field & 0xFF) << 8) | (key & 0xFF);
    final payload = [bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF, ...meta.toBytes(), ...value];
    final reply = await _request(2, payload: payload);
    if (reply == null || reply.length < 8) return null;
    final echoMeta = BlockMeta.fromBytes(reply, 4);
    return _valueSlice(reply, echoMeta.size);
  }

  /// Read System block field (type=0, inst=0).
  Future<List<int>?> readSystemField(int field, int key) async {
    final bi = (field & 0xFF) << 8 | (key & 0xFF);
    final payload = [bi & 0xFF, (bi >> 8) & 0xFF, 0, 0];
    final reply = await _request(1, payload: payload);
    if (reply == null || reply.length < 8) return null;
    return reply.sublist(8);
  }

  /// Reads all blocks (static + dynamic) by enumerating types and instances.
  /// The System block (type 0, inst 0) is a virtual block not in the static registry.
  /// Dynamic blocks use type 0x3FF and are not returned by enumerateBlockTypes.
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
    
    // Add Dynamic blocks (type 0x3FF) - not returned by enumerateBlockTypes
    final dynCount = await getInstanceCount(0x3FF);
    if (dynCount != null && dynCount > 0) {
      for (var inst = 0; inst < dynCount; inst++) {
        final block = await readBlockMeta(0x3FF, inst);
        if (block != null) {
          blocks.add((type: 0x3FF, inst: inst, meta: block.meta, name: block.name));
        }
      }
    }
    
    return blocks;
  }
}