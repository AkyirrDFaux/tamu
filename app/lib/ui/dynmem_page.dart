import 'dart:async';

import 'package:flutter/material.dart';

import '../core/block_registry.dart';
import '../core/connection.dart';
import '../core/dynmem.dart';
import '../core/types.dart';
import 'theme.dart';
import 'value_editor.dart' show dataTypeLabel, formatValue, showValueEditor;
import 'widgets.dart';

/// Dynamic Memory service view (Docs/App/Service views/Dynamic Memory.md):
/// nested list of user blocks and entries with editing, save/recall,
/// create/delete and per-block type/name editing.
class DynamicMemoryPage extends StatefulWidget {
  final int deviceId;

  const DynamicMemoryPage({super.key, required this.deviceId});

  @override
  State<DynamicMemoryPage> createState() => _DynamicMemoryPageState();
}

class _DynamicMemoryPageState extends State<DynamicMemoryPage>
    with AutoRefreshMixin<DynamicMemoryPage> {
  late final DynamicMemoryClient _client =
      DynamicMemoryClient(deviceId: widget.deviceId);

  bool _backupView = false;
  List<DynBlock>? _blocks;
  String? _error;
  final Set<int> _expanded = {};
  bool _refreshing = false;


  @override
  void initState() {
    super.initState();
    _refresh();
    // Docs: "Automatically refreshes the visible view (0.5s)" - start the timer now
    // (post-frame so applyAuto's setState is legal) instead of waiting for the user
    // to open the refresh menu, so live values track the hardware.
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) applyAuto(const Duration(milliseconds: 500));
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
    if (_refreshing) return; // skip a tick while one is still in flight
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
      // Keep already-loaded field values of blocks that still exist so the visible
      // (expanded) blocks keep their content during the reload (no flicker).
      final kept = <DynBlock>[];
      for (final fresh in blocks) {
        final old = _blocks?.where((b) => b.index == fresh.index).firstOrNull;
        if (old != null) fresh.fields.addAll(old.fields);
        kept.add(fresh);
      }
      _blocks = kept;
      // Reload every visible (expanded) block's entries so live values track the
      // hardware, then rebuild once (_loadVisibleFields ends with setState).
      await _loadVisibleFields();
    } finally {
      _refreshing = false;
    }
  }

  Future<void> _loadVisibleFields() async {
    final blocks = _blocks;
    if (blocks == null) return;
    for (final block in blocks) {
      if (!_expanded.contains(block.index)) continue;
      await _loadBlockFields(block);
    }
    if (mounted) setState(() {});
  }

  Future<void> _loadBlockFields(DynBlock block) async {
    for (var f = 0; f < block.fieldCount; f++) {
      // Backup view reads what is STORED in the backup file (CID 4); the
      // current view reads the live value (CID 2).
      if (_backupView) {
        await _client.readBackupField(block, f);
      } else {
        await _client.readField(block, f);
      }
    }
  }


  // ---------------------------------------------------------------------------
  // Actions
  // ---------------------------------------------------------------------------

  Future<void> _createBlock() async {
    final result = await promptBlockNameAndType(context,
        title: 'New dynamic block', withIndex: true);
    if (result == null || !mounted) return;
    final (name, type, index) = result;
    final created =
        await _client.createBlock(type, name, index: index);
    _snack(created != null ? 'Block created' : 'Create failed');
    await _refresh();
  }

  Future<void> _editBlock(DynBlock block) async {
    final result = await promptBlockNameAndType(context,
        title: 'Edit block', initialName: block.name, initialType: block.blockType);
    if (result == null || !mounted) return;
    final (name, type, _) = result;
    final ok = await _client.writeBlockMeta(block, name, type);
    _snack(ok ? 'Block updated' : 'Update failed');
    await _refresh();
  }

  Future<void> _deleteBlock(DynBlock block) async {
    final ok = await _client.delete(block: block.index);
    _snack(ok ? 'Block deleted (save to free)' : 'Delete failed');
    await _refresh();
  }

  /// Appends an entry: pick a data type, enter its initial value.
  Future<void> _addEntry(DynBlock block) async {
    final dataType = await _pickDataType();
    if (dataType == null || !mounted) return;
    // Initial value via the type's editor (empty current bytes).
    final seed = await showValueEditor(context, dataType, []);
    if (seed == null || !mounted) return;
    // Optional explicit index fills a None placeholder instead of appending.
    final index = await _promptIndex(block.fieldCount);
    if (index == _promptCancelled) return;
    final live = await _client.refreshBlockMeta(block) ?? block;
    // A plain append (null index) fills the FIRST deleted (None) placeholder so
    // the block does not accumulate uneditable None rows; otherwise it lands at
    // the end.
    final target = index ?? _firstNoneField(block, live.fieldCount);
    final confirmed = await _client.appendEntry(
        live,
        BlockMeta(flagsAndType: dataType.value, size: seed.length),
        seed,
        index: target);
    _snack(confirmed != null ? 'Entry added' : 'Add failed');
    await _refresh();
    // _refresh() swaps in fresh DynBlock objects; reload into the fresh one so
    // the new entry renders instead of a detached copy's spinner.
    final fresh = _blocks?.where((b) => b.index == block.index).firstOrNull;
    if (fresh != null && _expanded.contains(fresh.index)) {
      await _loadBlockFields(fresh);
      if (mounted) setState(() {});
    }
  }

  /// The lowest None-marked field index (a deleted slot to fill), or null.
  int? _firstNoneField(DynBlock block, int count) {
    for (var f = 0; f < count; f++) {
      final existing = block.fields[f];
      if (existing != null && existing.meta.dataType == DataType.none) return f;
    }
    return null;
  }

  static const _promptCancelled = -2;

  /// Asks for an optional entry index; null appends, -1 skips the prompt.
  Future<int?> _promptIndex(int maxAppend) async {
    final controller = TextEditingController();
    final choice = await showDialog<int?>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Entry index'),
        content: TextField(
          controller: controller,
          keyboardType: TextInputType.number,
          decoration: InputDecoration(
              labelText: 'Index (empty = append at $maxAppend)',
              helperText: 'Fills a deleted (None) slot when given'),
        ),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(context, _promptCancelled),
              child: const Text('Cancel')),
          TextButton(
              onPressed: () => Navigator.pop(context, null),
              child: const Text('Append')),
          FilledButton(
            onPressed: () {
              final t = controller.text.trim();
              if (t.isEmpty) { Navigator.pop(context, null); return; }
              final v = int.tryParse(t);
              if (v == null || v < 0) return;
              Navigator.pop(context, v);
            },
            child: const Text('OK'),
          ),
        ],
      ),
    );
    controller.dispose();
    return choice;
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

  // ---------------------------------------------------------------------------
  // Build
  // ---------------------------------------------------------------------------

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text('Dynamic Memory - ${idToString(widget.deviceId)}'),
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
          IconButton(
              tooltip: 'New block',
              icon: const Icon(Icons.add),
              onPressed: _createBlock),
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
    final blocks = _blocks;
    if (blocks == null) {
      return Center(child: Text(_error ?? 'Loading...'));
    }
    if (blocks.isEmpty) {
      return Center(
          child: Column(mainAxisSize: MainAxisSize.min, children: [
        const Text('No dynamic blocks'),
        const SizedBox(height: 12),
        OutlinedButton.icon(
            onPressed: _createBlock,
            icon: const Icon(Icons.add),
            label: const Text('Create one')),
      ]));
    }
    return ListView.separated(
      padding: const EdgeInsets.fromLTRB(10, 10, 10, 24),
      itemCount: blocks.length,
      separatorBuilder: (_, _) => const SizedBox(height: 6),
      itemBuilder: (context, index) =>
          _blockCard(context, blocks[index]),
    );
  }

  Widget _blockCard(BuildContext context, DynBlock block) {
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
              ChipLabel('${block.fieldCount} entries', subtle: true),
            ]),
          ),
          trailing: PopupMenuButton<String>(
            tooltip: 'Block actions',
            onSelected: (action) {
              switch (action) {
                case 'edit':
                  _editBlock(block);
                case 'delete':
                  _deleteBlock(block);
              }
            },
            itemBuilder: (_) => const [
              PopupMenuItem(value: 'edit', child: Text('Rename / set type')),
              PopupMenuItem(value: 'delete', child: Text('Delete')),
            ],
          ),
          onTap: () async {
            setState(() {
              isExpanded
                  ? _expanded.remove(block.index)
                  : _expanded.add(block.index);
            });
            if (_expanded.contains(block.index)) {
              await _loadBlockFields(block);
              if (mounted) setState(() {});
            }
          },
        ),
        if (isExpanded)
          Material(
            color: Colors.black26,
            child: Column(children: [
              const Divider(height: 1),
              if (!_backupView && block.fieldCount > 0)
                for (var f = 0; f < block.fieldCount; f++)
                  _fieldTile(block, block.fields[f]),
              ListTile(
                dense: true,
                contentPadding: const EdgeInsets.only(left: 40, right: 12),
                leading: const Icon(Icons.add, size: 18, color: kOrange),
                title: const Text('Add entry',
                    style: TextStyle(fontSize: 13, color: kOrange)),
                onTap: () => _addEntry(block),
              ),
            ]),
          ),
      ]),
    );
  }

  Widget _fieldTile(DynBlock block, DynField? field) {
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
    // A None-marked field is a deleted/empty placeholder: not editable, but the
    // next "Add entry" fills it (indexes never move - Docs/Data Formats.md).
    if (field.meta.dataType == DataType.none) {
      return ListTile(
        dense: true,
        contentPadding: const EdgeInsets.only(left: 40, right: 12),
        leading: const Icon(Icons.delete_outline, size: 18, color: Colors.white24),
        title: Text('Entry ${field.index} (deleted)',
            style: const TextStyle(fontSize: 12, color: Colors.white38)),
        subtitle: const Text('Add entry fills this slot',
            style: TextStyle(fontSize: 10, color: Colors.white24)),
      );
    }
    final flags = FieldFlags.describe(field.meta.flags);
    final info = blockInfoFor(block.blockType)?.field(field.index);
    final valueText =
        valueWithUnit(formatValue(field.meta.dataType, field.value), info);
    return ListTile(
      dense: true,
      contentPadding: const EdgeInsets.only(left: 40, right: 12),
      title: Row(children: [
        SizedBox(
            width: 120,
            child: Text(info?.name ?? 'Entry ${field.index}',
                style: const TextStyle(fontSize: 12, color: Colors.white54))),
        Expanded(
            child: Text(valueText,
                style: const TextStyle(fontFamily: 'monospace', fontSize: 13))),
        for (final flag in flags)
          Padding(
            padding: const EdgeInsets.only(left: 4),
            child: Text(flag,
                style: TextStyle(
                    fontSize: 9,
                    fontWeight: FontWeight.w700,
                    color: flag == 'RO' ? kOrange : Colors.white38)),
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
                // Refresh the shown (backup) value now that recall rewrote it.
                await _client.readBackupField(block, field.index);
                if (mounted) setState(() {});
              },
            )
          : PopupMenuButton<String>(
              icon: const Icon(Icons.more_vert, size: 18),
              tooltip: 'Entry actions',
              onSelected: (action) {
                switch (action) {
                  case 'edit':
                    _editValue(block, field);
                  case 'type':
                    _changeType(block, field);
                  case 'delete':
                    _deleteEntry(block, field);
                }
              },
              itemBuilder: (_) => [
                if (!field.readOnly)
                  const PopupMenuItem(value: 'edit', child: Text('Edit value')),
                if (!field.readOnly)
                  const PopupMenuItem(
                      value: 'type', child: Text('Change type')),
                const PopupMenuItem(value: 'delete', child: Text('Delete')),
              ],
            ),
      onTap: (!_backupView && !field.readOnly)
          ? () => _editValue(block, field)
          : null,
    );
  }

  Future<void> _changeType(DynBlock block, DynField field) async {
    final dataType = await _pickDataType();
    if (dataType == null || !mounted) return;
    // Seed a fresh value of the new type via its editor.
    final seed = await showValueEditor(context, dataType, []);
    if (seed == null || !mounted) return;
    final confirmed = await _client.writeField(block, field, seed,
        newType: dataType);
    _snack(confirmed != null ? 'Type changed' : 'Write failed');
    if (confirmed != null) {
      setState(() => field.value = confirmed);
      await _client.readField(block, field.index);
      if (mounted) setState(() {});
    }
  }

  Future<void> _deleteEntry(DynBlock block, DynField field) async {
    final ok =
        await _client.delete(block: block.index, field: field.index);
    _snack(ok ? 'Entry deleted' : 'Delete failed');
    await _refresh();
    // Reload into the fresh block from _blocks (the captured one is detached).
    final fresh = _blocks?.where((b) => b.index == block.index).firstOrNull;
    if (fresh != null && _expanded.contains(fresh.index)) {
      await _loadBlockFields(fresh);
      if (mounted) setState(() {});
    }
  }

  Future<void> _editValue(DynBlock block, DynField field) async {
    if (!mounted) return;
    final newValue = await showValueEditor(
        context, field.meta.dataType, field.value,
        info: blockInfoFor(block.blockType)?.field(field.index));
    if (newValue == null) return;
    final confirmed = await _client.writeField(block, field, newValue);
    _snack(confirmed != null ? 'Value written' : 'Write failed');
    if (confirmed != null) {
      setState(() => field.value = confirmed);
    }
  }
}

/// Small rounded label chip used across the memory pages.
class ChipLabel extends StatelessWidget {
  final String text;
  final bool subtle;

  const ChipLabel(this.text, {super.key, this.subtle = false});

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
