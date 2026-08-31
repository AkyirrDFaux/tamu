/// System Memory service client (Docs/Services/System Memory.md).
///
/// Block numbers are local to the service; the first Read level returns the
/// number of blocks, the second a block's meta + name, the third a field value.
library;


import 'memory_client.dart';
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

class SystemMemoryClient extends MemoryClientBase {
  SystemMemoryClient({required super.deviceId});

  @override
  ServiceType get service => ServiceType.systemMemory;
  @override
  String get logTag => 'sysmem';

  /// Reads the block list (block count + per-block meta and name).
  Future<List<SysBlock>?> readBlocks() async {
    final count = await readBlockCount();
    if (count == null) return null;
    final blocks = <SysBlock>[];
    for (var i = 0; i < count; i++) {
      final block = await readBlockMeta(i);
      if (block != null) blocks.add(block);
    }
    return blocks;
  }

  /// Reads one block's meta + name.
  Future<SysBlock?> readBlockMeta(int block) async {
    final payload = await readBlockMetaPayload(block);
    if (payload == null) return null;
    return SysBlock(index: block, meta: payload.meta, name: payload.name);
  }

  /// Reads one field's current value into `block.fields`.
  Future<SysField?> readField(SysBlock block, int field) async {
    final payload =
        await readValue(BlockIndex(block: block.index, field: field));
    if (payload == null) return null;
    return _storeField(block, field, payload.meta, payload.value);
  }

  /// Reads one field's BACKUP value (CID 4) into `block.fields` (what is stored
  /// in the device's backup file, not the live value).
  Future<SysField?> readBackupField(SysBlock block, int field) async {
    final payload = await readBackupValue(
        BlockIndex(block: block.index, field: field));
    if (payload == null) return null;
    return _storeField(block, field, payload.meta, payload.value);
  }

  SysField _storeField(
      SysBlock block, int field, BlockMeta meta, List<int> value) {
    final existing = block.fields[field];
    if (existing != null) {
      existing
        ..meta = meta
        ..value = value;
      return existing;
    }
    final result = SysField(index: field, meta: meta, value: value);
    block.fields[field] = result;
    return result;
  }

  /// Writes a field's value (CID 3). Returns the confirmed value or null.
  Future<List<int>?> writeField(
      SysBlock block, SysField field, List<int> newValue) async {
    final sizedMeta = BlockMeta(
      flagsAndType: field.meta.flagsAndType,
      key: field.meta.key,
      size: newValue.length,
    );
    return writeValue(
        BlockIndex(block: block.index, field: field.index), sizedMeta, newValue);
  }

  // --- Backup access ---------------------------------------------------------

  /// Save (CID 5): invalid block saves everything.
  Future<bool> save({int? block}) => memoryOp(5, block);

  /// Recall (CID 6): invalid block recalls everything.
  Future<bool> recall({int? block}) => memoryOp(6, block);
}
