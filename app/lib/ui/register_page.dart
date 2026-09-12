import 'dart:async';

import 'package:flutter/material.dart';

import '../core/block_registry.dart';
import '../core/connection.dart';
import '../core/device_db.dart';
import '../core/register_client.dart';
import '../core/types.dart';
import 'theme.dart';
import 'value_editor.dart' show dataTypeLabel, formatValue, showValueEditor;
import 'widgets.dart';

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
  String? _error;
  final Set<int> _expanded = {};
  bool _refreshing = false;
  bool _busy = false;

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
    ScaffoldMessenger.of(context)
        .showSnackBar(SnackBar(content: Text(message)));
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
    ok &= await _requestStatus(c, 3, makeBlockInfo(0, 0, 0xFF, 0)); // system Name/NetID
    for (final b in _blockMetas ?? const []) {
      if (b == null || b.type == 0 || b.type == 0x3FF) continue;
      ok &= await _requestStatus(c, 3, makeBlockInfo(b.type, b.inst, 0xFF, 0));
    }
    return ok;
  }

  Future<bool> _recallAll() async {
    final c = _client;
    bool ok = true;
    if (widget.hasDynamicMemory) {
      ok &= await _requestStatus(c, 4, const [0xFF, 0xFF, 0xFF, 0xFF]);
    }
    ok &= await _requestStatus(c, 4, makeBlockInfo(0, 0, 0xFF, 0));
    for (final b in _blockMetas ?? const []) {
      if (b == null || b.type == 0 || b.type == 0x3FF) continue;
      ok &= await _requestStatus(c, 4, makeBlockInfo(b.type, b.inst, 0xFF, 0));
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
    final bi = makeBlockInfo(blockType == 0 ? 0 : blockType, inst, fieldIndex, 0);
    final ok = await _withBusy(() async {
      final reply = await _client.request(3, payload: bi);
      return reply != null && reply.isNotEmpty && reply[0] == 0;
    });
    _snack(ok ? 'Saved' : 'Save failed');
  }

  Future<void> _recallField(int blockType, int inst,
      ({int type, int inst, BlockMeta meta, String name})? block, int fieldIndex) async {
    final bi = makeBlockInfo(blockType == 0 ? 0 : blockType, inst, fieldIndex, 0);
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
    final fieldCount = block.meta.size;
    final cacheKey = (blockType << 8) | instance;
    final cache = _fieldCache.putIfAbsent(cacheKey, () => {});

    if (forceRefresh) {
      cache.clear();
    }

    for (var f = 0; f < fieldCount; f++) {
      if (!forceRefresh && cache.containsKey(f)) continue;
      if (blockType == 0) {
        final keys = _systemKeysForField(f);
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
          final key = (blockType == BlockType.dynamic.value) ? 0 : 0xFF;
          final field = await _client.readBlockField(blockType, instance, f, key);
        if (field != null) {
          cache[f] = field;
        } else {
          cache.remove(f);
        }
      }
    }
  }

  List<int> _systemKeysForField(int field) {
    switch (field) {
      case 0: return [0, 1, 2];
      case 1: return [0xFF];
      case 2: return [0];
      case 3: return [0, 1, 2, 3, 4];
      case 4: return [0, 1];
      case 5: return [0, 1];
      case 6: return [0xFF];
      case 7: return [0];
      case 8: return [0, 1];
      default: return [0];
    }
  }

  int _systemKeyForField(int field) {
    return _systemKeysForField(field).first;
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
              tooltip: 'Add dynamic block',
              icon: const Icon(Icons.add_box_outlined),
              onPressed: _createBlock,
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
    return ListView.separated(
      padding: const EdgeInsets.fromLTRB(10, 10, 10, 24),
      itemCount: blocks.length,
      separatorBuilder: (_, _) => const SizedBox(height: 6),
      itemBuilder: (context, index) {
        return _blockCard(context, index, blocks[index]);
      },
    );
  }

  Widget _blockCard(BuildContext context, int blockIndex, ({int type, int inst, BlockMeta meta, String name})? block) {
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

    return Card(
      color: kSurfaceAlt,
      margin: EdgeInsets.zero,
      clipBehavior: Clip.antiAlias,
      child: Column(children: [
        ListTile(
          leading: Icon(isExpanded ? Icons.folder_open : Icons.folder,
              color: kOrange),
          title: Row(children: [
            Expanded(
                child: Text(block.name.isNotEmpty ? block.name : (isSystem ? 'System' : (isDynamic ? 'Dynamic #${block.inst}' : 'Block #${block.inst}')),
                    style: const TextStyle(fontWeight: FontWeight.w600))),
            Container(
              padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 1),
              decoration: BoxDecoration(
                  color: Colors.white.withAlpha(20),
                  borderRadius: BorderRadius.circular(4)),
              child: Text(isSystem ? 'System' : (isDynamic ? '#${block.inst}' : '#${block.inst}'),
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
              if (block.meta.size > 0)
                for (var f = 0; f < block.meta.size; f++)
                  _fieldTile(block.type, block.inst, block, f),
            ]),
          ),
      ]),
    );
  }

  Widget _fieldTile(int blockType, int inst, ({int type, int inst, BlockMeta meta, String name})? block, int fieldIndex) {
    final cacheKey = (blockType << 8) | inst;
    final cache = _fieldCache[cacheKey];
    final field = cache?[fieldIndex];
    final isSystem = blockType == 0 && inst == 0;

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

    // Custom display for the DAS ResistiveMeasure block: the Sensor Type (field 2)
    // shows its enum name, and the Measured Value (field 3) shows a unit derived from
    // the selected sensor, plus a fuzzy lux level for the LDR.
    String displayValue() {
      if (blockType != BlockType.resistiveMeasure.value || isSystemField) {
        return formatValue(field.meta.dataType, field.value);
      }
      final sensorRaw = cache?[2];
      final sensor = (sensorRaw != null && sensorRaw.value.isNotEmpty)
          ? sensorRaw.value[0]
          : -1;
      if (fieldIndex == 2) {
        return sensor >= 0 ? sensorTypeLabel(sensor) : formatValue(field.meta.dataType, field.value);
      }
      if (fieldIndex == 3 && sensor >= 0) {
        final numVal = numberFromBytes(field.value);
        final unit = sensorUnits[sensor] ?? '';
        var text = formatValue(DataType.number, field.value);
        if (unit.isNotEmpty) text += ' $unit';
        if (sensor == 3) text += ' · ${luxLevel(numVal)}';
        return text;
      }
      return formatValue(field.meta.dataType, field.value);
    }

    // For system block, collect all keys for this field
    List<({int key, ({BlockMeta meta, List<int> value})? field})> systemFieldKeys = [];
    if (isSystemField && cache != null) {
      final keys = _systemKeysForField(fieldIndex);
      for (final key in keys) {
        // Use offset 256 to match _loadBlockFields storage
        final extraField = cache[256 + fieldIndex * 256 + key];
        if (extraField != null) {
          systemFieldKeys.add((key: key, field: extraField));
        }
      }
      if (!systemFieldKeys.any((e) => e.key == _systemKeysForField(fieldIndex).first)) {
        systemFieldKeys.insert(0, (key: _systemKeysForField(fieldIndex).first, field: field));
      }
    }

    if (isSystemField && systemFieldKeys.length > 1) {
      return ExpansionTile(
        dense: true,
        title: Row(children: [
          SizedBox(
              width: 120,
              child: Text(_systemFieldName(fieldIndex),
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
              SizedBox(width: 100, child: Text(_systemStructMemberName(fieldIndex, key), style: const TextStyle(fontSize: 11, color: Colors.white54))),
              Expanded(child: Text(_formatSystemValue(f.meta.dataType, f.value), style: const TextStyle(fontFamily: 'monospace', fontSize: 12))),
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
            subtitle: Text('${dataTypeLabel(f.meta.dataType)} [member=${_systemStructMemberName(fieldIndex, key)}]', style: const TextStyle(fontSize: 10)),
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

    return ListTile(
      dense: true,
      contentPadding: const EdgeInsets.only(left: 40, right: 12),
      title: Row(children: [
        SizedBox(
            width: 120,
            child: Text(
                isSystemField ? _systemFieldName(fieldIndex) : (fieldInfo?.name ?? 'Field $fieldIndex'),
                style: const TextStyle(fontSize: 12, color: Colors.white54))),
        Expanded(
            child: Text(
                isSystemField ? _formatSystemValue(field.meta.dataType, field.value) : displayValue(),
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

  String _systemFieldName(int field) {
    switch (field) {
      case 0: return 'Device Type';
      case 1: return 'Serial Number';
      case 2: return 'Short Address';
      case 3: return 'Time';
      case 4: return 'RAM';
      case 5: return 'Storage';
      case 6: return 'Name';
      case 7: return 'NetID';
      case 8: return 'App/CLI Active';
      default: return 'Field $field';
    }
  }

  String _systemStructMemberName(int field, int key) {
    switch (field) {
      case 0:
        switch (key) {
          case 0: return '.deviceType';
          case 1: return '.capabilities';
          case 2: return '.softwareVersion';
        }
      case 3:
        switch (key) {
          case 0: return '.uptime';
          case 1: return '.currentTime';
          case 2: return '.timeOffsetMs';
          case 3: return '.avgLoopTimeMs';
          case 4: return '.maxLoopTimeMs';
        }
      case 4:
        switch (key) {
          case 0: return '.usedRAM';
          case 1: return '.totalRAM';
        }
      case 5:
        switch (key) {
          case 0: return '.usedFlash';
          case 1: return '.totalFlash';
        }
      case 8:
        switch (key) {
          case 0: return '.appActive';
          case 1: return '.cliActive';
        }
    }
    return 'Key $key';
  }

  String _formatSystemValue(DataType type, List<int> value) {
    switch (type) {
      case DataType.enum_:
        // Device type is a 32-bit enum
        if (value.length >= 4) {
          final val = value[0] | (value[1] << 8) | (value[2] << 16) | (value[3] << 24);
          return DeviceType.fromValue(val).label;
        }
        if (value.isNotEmpty) return '0x${value[0].toRadixString(16).padLeft(2, '0')}';
        return '-';
      case DataType.sn:
        return serialNumberToHex(value);
      case DataType.id:
        return value.length >= 2 ? idToString(value[0] | (value[1] << 8)) : '-';
      case DataType.integer:
        // System block fields like Time offset are signed 32-bit: sign-extend.
        if (value.length >= 4) return int32FromBytes(value).toString();
        if (value.length >= 2) {
          final v = value[0] | (value[1] << 8);
          return (v >= 0x8000 ? v - 0x10000 : v).toString();
        }
        return '-';
      case DataType.string:
        // Software version is 4 bytes (YY, MM, DD, iteration)
        if (value.length == 4) {
          return '${value[0]}.${value[1]}.${value[2]}.${value[3]}';
        }
        return String.fromCharCodes(value).replaceAll('\x00', '');
      case DataType.bool_:
        return value.isNotEmpty && value[0] != 0 ? 'true' : 'false';
      default:
        return formatValue(type, value);
    }
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
        fixedType: BlockType.dynamic);
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
    final result = await promptBlockNameAndType(context,
        title: 'New dynamic block', withIndex: true, fixedType: BlockType.dynamic);
    if (result == null || !mounted) return;
    final (name, type, index) = result;
    
    final created = await _client.createDynamicBlock(type, name, index: index);
    _snack(created != null ? 'Block created' : 'Create failed');
    await _refresh();
  }

  Future<void> _deleteBlock(int blockIndex, ({int type, int inst, BlockMeta meta, String name})? block) async {
    if (block == null || !mounted) return;
    
    final ok = await _client.deleteDynamic(block: block.inst);
    _snack(ok ? 'Block deleted (save to free)' : 'Delete failed');
    await _refresh();
  }

  Future<void> _addEntry(int blockIndex, ({int type, int inst, BlockMeta meta, String name})? block) async {
    if (block == null || !mounted) return;
    final dataType = await _pickDataType();
    if (dataType == null || !mounted) return;
    final seed = await showValueEditor(context, dataType, []);
    if (seed == null || !mounted) return;
    
    final live = await _client.readDynamicBlockMeta(block.inst);
    if (live == null) return;
    final target = _firstNoneField(live, live.fieldCount);
    final confirmed = await _client.appendDynamicEntry(
        live,
        BlockMeta(flagsAndType: dataType.value, size: seed.length),
        seed,
        index: target);
    _snack(confirmed != null ? 'Entry added' : 'Add failed');
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

  Future<DataType?> _pickDataType() {
    return showDialog<DataType>(
      context: context,
      builder: (context) => SimpleDialog(
        title: const Text('Entry data type'),
        children: [
          for (final t in DataType.values)
            if (t != DataType.deleted && t != DataType.none && t != DataType.undefined)
              SimpleDialogOption(
                onPressed: () => Navigator.pop(context, t),
                child: Text(dataTypeLabel(t)),
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

    final key = (blockType == 0) ? _systemKeyForField(fieldIndex) : ((blockType == BlockType.dynamic.value) ? 0 : 0xFF);
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

}

/// Small rounded label chip used across the memory pages.
class ChipLabel extends StatelessWidget {
  const ChipLabel(this.text, {super.key, this.subtle = false});

  final String text;
  final bool subtle;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 1),
      decoration: BoxDecoration(
        color: subtle ? Colors.white.withAlpha(14) : kOrange.withAlpha(46),
        borderRadius: BorderRadius.circular(4),
      ),
      child: Text(text,
          style: TextStyle(
              fontSize: 10,
              color: subtle ? Colors.white54 : kOrange,
              fontWeight: subtle ? FontWeight.w400 : FontWeight.w600)),
    );
  }
}