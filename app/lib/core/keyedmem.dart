/// Keyed Memory client using Register service (Docs/Services/Register.md).
///
/// Keyed blocks are accessed via Register service (0x01) with BlockInfo:
/// Type=0x3FF (Dynamic), Instance=block index, Field=dict index, Key=key index.
/// CIDs: 0=Enumerate, 1=Read, 2=Write, 3=Save, 4=Recall, 0x10=Create, 0x11=Delete,
/// 0x12=GetName, 0x13=SetName, 0x14=GetMemoryUsage.
library;

import 'dart:typed_data';

import 'register_client.dart';
import 'types.dart';

/// One keyed entry inside a dictionary.
class KeyedEntry {
  final int key;
  BlockMeta meta;
  List<int> value;

  KeyedEntry({required this.key, required this.meta, required this.value});

  bool get readOnly => meta.readOnly;
  bool get notSaved => meta.notSaved;
}

/// One dictionary of a keyed block.
class KeyedDict {
  final int index;
  final BlockMeta meta;
  final List<int> keys;

  KeyedDict({required this.index, required this.meta, required this.keys});

  int get entryCount => meta.size;

  /// Keys in ascending order for a stable display (entries are key-addressed;
  /// the storage order is irrelevant, so the UI sorts for readability).
  List<int> get sortedKeys => [...keys]..sort();
}

/// A user-created keyed block.
class KeyedBlock {
  final int index;
  final BlockMeta meta;
  String name;

  /// Dictionaries loaded lazily.
  final Map<int, KeyedDict> dicts = <int, KeyedDict>{};

  /// Keyed entries loaded lazily, per dictionary: key -> entry.
  final Map<int, Map<int, KeyedEntry>> entries = <int, Map<int, KeyedEntry>>{};

  KeyedBlock({required this.index, required this.meta, required this.name});

  BlockType get blockType => meta.blockType;
  int get dictCount => meta.size;
}

class KeyedMemoryClient {
  final int deviceId;
  final RegisterClient _register;

  KeyedMemoryClient({required this.deviceId})
      : _register = RegisterClient(deviceId: deviceId);

  static const int kDynamicBlockType = 0x3FF;

  /// Builds a BlockInfo for keyed blocks (Type=0x3FF).
  static Uint8List _makeBlockInfo(int inst, int field, [int key = 0]) {
    final bi = ((0x3FF & 0x3FF) << 22) | ((inst & 0x3F) << 16) | ((field & 0xFF) << 8) | (key & 0xFF);
    return Uint8List(4)
      ..[0] = bi & 0xFF
      ..[1] = (bi >> 8) & 0xFF
      ..[2] = (bi >> 16) & 0xFF
      ..[3] = (bi >> 24) & 0xFF;
  }

  /// Reads the block list (CID 0 Enum 1 for instances).
  Future<List<KeyedBlock>?> readBlocks() async {
    final count = await _register.getInstanceCount(0x3FF);
    if (count == null) return null;
    final blocks = <KeyedBlock>[];
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

  Future<KeyedBlock?> readBlockMeta(int block) async {
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
    return KeyedBlock(index: block, meta: meta, name: name);
  }

  /// Reads one dictionary: its BlockMeta (Size = number of keys) + key id array (CID 1).
  Future<KeyedDict?> readDict(KeyedBlock block, int dict) async {
    final bi = ((0x3FF & 0x3FF) << 22) | ((block.index & 0x3F) << 16) | ((dict & 0xFF) << 8);
    final payload = [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF
    ];
    final reply = await _register.request(1, payload: payload);
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    // Keys follow the BlockMeta (8 bytes header + meta.size bytes of key IDs)
    final keys = reply.sublist(8, 8 + meta.size);
    final result = KeyedDict(index: dict, meta: meta, keys: keys);
    block.dicts[dict] = result;
    return result;
  }

  /// Reads one keyed entry's value (CID 1).
  Future<KeyedEntry?> readEntry(KeyedBlock block, int dict, int key) async {
    final bi = ((0x3FF & 0x3FF) << 22) | ((block.index & 0x3F) << 16) | ((dict & 0xFF) << 8) | (key & 0xFF);
    final payload = [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF
    ];
    final reply = await _register.request(1, payload: payload);
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    final value = RegisterClient.valueSlice(reply, meta.size);
    final existing = block.entries[dict]?[key];
    if (existing != null) {
      existing
        ..meta = meta
        ..value = value;
      return existing;
    }
    final result = KeyedEntry(key: key, meta: meta, value: value);
    block.entries.putIfAbsent(dict, () => <int, KeyedEntry>{})[key] = result;
    return result;
  }

  /// Writes a keyed entry (CID 2); creates it when missing. Returns confirmed bytes.
  Future<List<int>?> writeEntry(
      KeyedBlock block, int dict, KeyedEntry entry, List<int> newValue) async {
    final sizedMeta = BlockMeta(
      flagsAndType: entry.meta.flagsAndType,
      key: entry.meta.key,
      size: newValue.length,
    );
    final bi = ((0x3FF & 0x3FF) << 22) | ((block.index & 0x3F) << 16) | ((dict & 0xFF) << 8) | (entry.meta.key & 0xFF);
    final payload = [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF,
      ...sizedMeta.toBytes(),
      ...newValue,
    ];
    final reply = await _register.request(2, payload: payload);
    if (reply == null || reply.length < 8) return null;
    final echoMeta = BlockMeta.fromBytes(reply, 4);
    return RegisterClient.valueSlice(reply, echoMeta.size);
  }

  /// Creates a new keyed block (CID 0x10). Returns assigned index or null.
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
    return reply[0];
  }

  /// Re-reads a single block's meta so callers get fresh map counts before
  /// index-sensitive operations (append dict/entry).
  Future<KeyedBlock?> refreshBlockMeta(KeyedBlock block) async =>
      readBlockMeta(block.index);

  /// Sets a block's name and/or type (CID 0x13). Returns true on success.
/// Firmware expects: BlockInfo (4 bytes) + BlockMeta (4 bytes) + name bytes (up to 12 chars, min 4 bytes total)
  Future<bool> writeBlockMeta(KeyedBlock block, String name, BlockType? type) async {
    final nameBytes = name.codeUnits.take(12).toList(); // 12 chars max per docs
    while (nameBytes.length < 4) nameBytes.add(0x20); // pad with spaces
    final desc = BlockMeta(
      flagsAndType: (type ?? block.blockType).value,
      size: nameBytes.length,
    );
    final bi = ((0x3FF & 0x3FF) << 22) | ((block.index & 0x3F) << 16) | (0xFF << 8);
    final payload = [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF,
      ...desc.toBytes(),
      ...nameBytes,
    ];
    final reply = await _register.request(0x13, payload: payload);
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  /// Appends a dictionary to a block (CID 2), or fills a None placeholder at `index` when given.
  /// The dictionary's initial type is `undefined` unless `type` is given.
  Future<bool> appendDict(KeyedBlock block, {int? index, DataType? type}) async {
    final dictIdx = index ?? block.dictCount;
    final bi = ((0x3FF & 0x3FF) << 22) | ((block.index & 0x3F) << 16) | ((dictIdx & 0xFF) << 8);
    final desc = BlockMeta(flagsAndType: (type ?? DataType.undefined).value, size: 0);
    final payload = [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF,
      ...desc.toBytes(),
    ];
    final reply = await _register.request(2, payload: payload, timeout: const Duration(seconds: 4));
    return reply != null && reply.length >= 8;
  }

  /// Sets a dictionary's data type (CID 2 with key=0xFF - "dictionary itself" branch).
  Future<bool> writeDictMeta(KeyedBlock block, int dict, DataType type) async {
    final bi = ((0x3FF & 0x3FF) << 22) | ((block.index & 0x3F) << 16) | ((dict & 0xFF) << 8) | 0xFF;
    final desc = BlockMeta(flagsAndType: type.value, size: 0);
    final payload = [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF,
      ...desc.toBytes(),
    ];
    final reply = await _register.request(2, payload: payload, timeout: const Duration(seconds: 4));
    return reply != null && reply.length >= 8;
  }

  /// Writes a keyed entry by key id (CID 2); the firmware creates missing keys.
  /// A None-typed meta with no value marks the key deleted in place.
  Future<List<int>?> writeKeyValue(
      KeyedBlock block, int dict, int key, BlockMeta meta, List<int> value) async {
    final sizedMeta = BlockMeta(
      flagsAndType: meta.flagsAndType,
      key: meta.key,
      size: value.length,
    );
    final bi = ((0x3FF & 0x3FF) << 22) | ((block.index & 0x3F) << 16) | ((dict & 0xFF) << 8) | (key & 0xFF);
    final payload = [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF,
      ...sizedMeta.toBytes(),
      ...value,
    ];
    final reply = await _register.request(2, payload: payload, timeout: const Duration(seconds: 4));
    if (reply == null || reply.length < 8) return null;
    final echoMeta = BlockMeta.fromBytes(reply, 4);
    return RegisterClient.valueSlice(reply, echoMeta.size);
  }

  /// Deletes a block / dictionary / keyed entry (CID 0x11).
  Future<bool> delete({required int block, int? dict, int? key}) async {
    final bi = ((0x3FF & 0x3FF) << 22) | ((block & 0x3F) << 16) |
        ((dict ?? 0xFF) << 8) | (key ?? 0xFF);
    final payload = [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF,
    ];
    final reply = await _register.request(0x11, payload: payload);
    return reply != null;
  }

  /// Reads one keyed entry's BACKUP value (CID 0x15).
  Future<KeyedEntry?> readBackupEntry(
      KeyedBlock block, int dict, int key) async {
    final bi = ((0x3FF & 0x3FF) << 22) | ((block.index & 0x3F) << 16) | ((dict & 0xFF) << 8) | (key & 0xFF);
    final payload = [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF
    ];
    final reply = await _register.request(0x15, payload: payload);
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    final value = RegisterClient.valueSlice(reply, meta.size);
    final existing = block.entries[dict]?[key];
    if (existing != null) {
      existing
        ..meta = meta
        ..value = value;
      return existing;
    }
    final result = KeyedEntry(key: key, meta: meta, value: value);
    block.entries.putIfAbsent(dict, () => <int, KeyedEntry>{})[key] = result;
    return result;
  }

  /// Save (CID 3): invalid block saves everything.
  Future<bool> save({int? block}) async {
    final bi = block != null
        ? ((0x3FF & 0x3FF) << 22) | ((block & 0x3F) << 16) | (0xFF << 8)
        : ((0x3FF & 0x3FF) << 22) | (0xFF << 8) | 0xFF;
    final payload = [bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF];
    final reply = await _register.request(3, payload: payload);
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  /// Recall (CID 4): invalid block recalls everything.
  Future<bool> recall({int? block}) async {
    final bi = block != null
        ? ((0x3FF & 0x3FF) << 22) | ((block & 0x3F) << 16) | (0xFF << 8)
        : ((0x3FF & 0x3FF) << 22) | (0xFF << 8) | 0xFF;
    final payload = [bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF];
    final reply = await _register.request(4, payload: payload);
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }
}