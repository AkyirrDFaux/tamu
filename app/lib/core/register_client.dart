/// Register service client (Docs/Services/Register.md).
///
/// Uses BlockInfo (Type10|Inst6|Field8|Key8) format instead of BlockIndex.
library;

import 'dart:typed_data';

import 'connection.dart';
import 'diagnostics.dart';
import 'protocol.dart';
import 'types.dart';

/// One entry of a dynamic block (dynamic/keyed memory is part of the Register service).
class DynField {
  final int index;
  BlockMeta meta;
  List<int> value;

  DynField({required this.index, required this.meta, required this.value});

  bool get readOnly => meta.readOnly;
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

  /// The u16 words of a CID 0 stream. The wire pads the payload to a 4-byte multiple, which is
  /// at most one extra word, and the entries are ordered - so a legitimate zero can only be the
  /// *first* one (the System block, or field 0 key 0). A trailing zero is padding.
  static List<int> _streamWords(List<int> reply) {
    final words = <int>[
      for (var i = 0; i + 1 < reply.length; i += 2) reply[i] | (reply[i + 1] << 8),
    ];
    if (words.length > 1 && words.last == 0) words.removeLast();
    return words;
  }

  /// Enumerate the present block types with their highest instance index (CID 0, empty
  /// request, Docs/Services/Register.md). The reply is a stream of packed
  /// `(type << 6) | maxInstance` words; a type with no instances is absent, and the
  /// trailing wire padding shows up as a zero word (a real type is never 0).
  Future<List<({int type, int maxInstance})>?> enumerateBlockTypes() async {
    final reply = await request(0, payload: const []);
    if (reply == null) return null;
    return [
      for (final w in _streamWords(reply))
        (type: (w >> 6) & 0x3FF, maxInstance: w & 0x3F),
    ];
  }

  /// The instance indexes of a block type. Static and dynamic blocks are dense
  /// (`0..maxInstance`); scripts are sparse, so their slots come from the Script service
  /// (CID 0 lists the loaded file ids, which equal the loaded slots).
  Future<List<int>?> enumerateInstanceIds(int blockType,
      {List<int>? scriptSlots}) async {
    if (blockType == BlockType.script.value) return scriptSlots ?? const [];
    final types = await enumerateBlockTypes();
    if (types == null) return null;
    for (final t in types) {
      if (t.type == blockType) {
        return [for (var i = 0; i <= t.maxInstance; i++) i];
      }
    }
    return const [];
  }

  /// How many instances a block type has (0 when the type has none).
  Future<int?> getInstanceCount(int blockType, {List<int>? scriptSlots}) async {
    final ids = await enumerateInstanceIds(blockType, scriptSlots: scriptSlots);
    return ids?.length;
  }

  /// The block instance's fields and keys (CID 0, packed `(type << 6) | instance`), as a
  /// stream of `Field&Key` words (field in the high byte, key in the low), ascending.
  ///
  /// The reply depends only on the type for static and System blocks (they share a schema),
  /// so those are cached; dynamic blocks and loaded scripts are per instance.
  final Map<int, List<int>> _fieldKeysCache = {};

  Future<List<int>?> enumerateFieldKeys(int blockType, int instance) async {
    final typeInvariant = blockType != BlockType.dynamic.value &&
        blockType != BlockType.script.value &&
        blockType != 0; // the System block's list is fixed too, but it is one instance
    if (typeInvariant && _fieldKeysCache.containsKey(blockType)) {
      return _fieldKeysCache[blockType];
    }
    final packed = ((blockType & 0x3FF) << 6) | (instance & 0x3F);
    final reply = await request(0,
        payload: [packed & 0xFF, (packed >> 8) & 0xFF, 0, 0]);
    if (reply == null) return null;
    final words = _streamWords(reply);
    if (typeInvariant) _fieldKeysCache[blockType] = words;
    return words;
  }

  /// The distinct field indexes of an instance, in ascending order.
  Future<List<int>?> enumerateFieldIndexes(int blockType, int instance) async {
    final keys = await enumerateFieldKeys(blockType, instance);
    if (keys == null) return null;
    final fields = <int>[];
    for (final fk in keys) {
      final f = fk >> 8;
      if (fields.isEmpty || fields.last != f) fields.add(f);
    }
    return fields;
  }

  /// The keys present at `field` of an instance (key 0 when that field is not keyed).
  Future<List<int>?> enumerateKeys(int blockType, int instance, int field) async {
    final keys = await enumerateFieldKeys(blockType, instance);
    if (keys == null) return null;
    return [for (final fk in keys) if ((fk >> 8) == field) fk & 0xFF];
  }

  // ===== thin wrappers the call sites still use (the enumeration is two requests now) =====

  /// The number of distinct fields in a block instance.
  Future<int?> getFieldCount(int blockType, int instance) async =>
      (await enumerateFieldIndexes(blockType, instance))?.length;

  /// The keys present at (instance, field). An unkeyed field reports `[0]`, which is what the
  /// call sites defaulted to for the old "no keys" reply.
  Future<List<int>?> getBlockKeys(int blockType, int instance, int field) async =>
      enumerateKeys(blockType, instance, field);

  /// The distinct field indexes of a dynamic block instance.
  Future<List<int>?> getDynamicFields(int inst) async =>
      enumerateFieldIndexes(BlockType.dynamic.value, inst);

  /// The keys present at (inst, field) of a dynamic block instance.
  Future<List<int>?> getDynamicKeys(int inst, int field) async =>
      enumerateKeys(BlockType.dynamic.value, inst, field);

  /// Read block meta + name (CID 1).
  Future<({BlockMeta meta, String name})?> readBlockMeta(int blockType, int instance) async {
    // For block meta, we need type|inst|field=0xFF|key=0
    final payload = blockInfoBytes(blockType, instance, 0xFF, 0);
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
    final payload = blockInfoBytes(0, 0, field, key);
    final reply = await request(1, payload: payload);
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    return (meta: meta, value: valueSlice(reply, meta.size));
  }

  /// Read field value (CID 1) for a specific block type and instance.
  Future<({BlockMeta meta, List<int> value})?> readBlockField(int blockType, int instance, int field, int key) async {
    final payload = blockInfoBytes(blockType, instance, field, key);
    final reply = await request(1, payload: payload);
    if (reply == null || reply.length < 8) return null;
    // The wire ValueInfo has no key, so the one we asked with goes back into the meta.
    final meta = BlockMeta.fromBytes(reply, 4, key);
    return (meta: meta, value: valueSlice(reply, meta.size));
  }

  /// Write field value (CID 2) for a specific block type and instance (static/dynamic blocks).
  /// Payload: BlockInfo (4) + BlockMeta (4) + value
  Future<List<int>?> writeBlockField(int blockType, int instance, int field, int key, BlockMeta meta, List<int> value) async {
    final payload = [
      ...blockInfoBytes(blockType, instance, field, key),
      ...meta.toBytes(),
      ...value,
    ];
    final reply = await request(2, payload: payload);
    if (reply == null || reply.length < 8) return null;
    final echoMeta = BlockMeta.fromBytes(reply, 4);
    return valueSlice(reply, echoMeta.size);
  }

  /// Reads all blocks (static + dynamic + loaded scripts) by enumerating types/instances.
  /// The System block (type 0, inst 0) is a virtual block not in the static registry.
  /// Dynamic blocks (0x3FF) and script blocks (0x3FE) are not returned by enumerateBlockTypes.
  Future<List<({int type, int inst, BlockMeta meta, String name})?>?> readBlocks(
      {List<int>? scriptSlots}) async {
    final types = await enumerateBlockTypes();
    if (types == null) return null;
    final blocks = <({int type, int inst, BlockMeta meta, String name})?>[];

    // System block (type 0, inst 0): a virtual block not present in the static registry.
    final sysBlock = await readBlockMeta(0, 0);
    if (sysBlock != null) {
      blocks.add((type: 0, inst: 0, meta: sysBlock.meta, name: 'System'));
    }

    Future<void> addType(int type, {List<int>? ids}) async {
      final list = ids ?? await enumerateInstanceIds(type);
      if (list == null) return;
      for (final inst in list) {
        final block = await readBlockMeta(type, inst);
        if (block != null) {
          blocks.add((type: type, inst: inst, meta: block.meta, name: block.name));
        }
      }
    }

    // The type list is the registry's first-seen order, which STATLOG indexes depend on -
    // keep it, then append the dynamic and script memories the firmware omits.
    // The type list carries every static type *and* the dynamic memory when it has instances;
    // only the script blocks are missing (the Script service owns their sparse slots).
    for (final t in types) {
      if (t.type == 0) continue; // the System block is added above
      await addType(t.type);
    }
    await addType(BlockType.script.value, ids: scriptSlots ?? const []);

    return blocks;
  }

  // ===========================================================================
  // Dynamic (and keyed) memory - fully part of the Register service
  // (Docs/Services/Register.md "Dynamic blocks", CIDs 0x10-0x15).
  // ===========================================================================

  static Uint8List _dynBi(int inst, int field, [int key = 0]) =>
      blockInfoBytes(BlockType.dynamic.value, inst, field, key);

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

  /// Enumerates a dynamic block's distinct field indexes (CID 0, Enum 2).
  /// Reads one dynamic entry's current value (CID 1) at (field, key).
  Future<DynField?> readDynamicField(DynBlock block, int field, [int key = 0]) async {
    final reply = await request(1, payload: _dynBi(block.index, field, key));
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4, key);
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
      {DataType? newType, int? key}) async {
    final meta = BlockMeta(
      flagsAndType: newType != null
          ? (field.meta.flags & FieldFlags.mask) | newType.value
          : field.meta.flagsAndType,
      key: key ?? field.meta.key,
      size: newValue.length,
    );
    return _writeDynamicValue(_dynBi(block.index, field.index, key ?? field.meta.key), meta, newValue);
  }

  /// Writes one (field, key) entry (CID 2), creating/updating it in the sorted table.
  /// Writing DataType.none deletes the entry (docs: "Setting the type to None deletes").
  Future<List<int>?> writeDynamicEntry(
      DynBlock block, int field, int key, BlockMeta meta, List<int> value) async {
    final sized = BlockMeta(flagsAndType: meta.flagsAndType, key: key, size: value.length);
    return _writeDynamicValue(_dynBi(block.index, field, key), sized, value);
  }

  /// Creates a new dynamic block (CID 0x10). Returns the assigned block index.
  /// When [index] is null the block is APPENDED right after the last live block (never
  /// at position 0, and ignoring trailing tombstones).
  Future<int?> createDynamicBlock(BlockType type, String name, {int? index}) async {
    final nameBytes = name.codeUnits.take(24).toList();
    while (nameBytes.length < 4) {
      nameBytes.add(0x20); // pad with spaces
    }
    final typeBits = type.value & 0x3FF;
    var target = index;
    if (target == null) {
      // Append = one past the highest LIVE block (tombstones are skipped).
      final count = await getInstanceCount(BlockType.dynamic.value) ?? 0;
      var maxLive = -1;
      for (var i = 0; i < count; i++) {
        final m = await readDynamicBlockMeta(i);
        if (m != null && m.meta.typeValue != BlockType.none.value) maxLive = i;
      }
      target = maxLive + 1;
    }
    final bi = (typeBits << 22) | ((target & 0x3F) << 16) | (0xFF << 8) | 0xFF;
    final reply = await request(0x10, timeout: const Duration(seconds: 5), payload: [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF,
      ...nameBytes,
    ]);
    if (reply == null || reply.length < 5) return null;
    return reply[0]; // BlockIndex echo, block byte
  }

  /// Sets a dynamic block's name and/or type (CID 2, field 0xFF = block meta).
  Future<bool> writeDynamicBlockMeta(DynBlock block, String name, BlockType? type) async {
    final nameBytes = name.codeUnits.take(24).toList();
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

  /// Sets the Read-only / Persistent flags of one (field, key) entry in place,
  /// preserving its type and value.
  Future<List<int>?> setDynamicEntryFlags(DynBlock block, int field, int key,
      {bool? readOnly, bool? persistent}) async {
    final e = await readDynamicField(block, field, key);
    if (e == null) return null;
    final curRO = (e.meta.flags & FieldFlags.readOnly) != 0;
    final curPer = (e.meta.flags & FieldFlags.persistent) != 0;
    final newRO = readOnly ?? curRO;
    final newPer = persistent ?? curPer;
    if (newRO == curRO && newPer == curPer) return e.value;
    final nf = (e.meta.flagsAndType & ~FieldFlags.mask) |
        (newRO ? FieldFlags.readOnly : 0) | (newPer ? FieldFlags.persistent : 0);
    return writeDynamicEntry(block, field, key,
        BlockMeta(flagsAndType: nf, key: key, size: e.value.length), e.value);
  }

  /// Deletes a dynamic block (tombstone), a field, or a (field, key) entry - CID 0x11.
  /// Field 0xFF = whole block; key 0xFF = the whole field; otherwise the entry.
  Future<bool> deleteDynamic({required int block, int? field, int? key}) async {
    final reply = await request(0x11, payload: _dynBi(block, field ?? 0xFF, key ?? 0xFF));
    return reply != null;
  }

  /// Reorders the live dynamic blocks to [newOrder] (the desired order of the live
  /// block indexes) using position ops: reads every block's content, tombstones all
  /// dynamic positions, then recreates them at positions 0..L-1 in the new order
  /// (tombstone slots are filled; leftover tombstones stay at the end).
  Future<bool> reorderDynamicBlocks(List<int> newOrder) async {
    final live = <
        ({
          String name,
          BlockType type,
          List<({int field, int key, BlockMeta meta, List<int> value})> entries,
        })>[];
    for (final i in newOrder) {
      final meta = await readDynamicBlockMeta(i);
      if (meta == null || meta.meta.typeValue == BlockType.none.value) continue;
      final entries = <({int field, int key, BlockMeta meta, List<int> value})>[];
      final block = DynBlock(index: i, meta: meta.meta, name: meta.name);
      for (final f in await getDynamicFields(i) ?? <int>[]) {
        for (final k in await getDynamicKeys(i, f) ?? <int>[0]) {
          final e = await readDynamicField(block, f, k);
          if (e != null) entries.add((field: f, key: k, meta: e.meta, value: e.value));
        }
      }
      live.add((name: meta.name, type: meta.meta.blockType, entries: entries));
    }
    final count = await getInstanceCount(BlockType.dynamic.value) ?? 0;
    for (var i = 0; i < count; i++) {
      await deleteDynamic(block: i);
    }
    for (var pos = 0; pos < live.length; pos++) {
      final entry = live[pos];
      final idx = await createDynamicBlock(entry.type, entry.name, index: pos);
      if (idx == null) return false;
      final b = DynBlock(
          index: pos,
          meta: BlockMeta(
              flagsAndType: entry.type.value, size: entry.entries.length),
          name: entry.name);
      for (final e in entry.entries) {
        final ok = await writeDynamicEntry(
            b, e.field, e.key, e.meta, e.value);
        if (ok == null) return false;
      }
    }
    return true;
  }

  /// Reorders a block's FIELDS: [newOrder] is the desired field order (old field
  /// indexes). Reads every entry, deletes all fields, then rewrites each entry under
  /// the remapped field index (position in newOrder).
  Future<bool> reorderDynamicFields(DynBlock block, List<int> newOrder) async {
    final all = <({int field, int key, BlockMeta meta, List<int> value})>[];
    for (final f in await getDynamicFields(block.index) ?? <int>[]) {
      for (final k in await getDynamicKeys(block.index, f) ?? <int>[0]) {
        final e = await readDynamicField(block, f, k);
        if (e != null) all.add((field: f, key: k, meta: e.meta, value: e.value));
      }
    }
    for (final f in await getDynamicFields(block.index) ?? <int>[]) {
      await deleteDynamic(block: block.index, field: f);
    }
    for (var pos = 0; pos < newOrder.length; pos++) {
      final oldField = newOrder[pos];
      for (final e in all.where((e) => e.field == oldField)) {
        final ok = await writeDynamicEntry(block, pos, e.key, e.meta, e.value);
        if (ok == null) return false;
      }
    }
    return true;
  }

  /// Moves the live block at [fromIndex] to [toIndex] (registry positions), shifting
  /// the others. Returns false when either position is invalid.
  Future<bool> moveDynamicBlockTo(int fromIndex, int toIndex) async {
    if (fromIndex == toIndex) return true;
    final count = await getInstanceCount(BlockType.dynamic.value) ?? 0;
    final live = <int>[];
    for (var i = 0; i < count; i++) {
      final m = await readDynamicBlockMeta(i);
      if (m != null && m.meta.typeValue != BlockType.none.value) live.add(i);
    }
    if (!live.contains(fromIndex) || toIndex < 0 || toIndex > live.length) return false;
    live.remove(fromIndex);
    live.insert(toIndex, fromIndex);
    return reorderDynamicBlocks(live);
  }

  /// Re-numbers a field from [oldField] to [newField] (rewrites every entry at the new
  /// index and removes the old). Returns false when newField collides.
  Future<bool> setDynamicFieldIndex(DynBlock block, int oldField, int newField) async {
    if (oldField == newField) return true;
    final existing = await getDynamicFields(block.index) ?? <int>[];
    if (existing.contains(newField)) return false;
    final all = <({int key, BlockMeta meta, List<int> value})>[];
    for (final k in await getDynamicKeys(block.index, oldField) ?? <int>[]) {
      final e = await readDynamicField(block, oldField, k);
      if (e != null) all.add((key: k, meta: e.meta, value: e.value));
    }
    for (final e in all) {
      if (await writeDynamicEntry(block, newField, e.key, e.meta, e.value) == null) {
        return false;
      }
    }
    for (final k in await getDynamicKeys(block.index, oldField) ?? <int>[]) {
      await deleteDynamic(block: block.index, field: oldField, key: k);
    }
    return true;
  }

  /// Saves the whole device to its backup (CID 4 "Save All"): the System block's persistent
  /// fields, every static block, and every dynamic block's DT_/DV_ files. Per
  /// Docs/Services/Register.md the command carries no BlockInfo; partial saving is the app's
  /// job (direct file writes - see `saveFieldToBackup`).
  Future<bool> saveAll() async {
    // Writing every dynamic block runs the 64-slot orphan cleanup (flash page erases), which
    // can exceed the default request timeout.
    final reply = await request(4, payload: const [], timeout: const Duration(seconds: 25));
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  /// Recalls the whole device from its backup (CID 3 "Recall All"), the counterpart of
  /// [saveAll]. Partial recall is the app's job (register writes of the stored values).
  Future<bool> recallAll() async {
    final reply = await request(3, payload: const [], timeout: const Duration(seconds: 25));
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }
}