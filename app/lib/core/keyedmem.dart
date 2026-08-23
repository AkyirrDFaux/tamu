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

  Future<List<int>?> _request(int cid, {List<int> payload = const []}) async {
    try {
      return await _link.request(deviceId, ServiceType.keyedMemory, cid,
          payload: payload);
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
      if (block != null) blocks.add(block);
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
    if (reply == null || reply.length <= 8) return null;
    return reply.sublist(8);
  }

  /// Creates a new keyed block named by `name`. Returns assigned index or null.
  Future<int?> createBlock(BlockType type, String name) async {
    final nameBytes = name.codeUnits.take(16).toList();
    final desc = BlockMeta(flagsAndType: type.value, size: nameBytes.length);
    final reply = await _request(3, payload: [
      ...const BlockIndex().toBytes(),
      ...desc.toBytes(),
      ...nameBytes,
    ]);
    if (reply == null || reply.length < 5) return null;
    return reply[0];
  }

  /// Deletes a block / dictionary / keyed entry (whichever levels are valid).
  Future<bool> delete({required int block, int? dict, int? key}) async {
    final reply = await _request(1,
        payload: BlockIndex(
                block: block, field: dict ?? invalidIndex, key: key ?? invalidIndex)
            .toBytes());
    return reply != null && reply.isNotEmpty && reply[0] == 0;
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
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }
}
