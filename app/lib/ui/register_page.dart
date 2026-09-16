import 'dart:async';

import 'package:flutter/material.dart';

import '../core/block_registry.dart';
import '../core/connection.dart';
import '../core/device_db.dart';
import '../core/register_client.dart';
import '../core/render_dict.dart' show KeyedEntry, buildKeyedDict, geometryDictType, geometryKeysForShape, isRenderDictType, parseKeyedDict, renderDictKeyName, renderKeyFieldInfo, textureKeysForType;
import '../core/types.dart';
import 'theme.dart';
import 'value_editor.dart' show dataTypeLabel, formatValue, showValueEditor;
import 'system_block_view.dart';import 'widgets.dart';

/// Register service view (Docs/App/Service views/Register.md):
/// System block, static blocks, and dynamic memory.
class RegisterPage extends StatefulWidget {
  final int deviceId;
  final bool hasDynamicMemory;

  const RegisterPage({super.key, required this.deviceId, this.hasDynamicMemory = true});

  @override
  State<RegisterPage> createState() => _RegisterPageState();
}

class _RegisterPageState extends State<RegisterPage>
    with AutoRefreshMixin<RegisterPage> {
  late final RegisterClient _client = RegisterClient(deviceId: widget.deviceId);

  /// Stores [type, instance, meta, name] for each block
  List<({int type, int inst, BlockMeta meta, String name})?>? _blockMetas;
  final Map<int, Map<int, ({BlockMeta meta, List<int> value})?>> _fieldCache = {};
  final Map<int, List<int>> _dynamicFields = {};
  final Map<int, Map<int, List<int>>> _dynamicKeys = {};
  String? _error;
  final Set<int> _expanded = {};
  bool _refreshing = false;
  bool _busy = false;
  bool _editMode = false;

  /// Runs a long operation (Save/Recall) with the busy spinner shown in the app bar.
  Future<bool> _withBusy(Future<bool> Function() task) async {
    if (_busy) return false;
    setState(() => _busy = true);
    try {
      return await task();
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  @override
  void initState() {
    super.initState();
    _refresh();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) applyAuto(const Duration(milliseconds: 2000));
    });
  }

  @override
  Future<void> onAutoRefresh() => _refresh();

  void _snack(String message) {
    if (!mounted) return;
    showSnack(context, message);
  }

  Future<void> _refresh() async {
    if (_refreshing) return;
    _refreshing = true;
    try {
      if (!ConnectionManager.instance.isConnected) return;
      final blocks = await _client.readBlocks();
      if (!mounted) return;
      if (blocks == null) {
        setState(() => _error = 'Device did not respond');
        return;
      }
      _error = null;
      // Preserve cache for blocks that still exist
      final newMetas = <({int type, int inst, BlockMeta meta, String name})?>[];
      final oldCache = Map<int, Map<int, ({BlockMeta meta, List<int> value})?>>.from(_fieldCache);
      _fieldCache.clear();
      for (final b in blocks) {
        if (b == null) { newMetas.add(null); continue; }
        final cacheKey = (b.type << 8) | b.inst;
        if (oldCache.containsKey(cacheKey)) {
          _fieldCache[cacheKey] = oldCache[cacheKey]!;
        }
        newMetas.add(b);
      }
      _blockMetas = newMetas;
      await _loadVisibleFields();
    } finally {
      _refreshing = false;
    }
  }
Future<void> _loadVisibleFields() async {
    final blocks = _blockMetas;
    if (blocks == null) return;
    for (var i = 0; i < blocks.length; i++) {
      if (!_expanded.contains(i)) continue;
      final block = blocks[i];
      if (block != null) {
        await _loadBlockFields(block.type, block.inst, block, forceRefresh: true);
        if (i < blocks.length - 1) await Future.delayed(const Duration(milliseconds: 50));
      }
    }
    if (mounted) setState(() {});

  }

  /// Persists every block to its backup (docs Register.md "Save"): the dynamic
  /// registry, the System block (Name/NetID) and each static block's persistent
  /// fields. Each target is a separate request so failures are reported accurately.
  Future<bool> _saveAll() async {
    final c = _client;
    // The dynamic registry only exists on devices advertising dynamic memory (the DAS
    // has none, so the request would report failure and mark the whole save as failed).
    // Check both the widget flag and the live capability report to stay correct even if
    // the page was opened before capabilities were loaded.
    final dev = DeviceDatabase.instance.byId(widget.deviceId);
    final hasDyn = widget.hasDynamicMemory ||
        (dev != null && dev.capabilities & Capability.dynamicMemory != 0);
    bool ok = true;
    if (hasDyn) {
      ok &= await _requestStatus(c, 3, const [0xFF, 0xFF, 0xFF, 0xFF]); // dynamic registry
    }
    ok &= await _requestStatus(c, 3, blockInfoBytes(0, 0, 0xFF, 0)); // system Name/NetID
    for (final b in _blockMetas ?? const []) {
      if (b == null || b.type == 0 || b.type == 0x3FF) continue;
      ok &= await _requestStatus(c, 3, blockInfoBytes(b.type, b.inst, 0xFF, 0));
    }
    return ok;
  }

  Future<bool> _recallAll() async {
    final c = _client;
    bool ok = true;
    if (widget.hasDynamicMemory) {
      ok &= await _requestStatus(c, 4, const [0xFF, 0xFF, 0xFF, 0xFF]);
    }
    ok &= await _requestStatus(c, 4, blockInfoBytes(0, 0, 0xFF, 0));
    for (final b in _blockMetas ?? const []) {
      if (b == null || b.type == 0 || b.type == 0x3FF) continue;
      ok &= await _requestStatus(c, 4, blockInfoBytes(b.type, b.inst, 0xFF, 0));
    }
    return ok;
  }

  Future<bool> _requestStatus(RegisterClient c, int cid, List<int> payload) async {
    final reply = await c.request(cid, payload: payload);
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  /// Saves one persistent field to its backup (docs System Memory view: "Saveable
  /// values show a Save button"). System/static fields are addressed by BlockInfo.
  Future<void> _saveField(int blockType, int inst,
      ({int type, int inst, BlockMeta meta, String name})? block, int fieldIndex) async {
    final bi = blockInfoBytes(blockType == 0 ? 0 : blockType, inst, fieldIndex, 0);
    final ok = await _withBusy(() async {
      final reply = await _client.request(3, payload: bi);
      return reply != null && reply.isNotEmpty && reply[0] == 0;
    });
    _snack(ok ? 'Saved' : 'Save failed');
  }

  Future<void> _recallField(int blockType, int inst,
      ({int type, int inst, BlockMeta meta, String name})? block, int fieldIndex) async {
    final bi = blockInfoBytes(blockType == 0 ? 0 : blockType, inst, fieldIndex, 0);
    final ok = await _withBusy(() async {
      final reply = await _client.request(4, payload: bi);
      return reply != null && reply.isNotEmpty && reply[0] == 0;
    });
    _snack(ok ? 'Recalled' : 'Recall failed');
    if (ok) {
      await _loadBlockFields(blockType, inst, block, forceRefresh: true);
      if (mounted) setState(() {});
    }
  }

  Future<void> _loadBlockFields(int blockType, int instance, ({int type, int inst, BlockMeta meta, String name})? block, {bool forceRefresh = false}) async {
    if (block == null) return;
    final cacheKey = (blockType << 8) | instance;
    final cache = _fieldCache.putIfAbsent(cacheKey, () => {});
    final fields = _dynamicFields.putIfAbsent(instance, () => <int>[]);

    if (forceRefresh) {
      cache.clear();
      fields.clear();
    }

    // Dynamic blocks use the flat Field&Key model: enumerate the distinct fields and
    // read every (field, key) entry (cached as cache[field*256 + key]).
    if (blockType == BlockType.dynamic.value) {
      final fieldList = await _client.getDynamicFields(instance) ?? <int>[];
      fields
        ..clear()
        ..addAll(fieldList);
      final keyMap = _dynamicKeys.putIfAbsent(instance, () => {});
      for (final f in fieldList) {
        final keys = await _client.getDynamicKeys(instance, f) ?? <int>[0];
        keyMap[f] = keys;
        for (final key in keys) {
          if (!forceRefresh && cache.containsKey(f * 256 + key)) continue;
          final entry = await _client.readBlockField(BlockType.dynamic.value, instance, f, key);
          if (entry != null) {
            cache[f * 256 + key] = entry;
          } else {
            cache.remove(f * 256 + key);
          }
        }
      }
      return;
    }

    final fieldCount = block.meta.size;
    for (var f = 0; f < fieldCount; f++) {
      if (!forceRefresh && cache.containsKey(f)) continue;
      if (blockType == 0) {
        final keys = systemKeysForField(f);
        final fieldMap = <int, ({BlockMeta meta, List<int> value})>{};
        for (final key in keys) {
          final field = await _client.readField(f, key);
          if (field != null) {
            fieldMap[key] = field;
          }
        }
        if (fieldMap.isNotEmpty) {
          final primaryKey = keys.first;
          cache[f] = fieldMap[primaryKey]!;
          for (final entry in fieldMap.entries) {
            if (entry.key != primaryKey) {
              // Use offset 256 to avoid collision with primary field indices (0-255)
              cache[256 + f * 256 + entry.key] = entry.value;
            }
          }
        } else {
          cache.remove(f);
        }
      } else {
        final field = await _client.readBlockField(blockType, instance, f, 0xFF);
        if (field != null) {
          cache[f] = field;
        } else {
          cache.remove(f);
        }
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text('Register - ${idToString(widget.deviceId)}'),
        actions: [
          IconButton(
            tooltip: 'Save all to backup',
            icon: const Icon(Icons.save),
            onPressed: () async {
              final ok = await _withBusy(_saveAll);
              _snack(ok ? 'Saved' : 'Save failed');
            },
          ),
          IconButton(
            tooltip: 'Recall all from backup',
            icon: const Icon(Icons.restore),
            onPressed: () async {
              final ok = await _withBusy(_recallAll);
              _snack(ok ? 'Recalled' : 'Recall failed');
              if (ok) await _loadVisibleFields();
            },
          ),
          if (widget.hasDynamicMemory)
            IconButton(
              tooltip: _editMode ? 'Exit edit mode' : 'Edit dynamic blocks',
              icon: Icon(
                  _editMode ? Icons.check : Icons.edit_outlined,
                  color: _editMode ? kOrange : null),
              onPressed: () => setState(() => _editMode = !_editMode),
            ),
          if (_busy)
            const Padding(
              padding: EdgeInsets.only(right: 12),
              child: SizedBox(
                width: 18,
                height: 18,
                child: CircularProgressIndicator(strokeWidth: 2),
              ),
            ),
          RefreshButton(
            onRefresh: _refresh,
            autoActive: autoRefreshActive,
            refreshing: _refreshing,
            error: _error != null,
            selectedInterval: selectedInterval,
            onSelectAuto: applyAuto,
          ),
        ],
      ),
      body: _buildBody(),
    );
  }

  Widget _buildBody() {
    final blocks = _blockMetas;
    if (blocks == null) {
      return Center(child: Text(_error ?? 'Loading...'));
    }
    if (blocks.isEmpty) {
      return const Center(child: Text('No blocks found'));
    }
    // Partition: static/system blocks keep their fixed order; dynamic blocks are a
    // separate section (reorderable in edit mode).
    final fixed = <int>[];
    final dynamic = <int>[];
    for (var i = 0; i < blocks.length; i++) {
      final b = blocks[i];
      // A None-typed block is a dynamic tombstone slot (static blocks are never None).
      if (b != null && b.meta.typeValue == BlockType.dynamic.value) {
        dynamic.add(i);
      } else {
        fixed.add(i);
      }
    }

    final children = <Widget>[
      for (final i in fixed) _blockCard(context, i, blocks[i]),
    ];

    if (dynamic.isNotEmpty || (_editMode && widget.hasDynamicMemory)) {
      if (_editMode) {
        final dynCards = <Widget>[
          for (final (si, i) in dynamic.indexed)
            _blockCard(context, i, blocks[i],
                cardKey: ValueKey('dyn-$i'), dragIndex: si),
        ];
        children.add(Padding(
          padding: const EdgeInsets.only(top: 8, bottom: 8),
          child: Divider(height: 1),
        ));
        children.add(ReorderableListView(
          header: const Padding(
            padding: EdgeInsets.only(top: 6, bottom: 6),
            child: Text('Dynamic blocks',
                style: TextStyle(fontWeight: FontWeight.w600))),
          footer: ListTile(
            leading: const Icon(Icons.add_box_outlined),
            title: const Text('Add dynamic block'),
            onTap: _createBlock,
          ),
          shrinkWrap: true,
          padding: const EdgeInsets.fromLTRB(10, 4, 10, 12),
          buildDefaultDragHandles: false,
          children: dynCards,
          onReorderItem: (oldIndex, newIndex) =>
              _reorderBlocks(dynamic, oldIndex, newIndex),
        ));
      } else {
        for (final i in dynamic) {
          if (blocks[i]?.meta.typeValue == BlockType.dynamic.value) {
            children.add(_blockCard(context, i, blocks[i]));
          }
        }
      }
    }

    return SingleChildScrollView(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(10, 10, 10, 24),
        child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch, children: children),
      ),
    );
  }

  /// Applies a drag-reorder of a block's fields.
  Future<void> _reorderFields(({int type, int inst, BlockMeta meta, String name})? block,
      List<int> fields, int oldIndex, int newIndex) async {
    if (block == null || oldIndex == newIndex) return;
    final order = fields.toList();
    final moved = order.removeAt(oldIndex);
    order.insert(newIndex, moved);
    final dynBlock =
        DynBlock(index: block.inst, meta: block.meta, name: block.name);
    final ok = await _client.reorderDynamicFields(dynBlock, order);
    _snack(ok ? 'Fields reordered' : 'Reorder failed');
    await _refresh();
  }

  /// Applies a drag-reorder of the dynamic block section (oldIndex/newIndex are
  /// positions in `dynamic`), then rebuilds the registry in the new order.
  Future<void> _reorderBlocks(List<int> dynamic, int oldIndex, int newIndex) async {
    if (oldIndex == newIndex) return;
    final order = dynamic.toList();
    final moved = order.removeAt(oldIndex);
    order.insert(newIndex, moved);
    // Recreate only the LIVE blocks in the dragged order (tombstones compact to the end).
    final liveOrder = order
        .where((i) => _blockMetas?[i]?.meta.typeValue == BlockType.dynamic.value)
        .map((i) => _blockMetas![i]!.inst)
        .toList();
    final ok = await _client.reorderDynamicBlocks(liveOrder);
    _snack(ok ? 'Blocks reordered' : 'Reorder failed');
    await _refresh();
  }

  Widget _blockCard(BuildContext context, int blockIndex, ({int type, int inst, BlockMeta meta, String name})? block, {Key? cardKey, int? dragIndex}) {
    if (block == null) {
      return Card(
        color: kSurfaceAlt,
        child: ListTile(
          leading: const Icon(Icons.memory, color: kOrange),
          title: const Text('System Block', style: TextStyle(fontWeight: FontWeight.w600)),
          subtitle: const Text('Loading...'),
        ),
      );
    }

    final isExpanded = _expanded.contains(blockIndex);
    final isSystem = block.type == 0 && block.inst == 0;
    final isDynamic = block.type == BlockType.dynamic.value;
    if (block.meta.typeValue == BlockType.none.value) {
      // Tombstone slots are hidden (they carry no block).
      return const SizedBox.shrink();
    }

    return Card(
      key: cardKey,
      color: kSurfaceAlt,
      margin: EdgeInsets.zero,
      clipBehavior: Clip.antiAlias,
      child: Column(children: [
        ListTile(
          leading: _editMode && isDynamic && dragIndex != null
              ? Row(mainAxisSize: MainAxisSize.min, children: [
                  ReorderableDragStartListener(
                    index: dragIndex,
                    child: const Icon(Icons.drag_handle, color: Colors.white38),
                  ),
                  const SizedBox(width: 8),
                  Icon(isExpanded ? Icons.folder_open : Icons.folder, color: kOrange),
                ])
              : Icon(isExpanded ? Icons.folder_open : Icons.folder, color: kOrange),
          title: Row(children: [
            Expanded(
                child: Text(block.name.isNotEmpty ? block.name : (isSystem ? 'System' : (isDynamic ? 'Dynamic #${block.inst}' : 'Block #${block.inst}')),
                    style: const TextStyle(fontWeight: FontWeight.w600))),
            Container(
              padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 1),
              decoration: BoxDecoration(
                  color: Colors.white.withAlpha(20),
                  borderRadius: BorderRadius.circular(4)),
              child: Text(isSystem ? 'System' : '#${block.inst}',
                  style: const TextStyle(fontSize: 10, color: Colors.white54)),
            ),
          ]),
          subtitle: Padding(
            padding: const EdgeInsets.only(top: 2),
            child: Wrap(spacing: 6, runSpacing: 2, children: [
              ChipLabel(isSystem ? 'System' : BlockType.fromValue(block.meta.typeValue).label),
              for (final flag in FieldFlags.describe(block.meta.flags)) ChipLabel(flag, subtle: flag != 'RO'),
              ChipLabel('${block.meta.size} fields', subtle: true),
            ]),
          ),
          trailing: PopupMenuButton<String>(
            tooltip: 'Block actions',
            onSelected: (action) {
              if (action == 'edit') {
                _editBlock(blockIndex, block);
              } else if (action == 'add') {
                _addEntry(blockIndex, block);
              } else if (action == 'delete') {
                _deleteBlock(blockIndex, block);
              }
            },
            itemBuilder: (_) => [
              if (block.meta.typeValue == BlockType.dynamic.value) ...[
                const PopupMenuItem(value: 'edit', child: Text('Rename / set type')),
                const PopupMenuItem(value: 'add', child: Text('Add entry')),
                const PopupMenuItem(value: 'delete', child: Text('Delete block')),
              ],
            ],
          ),
          onTap: () async {
            setState(() {
              isExpanded
                  ? _expanded.remove(blockIndex)
                  : _expanded.add(blockIndex);
            });
            if (_expanded.contains(blockIndex)) {
              final block = _blockMetas?[blockIndex];
              if (block != null) {
                await _loadBlockFields(block.type, block.inst, block, forceRefresh: true);
              }
              if (mounted) setState(() {});
            }
          },
        ),
        if (isExpanded)
          Material(
            color: Colors.black26,
            child: Column(children: [
              const Divider(height: 1),
              if (block.meta.size > 0) ...[
                if (_editMode && isDynamic)
                  ReorderableListView(
                    shrinkWrap: true,
                    buildDefaultDragHandles: false,
                    children: [
                      for (final (si, f)
                          in (_dynamicFields[block.inst] ?? <int>[]).indexed)
                        _fieldTile(block.type, block.inst, block, f,
                            cardKey: ValueKey('fld-$f'), dragIndex: si),
                    ],
                    onReorderItem: (oldIndex, newIndex) =>
                        _reorderFields(block, _dynamicFields[block.inst] ?? <int>[], oldIndex, newIndex),
                  )
                else if (isDynamic)
                  for (final f in (_dynamicFields[block.inst] ?? <int>[]))
                    _fieldTile(block.type, block.inst, block, f)
                else
                  for (var f = 0; f < block.meta.size; f++)
                    _fieldTile(block.type, block.inst, block, f),
              ],
            ]),
          ),
      ]),
    );
  }

  Widget _fieldTile(int blockType, int inst, ({int type, int inst, BlockMeta meta, String name})? block, int fieldIndex, {Key? cardKey, int? dragIndex}) {
    final cacheKey = (blockType << 8) | inst;
    final cache = _fieldCache[cacheKey];
    final field = cache?[fieldIndex];
    final isSystem = blockType == 0 && inst == 0;

    // Dynamic blocks: flat (field, key) entries. Show every entry of this field.
    if (blockType == BlockType.dynamic.value) {
      final keys = _dynamicKeys[inst]?[fieldIndex] ?? <int>[0];
      final entries = <(int, ({BlockMeta meta, List<int> value})?)>[
        for (final k in keys) (k, cache?[fieldIndex * 256 + k]),
      ];
      final head = entries.firstOrNull?.$2;
      final isDict = head != null && isRenderDictType(head.meta.typeValue);
      final dictType = head?.meta.typeValue ?? 0;
      final shown = entries;
      if (entries.every((e) => e.$2 == null)) {
        return const ListTile(
            dense: true,
            leading: SizedBox(
                width: 14, height: 14, child: CircularProgressIndicator(strokeWidth: 2)),
            title: Text('...', style: TextStyle(fontSize: 13, color: Colors.white38)));
      }
      return ExpansionTile(
        key: cardKey,
        dense: true,
        title: Row(children: [
          if (dragIndex != null) ...[
            ReorderableDragStartListener(
              index: dragIndex,
              child: const Icon(Icons.drag_handle, color: Colors.white38),
            ),
            const SizedBox(width: 6),
          ],
          SizedBox(
              width: 120,
              child: Text('Field $fieldIndex',
                  style: const TextStyle(fontSize: 12, color: Colors.white54))),
          Expanded(
              child: Text('[${shown.length} entries]',
                  style: const TextStyle(fontFamily: 'monospace', fontSize: 13))),
          if (_editMode)
            IconButton(
              tooltip: 'Add entry to field $fieldIndex',
              icon: const Icon(Icons.add, size: 18),
              onPressed: () => _addDynamicEntry(blockType, inst, block, fieldIndex),
            ),
        ]),
        children: [
          for (final (k, e) in shown)
            if (e != null)
              ListTile(
                dense: true,
                contentPadding: const EdgeInsets.only(left: 56, right: 12),
                title: Row(children: [
                  SizedBox(
                      width: 90,
                      child: Text(isDict
                          ? renderDictKeyName(dictType, k)
                          : (k == 0 ? 'value' : 'key $k'),
                          style: const TextStyle(fontSize: 11, color: Colors.white54))),
                  if (e.meta.dataType == DataType.colour && e.value.length >= 4)
                    Padding(
                      padding: const EdgeInsets.only(right: 8),
                      child: Container(
                        width: 14,
                        height: 14,
                        decoration: BoxDecoration(
                            shape: BoxShape.circle,
                            color: Color.fromARGB(
                                e.value[3], e.value[0], e.value[1], e.value[2]),
                            border: Border.all(color: Colors.white38)),
                      ),
                    ),
                  Expanded(
                      child: Text(_formatDynamicValue(e, isDict, dictType, k),
                          style: const TextStyle(fontFamily: 'monospace', fontSize: 12))),
                ]),
                subtitle: Text(dataTypeLabel(e.meta.dataType),
                    style: const TextStyle(fontSize: 10)),
                trailing: PopupMenuButton<String>(
                  icon: const Icon(Icons.more_vert, size: 16),
                  onSelected: (action) {
                    if (action == 'edit' && !e.meta.readOnly) {
                      _editDynamicEntry(blockType, inst, block, fieldIndex, k);
                    } else if (action == 'delete' && !e.meta.readOnly && k != 0) {
                      _deleteDynamicEntry(blockType, inst, block, fieldIndex, k);
                    } else if (action == 'type' && !e.meta.readOnly) {
                      _changeDynamicType(blockType, inst, block, fieldIndex, k);
                    } else if (action == 'key' && !e.meta.readOnly && k != 0) {
                      _changeDynamicKey(blockType, inst, block, fieldIndex, k);
                    }
                  },
                  itemBuilder: (_) => [
                    if (!e.meta.readOnly)
                      const PopupMenuItem(value: 'edit', child: Text('Edit value')),
                    if (!e.meta.readOnly && _editMode && k != 0) ...[
                      const PopupMenuItem(value: 'type', child: Text('Change type')),
                      const PopupMenuItem(value: 'key', child: Text('Change key')),
                    ],
                    if (!e.meta.readOnly && k != 0)
                      const PopupMenuItem(value: 'delete', child: Text('Delete entry')),
                  ],
                ),
                onTap: !e.meta.readOnly
                    ? () => _editDynamicEntry(blockType, inst, block, fieldIndex, k)
                    : null,
              ),
        ],
      );
    }

    if (field == null) {
      return const ListTile(
          dense: true,
          leading: SizedBox(
              width: 14,
              height: 14,
              child: CircularProgressIndicator(strokeWidth: 2)),
          title: Text('...',
              style: TextStyle(fontSize: 13, color: Colors.white38)));
    }

    final primaryFlags = FieldFlags.describe(field.meta.flags);
    final isSystemField = isSystem;

    // Field display: resolves the registry's per-type field metadata for enum labels and
    // the DAS ResistiveMeasure custom view (Sensor Type at field 1, Measured Value at
    // field 4 with a sensor-derived unit + fuzzy lux level for the LDR).
    String displayValue() {
      if (isSystemField) return formatValue(field.meta.dataType, field.value);
      final blockInfo = blockInfoFor(BlockType.fromValue(blockType));
      final fieldInfo = blockInfo?.field(fieldIndex);

      if (blockType == BlockType.resistiveMeasure.value) {
        final sensorRaw = cache?[1];
        final sensor = (sensorRaw != null && sensorRaw.value.isNotEmpty)
            ? sensorRaw.value[0]
            : -1;
        if (fieldIndex == 1) {
          return sensor >= 0
              ? sensorTypeLabel(sensor)
              : formatValue(field.meta.dataType, field.value);
        }
        if (fieldIndex == 4 && sensor >= 0 && field.value.length >= 4) {
          final numVal = numberFromBytes(field.value);
          final unit = sensorUnits[sensor] ?? '';
          var text = formatValue(DataType.number, field.value);
          if (unit.isNotEmpty) text += ' $unit';
          if (sensor == 3) text += ' · ${luxLevel(numVal)}';
          return text;
        }
      }

      // Enum fields with known option labels (button edges, Acc&Gyr ODR/ranges, sensor
      // type): show the label instead of the raw index.
      final enumLabels = fieldInfo?.enumValues;
      if (enumLabels != null && field.value.isNotEmpty) {
        final raw = field.value[0];
        return enumLabels[raw] ?? 'Enum $raw';
      }
      return formatValue(field.meta.dataType, field.value);
    }

    // For system block, collect all keys for this field
    List<({int key, ({BlockMeta meta, List<int> value})? field})> systemFieldKeys = [];
    if (isSystemField && cache != null) {
      final keys = systemKeysForField(fieldIndex);
      for (final key in keys) {
        // Use offset 256 to match _loadBlockFields storage
        final extraField = cache[256 + fieldIndex * 256 + key];
        if (extraField != null) {
          systemFieldKeys.add((key: key, field: extraField));
        }
      }
      if (!systemFieldKeys.any((e) => e.key == systemKeysForField(fieldIndex).first)) {
        systemFieldKeys.insert(0, (key: systemKeysForField(fieldIndex).first, field: field));
      }
    }

    if (isSystemField && systemFieldKeys.length > 1) {
      return ExpansionTile(
        dense: true,
        title: Row(children: [
          SizedBox(
              width: 120,
              child: Text(systemFieldName(fieldIndex),
                  style: const TextStyle(fontSize: 12, color: Colors.white54))),
          Expanded(
              child: Text('[${systemFieldKeys.length} fields]',
                  style: const TextStyle(fontFamily: 'monospace', fontSize: 13))),
        ]),
        children: systemFieldKeys.map((entry) {
          final key = entry.key;
          final f = entry.field;
          if (f == null) return const SizedBox.shrink();
          return ListTile(
            dense: true,
            contentPadding: const EdgeInsets.only(left: 56, right: 12),
            title: Row(children: [
              SizedBox(width: 100, child: Text(systemStructMemberName(fieldIndex, key), style: const TextStyle(fontSize: 11, color: Colors.white54))),
              Expanded(child: Text(formatSystemValue(f.meta.dataType, f.value, fieldIndex, key), style: const TextStyle(fontFamily: 'monospace', fontSize: 12))),
              for (final flag in primaryFlags)
                Padding(
                  padding: const EdgeInsets.only(left: 4),
                  child: Text(flag,
                      style: TextStyle(
                          fontSize: 8,
                          fontWeight: FontWeight.w700,
                          color: flag == 'RO' ? kOrange : Colors.white38)),
                ),
            ]),
            subtitle: Text('${dataTypeLabel(f.meta.dataType)} [member=${systemStructMemberName(fieldIndex, key)}]', style: const TextStyle(fontSize: 10)),
            trailing: PopupMenuButton<String>(
              icon: const Icon(Icons.more_vert, size: 16),
              onSelected: (action) {
                if (action == 'edit' && !f.meta.readOnly) {
                  _editValue(blockType, inst, block, fieldIndex);
                } else if (action == 'save') {
                  _saveField(blockType, inst, block, fieldIndex);
                } else if (action == 'recall') {
                  _recallField(blockType, inst, block, fieldIndex);
                }
              },
              itemBuilder: (_) => [
                if (!f.meta.readOnly)
                  const PopupMenuItem(value: 'edit', child: Text('Edit value')),
                if (!f.meta.readOnly && f.meta.persistent)
                  const PopupMenuItem(value: 'save', child: Text('Save to backup')),
                if (!f.meta.readOnly && f.meta.persistent)
                  const PopupMenuItem(value: 'recall', child: Text('Recall from backup')),
              ],
            ),
          );
        }).toList(),
      );
    }

    final fieldInfo = isSystemField
        ? null
        : blockInfoFor(BlockType.fromValue(block?.meta.typeValue ?? 0))?.field(fieldIndex);

    // Render-block dictionaries (Geometry 0x101 / Texture 0x102): show each keyed
    // entry as an expandable sub-row so the square/fill can be edited per-key
    // (Docs/Modules and blocks/LED display.md).
    if (blockType == BlockType.dynamic.value && isRenderDictType(field.meta.typeValue)) {
      final dictType = field.meta.typeValue;
      final entries = parseKeyedDict(field.value) ?? <KeyedEntry>[];
      return ExpansionTile(
        dense: true,
        title: Row(children: [
          SizedBox(
              width: 120,
              child: Text(dictType == geometryDictType ? 'Geometry' : 'Texture',
                  style: const TextStyle(fontSize: 12, color: Colors.white54))),
          Expanded(
              child: Text('[${entries.length} keys]',
                  style: const TextStyle(fontFamily: 'monospace', fontSize: 13))),
        ]),
        children: [
          for (final e in entries)
            ListTile(
              dense: true,
              contentPadding: const EdgeInsets.only(left: 56, right: 12),
              title: Row(children: [
                SizedBox(
                    width: 100,
                    child: Text(renderDictKeyName(dictType, e.key),
                        style: const TextStyle(fontSize: 11, color: Colors.white54))),
                if (e.meta.dataType == DataType.colour && e.value.length >= 4)
                  Padding(
                    padding: const EdgeInsets.only(right: 8),
                    child: Container(
                      width: 14,
                      height: 14,
                      decoration: BoxDecoration(
                          shape: BoxShape.circle,
                          color: Color.fromARGB(
                              e.value[3], e.value[0], e.value[1], e.value[2]),
                          border: Border.all(color: Colors.white38)),
                    ),
                  ),
                Expanded(
                    child: Text(_formatDictEntry(dictType, e),
                        style: const TextStyle(fontFamily: 'monospace', fontSize: 12))),
              ]),
              subtitle: Text(dataTypeLabel(e.meta.dataType),
                  style: const TextStyle(fontSize: 10)),
              trailing: PopupMenuButton<String>(
                icon: const Icon(Icons.more_vert, size: 16),
                onSelected: (action) {
                  if (action == 'edit' && !e.meta.readOnly) {
                    _editDictEntry(blockType, inst, block, fieldIndex, e.key);
                  }
                },
                itemBuilder: (_) => [
                  if (!e.meta.readOnly)
                    const PopupMenuItem(value: 'edit', child: Text('Edit value')),
                ],
              ),
              onTap: !e.meta.readOnly
                  ? () => _editDictEntry(blockType, inst, block, fieldIndex, e.key)
                  : null,
            ),
        ],
      );
    }

    return ListTile(
      dense: true,
      contentPadding: const EdgeInsets.only(left: 40, right: 12),
      title: Row(children: [
        SizedBox(
            width: 120,
            child: Text(
                isSystemField ? systemFieldName(fieldIndex) : (fieldInfo?.name ?? 'Field $fieldIndex'),
                style: const TextStyle(fontSize: 12, color: Colors.white54))),
        Expanded(
            child: Text(
                isSystemField ? formatSystemValue(field.meta.dataType, field.value, fieldIndex, systemKeysForField(fieldIndex).first) : displayValue(),
                style: const TextStyle(fontFamily: 'monospace', fontSize: 13))),
        for (final flag in FieldFlags.describe(field.meta.flags))
          Padding(
            padding: const EdgeInsets.only(left: 4),
            child: Text(flag,
                style: TextStyle(
                    fontSize: 9,
                    fontWeight: FontWeight.w700,
                    color: flag == 'RO' ? kOrange : Colors.white38)),
          ),
      ]),
      subtitle: Text('${dataTypeLabel(field.meta.dataType)}${!isSystemField && fieldInfo?.unit != null ? ' [${fieldInfo!.unit}]' : ''}',
          style: const TextStyle(fontSize: 11)),
      trailing: PopupMenuButton<String>(
        icon: const Icon(Icons.more_vert, size: 18),
        tooltip: 'Field actions',
        onSelected: (action) {
          if (action == 'edit' && !field.meta.readOnly) {
            _editValue(blockType, inst, block, fieldIndex);
          } else if (action == 'save') {
            _saveField(blockType, inst, block, fieldIndex);
          } else if (action == 'recall') {
            _recallField(blockType, inst, block, fieldIndex);
          } else if (action == 'type') {
            _changeType(blockType, inst, block, fieldIndex);
          } else if (action == 'delentry') {
            _deleteEntry(blockType, inst, block, fieldIndex);
          }
        },
        itemBuilder: (_) => [
          if (!field.meta.readOnly)
            const PopupMenuItem(value: 'edit', child: Text('Edit value')),
          if (!field.meta.readOnly && field.meta.persistent)
            const PopupMenuItem(value: 'save', child: Text('Save to backup')),
          if (!field.meta.readOnly && field.meta.persistent)
            const PopupMenuItem(value: 'recall', child: Text('Recall from backup')),
          if (blockType == BlockType.dynamic.value && !field.meta.readOnly) ...[
            const PopupMenuItem(value: 'type', child: Text('Change type')),
            const PopupMenuItem(value: 'delentry', child: Text('Delete entry')),
          ],
        ],
      ),
      onTap: (!field.meta.readOnly) ? () => _editValue(blockType, inst, block, fieldIndex) : null,
    );
  }

  Future<void> _editBlock(int blockIndex, ({int type, int inst, BlockMeta meta, String name})? block) async {
    if (block == null || !mounted) return;
    if (block.type != BlockType.dynamic.value) {
      _snack('Only dynamic blocks can be renamed');
      return;
    }
    final result = await promptBlockNameAndType(context,
        title: 'Edit block',
        initialName: block.name,
        fixedType: BlockType.dynamic,
        availableTypes: _availableBlockTypes());
    if (result == null || !mounted) return;
    final (name, type, _) = result;
    
    final ok = await _client.writeDynamicBlockMeta(
        DynBlock(index: block.inst, meta: block.meta, name: block.name),
        name, type);
    _snack(ok ? 'Block updated' : 'Update failed');
    await _refresh();
  }

  Future<void> _createBlock() async {
    if (!mounted) return;
    // Dynamic blocks are always the Dynamic type; no type selection needed.
    final result = await promptBlockNameAndType(context,
        title: 'New dynamic block', withIndex: true, fixedType: BlockType.dynamic);
    if (result == null || !mounted) return;
    final (name, type, index) = result;

    final created = await _client.createDynamicBlock(type, name, index: index);
    _snack(created != null ? 'Block created' : 'Create failed');
    await _refresh();
  }

  /// Creates a dynamic block at a specific (tombstone) position.
  Future<void> _createBlockAt(int index) async {
    if (!mounted) return;
    final result = await promptBlockNameAndType(context,
        title: 'Create block at slot #$index',
        initialName: 'Dynamic $index',
        fixedType: BlockType.dynamic,
        availableTypes: _availableBlockTypes());
    if (result == null || !mounted) return;
    final (name, type, _) = result;
    final created = await _client.createDynamicBlock(type, name, index: index);
    _snack(created != null ? 'Block created' : 'Create failed');
    await _refresh();
  }

  /// The block types this device actually exposes (static blocks + Dynamic),
/// used to limit the type dropdown when creating a dynamic block.
  List<BlockType> _availableBlockTypes() {
    final types = <BlockType>{BlockType.dynamic};
    final metas = _blockMetas;
    if (metas != null) {
      for (final b in metas) {
        if (b == null || b.type == 0 || b.type == BlockType.dynamic.value) continue;
        types.add(BlockType.fromValue(b.type));
      }
    }
    return types.where((t) => t != BlockType.none).toList();
  }

  Future<void> _deleteBlock(int blockIndex, ({int type, int inst, BlockMeta meta, String name})? block) async {
    if (block == null || !mounted) return;

    final ok = await _client.deleteDynamic(block: block.inst);
    _snack(ok ? 'Block deleted (save to free)' : 'Delete failed');
    await _refresh();
  }

  Future<void> _addEntry(int blockIndex, ({int type, int inst, BlockMeta meta, String name})? block) async {
    if (block == null || !mounted) return;
    final dataType = await _pickDataType(selected: DataType.number);
    if (dataType == null || !mounted) return;
    final seed = await showValueEditor(context, dataType, []);
    if (seed == null || !mounted) return;
    // Flat model: add a new FIELD at the first free field index, key 0 (head).
    final fields = _dynamicFields[block.inst] ?? <int>[];
    var newField = 0;
    while (fields.contains(newField)) newField++;
    final dynBlock = DynBlock(index: block.inst, meta: block.meta, name: block.name);
    final meta = BlockMeta(flagsAndType: dataType.value, key: 0, size: seed.length);
    final confirmed =
        await _client.writeDynamicEntry(dynBlock, newField, 0, meta, seed);
    _snack(confirmed != null ? 'Field added' : 'Add failed');
    await _refresh();
  }

  Future<void> _changeType(int blockType, int inst,
      ({int type, int inst, BlockMeta meta, String name})? block, int fieldIndex) async {
    if (block == null || !mounted) return;
    final dataType = await _pickDataType();
    if (dataType == null || !mounted) return;
    final seed = await showValueEditor(context, dataType, []);
    if (seed == null || !mounted) return;
    final cache = _fieldCache[(blockType << 8) | inst];
    final field = cache?[fieldIndex];
    if (field == null) return;
    
    final dynBlock = DynBlock(index: block.inst, meta: block.meta, name: block.name);
    final dynField = DynField(index: fieldIndex, meta: field.meta, value: field.value);
    final confirmed = await _client.writeDynamicField(dynBlock, dynField, seed, newType: dataType);
    _snack(confirmed != null ? 'Type changed' : 'Change failed');
    await _refresh();
  }

  Future<void> _deleteEntry(int blockType, int inst,
      ({int type, int inst, BlockMeta meta, String name})? block, int fieldIndex) async {
    if (block == null || !mounted) return;
    final cache = _fieldCache[(blockType << 8) | inst];
    final field = cache?[fieldIndex];
    if (field == null) return;
    
    final dynBlock = DynBlock(index: block.inst, meta: block.meta, name: block.name);
    final dynField = DynField(index: fieldIndex, meta: field.meta, value: field.value);
    final confirmed = await _client.writeDynamicField(dynBlock, dynField, [], newType: DataType.deleted);
    _snack(confirmed != null ? 'Entry deleted (save to free)' : 'Delete failed');
    await _refresh();
  }

  Future<DataType?> _pickDataType({DataType? selected}) {
    return showDialog<DataType>(
      context: context,
      builder: (context) => SimpleDialog(
        title: const Text('Entry data type'),
        children: [
          for (final t in DataType.values)
            if (t != DataType.deleted && t != DataType.none && t != DataType.undefined)
              SimpleDialogOption(
                onPressed: () => Navigator.pop(context, t),
                child: Row(children: [
                  if (t == selected)
                    const Icon(Icons.check, size: 16, color: Colors.white38),
                  if (t != selected) const SizedBox(width: 20),
                  Expanded(child: Text(dataTypeLabel(t))),
                ]),
              ),
        ],
      ),
    );
  }

  /// The lowest None-marked field index (a deleted slot to fill), or null.
  int? _firstNoneField(DynBlock block, int count) {
    for (var f = 0; f < count; f++) {
      final existing = block.fields[f];
      if (existing != null && existing.meta.dataType == DataType.none) return f;
    }
    return null;
  }

  Future<void> _editValue(int blockType, int inst, ({int type, int inst, BlockMeta meta, String name})? block, int fieldIndex) async {
    if (block == null || !mounted) return;
    final cacheKey = (blockType << 8) | inst;
    final cache = _fieldCache[cacheKey];
    final field = cache?[fieldIndex];
    if (field == null || field.meta.readOnly) return;
    if (!mounted) return;

    final isSystemField = blockType == 0 && inst == 0;
    final blockInfo = blockInfoFor(BlockType.fromValue(block.meta.typeValue));
    final fieldInfo = isSystemField ? null : blockInfo?.field(fieldIndex);

    final newValue = await showValueEditor(
        context, field.meta.dataType, field.value,
        info: fieldInfo);
    if (newValue == null) return;

    final key = (blockType == 0) ? systemKeysForField(fieldIndex).first : ((blockType == BlockType.dynamic.value) ? 0 : 0xFF);
    final meta = BlockMeta(flagsAndType: field.meta.flagsAndType, size: newValue.length, key: key);
    final confirmed = await _client.writeBlockField(blockType, inst, fieldIndex, key, meta, newValue);
    _snack(confirmed != null ? 'Value written' : 'Write failed');
    if (confirmed != null) {
      final field = await _client.readBlockField(blockType, inst, fieldIndex, key);
      if (field != null) {
        final cache = _fieldCache[cacheKey];
        if (cache != null) cache[fieldIndex] = field;
      }
      if (mounted) setState(() {});
    }
  }

  /// Edits one (field, key) entry of a dynamic block.
  Future<void> _editDynamicEntry(int blockType, int inst,
      ({int type, int inst, BlockMeta meta, String name})? block, int fieldIndex, int key) async {
    if (block == null || !mounted) return;
    final cache = _fieldCache[(blockType << 8) | inst];
    final entry = cache?[fieldIndex * 256 + key];
    if (entry == null || entry.meta.readOnly) return;
    final head = cache?[fieldIndex * 256 + 0];
    final dictType =
        (head != null && isRenderDictType(head.meta.typeValue)) ? head.meta.typeValue : entry.meta.typeValue;
    final info = isRenderDictType(dictType) ? renderKeyFieldInfo(dictType, key) : null;
    final newValue =
        await showValueEditor(context, entry.meta.dataType, entry.value, info: info);
    if (newValue == null) return;
    final dynBlock = DynBlock(index: inst, meta: block.meta, name: block.name);
    final dynField = DynField(index: fieldIndex, meta: entry.meta, value: entry.value);
    final confirmed =
        await _client.writeDynamicField(dynBlock, dynField, newValue, key: key);
    _snack(confirmed != null ? 'Value written' : 'Write failed');
    await _refresh();
  }

  /// Deletes one (field, key) entry of a dynamic block.
  Future<void> _deleteDynamicEntry(int blockType, int inst,
      ({int type, int inst, BlockMeta meta, String name})? block, int fieldIndex, int key) async {
    if (block == null || !mounted) return;
    final ok = await _client.deleteDynamic(block: inst, field: fieldIndex, key: key);
    _snack(ok ? 'Entry deleted' : 'Delete failed');
    await _refresh();
  }

  /// Adds an entry to a dynamic block's (field, key) record at the first free key.
  Future<void> _addDynamicEntry(int blockType, int inst,
      ({int type, int inst, BlockMeta meta, String name})? block, int fieldIndex) async {
    if (block == null || !mounted) return;
    final dataType = await _pickDataType(selected: DataType.number);
    if (dataType == null || !mounted) return;
    final seed = await showValueEditor(context, dataType, []);
    if (seed == null || !mounted) return;
    final keys = _dynamicKeys[inst]?[fieldIndex] ?? <int>[];
    final cache = _fieldCache[(blockType << 8) | inst];
    final head = cache?[fieldIndex * 256 + 0];
    final dictType =
        (head != null && isRenderDictType(head.meta.typeValue)) ? head.meta.typeValue : 0;
    int freeKey;
    if (isRenderDictType(dictType)) {
      // Offer only keys meaningful for the current shape/effect.
      final selector = cache?[fieldIndex * 256 + 1]?.value.first ?? 0;
      final relevant = dictType == geometryDictType
          ? geometryKeysForShape(selector)
          : textureKeysForType(selector);
      final missing = relevant.where((k) => !keys.contains(k)).toList();
      if (missing.isEmpty) {
        _snack('All keys for this shape are already present');
        return;
      }
      freeKey = missing.first;
    } else {
      freeKey = 0;
      while (keys.contains(freeKey)) freeKey++;
    }
    final dynBlock = DynBlock(index: inst, meta: block.meta, name: block.name);
    final meta = BlockMeta(flagsAndType: dataType.value, key: freeKey, size: seed.length);
    final confirmed =
        await _client.writeDynamicEntry(dynBlock, fieldIndex, freeKey, meta, seed);
    _snack(confirmed != null ? 'Entry added' : 'Add failed');
    await _refresh();
  }

  /// Changes one entry's data type (writes a fresh value of the new type).
  Future<void> _changeDynamicType(int blockType, int inst,
      ({int type, int inst, BlockMeta meta, String name})? block, int fieldIndex, int key) async {
    if (block == null || !mounted) return;
    final cache = _fieldCache[(blockType << 8) | inst];
    final entry = cache?[fieldIndex * 256 + key];
    if (entry == null) return;
    final dataType = await _pickDataType(selected: entry.meta.dataType);
    if (dataType == null || !mounted) return;
    final seed = await showValueEditor(context, dataType, []);
    if (seed == null || !mounted) return;
    final dynBlock = DynBlock(index: inst, meta: block.meta, name: block.name);
    final dynField = DynField(index: fieldIndex, meta: entry.meta, value: entry.value);
    final confirmed = await _client.writeDynamicField(
        dynBlock, dynField, seed, newType: dataType, key: key);
    _snack(confirmed != null ? 'Type changed' : 'Change failed');
    await _refresh();
  }

  /// Re-keys one entry (moves it to a different key byte within the field).
  Future<void> _changeDynamicKey(int blockType, int inst,
      ({int type, int inst, BlockMeta meta, String name})? block, int fieldIndex, int key) async {
    if (block == null || !mounted) return;
    final cache = _fieldCache[(blockType << 8) | inst];
    final entry = cache?[fieldIndex * 256 + key];
    if (entry == null) return;
    final controller = TextEditingController(text: '$key');
    final newKey = await showDialog<int>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Change key'),
        content: TextField(
          controller: controller,
          autofocus: true,
          keyboardType: TextInputType.number,
        ),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(
            onPressed: () {
              final v = int.tryParse(controller.text.trim());
              Navigator.pop(context, v == null ? null : v);
            },
            child: const Text('OK'),
          ),
        ],
      ),
    );
    if (newKey == null || newKey < 0 || newKey > 255 || newKey == key) return;
    if ((_dynamicKeys[inst]?[fieldIndex] ?? <int>[]).contains(newKey)) {
      _snack('Key $newKey already exists in this field');
      return;
    }
    final dynBlock = DynBlock(index: inst, meta: block.meta, name: block.name);
    final written = await _client.writeDynamicEntry(
        dynBlock, fieldIndex, newKey, entry.meta, entry.value);
    if (written != null) {
      await _client.deleteDynamic(block: inst, field: fieldIndex, key: key);
    }
    _snack(written != null ? 'Key changed' : 'Change failed');
    await _refresh();
  }

  /// Formats a dynamic (field, key) entry with enum labels where known.
  String _formatDynamicValue(
      ({BlockMeta meta, List<int> value}) e, bool isDict, int dictType, int key) {
    if (e.meta.dataType == DataType.enum_ && e.value.isNotEmpty) {
      final enums = isDict ? renderKeyFieldInfo(dictType, key).enumValues : null;
      final label = enums?[e.value[0]];
      if (label != null) return label;
    }
    return formatValue(e.meta.dataType, e.value);
  }

  /// Formats one keyed-dict entry by its data type, with enum labels where known.
  String _formatDictEntry(int dictType, KeyedEntry e) {
    if (e.meta.dataType == DataType.enum_ && e.value.isNotEmpty) {
      final enums = renderKeyFieldInfo(dictType, e.key).enumValues;
      final label = enums?[e.value[0]];
      if (label != null) return label;
    }
    return formatValue(e.meta.dataType, e.value);
  }

  /// Edits one key of a render-block dictionary field and writes the whole dict back.
  Future<void> _editDictEntry(int blockType, int inst,
      ({int type, int inst, BlockMeta meta, String name})? block, int fieldIndex, int key) async {
    if (block == null || !mounted) return;
    final cacheKey = (blockType << 8) | inst;
    final cache = _fieldCache[cacheKey];
    final field = cache?[fieldIndex];
    if (field == null || field.meta.readOnly) return;
    final dictType = field.meta.typeValue;
    final entries = parseKeyedDict(field.value);
    if (entries == null) return;
    final idx = entries.indexWhere((e) => e.key == key);
    if (idx < 0) return;
    final entry = entries[idx];

    final newValue = await showValueEditor(
        context, entry.meta.dataType, entry.value,
        info: renderKeyFieldInfo(dictType, key));
    if (newValue == null) return;

    entries[idx] = KeyedEntry(key: key, meta: entry.meta, value: newValue);
    final newBlob = buildKeyedDict(entries);
    final dynBlock = DynBlock(index: inst, meta: block.meta, name: block.name);
    final dynField = DynField(index: fieldIndex, meta: field.meta, value: field.value);
    final confirmed = await _client.writeDynamicField(dynBlock, dynField, newBlob);
    _snack(confirmed != null ? 'Value written' : 'Write failed');
    await _refresh();
  }

}
