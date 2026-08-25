/// Keyed Memory service client (Docs/Services/Keyed Memory.md).
///
/// Blocks contain dictionaries (block index -> dictionary index), dictionaries
/// contain keyed entries (-> key index). A block-level read returns the block
/// name (value) and map count; a dictionary read (key invalid) returns the
/// dictionary BlockMeta followed by the contained key ids as a byte array.
library;


import 'memory_client.dart';
import 'protocol.dart';
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
  final Map<int, KeyedDict> dicts = {};

  /// Keyed entries loaded lazily, per dictionary: key -> entry.
  final Map<int, Map<int, KeyedEntry>> entries = {};

  KeyedBlock({required this.index, required this.meta, required this.name});

  BlockType get blockType => meta.blockType;
  int get dictCount => meta.size;
}

class KeyedMemoryClient extends MemoryClientBase {
  KeyedMemoryClient({required super.deviceId});

  @override
  ServiceType get service => ServiceType.keyedMemory;
  @override
  String get logTag => 'keyedmem';

  /// Reads the block list (summary reply: BlockIndex + 1 byte count).
  Future<List<KeyedBlock>?> readBlocks() async {
    final count = await readBlockCount();
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
    final payload = await readBlockMetaPayload(block);
    if (payload == null) return null;
    return KeyedBlock(index: block, meta: payload.meta, name: payload.name);
  }

  /// Reads one dictionary: its BlockMeta (Size = number of keys) + key id array.
  Future<KeyedDict?> readDict(KeyedBlock block, int dict) async {
    final payload =
        await readValue(BlockIndex(block: block.index, field: dict));
    if (payload == null) return null;
    final result = KeyedDict(index: dict, meta: payload.meta, keys: payload.value);
    block.dicts[dict] = result;
    return result;
  }

  /// Reads one keyed entry's value into `block.entries`.
  Future<KeyedEntry?> readEntry(KeyedBlock block, int dict, int key) async {
    final payload = await readValue(
        BlockIndex(block: block.index, field: dict, key: key));
    if (payload == null) return null;
    final existing = block.entries[dict]?[key];
    if (existing != null) {
      existing
        ..meta = payload.meta
        ..value = payload.value;
      return existing;
    }
    final result = KeyedEntry(key: key, meta: payload.meta, value: payload.value);
    block.entries.putIfAbsent(dict, () => {})[key] = result;
    return result;
  }

  /// Writes a keyed entry (CID 3); creates it when missing. Returns confirmed bytes.
  Future<List<int>?> writeEntry(
      KeyedBlock block, int dict, KeyedEntry entry, List<int> newValue) async {
    return writeValue(
        BlockIndex(block: block.index, field: dict, key: entry.key),
        entry.meta,
        newValue);
  }

  /// Creates a new keyed block named by `name`. Returns assigned index or null.
  /// An explicit `index` repurposes a None tombstone slot in place (stable
  /// indexes); null/omitted appends a fresh block at the end.
  Future<int?> createBlock(BlockType type, String name, {int? index}) async {
    final nameBytes = name.codeUnits.take(16).toList();
    final desc = BlockMeta(flagsAndType: type.value, size: nameBytes.length);
    final reply = await request(index != null ? 0 : 3, payload: [
      ...BlockIndex(block: index ?? invalidIndex).toBytes(),
      ...desc.toBytes(),
      ...nameBytes,
    ]);
    if (reply == null || reply.length < 5) return null;
    return reply[0];
  }

  /// Re-reads a single block's meta so callers get fresh map counts before
  /// index-sensitive operations (append dict/entry).
  Future<KeyedBlock?> refreshBlockMeta(KeyedBlock block) async =>
      readBlockMeta(block.index);

  /// Sets a block's name and/or type (CID 3, field index invalid).
  Future<bool> writeBlockMeta(KeyedBlock block, String name, BlockType? type) async {
    final nameBytes = name.codeUnits.take(16).toList();
    final desc = BlockMeta(
      flagsAndType: (type ?? block.blockType).value,
      size: nameBytes.length,
    );
    final reply = await request(3, payload: [
      ...BlockIndex(block: block.index).toBytes(),
      ...desc.toBytes(),
      ...nameBytes,
    ]);
    return reply != null && reply.length >= 8;
  }

  /// Appends a dictionary to a block (CID 3 at dict index == map_count), or
  /// fills a None placeholder at `index` when given (dict indexes stay stable).
  /// The dictionary's initial type is `undefined` unless `type` is given.
  Future<bool> appendDict(KeyedBlock block, {int? index, DataType? type}) async {
    final reply = await request(
        3,
        payload: [
          ...BlockIndex(block: block.index,
              field: index ?? block.dictCount).toBytes(),
          ...BlockMeta(
              flagsAndType: (type ?? DataType.undefined).value).toBytes(),
        ],
        timeout: const Duration(seconds: 4));
    return reply != null && reply.length >= 8;
  }

  /// Sets a dictionary's data type (CID 3 with key invalid - the firmware's
  /// "dictionary itself: update its type only" branch). Returns true when the
  /// device echoes the write.
  Future<bool> writeDictMeta(KeyedBlock block, int dict, DataType type) async {
    final reply = await request(3, payload: [
      ...BlockIndex(block: block.index, field: dict).toBytes(),
      ...BlockMeta(flagsAndType: type.value).toBytes(),
    ], timeout: const Duration(seconds: 4));
    return reply != null && reply.length >= 8;
  }

  /// Reads ALL entries of a dictionary in one round trip (CID 7): the reply
  /// carries the dict BlockMeta followed by aligned [BlockMeta + value] pairs.
  /// Returns entries (including None-marked placeholders) or null.
  Future<List<KeyedEntry>?> readAllDictEntries(KeyedBlock block, int dict) async {
    final reply = await request(7,
        payload: BlockIndex(block: block.index, field: dict).toBytes(),
        timeout: const Duration(seconds: 4));
    if (reply == null || reply.length < 8) return null;
    final dictMeta = BlockMeta.fromBytes(reply, 4);
    final entries = <KeyedEntry>[];
    var offset = 8;
    while (offset + 4 <= reply.length) {
      final meta = BlockMeta.fromBytes(reply, offset);
      offset += 4;
      final valueLen = meta.size;
      if (offset + valueLen > reply.length) break;
      entries.add(KeyedEntry(
          key: meta.key, meta: meta, value: reply.sublist(offset, offset + valueLen)));
      offset += valueLen;
      while (offset % 4 != 0 && offset < reply.length) {
        offset++;
      }
    }
    final dictObj = KeyedDict(index: dict, meta: dictMeta, keys: [
      for (final e in entries)
        if (e.meta.typeValue != DataType.none.value) e.key
    ]);
    block.dicts[dict] = dictObj;
    block.entries[dict] = {for (final e in entries) e.key: e};
    return entries;
  }

  /// Writes a keyed entry by key id (CID 3); the firmware creates missing keys.
  /// A None-typed meta with no value marks the key deleted in place.
  Future<List<int>?> writeKeyValue(
      KeyedBlock block, int dict, int key, BlockMeta meta, List<int> value) async {
    return writeValue(
        BlockIndex(block: block.index, field: dict, key: key),
        meta,
        value,
        timeout: const Duration(seconds: 4));
  }

  /// Deletes a block / dictionary / keyed entry (whichever levels are valid).
  Future<bool> delete({required int block, int? dict, int? key}) async {
    final reply = await request(1,
        payload: BlockIndex(
                block: block, field: dict ?? invalidIndex, key: key ?? invalidIndex)
            .toBytes());
    // Success signalling varies by level (empty vs single-status payloads);
    // any answer at all means the device processed the request.
    return reply != null;
  }

  Future<bool> save({int? block}) => memoryOp(5, block);

  Future<bool> recall({int? block}) => memoryOp(6, block);
}
