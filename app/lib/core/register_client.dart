/// Register service client (Docs/Services/Register.md).
///
/// Uses BlockInfo (Type10|Inst6|Field8|Key8) format instead of BlockIndex.
library;

import 'dart:typed_data';

import 'protocol.dart';
import 'request_channel.dart';
import 'service_request.dart';
import 'system_schema.dart';
import 'types.dart';

/// One block instance a device reported: its type, instance, own metadata and name.
typedef ReportedBlock = ({int type, int inst, ValueInfo meta, String name});

/// What [RegisterClient.readBlocks] found: the blocks, and the block types the device did not
/// answer for.
///
/// [unreadTypes] is NOT a list of absent blocks. A type lands there when an instance's metadata
/// read came back with no answer even after the bounded retry, so as far as anyone knows the
/// device still carries it. A caller that decides device SHAPE (a restore's target match) has to
/// read such a type as *unread*, never as *absent*: on 2026-10-22 a healthy rig was refused as
/// "the target carries no Button block" out of exactly that confusion, one lost reply in a block
/// walk that had no retry.
typedef BlocksRead = ({List<ReportedBlock> blocks, Set<int> unreadTypes});

/// One entry of a dynamic block (dynamic/keyed memory is part of the Register service).
class DynField {
  final int index;
  ValueInfo meta;
  List<int> value;

  DynField({required this.index, required this.meta, required this.value});
}

/// A user-created dynamic memory block.
class DynBlock {
  final int index;
  final ValueInfo meta;
  String name;

  DynBlock({required this.index, required this.meta, required this.name});

  BlockType get blockType => meta.blockType;
  int get fieldCount => meta.size;
}

/// Register service client (Docs/Services/Register.md).
///
/// Uses BlockInfo (Type10|Inst6|Field8|Key8) format instead of BlockIndex.
class RegisterClient {
  final int deviceId;

  RegisterClient({required this.deviceId, this.readAttempts = 1});

  /// The first failure of this client's requests since the caller last cleared it, so a caller that
  /// must report WHY a write did not land can say what the device answered (see [RequestTrace]).
  final RequestTrace trace = RequestTrace();

  /// How many times a READ is re-sent before the caller treats the reply as lost.
  ///
  /// Default 1 - a read is sent once, as it always was, and no timer is scheduled - because the
  /// UI polls a device that may simply be unplugged, and a polling refresh must not slow down
  /// three-fold for a reply that is never going to come.
  ///
  /// A caller that builds a SNAPSHOT (a capture, a restore's live walk) passes
  /// [snapshotReadAttempts] instead, because there a lost reply does not read as "no data": it
  /// reads as a device carrying fewer blocks than it does, or as a device of the wrong type.
  final int readAttempts;

  /// The attempt bound a SNAPSHOT read uses.
  ///
  /// The RS link is half-duplex and a reply can be lost to a collision or to a reset still in
  /// progress. [readBlocks] skips an instance whose metadata read came back null, so a device
  /// that loses one reply looks exactly like a device with a shorter block list - and on
  /// 2026-10-22 a healthy rig was refused as "the target carries no Button block" that way.
  /// The bound is the core ping's order of magnitude (`tool/upload_setup.dart` `_awaitCore`
  /// re-sends 20 times, 250 ms apart, with an 800 ms timeout): 3 attempts of the 2 s request
  /// timeout, the same 250 ms apart, spans at most 3 x 2 s + 2 x 250 ms = 6.5 s.
  static const int snapshotReadAttempts = 3;

  /// The gap between two attempts of [requestRead].
  static const Duration readRetryDelay = Duration(milliseconds: 250);

  Duration get _requestTimeout => const Duration(seconds: 2);

  /// Sends a request to the Register service and returns the reply payload.
  Future<List<int>?> request(
    int cid, {
    List<int> payload = const [],
    Duration? timeout,
  }) => serviceRequest(
    deviceId,
    ServiceType.register,
    cid,
    payload: payload,
    timeout: timeout,
    defaultTimeout: _requestTimeout,
    tag: 'register',
    trace: trace,
  );

  /// Sends a READ - a CID that returns data and changes nothing - and re-sends it while no reply
  /// arrives, so one lost reply does not read as empty data. Returns null only when every
  /// attempt went unanswered.
  ///
  /// Writes go through [request] unchanged: a re-sent write is not a safe thing to guess at.
  Future<List<int>?> requestRead(
    int cid, {
    List<int> payload = const [],
    Duration? timeout,
  }) async {
    for (var attempt = 1; attempt <= readAttempts; attempt++) {
      final reply = await request(cid, payload: payload, timeout: timeout);
      if (reply != null) return reply;
      if (attempt < readAttempts) await Future<void>.delayed(readRetryDelay);
    }
    return null;
  }

  /// Like [request], but keeps the packet-level SUCCESS/FAIL flags alongside the payload.
  /// The flag-answering commands (dynamic create/set-name/delete, Save/Recall All) report
  /// their status there, not in a payload byte.
  Future<PacketResponse?> requestWithFlags(
    int cid, {
    List<int> payload = const [],
    Duration? timeout,
  }) => serviceRequestWithFlags(
    deviceId,
    ServiceType.register,
    cid,
    payload: payload,
    timeout: timeout,
    defaultTimeout: _requestTimeout,
    tag: 'register',
    trace: trace,
  );

  /// True when a flag-answering command replies FLAG_SUCCESS; a transport failure or a
  /// FLAG_FAIL reply is false.
  Future<bool> _requestOk(
    int cid, {
    List<int> payload = const [],
    Duration? timeout,
  }) async =>
      (await requestWithFlags(cid, payload: payload, timeout: timeout))?.ok ??
      false;

  /// Slices the value bytes after a ValueInfo header, clamped to the declared size
  /// (the reply may carry service-level padding past the value).
  static List<int> valueSlice(List<int> reply, int size) {
    if (size <= 0) return <int>[];
    final avail = reply.length - 8;
    return reply.sublist(8, 8 + ((size > avail) ? avail : size));
  }

  /// Decodes a block name: a fixed 16-char field, null-padded (Docs "Dynamic Block Table").
  static String _blockName(List<int> bytes) => decodePaddedString(bytes);

  /// Encodes a block name into the fixed 16-char field (null-padded).
  static List<int> _padBlockName(String name) {
    final out = name.codeUnits.take(16).toList();
    while (out.length < 16) {
      out.add(0x00);
    }
    return out;
  }

  /// The u16 words of a CID 0 stream. The register service emits whole words; a trailing zero
  /// can only ever be padding, because the entries are ordered and a legitimate zero (the System
  /// block, or field 0 key 0) is always *first*.
  static List<int> _streamWords(List<int> reply) {
    final words = <int>[
      for (var i = 0; i + 1 < reply.length; i += 2)
        reply[i] | (reply[i + 1] << 8),
    ];
    if (words.length > 1 && words.last == 0) words.removeLast();
    return words;
  }

  /// The block types currently present, with their highest instance index. Cached for the
  /// session because the static registry is fixed; dynamic mutations call
  /// [invalidateBlockTypes].
  List<({int type, int maxInstance})>? _blockTypesCache;

  /// Enumerate the present block types with their highest instance index (CID 0, empty
  /// request, Docs/Services/Register.md). The reply is a stream of packed words; a type with no
  /// instances is absent (a real type is never 0).
  ///
  /// Static/System types use the normal **10.6** split `(type << 6) | maxInstance`. The banked
  /// dynamic range is a single entry in an **8.8** split: the high byte is the owning bank type's
  /// low byte (0xF0-0xF3) and the low byte the highest occupied global index (0..255).
  Future<List<({int type, int maxInstance})>?> enumerateBlockTypes() async {
    final cached = _blockTypesCache;
    if (cached != null) return cached;
    final reply = await requestRead(
      RegisterCid.enumerateBlocks,
      payload: const [],
    );
    if (reply == null) return null;
    final types = [
      for (final w in _streamWords(reply))
        if ((w >> 8) >= 0xF0) // banked: reconstruct the 0x3F0-0x3F7 type
          (type: 0x300 | (w >> 8), maxInstance: w & 0xFF)
        else
          (type: (w >> 6) & 0x3FF, maxInstance: w & 0x3F),
    ];
    _blockTypesCache = types;
    return types;
  }

  /// Drops the cached block-type enumeration (a dynamic create/delete changes it).
  void invalidateBlockTypes() => _blockTypesCache = null;

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
  Future<List<int>?> enumerateInstanceIds(
    int blockType, {
    List<int>? scriptSlots,
  }) async {
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

  /// The block instance's fields and keys (CID 0, packed `(type << 6) | instance`), as a
  /// stream of `Field&Key` words (field in the high byte, key in the low), ascending.
  ///
  /// The reply depends only on the type for static and System blocks (they share a schema),
  /// so those are cached; dynamic blocks and loaded scripts are per instance.
  final Map<int, List<int>> _fieldKeysCache = {};

  Future<List<int>?> enumerateFieldKeys(int blockType, int instance) async {
    final typeInvariant =
        !isDynamicType(blockType) &&
        !isScriptType(blockType) &&
        blockType !=
            systemBlockTypeValue; // the System block's list is fixed too, but it is one instance
    if (typeInvariant && _fieldKeysCache.containsKey(blockType)) {
      return _fieldKeysCache[blockType];
    }
    final packed = ((blockType & 0x3FF) << 6) | (instance & 0x3F);
    final reply = await requestRead(
      RegisterCid.enumerateFields,
      payload: [packed & 0xFF, (packed >> 8) & 0xFF, 0, 0],
    );
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
  Future<List<int>?> enumerateKeys(
    int blockType,
    int instance,
    int field,
  ) async {
    final keys = await enumerateFieldKeys(blockType, instance);
    if (keys == null) return null;
    return [
      for (final fk in keys)
        if ((fk >> 8) == field) fk & 0xFF,
    ];
  }

  // ===== thin wrappers the call sites still use (the enumeration is two requests now) =====

  /// The keys present at (instance, field). An unkeyed field reports `[0]`, which is what the
  /// call sites defaulted to for the old "no keys" reply.
  Future<List<int>?> getBlockKeys(
    int blockType,
    int instance,
    int field,
  ) async => enumerateKeys(blockType, instance, field);

  /// The distinct field indexes of a dynamic block (addressed by its global index).
  Future<List<int>?> getDynamicFields(int global) async =>
      enumerateFieldIndexes(
        dynamicTypeForIndex(global),
        dynamicInstanceForIndex(global),
      );

  /// The keys present at (global, field) of a dynamic block.
  Future<List<int>?> getDynamicKeys(int global, int field) async =>
      enumerateKeys(
        dynamicTypeForIndex(global),
        dynamicInstanceForIndex(global),
        field,
      );

  /// Read block meta + name (CID 1).
  Future<({ValueInfo meta, String name})?> readBlockMeta(
    int blockType,
    int instance,
  ) async {
    // For block meta, we need type|inst|field=0xFF|key=0
    final payload = blockInfoBytes(blockType, instance, 0xFF, 0);
    final reply = await requestRead(RegisterCid.read, payload: payload);
    if (reply == null || reply.length < 8) return null;
    final meta = ValueInfo.fromBytes(reply, 4);
    final name = reply.length > 8 ? _blockName(reply.sublist(8)) : '';
    return (meta: meta, name: name);
  }

  /// Read field value (CID 1) for system block (type=0, inst=0). The struct fields (0, 3, 4, 5)
  /// are read whole at key 0 (the struct position never goes on the wire) and the member the
  /// key selects is sliced out; the scalar fields (1, 2, 6, 7) return as-is.
  Future<({ValueInfo meta, List<int> value})?> readField(
    int field,
    int key,
  ) async {
    final member = systemStructFields[field]?[key];
    final payload = blockInfoBytes(
      systemBlockTypeValue,
      0,
      field,
      member != null ? 0 : key,
    );
    final reply = await requestRead(RegisterCid.read, payload: payload);
    if (reply == null || reply.length < 8) return null;
    final meta = ValueInfo.fromBytes(reply, 4);
    var value = valueSlice(reply, meta.size);
    if (member != null) {
      final off = systemStructOffset(field, key) ?? 0;
      if (off + member.size <= value.length) {
        value = value.sublist(off, off + member.size);
      }
      return (
        meta: ValueInfo(
          type: member.type.value,
          size: member.size,
          flags: meta.flags,
          key: key,
        ),
        value: value,
      );
    }
    return (meta: meta, value: value);
  }

  /// Read field value (CID 1) for a specific block type and instance.
  Future<({ValueInfo meta, List<int> value})?> readBlockField(
    int blockType,
    int instance,
    int field,
    int key,
  ) async {
    final payload = blockInfoBytes(blockType, instance, field, key);
    final reply = await requestRead(RegisterCid.read, payload: payload);
    if (reply == null || reply.length < 8) return null;
    // The wire ValueInfo has no key, so the one we asked with goes back into the meta.
    final meta = ValueInfo.fromBytes(reply, 4, key);
    return (meta: meta, value: valueSlice(reply, meta.size));
  }

  /// Write field value (CID 2) for a specific block type and instance (static/dynamic blocks).
  /// Payload: BlockInfo (4) + ValueInfo (4) + value
  Future<List<int>?> writeBlockField(
    int blockType,
    int instance,
    int field,
    int key,
    ValueInfo meta,
    List<int> value,
  ) => _writeDynamicValue(
    blockInfoBytes(blockType, instance, field, key),
    meta,
    value,
  );

  /// Reads all blocks (static + dynamic + loaded scripts) by enumerating types/instances.
  /// The System block (type 0, inst 0) is a virtual block not in the static registry.
  /// The banked dynamic (0x3F0-0x3F3) and script (0x3F4-0x3F7) ranges are handled separately.
  Future<BlocksRead?> readBlocks({List<int>? scriptSlots}) async {
    final types = await enumerateBlockTypes();
    if (types == null) return null;
    final blocks = <ReportedBlock>[];
    final unread = <int>{};

    // System block (type 0, inst 0): a virtual block not present in the static registry.
    final sysBlock = await readBlockMeta(systemBlockTypeValue, 0);
    if (sysBlock != null) {
      blocks.add((
        type: systemBlockTypeValue,
        inst: 0,
        meta: sysBlock.meta,
        name: 'System',
      ));
    } else {
      unread.add(systemBlockTypeValue);
    }

    // Use the already-fetched type list; static instances are dense (0..maxInstance),
    // so no per-type re-enumeration is needed.
    Future<void> addType(({int type, int maxInstance}) t) async {
      for (var inst = 0; inst <= t.maxInstance; inst++) {
        final block = await readBlockMeta(t.type, inst);
        if (block == null) {
          unread.add(t.type);
          continue;
        }
        blocks.add((
          type: t.type,
          inst: inst,
          meta: block.meta,
          name: block.name,
        ));
      }
    }

    // The type list is the registry's first-seen order, which the static `.SV` layout depends
    // on - keep it, then append the dynamic and script memories the firmware omits.
    // The type list carries every static type *and* the dynamic memory when it has instances;
    // only the script blocks are missing (the Script service owns their sparse slots).
    for (final t in types) {
      if (t.type == systemBlockTypeValue) {
        continue; // the System block is added above
      }
      if (isDynamicType(t.type)) {
        // The banked range: iterate the global indices; a block's `inst` is its global index.
        for (var g = 0; g <= t.maxInstance; g++) {
          final block = await readBlockMeta(
            dynamicTypeForIndex(g),
            dynamicInstanceForIndex(g),
          );
          if (block == null) {
            unread.add(t.type);
            continue;
          }
          blocks.add((
            type: dynamicTypeForIndex(g),
            inst: g,
            meta: block.meta,
            name: block.name,
          ));
        }
        continue;
      }
      await addType(t);
    }
    // Loaded scripts: addressed by their global slot (the Script service owns the file↔slot map).
    for (final slot in scriptSlots ?? const []) {
      final block = await readBlockMeta(
        scriptTypeForIndex(slot),
        scriptInstanceForIndex(slot),
      );
      if (block == null) {
        unread.add(scriptTypeForIndex(slot));
        continue;
      }
      blocks.add((
        type: scriptTypeForIndex(slot),
        inst: slot,
        meta: block.meta,
        name: block.name,
      ));
    }

    return (blocks: blocks, unreadTypes: unread);
  }

  /// The persistent fields of each static block type, read from the device (CID 1 field list
  /// + CID 2 per-field ValueInfo). The `.SV` layout is recomputed from these sizes + the
  /// 32-bit alignment rule, so the app does not mirror the firmware's compile-time table.
  Future<StaticFieldLayout> readStaticFieldLayout() async {
    final out = <int, List<({int field, int size, int type})>>{};
    final types = await enumerateBlockTypes();
    if (types == null) return out;
    for (final t in types) {
      if (t.type == systemBlockTypeValue ||
          isDynamicType(t.type) ||
          isScriptType(t.type)) {
        continue;
      }
      final fields = await enumerateFieldIndexes(t.type, 0) ?? const <int>[];
      final list = <({int field, int size, int type})>[];
      for (final f in fields) {
        final r = await readBlockField(t.type, 0, f, 0);
        if (r == null || !r.meta.persistent) continue;
        list.add((field: f, size: r.meta.size, type: r.meta.type));
      }
      if (list.isNotEmpty) out[t.type] = list;
    }
    return out;
  }

  // ===========================================================================
  // Dynamic (and keyed) memory - fully part of the Register service
  // (Docs/Services/Register.md "Dynamic blocks", CIDs 0x10-0x13).
  // ===========================================================================

  // A dynamic block is addressed by its global index (0..255); the bank type is derived.
  static Uint8List _dynBi(int global, int field, [int key = 0]) =>
      blockInfoBytes(
        dynamicTypeForIndex(global),
        dynamicInstanceForIndex(global),
        field,
        key,
      );

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

  /// Reads one dynamic block's meta + name. The meta (type + field count) comes from the basic
  /// Read at field `0xFF`; the name comes from the Dynamic `Get Name` command (CID 0x12), which
  /// returns the fixed 16-char name. Pass `withName: false` to skip that second round trip when
  /// only [DynBlock.meta] is needed.
  Future<DynBlock?> readDynamicBlockMeta(
    int block, {
    bool withName = true,
  }) async {
    final reply = await requestRead(
      RegisterCid.read,
      payload: _dynBi(block, 0xFF),
    );
    if (reply == null || reply.length < 8) return null;
    final meta = ValueInfo.fromBytes(reply, 4);

    var name = '';
    if (withName) {
      // The name is a fixed 16-char field (Docs "Dynamic Block Table"), read by CID 0x12.
      final nameReply = await requestRead(
        DynamicCid.getName,
        payload: [block & 0xFF, (block >> 8) & 0xFF],
      );
      if (nameReply != null && nameReply.length >= 16) {
        name = _blockName(nameReply);
      }
    }
    return DynBlock(index: block, meta: meta, name: name);
  }

  /// Reads one dynamic entry's current value (CID 1) at (field, key).
  Future<DynField?> readDynamicField(
    DynBlock block,
    int field, [
    int key = 0,
  ]) async {
    final reply = await requestRead(
      RegisterCid.read,
      payload: _dynBi(block.index, field, key),
    );
    if (reply == null || reply.length < 8) return null;
    final meta = ValueInfo.fromBytes(reply, 4, key);
    return DynField(
      index: field,
      meta: meta,
      value: valueSlice(reply, meta.size),
    );
  }

  /// Writes a dynamic entry (CID 2). Writing type Deleted marks it for deletion.
  Future<List<int>?> writeDynamicField(
    DynBlock block,
    DynField field,
    List<int> newValue, {
    DataType? newType,
    int? key,
  }) async {
    final meta = ValueInfo(
      type: newType != null ? newType.value : field.meta.type,
      flags: field.meta.flags,
      key: key ?? field.meta.key,
      size: newValue.length,
    );
    return _writeDynamicValue(
      _dynBi(block.index, field.index, key ?? field.meta.key),
      meta,
      newValue,
    );
  }

  /// Writes one (field, key) entry (CID 2), creating/updating it in the sorted table.
  /// Writing DataType.none deletes the entry (docs: "Setting the type to None deletes").
  Future<List<int>?> writeDynamicEntry(
    DynBlock block,
    int field,
    int key,
    ValueInfo meta,
    List<int> value,
  ) async {
    final sized = ValueInfo(
      type: meta.type,
      flags: meta.flags,
      key: key,
      size: value.length,
    );
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
        final m = await readDynamicBlockMeta(i, withName: false);
        if (m != null && m.meta.type != BlockType.none.value) maxLive = i;
      }
      target = maxLive + 1;
    }
    final ok = await _requestOk(
      DynamicCid.create,
      timeout: const Duration(seconds: 5),
      payload: [target & 0xFF, (target >> 8) & 0xFF, ...nameBytes],
    );
    if (!ok) return null;
    invalidateBlockTypes();
    // The device no longer echoes the BlockIndex; the host already picked the target.
    return target;
  }

  /// Sets a dynamic block's name through the Dynamic `Set Name` command (CID 0x13):
  /// `Index (uint16)` + the fixed 16-char name. [type] is a legacy block-type argument the
  /// command no longer carries (block deletion goes through [deleteDynamic]); it is ignored.
  Future<bool> writeDynamicBlockMeta(
    DynBlock block,
    String name,
    BlockType? type,
  ) async {
    final nameBytes = _padBlockName(name);
    // The command reports success through the packet SUCCESS flag, not a payload byte.
    return _requestOk(
      DynamicCid.setName,
      payload: [block.index & 0xFF, (block.index >> 8) & 0xFF, ...nameBytes],
    );
  }

  /// Appends an entry to a dynamic block (CID 2), or fills the slot at `index`
  /// when given (a None placeholder keeps indexes stable).
  Future<List<int>?> appendDynamicEntry(
    DynBlock block,
    ValueInfo meta,
    List<int> value, {
    int? index,
  }) async {
    final sizedMeta = ValueInfo(
      type: meta.type,
      flags: meta.flags,
      key: meta.key,
      size: value.length,
    );
    final fieldIdx = index ?? block.fieldCount;
    return _writeDynamicValue(
      _dynBi(block.index, fieldIdx),
      sizedMeta,
      value,
      timeout: const Duration(seconds: 4),
    );
  }

  Future<List<int>?> _writeDynamicValue(
    Uint8List bi,
    ValueInfo meta,
    List<int> value, {
    Duration? timeout,
  }) async {
    // Asked WITH the packet flags on purpose: a register write answers FAIL - and carries no payload
    // at all when it does (`RespondStatus(Frame, false)` in the firmware) - so the flag is the only
    // place the refusal is visible. `request` is this same call minus the flags, so nothing about the
    // accepted path changes; what changes is that a refused write can say so (see [RequestTrace]).
    final response = await requestWithFlags(
      RegisterCid.write,
      payload: [...bi, ...meta.toBytes(), ...value],
      timeout: timeout ?? const Duration(seconds: 4),
    );
    // No link or no reply: the request layer has already recorded which.
    if (response == null) return null;
    final reply = response.payload;
    // The device refused it, and the flag - recorded on the trace - is its answer.
    if (response.fail) return null;
    if (reply.length < 8) {
      // The device echoes the ValueInfo of an accepted write, so an answer with no usable ValueInfo
      // is a refusal the flag did not mark.
      trace.unusable(
        deviceId,
        service: ServiceType.register,
        cid: RegisterCid.write,
        payload: reply,
        detail:
            'an accepted write echoes the ValueInfo; this reply has '
            '${reply.length} byte(s)',
      );
      return null;
    }
    final echoMeta = ValueInfo.fromBytes(reply, 4);
    return valueSlice(reply, echoMeta.size);
  }

  /// Sets the Read-only / Persistent flags of one (field, key) entry in place,
  /// preserving its type and value.
  Future<List<int>?> setDynamicEntryFlags(
    DynBlock block,
    int field,
    int key, {
    bool? readOnly,
    bool? persistent,
  }) async {
    final e = await readDynamicField(block, field, key);
    if (e == null) return null;
    final curRO = (e.meta.flags & ValueFlags.readOnly) != 0;
    final curPer = (e.meta.flags & ValueFlags.persistent) != 0;
    final newRO = readOnly ?? curRO;
    final newPer = persistent ?? curPer;
    if (newRO == curRO && newPer == curPer) return e.value;
    final nf =
        (e.meta.flags & ~(ValueFlags.readOnly | ValueFlags.persistent)) |
        (newRO ? ValueFlags.readOnly : 0) |
        (newPer ? ValueFlags.persistent : 0);
    return writeDynamicEntry(
      block,
      field,
      key,
      ValueInfo(type: e.meta.type, flags: nf, key: key, size: e.value.length),
      e.value,
    );
  }

  /// Deletes a dynamic block (tombstone), a field, or a (field, key) entry.
  ///
  /// The whole block goes through the Dynamic Delete command, whose request is the global
  /// `Index (uint16)` (Docs/Services/Register.md "Dynamic commands"). Entry/field deletion
  /// is the basic Write with type None, the documented way to delete here.
  Future<bool> deleteDynamic({required int block, int? field, int? key}) async {
    if (field == null) {
      // The command reports success through the packet SUCCESS flag, not a payload byte.
      final ok = await _requestOk(
        DynamicCid.delete,
        payload: [block & 0xFF, (block >> 8) & 0xFF],
      );
      if (ok) invalidateBlockTypes();
      return ok;
    }
    final dyn = DynBlock(
      index: block,
      meta: ValueInfo(type: dynamicTypeForIndex(block)),
      name: '',
    );
    if (key != null) {
      final e = await readDynamicField(dyn, field, key);
      if (e == null) return false;
      return await writeDynamicField(
            dyn,
            e,
            const [],
            newType: DataType.none,
            key: key,
          ) !=
          null;
    }
    // Delete every key at the field (nothing to delete is a success).
    var ok = true;
    for (final k in await getDynamicKeys(block, field) ?? const <int>[]) {
      final e = await readDynamicField(dyn, field, k);
      if (e == null ||
          await writeDynamicField(
                dyn,
                e,
                const [],
                newType: DataType.none,
                key: k,
              ) ==
              null) {
        ok = false;
      }
    }
    return ok;
  }

  /// Reorders the live dynamic blocks to [newOrder] (the desired order of the live
  /// block indexes) using position ops: reads every block's content, tombstones all
  /// dynamic positions, then recreates them at positions 0..L-1 in the new order
  /// (tombstone slots are filled; leftover tombstones stay at the end).
  Future<bool> reorderDynamicBlocks(List<int> newOrder) async {
    final live =
        <
          ({
            String name,
            BlockType type,
            List<({int field, int key, ValueInfo meta, List<int> value})>
            entries,
          })
        >[];
    for (final i in newOrder) {
      final meta = await readDynamicBlockMeta(i);
      if (meta == null || meta.meta.type == BlockType.none.value) continue;
      final entries =
          <({int field, int key, ValueInfo meta, List<int> value})>[];
      final block = DynBlock(index: i, meta: meta.meta, name: meta.name);
      for (final f in await getDynamicFields(i) ?? <int>[]) {
        for (final k in await getDynamicKeys(i, f) ?? <int>[0]) {
          final e = await readDynamicField(block, f, k);
          if (e != null) {
            entries.add((field: f, key: k, meta: e.meta, value: e.value));
          }
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
          type: dynamicTypeForIndex(pos),
          size: entry.entries.length,
        ),
        name: entry.name,
      );
      for (final e in entry.entries) {
        final ok = await writeDynamicEntry(b, e.field, e.key, e.meta, e.value);
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
        if (e != null) {
          all.add((field: f, key: k, meta: e.meta, value: e.value));
        }
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
      final m = await readDynamicBlockMeta(i, withName: false);
      if (m != null && m.meta.type != BlockType.none.value) live.add(i);
    }
    if (!live.contains(fromIndex) || toIndex < 0 || toIndex > live.length) {
      return false;
    }
    live.remove(fromIndex);
    live.insert(toIndex, fromIndex);
    return reorderDynamicBlocks(live);
  }

  /// Re-numbers a field from [oldField] to [newField] (rewrites every entry at the new
  /// index and removes the old). Returns false when newField collides.
  Future<bool> setDynamicFieldIndex(
    DynBlock block,
    int oldField,
    int newField,
  ) async {
    if (oldField == newField) return true;
    final existing = await getDynamicFields(block.index) ?? <int>[];
    if (existing.contains(newField)) return false;
    final all = <({int key, ValueInfo meta, List<int> value})>[];
    for (final k in await getDynamicKeys(block.index, oldField) ?? <int>[]) {
      final e = await readDynamicField(block, oldField, k);
      if (e != null) all.add((key: k, meta: e.meta, value: e.value));
    }
    for (final e in all) {
      if (await writeDynamicEntry(block, newField, e.key, e.meta, e.value) ==
          null) {
        return false;
      }
    }
    for (final k in await getDynamicKeys(block.index, oldField) ?? <int>[]) {
      await deleteDynamic(block: block.index, field: oldField, key: k);
    }
    return true;
  }

  /// Saves the whole device to its backup (CID 4 "Save All"): the System block's persistent
  /// fields, every static block, and every dynamic block's .DT_/.DV_ files. Per
  /// Docs/Services/Register.md the command carries no BlockInfo; partial saving is the app's
  /// job (direct file writes - see `saveFieldToBackup`).
  Future<bool> saveAll() async {
    // Writing every dynamic block runs the 64-slot orphan cleanup (flash page erases), which
    // can exceed the default request timeout.
    return _requestOk(
      RegisterCid.saveAll,
      payload: const [],
      timeout: const Duration(seconds: 25),
    );
  }

  /// Recalls the whole device from its backup (CID 3 "Recall All"), the counterpart of
  /// [saveAll]. Partial recall is the app's job (register writes of the stored values).
  Future<bool> recallAll() async {
    return _requestOk(
      RegisterCid.recallAll,
      payload: const [],
      timeout: const Duration(seconds: 25),
    );
  }
}
