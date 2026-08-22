/// System Memory service client (Docs/Services/System Memory.md).
///
/// Block numbers are local to the service; the first Read level returns the
/// number of blocks, the second a block's meta + name, the third a field value.
library;

import 'connection.dart';
import 'protocol.dart';
import 'types.dart';

/// One field of a block as shown in the System Memory view.
class SysField {
  final int index;
  BlockMeta meta;
  List<int> value;

  SysField({required this.index, required this.meta, required this.value});

  bool get readOnly => meta.readOnly;
  bool get notSaved => meta.notSaved;
  bool get scriptUpdated => meta.scriptUpdated;
  bool get valid => meta.valid;
}

/// One block of a device's system memory.
class SysBlock {
  final int index;
  final BlockMeta meta;
  String name;

  /// Fields are loaded lazily (one request per field).
  final Map<int, SysField> fields = {};

  SysBlock({required this.index, required this.meta, required this.name});

  BlockType get blockType => meta.blockType;
}

class SystemMemoryClient {
  final int deviceId;

  SystemMemoryClient({required this.deviceId});

  ConnectionManager get _link => ConnectionManager.instance;

  Future<List<int>?> _request(int cid, {List<int> payload = const []}) async {
    try {
      return await _link.request(deviceId, ServiceType.systemMemory, cid,
          payload: payload);
    } catch (_) {
      return null;
    }
  }

  /// Reads the block list (block count + per-block meta and name).
  Future<List<SysBlock>?> readBlocks() async {
    final summary = await _request(2, payload: const BlockIndex().toBytes());    if (summary == null || summary.length < 5) return null;
    final count = summary[4];
    final blocks = <SysBlock>[];
    for (var i = 0; i < count; i++) {
      final block = await readBlockMeta(i);
      if (block != null) blocks.add(block);
    }
    return blocks;
  }

  /// Reads one block's meta + name.
  Future<SysBlock?> readBlockMeta(int block) async {
    final reply = await _request(2,
        payload: BlockIndex(block: block).toBytes());
    if (reply == null) return null;
    var offset = 4; // BlockIndex echo
    if (reply.length < offset + 4) return null;
    final meta = BlockMeta.fromBytes(reply, offset);
    offset += 4;
    final name = reply.length > offset
        ? String.fromCharCodes(reply.sublist(offset))
        : 'Block $block';
    return SysBlock(index: block, meta: meta, name: name);
  }

  /// Reads one field's current value into `block.fields`.
  Future<SysField?> readField(SysBlock block, int field) async {
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
    final result =
        SysField(index: field, meta: meta, value: value);
    block.fields[field] = result;
    return result;
  }

  /// Writes a field's value (CID 3). Returns the confirmed value or null.
  Future<List<int>?> writeField(
      SysBlock block, SysField field, List<int> newValue) async {
    final payload = <int>[
      ...BlockIndex(block: block.index, field: field.index).toBytes(),
      ...field.meta.toBytes(),
      ...newValue,
    ];
    final reply = await _request(3, payload: payload);
    if (reply == null || reply.length <= 8) return null;
    return reply.sublist(8);
  }

  // --- Backup access ---------------------------------------------------------

  /// Reads one field's backup value (CID 4); null when not stored.
  Future<List<int>?> readBackupField(SysBlock block, int field) async {
    final reply = await _request(4,
        payload: BlockIndex(block: block.index, field: field).toBytes());
    if (reply == null || reply.length < 8) return null;
    return reply.sublist(8);
  }

  /// Save (CID 5): invalid block saves everything.
  Future<bool> save({int? block}) =>
      _memoryOp(5, block);

  /// Recall (CID 6): invalid block recalls everything.
  Future<bool> recall({int? block}) => _memoryOp(6, block);

  Future<bool> _memoryOp(int cid, int? block) async {
    final reply = await _request(cid,
        payload: BlockIndex(block: block ?? invalidBlock).toBytes());
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }
}
