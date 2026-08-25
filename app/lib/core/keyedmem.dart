/// Keyed Memory service client (Docs/Services/Keyed Memory.md).
///
/// Blocks contain dictionaries (block index -> dictionary index), dictionaries
/// contain keyed entries (-> key index). A block-level read returns the block
/// name (value) and map count; a dictionary read (key invalid) returns the
/// dictionary BlockMeta followed by the contained key ids as a byte array.
library;

import 'connection.dart';
import 'diagnostics.dart';
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

class KeyedMemoryClient {
  final int deviceId;

  KeyedMemoryClient({required this.deviceId});

  ConnectionManager get _link => ConnectionManager.instance;

  Future<List<int>?> _request(int cid,
      {List<int> payload = const [], Duration timeout = const Duration(seconds: 2)}) async {
    try {
      return await _link.request(deviceId, ServiceType.keyedMemory, cid,
          payload: payload, timeout: timeout);
    } catch (error) {
      AppDiagnostics.log('keyedmem', 'request failed: $error');
      return null;
    }
  }

  /// Reads the block list (summary reply: BlockIndex + 1 byte count).
  Future<List<KeyedBlock>?> readBlocks() async {
    final summary =
        await _request(2, payload: const BlockIndex().toBytes());
    if (summary == null || summary.length < 5) return null;
    final count = summary[4];
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
    final reply =
        await _request(2, payload: BlockIndex(block: block).toBytes());
    if (reply == null || reply.length < 8) return null;
    var offset = 4;
    final meta = BlockMeta.fromBytes(reply, offset);
    offset += 4;
    final name = reply.length > offset
        ? String.fromCharCodes(reply.sublist(offset))
        : 'Block $block';
    return KeyedBlock(index: block, meta: meta, name: name);
  }

  /// Reads one dictionary: its BlockMeta (Size = number of keys) + key id array.
  Future<KeyedDict?> readDict(KeyedBlock block, int dict) async {
    final reply = await _request(2,
        payload: BlockIndex(block: block.index, field: dict).toBytes());
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    final keys = reply.sublist(8).toList();
    final result = KeyedDict(index: dict, meta: meta, keys: keys);
    block.dicts[dict] = result;
    return result;
  }

  /// Reads one keyed entry's value into `block.entries`.
  Future<KeyedEntry?> readEntry(KeyedBlock block, int dict, int key) async {
    final reply = await _request(2,
        payload:
            BlockIndex(block: block.index, field: dict, key: key).toBytes());
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    final value = reply.sublist(8).toList();
    final existing = block.entries[dict]?[key];
    if (existing != null) {
      existing
        ..meta = meta
        ..value = value;
      return existing;
    }
    final result = KeyedEntry(key: key, meta: meta, value: value);
    block.entries.putIfAbsent(dict, () => {})[key] = result;
    return result;
  }

  /// Writes a keyed entry (CID 3); creates it when missing. Returns confirmed bytes.
  Future<List<int>?> writeEntry(
      KeyedBlock block, int dict, KeyedEntry entry, List<int> newValue) async {
    final payload = <int>[
      ...BlockIndex(block: block.index, field: dict, key: entry.key).toBytes(),
      ...entry.meta.toBytes(),
      ...newValue,
    ];
    final reply = await _request(3, payload: payload);
    if (reply == null || reply.length < 8) return null;
    return reply.sublist(8);
  }

  /// Creates a new keyed block named by `name`. Returns assigned index or null.
  /// An explicit `index` repurposes a None tombstone slot in place (stable
  /// indexes); null/omitted appends a fresh block at the end.
  Future<int?> createBlock(BlockType type, String name, {int? index}) async {
    final nameBytes = name.codeUnits.take(16).toList();
    final desc = BlockMeta(flagsAndType: type.value, size: nameBytes.length);
    final reply = await _request(index != null ? 0 : 3, payload: [
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
    final reply = await _request(3, payload: [
      ...BlockIndex(block: block.index).toBytes(),
      ...desc.toBytes(),
      ...nameBytes,
    ]);
    return reply != null && reply.length >= 8;
  }

  /// Appends an empty dictionary to a block (CID 3 at dict index == map_count),
  /// or fills a None placeholder at `index` when given (indexes stay stable).
  Future<bool> appendDict(KeyedBlock block, {int? index}) async {
    final reply = await _request(
        3,
        payload: [
          ...BlockIndex(block: block.index,
              field: index ?? block.dictCount).toBytes(),
          ...BlockMeta(flagsAndType: DataType.undefined.value).toBytes(),
        ],
        timeout: const Duration(seconds: 4));
    return reply != null && reply.length >= 8;
  }

  /// Reads ALL entries of a dictionary in one round trip (CID 7): the reply
  /// carries the dict BlockMeta followed by aligned [BlockMeta + value] pairs.
  /// Returns entries (including None-marked placeholders) or null.
  Future<List<KeyedEntry>?> readAllDictEntries(KeyedBlock block, int dict) async {
    final reply = await _request(7,
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
    final reply = await _request(3, payload: [
      ...BlockIndex(block: block.index, field: dict, key: key).toBytes(),
      ...meta.toBytes(),
      ...value,
    ], timeout: const Duration(seconds: 4));
    // Success echo = BlockIndex(4) + BlockMeta(4) + value; an empty-value
    // (None delete) write legitimately echoes exactly 8 bytes.
    if (reply == null || reply.length < 8) return null;
    return reply.sublist(8);
  }

  /// Deletes a block / dictionary / keyed entry (whichever levels are valid).
  Future<bool> delete({required int block, int? dict, int? key}) async {
    final reply = await _request(1,
        payload: BlockIndex(
                block: block, field: dict ?? invalidIndex, key: key ?? invalidIndex)
            .toBytes());
    // Success signalling varies by level (empty vs single-status payloads);
    // any answer at all means the device processed the request.
    return reply != null;
  }

  /// Reads one keyed entry's backup value (CID 4); null when not stored.
  Future<List<int>?> readBackupEntry(
      KeyedBlock block, int dict, int key) async {
    final reply = await _request(4,
        payload:
            BlockIndex(block: block.index, field: dict, key: key).toBytes());
    if (reply == null || reply.length < 8) return null;
    return reply.sublist(8);
  }

  Future<bool> save({int? block}) => _memoryOp(5, block);

  Future<bool> recall({int? block}) => _memoryOp(6, block);

  Future<bool> _memoryOp(int cid, int? block) async {
    final reply = await _request(cid,
        payload: BlockIndex(block: block ?? invalidBlock).toBytes());
    // Success signalling varies by level (empty vs single-status payloads);
    // any answer at all means the device processed the request.
    return reply != null;
  }
}
