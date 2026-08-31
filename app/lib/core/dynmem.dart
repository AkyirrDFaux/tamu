/// Dynamic Memory service client (Docs/Services/Dynamic Memory.md).
///
/// Same request/response shapes as System Memory, but blocks are user-defined:
/// the block's value IS its name; BlockMeta.Size of a block-level read carries
/// the number of entries (map count).
library;


import 'memory_client.dart';
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

class DynamicMemoryClient extends MemoryClientBase {
  DynamicMemoryClient({required super.deviceId});

  @override
  ServiceType get service => ServiceType.dynamicMemory;
  @override
  String get logTag => 'dynmem';

  /// Reads the block list. The summary reply is BlockIndex + 1 byte count.
  Future<List<DynBlock>?> readBlocks() async {
    final count = await readBlockCount();
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

  /// Reads one block's meta + name (field index invalid in the request).
  Future<DynBlock?> readBlockMeta(int block) async {
    final payload = await readBlockMetaPayload(block);
    if (payload == null) return null;
    return DynBlock(index: block, meta: payload.meta, name: payload.name);
  }

  /// Reads one entry's current value into `block.fields`.
  Future<DynField?> readField(DynBlock block, int field) async {
    final payload =
        await readValue(BlockIndex(block: block.index, field: field));
    if (payload == null) return null;
    return _storeField(block, field, payload.meta, payload.value);
  }

  /// Reads one entry's BACKUP value (CID 4) into `block.fields` (what is stored
  /// in the device's backup file, not the live value).
  Future<DynField?> readBackupField(DynBlock block, int field) async {
    final payload = await readBackupValue(
        BlockIndex(block: block.index, field: field));
    if (payload == null) return null;
    return _storeField(block, field, payload.meta, payload.value);
  }

  DynField _storeField(
      DynBlock block, int field, BlockMeta meta, List<int> value) {
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
  /// Deleted deletes it. `newType` swaps the entry's data type (the firmware
  /// Set() updates FlagsAndType, enabling type changes on existing entries).
  /// Returns the confirmed value or null.
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
    return writeValue(BlockIndex(block: block.index, field: field.index), meta, newValue);
  }

  /// Creates a new block whose value/name is `name` (CID 3, invalid block).
  /// Returns the assigned block index or null.
  Future<int?> createBlock(BlockType type, String name, {int? index}) async {
    final nameBytes = name.codeUnits.take(16).toList();
    final desc = BlockMeta(
      flagsAndType: type.value,
      size: nameBytes.length,
    );
    final reply = await request(index != null ? 0 : 3,
        payload: [
          ...BlockIndex(block: index ?? invalidIndex).toBytes(),
          ...desc.toBytes(),
          ...nameBytes,
        ]);
    if (reply == null || reply.length < 5) return null;
    return reply[0]; // BlockIndex echo, block byte
  }

  /// Sets a block's name and/or type (CID 3, field index invalid). Returns true
  /// when the device echoes the write.
  Future<bool> writeBlockMeta(DynBlock block, String name, BlockType? type) async {
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

  /// Re-reads a single block's meta so callers get fresh map counts before
  /// index-sensitive operations.
  Future<DynBlock?> refreshBlockMeta(DynBlock block) async =>
      readBlockMeta(block.index);

  /// Appends an entry to a block (CID 3), or fills the slot at `index` when
  /// given (a None placeholder keeps indexes stable). `meta` carries the data
  /// type and flags; `value` the initial bytes.
  Future<List<int>?> appendEntry(DynBlock block, BlockMeta meta, List<int> value,
      {int? index}) async {
    final sizedMeta = BlockMeta(
      flagsAndType: meta.flagsAndType,
      key: meta.key,
      size: value.length,
    );
    return writeValue(
        BlockIndex(block: block.index, field: index ?? block.fieldCount),
        sizedMeta,
        value,
        timeout: const Duration(seconds: 4));
  }

  /// Deletes a block / entry (marked Deleted; deallocated on save).
  Future<bool> delete({required int block, int? field}) async {
    final reply = await request(1,
        payload:
            BlockIndex(block: block, field: field ?? invalidIndex).toBytes());
    // Success signalling varies by level (empty vs single-status payloads);
    // any answer at all means the device processed the request.
    return reply != null;
  }

  /// Save (CID 5): invalid block saves everything.
  Future<bool> save({int? block}) => memoryOp(5, block);

  /// Recall (CID 6): invalid block recalls everything.
  Future<bool> recall({int? block}) => memoryOp(6, block);
}
