import 'dart:async';

import 'package:flutter/material.dart';

import '../core/block_registry.dart';
import '../core/connection.dart';
import '../core/register_client.dart';
import '../core/types.dart';
import 'dynmem_page.dart';
import 'theme.dart';
import 'value_editor.dart' show dataTypeLabel, formatValue, showValueEditor;
import 'widgets.dart';

/// Register service view (Docs/App/Service views/Register.md):
/// System block, static blocks, and dynamic memory.
class RegisterPage extends StatefulWidget {
  final int deviceId;

  const RegisterPage({super.key, required this.deviceId});

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
              cache[f * 256 + entry.key] = entry.value;
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
              final ok = await _client.request(3, payload: [0xFF, 0xFF, 0xFF, 0xFF]);
              _snack(ok != null && ok.isNotEmpty && ok[0] == 0
                  ? 'Saved' : 'Save failed');
            },
          ),
          IconButton(
            tooltip: 'Recall all from backup',
            icon: const Icon(Icons.restore),
            onPressed: () async {
              final ok = await _client.request(4, payload: [0xFF, 0xFF, 0xFF, 0xFF]);
              _snack(ok != null && ok.isNotEmpty && ok[0] == 0
                  ? 'Recalled' : 'Recall failed');
              if (ok != null && ok.isNotEmpty && ok[0] == 0) await _loadVisibleFields();
            },
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
      itemCount: blocks.length + 1,
      separatorBuilder: (_, _) => const SizedBox(height: 6),
      itemBuilder: (context, index) {
        if (index < blocks.length) {
          return _blockCard(context, index, blocks[index]);
        } else {
          return _linkTile(
              context, Icons.dashboard_customize, 'Dynamic Memory',
              'User-created blocks with typed entries',
              () => DynamicMemoryPage(deviceId: widget.deviceId));
        }
      },
    );
  }

  Widget _linkTile(BuildContext context, IconData icon, String title,
      String subtitle, Widget Function() page) {
    return Card(
      color: kSurfaceAlt,
      margin: EdgeInsets.zero,
      child: ListTile(
        leading: Icon(icon, color: kOrange),
        title: Text(title, style: const TextStyle(fontWeight: FontWeight.w600)),
        subtitle: Text(subtitle, style: const TextStyle(fontSize: 12)),
        trailing: const Icon(Icons.chevron_right),
        onTap: () => Navigator.of(context)
            .push(MaterialPageRoute(builder: (_) => page())),
      ),
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
              if (action == 'edit' && !isSystem) {
                _editBlock(blockIndex, block);
              }
            },
            itemBuilder: (_) => [
              if (!isSystem)
                const PopupMenuItem(value: 'edit', child: Text('Rename / set type')),
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

    // For system block, collect all keys for this field
    List<({int key, ({BlockMeta meta, List<int> value})? field})> systemFieldKeys = [];
    if (isSystemField && cache != null) {
      final keys = _systemKeysForField(fieldIndex);
      for (final key in keys) {
        final extraField = cache[fieldIndex * 256 + key];
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
                }
              },
              itemBuilder: (_) => [
                if (!f.meta.readOnly)
                  const PopupMenuItem(value: 'edit', child: Text('Edit value')),
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
                isSystemField ? _formatSystemValue(field.meta.dataType, field.value) : formatValue(field.meta.dataType, field.value),
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
          }
        },
        itemBuilder: (_) => [
          if (!field.meta.readOnly)
            const PopupMenuItem(value: 'edit', child: Text('Edit value')),
        ],
      ),
      onTap: (!field.meta.readOnly) ? () => _editValue(blockType, inst, block, fieldIndex) : null,
    );
  }

  String _systemFieldName(int field) {
    switch (field) {
      case 0: return 'DeviceType';
      case 1: return 'Serial Number';
      case 2: return 'Short Address';
      case 3: return 'Time';
      case 4: return 'RAM';
      case 5: return 'Storage';
      case 6: return 'Name';
      case 7: return 'Reserved';
      case 8: return 'App Connected';
      default: return 'Field $field';
    }
  }

  String _systemStructMemberName(int field, int key) {
    switch (field) {
      case 0:
        switch (key) {
          case 0: return '.deviceType';
          case 1: return '.capabilities';
          case 2: return '.version';
        }
      case 3:
        switch (key) {
          case 0: return '.timeFromBoot';
          case 1: return '.now';
          case 2: return '.timeOffsetMs';
          case 3: return '.avgLoopTimeMs';
          case 4: return '.maxLoopTimeMs';
        }
      case 4:
        switch (key) {
          case 0: return '.freeRAM';
          case 1: return '.totalFlash';
        }
      case 5:
        switch (key) {
          case 0: return '.fileCount';
          case 1: return '.storageFlashSize';
        }
      case 8:
        switch (key) {
          case 0: return '.appConnected';
          case 1: return '.reserved';
        }
    }
    return 'Key $key';
  }

  String _formatSystemValue(DataType type, List<int> value) {
    switch (type) {
      case DataType.enum_:
        return value.isNotEmpty ? '0x${value[0].toRadixString(16).padLeft(2, '0')}' : '-';
      case DataType.sn:
        return serialNumberToHex(value);
      case DataType.id:
        return value.length >= 2 ? idToString(value[0] | (value[1] << 8)) : '-';
      case DataType.integer:
        if (value.length >= 4) return (value[0] | (value[1] << 8) | (value[2] << 16) | (value[3] << 24)).toString();
        if (value.length >= 2) return (value[0] | (value[1] << 8)).toString();
        return '-';
      case DataType.string:
        return String.fromCharCodes(value).replaceAll('\x00', '');
      case DataType.bool_:
        return value.isNotEmpty && value[0] != 0 ? 'true' : 'false';
      default:
        return formatValue(type, value);
    }
  }

  Future<void> _editBlock(int blockIndex, ({int type, int inst, BlockMeta meta, String name})? block) async {
    if (block == null) return;
    _snack('Block edit not implemented yet');
  }

  Future<void> _editValue(int blockType, int inst, ({int type, int inst, BlockMeta meta, String name})? block, int fieldIndex) async {
    if (block == null || !mounted) return;
    final cacheKey = (blockType << 8) | inst;
    final cache = _fieldCache[cacheKey];
    final field = cache?[fieldIndex];
    if (field == null || field.meta.readOnly) return;
    if (!mounted) return;

    final isSystemField = blockType == 0 && inst == 0;
    final blockInfo = blockInfoFor(BlockType.fromValue(block?.meta.typeValue ?? 0));
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

  @override
  void dispose() {
    super.dispose();
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