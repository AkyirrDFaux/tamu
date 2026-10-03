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
  ValueInfo meta;
  List<int> value;

  DynField({required this.index, required this.meta, required this.value});

  bool get readOnly => meta.readOnly;
}

/// A user-created dynamic memory block.
class DynBlock {
  final int index;
  final ValueInfo meta;
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

  /// Slices the value bytes after a ValueInfo header, clamped to the declared size
  /// (the wire payload is padded to 4 bytes).
  static List<int> valueSlice(List<int> reply, int size) {
    if (size <= 0) return <int>[];
    final avail = reply.length - 8;
    return reply.sublist(8, 8 + ((size > avail) ? avail : size));
  }

  /// Decodes a block name: a fixed 16-char field, space-padded (Docs "Dynamic Block Table").
  static String _blockName(List<int> bytes) =>
      String.fromCharCodes(bytes).replaceAll('\x00', '').trimRight();

  /// Encodes a block name into the fixed 16-char field (space-padded).
  static List<int> _padBlockName(String name) {
    final out = name.codeUnits.take(16).toList();
    while (out.length < 16) {
      out.add(0x20);
    }
    return out;
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
  /// request, Docs/Services/Register.md). The reply is a stream of packed words; a type with no
  /// instances is absent, and the trailing wire padding shows up as a zero word (a real type is
  /// never 0).
  ///
  /// Static/System types use the normal **10.6** split `(type << 6) | maxInstance`. The banked
  /// dynamic range is a single entry in an **8.8** split: the high byte is the owning bank type's
  /// low byte (0xF0-0xF3) and the low byte the highest occupied global index (0..255).
  Future<List<({int type, int maxInstance})>?> enumerateBlockTypes() async {
    final reply = await request(RegisterCid.enumerateBlocks, payload: const []);
    if (reply == null) return null;
    return [
      for (final w in _streamWords(reply))
        if ((w >> 8) >= 0xF0) // banked: reconstruct the 0x3F0-0x3F7 type
          (type: 0x300 | (w >> 8), maxInstance: w & 0xFF)
        else
          (type: (w >> 6) & 0x3FF, maxInstance: w & 0x3F),
    ];
  }

  /// The occupied dynamic global indices (0..255), from the banked 8.8 enumerate entry.
  Future<List<int>?> enumerateDynamicIndices() async {
    final types = await enumerateBlockTypes();
    if (types == null) return null;
    for (final t in types) {
      if (isDynamicType(t.type)) {
        return [for (var g = 0; g <= t.maxInstance; g++) g];
      }
    }
    return const [];
  }

  /// The instance indexes of a block type. Static and dynamic blocks are dense
  /// (`0..maxInstance`); scripts are sparse, so their slots come from the Script service
  /// (CID 0 lists the loaded file ids, which equal the loaded slots).
  Future<List<int>?> enumerateInstanceIds(int blockType,
      {List<int>? scriptSlots}) async {
    if (isScriptType(blockType)) return scriptSlots ?? const [];
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
    final typeInvariant = !isDynamicType(blockType) &&
        !isScriptType(blockType) &&
        blockType != 0; // the System block's list is fixed too, but it is one instance
    if (typeInvariant && _fieldKeysCache.containsKey(blockType)) {
      return _fieldKeysCache[blockType];
    }
    final packed = ((blockType & 0x3FF) << 6) | (instance & 0x3F);
    final reply = await request(RegisterCid.enumerateFields,
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

  /// The distinct field indexes of a dynamic block (addressed by its global index).
  Future<List<int>?> getDynamicFields(int global) async =>
      enumerateFieldIndexes(dynamicTypeForIndex(global), dynamicInstanceForIndex(global));

  /// The keys present at (global, field) of a dynamic block.
  Future<List<int>?> getDynamicKeys(int global, int field) async =>
      enumerateKeys(dynamicTypeForIndex(global), dynamicInstanceForIndex(global), field);

  /// Read block meta + name (CID 1).
  Future<({ValueInfo meta, String name})?> readBlockMeta(int blockType, int instance) async {
    // For block meta, we need type|inst|field=0xFF|key=0
    final payload = blockInfoBytes(blockType, instance, 0xFF, 0);
    final reply = await request(RegisterCid.read, payload: payload);
    if (reply == null || reply.length < 8) return null;
    final meta = ValueInfo.fromBytes(reply, 4);
    final name = reply.length > 8 ? _blockName(reply.sublist(8)) : '';
    return (meta: meta, name: name);
  }

  /// Read field value (CID 1) for system block (type=0, inst=0).
  Future<({ValueInfo meta, List<int> value})?> readField(int field, int key) async {
    final payload = blockInfoBytes(0, 0, field, key);
    final reply = await request(RegisterCid.read, payload: payload);
    if (reply == null || reply.length < 8) return null;
    final meta = ValueInfo.fromBytes(reply, 4);
    return (meta: meta, value: valueSlice(reply, meta.size));
  }

  /// Read field value (CID 1) for a specific block type and instance.
  Future<({ValueInfo meta, List<int> value})?> readBlockField(int blockType, int instance, int field, int key) async {
    final payload = blockInfoBytes(blockType, instance, field, key);
    final reply = await request(RegisterCid.read, payload: payload);
    if (reply == null || reply.length < 8) return null;
    // The wire ValueInfo has no key, so the one we asked with goes back into the meta.
    final meta = ValueInfo.fromBytes(reply, 4, key);
    return (meta: meta, value: valueSlice(reply, meta.size));
  }

  /// Write field value (CID 2) for a specific block type and instance (static/dynamic blocks).
  /// Payload: BlockInfo (4) + ValueInfo (4) + value
  Future<List<int>?> writeBlockField(int blockType, int instance, int field, int key, ValueInfo meta, List<int> value) async {
    final payload = [
      ...blockInfoBytes(blockType, instance, field, key),
      ...meta.toBytes(),
      ...value,
    ];
    final reply = await request(RegisterCid.write, payload: payload);
    if (reply == null || reply.length < 8) return null;
    final echoMeta = ValueInfo.fromBytes(reply, 4);
    return valueSlice(reply, echoMeta.size);
  }

  /// Reads all blocks (static + dynamic + loaded scripts) by enumerating types/instances.
  /// The System block (type 0, inst 0) is a virtual block not in the static registry.
  /// The banked dynamic (0x3F0-0x3F3) and script (0x3F4-0x3F7) ranges are handled separately.
  Future<List<({int type, int inst, ValueInfo meta, String name})?>?> readBlocks(
      {List<int>? scriptSlots}) async {
    final types = await enumerateBlockTypes();
    if (types == null) return null;
    final blocks = <({int type, int inst, ValueInfo meta, String name})?>[];

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

    // The type list is the registry's first-seen order, which the static `.SV` layout depends
    // on - keep it, then append the dynamic and script memories the firmware omits.
    // The type list carries every static type *and* the dynamic memory when it has instances;
    // only the script blocks are missing (the Script service owns their sparse slots).
    for (final t in types) {
      if (t.type == 0) continue; // the System block is added above
      if (isDynamicType(t.type)) {
        // The banked range: iterate the global indices; a block's `inst` is its global index.
        for (var g = 0; g <= t.maxInstance; g++) {
          final block =
              await readBlockMeta(dynamicTypeForIndex(g), dynamicInstanceForIndex(g));
          if (block != null) {
            blocks.add(
                (type: dynamicTypeForIndex(g), inst: g, meta: block.meta, name: block.name));
          }
        }
        continue;
      }
      await addType(t.type);
    }
    // Loaded scripts: addressed by their global slot (the Script service owns the file↔slot map).
    for (final slot in scriptSlots ?? const []) {
      final block =
          await readBlockMeta(scriptTypeForIndex(slot), scriptInstanceForIndex(slot));
      if (block != null) {
        blocks.add(
            (type: scriptTypeForIndex(slot), inst: slot, meta: block.meta, name: block.name));
      }
    }

    return blocks;
  }

  // ===========================================================================
  // Dynamic (and keyed) memory - fully part of the Register service
  // (Docs/Services/Register.md "Dynamic blocks", CIDs 0x10-0x13).
  // ===========================================================================

  // A dynamic block is addressed by its global index (0..255); the bank type is derived.
  static Uint8List _dynBi(int global, int field, [int key = 0]) => blockInfoBytes(
      dynamicTypeForIndex(global), dynamicInstanceForIndex(global), field, key);

  /// Reads the dynamic block list (CID 0 Enum 1 for instances). None/Deleted
  /// blocks are hidden (their index stays reserved until a save compacts).
  Future<List<DynBlock>?> readDynamicBlocks() async {
    final indices = await enumerateDynamicIndices();
    if (indices == null) return null;
    final blocks = <DynBlock>[];
    for (final i in indices) {
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
    final reply = await request(RegisterCid.read, payload: _dynBi(block, 0xFF));
    if (reply == null || reply.length < 8) return null;
    final meta = ValueInfo.fromBytes(reply, 4);
    // The name is a fixed 16-char field, space-padded (Docs "Dynamic Block Table").
    final name = reply.length > 8 ? _blockName(reply.sublist(8)) : '';
    return DynBlock(index: block, meta: meta, name: name);
  }

  /// Enumerates a dynamic block's distinct field indexes (CID 0, Enum 2).
  /// Reads one dynamic entry's current value (CID 1) at (field, key).
  Future<DynField?> readDynamicField(DynBlock block, int field, [int key = 0]) async {
    final reply = await request(RegisterCid.read, payload: _dynBi(block.index, field, key));
    if (reply == null || reply.length < 8) return null;
    final meta = ValueInfo.fromBytes(reply, 4, key);
    return DynField(index: field, meta: meta, value: valueSlice(reply, meta.size));
  }

  /// Writes a dynamic entry (CID 2). Writing type Deleted marks it for deletion.
  Future<List<int>?> writeDynamicField(
      DynBlock block, DynField field, List<int> newValue,
      {DataType? newType, int? key}) async {
    final meta = ValueInfo(
      type: newType != null ? newType.value : field.meta.type,
      flags: field.meta.flags,
      key: key ?? field.meta.key,
      size: newValue.length,
    );
    return _writeDynamicValue(_dynBi(block.index, field.index, key ?? field.meta.key), meta, newValue);
  }

  /// Writes one (field, key) entry (CID 2), creating/updating it in the sorted table.
  /// Writing DataType.none deletes the entry (docs: "Setting the type to None deletes").
  Future<List<int>?> writeDynamicEntry(
      DynBlock block, int field, int key, ValueInfo meta, List<int> value) async {
    final sized = ValueInfo(type: meta.type, flags: meta.flags, key: key, size: value.length);
    return _writeDynamicValue(_dynBi(block.index, field, key), sized, value);
  }

  /// Creates a new dynamic block (CID 0x10). Returns the assigned global index (0..255).
  /// When [index] is null the block is APPENDED right after the last live block (never
  /// at position 0, and ignoring trailing tombstones).
  Future<int?> createDynamicBlock(String name, {int? index}) async {
    final nameBytes = _padBlockName(name);
    var target = index;
    if (target == null) {
      // Append = one past the highest LIVE block (tombstones are skipped).
      final indices = await enumerateDynamicIndices() ?? const [];
      var maxLive = -1;
      for (final i in indices) {
        final m = await readDynamicBlockMeta(i);
        if (m != null && m.meta.type != BlockType.none.value) maxLive = i;
      }
      target = maxLive + 1;
    }
    final typeBits = dynamicTypeForIndex(target) & 0x3FF;
    final bi = (typeBits << 22) | (dynamicInstanceForIndex(target) << 16) | (0xFF << 8) | 0xFF;
    final reply = await request(DynamicCid.create, timeout: const Duration(seconds: 5), payload: [
      bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF,
      ...nameBytes,
    ]);
    if (reply == null || reply.length < 5) return null;
    return reply[0]; // BlockIndex echo, block byte
  }

  /// Sets a dynamic block's name and/or type (CID 2, field 0xFF = block meta).
  Future<bool> writeDynamicBlockMeta(DynBlock block, String name, BlockType? type) async {
    final nameBytes = _padBlockName(name);
    final meta = ValueInfo(
      type: type?.value ?? dynamicTypeForIndex(block.index),
      size: nameBytes.length,
    );
    final reply = await request(RegisterCid.write, payload: [
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
  Future<List<int>?> appendDynamicEntry(DynBlock block, ValueInfo meta, List<int> value,
      {int? index}) async {
    final sizedMeta = ValueInfo(
      type: meta.type,
      flags: meta.flags,
      key: meta.key,
      size: value.length,
    );
    final fieldIdx = index ?? block.fieldCount;
    return _writeDynamicValue(_dynBi(block.index, fieldIdx), sizedMeta, value,
        timeout: const Duration(seconds: 4));
  }

  Future<List<int>?> _writeDynamicValue(Uint8List bi, ValueInfo meta, List<int> value,
      {Duration? timeout}) async {
    final reply = await request(RegisterCid.write, payload: [...bi, ...meta.toBytes(), ...value],
        timeout: timeout ?? const Duration(seconds: 4));
    if (reply == null || reply.length < 8) return null;
    final echoMeta = ValueInfo.fromBytes(reply, 4);
    return valueSlice(reply, echoMeta.size);
  }

  /// Sets the Read-only / Persistent flags of one (field, key) entry in place,
  /// preserving its type and value.
  Future<List<int>?> setDynamicEntryFlags(DynBlock block, int field, int key,
      {bool? readOnly, bool? persistent}) async {
    final e = await readDynamicField(block, field, key);
    if (e == null) return null;
    final curRO = (e.meta.flags & ValueFlags.readOnly) != 0;
    final curPer = (e.meta.flags & ValueFlags.persistent) != 0;
    final newRO = readOnly ?? curRO;
    final newPer = persistent ?? curPer;
    if (newRO == curRO && newPer == curPer) return e.value;
    final nf = (e.meta.flags & ~(ValueFlags.readOnly | ValueFlags.persistent)) |
        (newRO ? ValueFlags.readOnly : 0) | (newPer ? ValueFlags.persistent : 0);
    return writeDynamicEntry(block, field, key,
        ValueInfo(type: e.meta.type, flags: nf, key: key, size: e.value.length), e.value);
  }

  /// Deletes a dynamic block (tombstone), a field, or a (field, key) entry - CID 0x11.
  /// Field 0xFF = whole block; key 0xFF = the whole field; otherwise the entry.
  Future<bool> deleteDynamic({required int block, int? field, int? key}) async {
    final reply = await request(DynamicCid.delete, payload: _dynBi(block, field ?? 0xFF, key ?? 0xFF));
    // The delete replies with a one-byte status (0 = success, 0xFF = refused).
    return reply != null && reply.isNotEmpty && reply[0] == 0;
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
          List<({int field, int key, ValueInfo meta, List<int> value})> entries,
        })>[];
    for (final i in newOrder) {
      final meta = await readDynamicBlockMeta(i);
      if (meta == null || meta.meta.type == BlockType.none.value) continue;
      final entries = <({int field, int key, ValueInfo meta, List<int> value})>[];
      final block = DynBlock(index: i, meta: meta.meta, name: meta.name);
      for (final f in await getDynamicFields(i) ?? <int>[]) {
        for (final k in await getDynamicKeys(i, f) ?? <int>[0]) {
          final e = await readDynamicField(block, f, k);
          if (e != null) entries.add((field: f, key: k, meta: e.meta, value: e.value));
        }
      }
      live.add((name: meta.name, type: meta.meta.blockType, entries: entries));
    }
    final count = (await enumerateDynamicIndices())?.length ?? 0;
    for (var i = 0; i < count; i++) {
      await deleteDynamic(block: i);
    }
    for (var pos = 0; pos < live.length; pos++) {
      final entry = live[pos];
      final idx = await createDynamicBlock(entry.name, index: pos);
      if (idx == null) return false;
      final b = DynBlock(
          index: pos,
          meta: ValueInfo(
              type: dynamicTypeForIndex(pos), size: entry.entries.length),
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
    final all = <({int field, int key, ValueInfo meta, List<int> value})>[];
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
    final indices = await enumerateDynamicIndices() ?? const [];
    final live = <int>[];
    for (final i in indices) {
      final m = await readDynamicBlockMeta(i);
      if (m != null && m.meta.type != BlockType.none.value) live.add(i);
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
    final all = <({int key, ValueInfo meta, List<int> value})>[];
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
    final reply = await request(RegisterCid.saveAll, payload: const [], timeout: const Duration(seconds: 25));
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  /// Recalls the whole device from its backup (CID 3 "Recall All"), the counterpart of
  /// [saveAll]. Partial recall is the app's job (register writes of the stored values).
  Future<bool> recallAll() async {
    final reply = await request(RegisterCid.recallAll, payload: const [], timeout: const Duration(seconds: 25));
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }
}