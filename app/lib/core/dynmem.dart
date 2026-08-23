/// Dynamic Memory service client (Docs/Services/Dynamic Memory.md).
///
/// Same request/response shapes as System Memory, but blocks are user-defined:
/// the block's value IS its name; BlockMeta.Size of a block-level read carries
/// the number of entries (map count).
library;

import 'connection.dart';
import 'diagnostics.dart';
import 'protocol.dart';
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

/// A user-created memory block.
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

  DynamicMemoryClient({required this.deviceId});

  ConnectionManager get _link => ConnectionManager.instance;

  Future<List<int>?> _request(int cid, {List<int> payload = const []}) async {
    try {
      return await _link.request(deviceId, ServiceType.dynamicMemory, cid,
          payload: payload);
    } catch (error) {
      AppDiagnostics.log('dynmem', 'request failed: $error');
      return null;
    }
  }

  /// Reads the block list. The summary reply is BlockIndex + 1 byte count.
  Future<List<DynBlock>?> readBlocks() async {
    final summary =
        await _request(2, payload: const BlockIndex().toBytes());
    if (summary == null || summary.length < 5) return null;
    final count = summary[4];
    final blocks = <DynBlock>[];
    for (var i = 0; i < count; i++) {
      final block = await readBlockMeta(i);
      if (block != null) blocks.add(block);
    }
    return blocks;
  }

  /// Reads one block's meta + name (field index invalid in the request).
  Future<DynBlock?> readBlockMeta(int block) async {
    final reply = await _request(2,
        payload: BlockIndex(block: block).toBytes());
    if (reply == null || reply.length < 8) return null;
    var offset = 4; // BlockIndex echo
    final meta = BlockMeta.fromBytes(reply, offset);
    offset += 4;
    final name = reply.length > offset
        ? String.fromCharCodes(reply.sublist(offset))
        : 'Block $block';
    return DynBlock(index: block, meta: meta, name: name);
  }

  /// Reads one entry's current value into `block.fields`.
  Future<DynField?> readField(DynBlock block, int field) async {
    final reply = await _request(2,
        payload: BlockIndex(block: block.index, field: field).toBytes());
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    final value = reply.sublist(8).toList();
    final existing = block.fields[field];
    if (existing != null) {
      existing
        ..meta = meta
        ..value = value;
      return existing;
    }
    final result = DynField(index: field, meta: meta, value: value);
    block.fields[field] = result;
    return result;
  }

  /// Writes an entry (CID 3); a non-existing entry gets created. Writing type
  /// Deleted deletes it. Returns the confirmed value or null.
  Future<List<int>?> writeField(
      DynBlock block, DynField field, List<int> newValue) async {
    final payload = <int>[
      ...BlockIndex(block: block.index, field: field.index).toBytes(),
      ...field.meta.toBytes(),
      ...newValue,
    ];
    final reply = await _request(3, payload: payload);
    if (reply == null || reply.length <= 8) return null;
    return reply.sublist(8);
  }

  /// Creates a new block whose value/name is `name` (CID 3, invalid block).
  /// Returns the assigned block index or null.
  Future<int?> createBlock(BlockType type, String name) async {
    final nameBytes = name.codeUnits.take(16).toList();
    final desc = BlockMeta(
      flagsAndType: type.value,
      size: nameBytes.length,
    );
    final reply = await _request(3,
        payload: [
          ...const BlockIndex().toBytes(),
          ...desc.toBytes(),
          ...nameBytes,
        ]);
    if (reply == null || reply.length < 5) return null;
    return reply[0]; // BlockIndex echo, block byte
  }

  /// Deletes a block / entry (marked Deleted; deallocated on save).
  Future<bool> delete({required int block, int? field}) async {
    final reply = await _request(1,
        payload:
            BlockIndex(block: block, field: field ?? invalidIndex).toBytes());
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  /// Reads one entry's backup value (CID 4); null when not stored.
  Future<List<int>?> readBackupField(DynBlock block, int field) async {
    final reply = await _request(4,
        payload: BlockIndex(block: block.index, field: field).toBytes());
    if (reply == null || reply.length < 8) return null;
    return reply.sublist(8);
  }

  /// Save (CID 5): invalid block saves everything.
  Future<bool> save({int? block}) => _memoryOp(5, block);

  /// Recall (CID 6): invalid block recalls everything.
  Future<bool> recall({int? block}) => _memoryOp(6, block);

  Future<bool> _memoryOp(int cid, int? block) async {
    final reply = await _request(cid,
        payload: BlockIndex(block: block ?? invalidBlock).toBytes());
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }
}
