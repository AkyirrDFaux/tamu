import 'dart:async';

import 'package:flutter/material.dart';

import '../core/block_registry.dart';
import '../core/connection.dart';
import '../core/sysmem.dart';
import '../core/types.dart';
import 'theme.dart';
import 'value_editor.dart' show dataTypeLabel, formatValue, showValueEditor;
import 'dynmem_page.dart' show ChipLabel;
import 'widgets.dart';

/// System Memory service view (Docs/App/Service views/System Memory.md):
/// nested list of blocks and their entries, current vs backup views, editing,
/// save/recall and refresh with selectable autorefresh.
class SystemMemoryPage extends StatefulWidget {
  final int deviceId;

  const SystemMemoryPage({super.key, required this.deviceId});

  @override
  State<SystemMemoryPage> createState() => _SystemMemoryPageState();
}

class _SystemMemoryPageState extends State<SystemMemoryPage>
    with AutoRefreshMixin<SystemMemoryPage> {
  late final SystemMemoryClient _client =
      SystemMemoryClient(deviceId: widget.deviceId);

  bool _backupView = false;
  List<SysBlock>? _blocks;
  String? _error;
  final Set<int> _expanded = {};

  @override
  void initState() {
    super.initState();
    _refresh();
  }

  @override
  Future<void> onAutoRefresh() => _refresh();

  void _snack(String message) {
    if (!mounted) return;
    ScaffoldMessenger.of(context)
        .showSnackBar(SnackBar(content: Text(message)));
  }

  Future<void> _refresh() async {
    if (!ConnectionManager.instance.isConnected) return;
    final blocks = await _client.readBlocks();
    if (!mounted) return;
    setState(() {
      _error = blocks == null ? 'Device did not respond' : null;
      if (blocks != null) {
        // Keep already-loaded field values of blocks that still exist.
        final kept = <SysBlock>[];
        for (final fresh in blocks) {
          final old =
              _blocks?.where((b) => b.index == fresh.index).firstOrNull;
          if (old != null) {
            fresh.fields.addAll(old.fields);
          }
          kept.add(fresh);
        }
        _blocks = kept;
      }
    });
    if (!autoRefreshActive) await _loadVisibleFields();
  }

  /// Loads every visible (expanded) block's current values.
  Future<void> _loadVisibleFields() async {
    final blocks = _blocks;
    if (blocks == null || _backupView) return;
    for (final block in blocks) {
      if (!_expanded.contains(block.index)) continue;
      await _loadFields(block);
    }
    if (mounted) setState(() {});
  }

  Future<void> _loadFields(SysBlock block) async {
    final count = block.meta.size; // block meta carries the field count
    for (var f = 0; f < count; f++) {
      // Backup view reads what is STORED in the backup file (CID 4); the
      // current view reads the live value (CID 2).
      if (_backupView) {
        await _client.readBackupField(block, f);
      } else {
        await _client.readField(block, f);
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text('System Memory - ${idToString(widget.deviceId)}'),
        actions: [
          SegmentedButton<bool>(
            showSelectedIcon: false,
            style: const ButtonStyle(
                visualDensity: VisualDensity.compact,
                textStyle: WidgetStatePropertyAll(TextStyle(fontSize: 12))),
            segments: const [
              ButtonSegment(value: false, label: Text('Current')),
              ButtonSegment(value: true, label: Text('Backup')),
            ],
            selected: {_backupView},
            onSelectionChanged: (selection) {
              setState(() => _backupView = selection.first);
              _loadVisibleFields();
            },
          ),
          IconButton(
            tooltip: _backupView
                ? 'Recall everything from backup'
                : 'Save everything to backup',
            icon: Icon(_backupView ? Icons.restore : Icons.save),
            onPressed: () async {
              final ok = _backupView
                  ? await _client.recall()
                  : await _client.save();
              _snack(ok
                  ? (_backupView ? 'Recalled' : 'Saved')
                  : 'Operation failed');
              if (ok && !_backupView) await _loadVisibleFields();
            },
          ),
          RefreshButton(
            onRefresh: _refresh,
            autoActive: autoRefreshActive,
            refreshing: false,
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
    if (_blocks == null) {
      return Center(child: Text(_error ?? 'Loading...'));
    }
    if (_blocks!.isEmpty) {
      return const Center(child: Text('No memory blocks'));
    }
    return ListView.separated(
      padding: const EdgeInsets.fromLTRB(10, 10, 10, 24),
      itemCount: _blocks!.length,
      separatorBuilder: (_, _) => const SizedBox(height: 6),
      itemBuilder: (context, index) => _blockCard(context, _blocks![index]),
    );
  }

  Widget _blockCard(BuildContext context, SysBlock block) {
    final isExpanded = _expanded.contains(block.index);
    final flags = FieldFlags.describe(block.meta.flags);
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
                child: Text(block.name,
                    style: const TextStyle(fontWeight: FontWeight.w600))),
            Container(
              padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 1),
              decoration: BoxDecoration(
                  color: Colors.white.withAlpha(20),
                  borderRadius: BorderRadius.circular(4)),
              child: Text('#${block.index}',
                  style: const TextStyle(fontSize: 10, color: Colors.white54)),
            ),
          ]),
          subtitle: Padding(
            padding: const EdgeInsets.only(top: 2),
            child: Wrap(spacing: 6, runSpacing: 2, children: [
              ChipLabel(block.blockType.label),
              for (final flag in flags) ChipLabel(flag, subtle: flag != 'RO'),
              ChipLabel('${block.meta.size} fields', subtle: true),
            ]),
          ),
          trailing: Row(mainAxisSize: MainAxisSize.min, children: [
            if (!_backupView)
              IconButton(
                icon: const Icon(Icons.save_outlined, size: 20),
                tooltip: 'Save block',
                onPressed: () async {
                  final ok = await _client.save(block: block.index);
                  _snack(ok ? 'Block saved' : 'Save failed');
                },
              )
            else
              IconButton(
                icon: const Icon(Icons.restore_outlined, size: 20),
                tooltip: 'Recall block',
                onPressed: () async {
                  final ok = await _client.recall(block: block.index);
                  _snack(ok ? 'Block recalled' : 'Recall failed');
                },
              ),
            Icon(isExpanded ? Icons.expand_less : Icons.expand_more),
          ]),
          onTap: () async {
            setState(() {
              isExpanded
                  ? _expanded.remove(block.index)
                  : _expanded.add(block.index);
            });
            if (_expanded.contains(block.index)) {
              await _loadFields(block);
              if (mounted) setState(() {});
            }
          },
        ),
        if (isExpanded)
          Material(
            color: Colors.black26,
            child: Column(children: [
              const Divider(height: 1),
              for (var f = 0; f < block.meta.size; f++)
                _fieldTile(block, block.fields[f]),
            ]),
          ),
      ]),
    );
  }

  Widget _fieldTile(SysBlock block, SysField? field) {
    if (field == null) {
      return const ListTile(
          dense: true,
          leading: SizedBox(
              width: 16,
              height: 16,
              child: CircularProgressIndicator(strokeWidth: 2)),
          title: Text('...'));
    }
    final flags = FieldFlags.describe(field.meta.flags);
    final info =
        blockInfoFor(block.blockType)?.field(field.index);
    final valueText = valueWithUnit(
        formatValue(field.meta.dataType, field.value), info);
    return ListTile(
      dense: true,
      contentPadding: const EdgeInsets.only(left: 40, right: 12),
      title: Row(children: [
        SizedBox(
            width: 120,
            child: Text(info?.name ?? 'Field ${field.index}',
                style: const TextStyle(fontSize: 12, color: Colors.white54))),
        Expanded(
            child: Text(valueText,
                style: const TextStyle(fontFamily: 'monospace', fontSize: 13))),
        for (final flag in flags)
          Padding(
            padding: const EdgeInsets.only(left: 4),
            child: Text(flag,
                style: TextStyle(
                    fontSize: 10,
                    color: flag == 'RO' ? kOrange : Colors.white54)),
          ),
      ]),
      subtitle: Text('${dataTypeLabel(field.meta.dataType)}'
              '${info?.unit == null ? '' : ' [${info!.unit}]'}',
          style: const TextStyle(fontSize: 11)),
      trailing: _backupView
          ? IconButton(
              icon: const Icon(Icons.restore_outlined, size: 18),
              tooltip: 'Recall entry',
              onPressed: () async {
                final ok = await _client.recall(block: block.index);
                if (!mounted) return;
                ScaffoldMessenger.of(context).showSnackBar(SnackBar(
                    content: Text(ok ? 'Recalled' : 'Recall failed')));
                await _loadFields(block);
                if (mounted) setState(() {});
              },
            )
          : (field.notSaved || !field.readOnly)
              ? IconButton(
                  icon: const Icon(Icons.save_outlined, size: 18),
                  tooltip: 'Save entry',
                  onPressed: () async {
                    final ok = await _client.save(block: block.index);
                    _snack(ok ? 'Saved' : 'Save failed');
                  },
                )
              : null,
      onTap: (!_backupView && !field.readOnly)
          ? () => _editValue(block, field)
          : null,
    );
  }

  Future<void> _editValue(SysBlock block, SysField field) async {
    if (!mounted) return;
    final newValue = await showValueEditor(context, field.meta.dataType, field.value,
        info: blockInfoFor(block.blockType)?.field(field.index));
    if (newValue == null) return;
    final confirmed =
        await _client.writeField(block, field, newValue);
    _snack(confirmed != null ? 'Value written' : 'Write failed');
    if (confirmed != null) {
      setState(() => field.value = confirmed);
    }
  }
}
