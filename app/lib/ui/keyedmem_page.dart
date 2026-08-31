import 'dart:async';

import 'package:flutter/material.dart';

import '../core/connection.dart';
import '../core/keyedmem.dart';
import '../core/types.dart';
import 'dynmem_page.dart' show ChipLabel;
import 'theme.dart';
import 'value_editor.dart' show dataTypeLabel, formatValue, showValueEditor;
import 'widgets.dart';

/// Keyed Memory service view (Docs/App/Service views/Keyed Memory.md):
/// blocks -> dictionaries -> keyed entries, with editing, save/recall and
/// creation at every level.
class KeyedMemoryPage extends StatefulWidget {
  final int deviceId;

  const KeyedMemoryPage({super.key, required this.deviceId});

  @override
  State<KeyedMemoryPage> createState() => _KeyedMemoryPageState();
}

class _KeyedMemoryPageState extends State<KeyedMemoryPage>
    with AutoRefreshMixin<KeyedMemoryPage> {
  late final KeyedMemoryClient _client =
      KeyedMemoryClient(deviceId: widget.deviceId);

  List<KeyedBlock>? _blocks;
  String? _error;
  final Set<int> _openBlocks = {};
  final Set<int> _openDicts = {};
  bool _backupView = false;
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
      // Preserve the open dicts/entries from the previous blocks so a refresh does
      // not blank the expanded view (no flicker); the reload below updates them in
      // place.
      for (final fresh in blocks) {
        final old = _blocks?.where((b) => b.index == fresh.index).firstOrNull;
        if (old != null) {
          fresh.dicts.addAll(old.dicts);
          fresh.entries.addAll(old.entries);
        }
      }
      _blocks = blocks;
      // Reload the open dicts' entries, then rebuild once.
      for (final b in blocks) {
        if (!_openBlocks.contains(b.index)) continue;
        for (var d = 0; d < b.dictCount; d++) {
          final dict = await _client.readDict(b, d);
          if (dict != null && _openDicts.contains(dict.index)) {
            await _loadDictEntries(b, dict);
          }
        }
      }
      if (mounted) setState(() {});
    } finally {
      _refreshing = false;
    }
  }

  Future<void> _loadDictEntries(KeyedBlock block, KeyedDict dict) async {
    if (_backupView) {
      for (final key in dict.keys) {
        await _client.readBackupEntry(block, dict.index, key);
      }
      return;
    }
    for (final key in dict.keys) {
      await _client.readEntry(block, dict.index, key);
    }
  }

  // ---------------------------------------------------------------------------
  // Actions
  // ---------------------------------------------------------------------------

  Future<void> _createBlock() async {
    final result = await promptBlockNameAndType(context,
        title: 'New keyed block', withIndex: true);
    if (result == null || !mounted) return;
    final (name, type, index) = result;
    final created =
        await _client.createBlock(type, name, index: index);
    _snack(created != null ? 'Block created' : 'Create failed');
    await _refresh();
  }

  Future<void> _editBlock(KeyedBlock block) async {
    final result = await promptBlockNameAndType(context,
        title: 'Edit block', initialName: block.name, initialType: block.blockType);
    if (result == null || !mounted) return;
    final (name, type, _) = result;
    final ok = await _client.writeBlockMeta(block, name, type);
    _snack(ok ? 'Block updated' : 'Update failed');
    await _refresh();
  }

  Future<void> _deleteBlock(KeyedBlock block) async {
    final ok = await _client.delete(block: block.index);
    _snack(ok ? 'Block deleted (save to free)' : 'Delete failed');
    await _refresh();
  }

  Future<void> _addDictionary(KeyedBlock block) async {
    // The cached count may be stale (another add earlier in this session):
    // appending at an outdated index would silently retype an existing
    // dictionary instead of creating a new one.
    final fresh = await _client.refreshBlockMeta(block) ?? block;
    final index = await _promptDictIndex(fresh.dictCount);
    if (index == _promptCancelled) return;
    final dataType = await _pickDataType(allowUndefined: true);
    if (dataType == null || !mounted) return;
    // A plain append (null index) fills the FIRST deleted (None) dictionary
    // placeholder so the block does not accumulate uneditable None rows.
    final target = index ?? _firstNoneDict(block);
    final ok = await _client.appendDict(fresh, index: target, type: dataType);
    _snack(ok ? 'Dictionary added' : 'Add failed');
    await _refresh();
    if (_openBlocks.contains(block.index) && mounted) {
      final reloaded =
          _blocks?.where((b) => b.index == block.index).firstOrNull;
      if (reloaded != null && reloaded.dictCount > 0) {
        await _client.readDict(reloaded, target ?? reloaded.dictCount - 1);
        setState(() {});
      }
    }
  }

  /// The lowest None-marked dictionary index (a deleted slot to fill), or null.
  int? _firstNoneDict(KeyedBlock block) {
    for (var d = 0; d < block.dictCount; d++) {
      final existing = block.dicts[d];
      if (existing != null && existing.meta.dataType == DataType.none) return d;
    }
    return null;
  }

  static const _promptCancelled = -2;

  /// Optional dictionary index (empty = append); -2 = cancelled.
  Future<int?> _promptDictIndex(int maxAppend) async {
    final controller = TextEditingController();
    final result = await showDialog<int?>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Dictionary index'),
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
    return result;
  }

  Future<void> _deleteDict(KeyedBlock block, KeyedDict dict) async {
    final ok = await _client.delete(block: block.index, dict: dict.index);
    _snack(ok ? 'Dictionary deleted' : 'Delete failed');
    await _refresh();
  }

  /// Changes a dictionary's data type (the firmware updates the dict meta in
  /// place; a None-marked dict is recreated fresh, dropping stale entries).
  Future<void> _changeDictType(KeyedBlock block, KeyedDict dict) async {
    final dataType = await _pickDataType(allowUndefined: true);
    if (dataType == null || !mounted) return;
    final ok = await _client.writeDictMeta(block, dict.index, dataType);
    _snack(ok
        ? 'Dictionary type: ${dataTypeLabel(dataType)}'
        : 'Type change failed');
    // Re-read the dict so its meta type refreshes.
    final fresh = await _client.readDict(block, dict.index);
    if (fresh != null && _openDicts.contains(dict.index)) {
      await _loadDictEntries(block, fresh);
    }
    if (mounted) setState(() {});
  }

  /// Picks a data type from a simple dialog; `allowUndefined` offers the
  /// "Undefined" option (used for dictionaries), otherwise it is excluded
  /// (entries need a concrete type). Returns null when cancelled.
  Future<DataType?> _pickDataType({bool allowUndefined = false}) {
    return showDialog<DataType>(
      context: context,
      builder: (context) => SimpleDialog(
        title: const Text('Data type'),
        children: [
          for (final t in DataType.values)
            if (t != DataType.deleted &&
                t != DataType.none &&
                (allowUndefined || t != DataType.undefined))
              SimpleDialogOption(
                onPressed: () => Navigator.pop(context, t),
                child: Text(dataTypeLabel(t)),
              ),
        ],
      ),
    );
  }

  /// Adds a keyed entry to a dictionary: pick data type + key id, edit value.
  /// Entries are KEY-addressed (a key identifies its value); position/order in
  /// the dictionary is irrelevant.
  Future<void> _addEntry(KeyedBlock block, KeyedDict dict) async {
    final dataType = await _pickDataType();
    if (dataType == null || !mounted) return;
    final keyText = await showDialog<String>(
      context: context,
      builder: (context) {
        final controller = TextEditingController(
            text: dict.keys.isEmpty
                ? '00'
                : (() {
                    // Keys are byte values; a max key of 0xFF must not suggest
                    // 0x100 ("100") - the field is capped at 2 hex chars.
                    final next =
                        dict.keys.reduce((a, b) => a > b ? a : b) + 1;
                    return (next > 0xFF ? 0xFF : next)
                        .toRadixString(16)
                        .padLeft(2, '0')
                        .toUpperCase();
                  })());
        return AlertDialog(
          title: const Text('Key (hex byte)'),
          content:
              TextField(controller: controller, autofocus: true, maxLength: 2),
          actions: [
            TextButton(
                onPressed: () => Navigator.pop(context),
                child: const Text('Cancel')),
            FilledButton(
                onPressed: () =>
                    Navigator.pop(context, controller.text.trim()),
                child: const Text('OK')),
          ],
        );
      },
    );
    if (keyText == null || !mounted) return;
    final key = int.tryParse(keyText.replaceFirst('0x', ''), radix: 16);
    if (key == null || key < 0 || key > 0xFF) {
      _snack('Invalid key');
      return;
    }
    final seed = await showValueEditor(context, dataType, []);
    if (seed == null || !mounted) return;
    // Fresh metadata: the dictionary's key list may have changed since render.
    final liveBlock = await _client.refreshBlockMeta(block) ?? block;
    final liveDict = await _client.readDict(liveBlock, dict.index) ?? dict;
    if (!mounted) return;
    if (liveDict.keys.contains(key)) {
      _snack('Key 0x${key.toRadixString(16)} already exists');
      return;
    }
    final confirmed = await _client.writeKeyValue(
        liveBlock,
        dict.index,
        key,
        BlockMeta(flagsAndType: dataType.value, key: key, size: seed.length),
        seed);
    _snack(confirmed != null ? 'Entry added' : 'Add failed');
    await _refresh();
    if (mounted) {
      final reloaded =
          _blocks?.where((b) => b.index == block.index).firstOrNull;
      if (reloaded != null) {
        final freshDict = await _client.readDict(reloaded, dict.index);
        if (freshDict != null) {
          await _loadDictEntries(reloaded, freshDict);
          if (mounted) setState(() {});
        }
      }
    }
  }

  // ---------------------------------------------------------------------------
  // Build
  // ---------------------------------------------------------------------------

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text('Keyed Memory - ${idToString(widget.deviceId)}'),
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
              _refresh();
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
              _snack(ok ? (_backupView ? 'Recalled' : 'Saved') : 'Operation failed');
              if (ok && !_backupView) await _refresh();
            },
          ),
          IconButton(
              tooltip: 'New keyed block',
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
    if (blocks == null) return Center(child: Text(_error ?? 'Loading...'));
    if (blocks.isEmpty) {
      return Center(
          child: Column(mainAxisSize: MainAxisSize.min, children: [
        const Text('No keyed blocks'),
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

  Widget _blockCard(BuildContext context, KeyedBlock block) {
    final blockOpen = _openBlocks.contains(block.index);
    return Card(
      color: kSurfaceAlt,
      margin: EdgeInsets.zero,
      clipBehavior: Clip.antiAlias,
      child: Column(children: [
        ListTile(
          leading: Icon(blockOpen ? Icons.folder_open : Icons.folder,
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
              ChipLabel('${block.dictCount} dictionaries', subtle: true),
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
              blockOpen
                  ? _openBlocks.remove(block.index)
                  : _openBlocks.add(block.index);
            });
            if (_openBlocks.contains(block.index)) {
              for (var d = 0; d < block.dictCount; d++) {
                await _client.readDict(block, d);
              }
              if (mounted) setState(() {});
            }
          },
        ),
        if (blockOpen)
          Material(
            color: Colors.black26,
            child: Column(children: [
              const Divider(height: 1),
              if (block.dictCount > 0)
                for (var d = 0; d < block.dictCount; d++)
                  _dictTile(block, block.dicts[d]),
              ListTile(
                dense: true,
                contentPadding: const EdgeInsets.only(left: 40, right: 12),
                leading: const Icon(Icons.add, size: 18, color: kOrange),
                title: const Text('Add dictionary',
                    style: TextStyle(fontSize: 13, color: kOrange)),
                onTap: () => _addDictionary(block),
              ),
            ]),
          ),
      ]),
    );
  }

  Widget _dictTile(KeyedBlock block, KeyedDict? dict) {
    if (dict == null) {
      return const ListTile(
          dense: true,
          contentPadding: EdgeInsets.only(left: 40, right: 12),
          leading: SizedBox(
              width: 14,
              height: 14,
              child: CircularProgressIndicator(strokeWidth: 2)),
          title: Text('...',
              style: TextStyle(fontSize: 13, color: Colors.white38)));
    }
    // A None-marked dictionary is a deleted placeholder: show it grayed and
    // non-interactive (its index stays reserved until save).
    if (dict.meta.dataType == DataType.none) {
      return ListTile(
        dense: true,
        contentPadding: const EdgeInsets.only(left: 40, right: 12),
        leading: const Icon(Icons.folder_off_outlined,
            size: 20, color: Colors.white24),
        title: Text('Dictionary ${dict.index} (deleted)',
            style: const TextStyle(fontSize: 13, color: Colors.white38)),
        trailing: const Icon(Icons.delete_outline, size: 16, color: Colors.white24),
      );
    }
    final dictOpen = _openDicts.contains(dict.index);
    return Column(children: [
      ListTile(
        dense: true,
        contentPadding: const EdgeInsets.only(left: 40, right: 12),
        leading: Icon(dictOpen ? Icons.folder_open : Icons.folder,
            size: 20, color: Colors.white70),
        title: Row(children: [
          Text('Dictionary ${dict.index}',
              style: const TextStyle(fontSize: 13)),
          const SizedBox(width: 8),
          ChipLabel('${dict.keys.length} keys', subtle: true),
          const SizedBox(width: 6),
          // Dictionaries are typed; tapping edits the type (undefined = not set).
          InkWell(
            onTap: () => _changeDictType(block, dict),
            child: ChipLabel(dataTypeLabel(dict.meta.dataType), subtle: true),
          ),
        ]),
        trailing: Row(mainAxisSize: MainAxisSize.min, children: [
          IconButton(
            icon: const Icon(Icons.tune, size: 18),
            tooltip: 'Dictionary type',
            onPressed: () => _changeDictType(block, dict),
          ),
          IconButton(
            icon: const Icon(Icons.delete_outline, size: 18),
            tooltip: 'Delete dictionary',
            onPressed: () => _deleteDict(block, dict),
          ),
          Icon(dictOpen ? Icons.expand_less : Icons.expand_more, size: 20),
        ]),
        onTap: () async {
          setState(() {
            dictOpen
                ? _openDicts.remove(dict.index)
                : _openDicts.add(dict.index);
          });
          if (_openDicts.contains(dict.index)) {
            await _client.readDict(block, dict.index);
            final fresh = block.dicts[dict.index];
            if (fresh != null) {
              await _loadDictEntries(block, fresh);
            }
            if (mounted) setState(() {});
          }
        },
      ),
      if (dictOpen)
        Material(
          color: Colors.black12,
          child: Column(children: [
            for (final key in dict.sortedKeys)
              _entryTile(block, dict, block.entries[dict.index]?[key]),
            ListTile(
              dense: true,
              contentPadding: const EdgeInsets.only(left: 64, right: 12),
              leading: const Icon(Icons.add, size: 16, color: kOrange),
              title: const Text('Add entry',
                  style: TextStyle(fontSize: 12, color: kOrange)),
              onTap: () => _addEntry(block, dict),
            ),
          ]),
        ),
    ]);
  }

  Widget _entryTile(KeyedBlock block, KeyedDict dict, KeyedEntry? entry) {
    if (entry == null) {
      return const ListTile(
          dense: true,
          contentPadding: EdgeInsets.only(left: 64, right: 12),
          leading: SizedBox(
              width: 12,
              height: 12,
              child: CircularProgressIndicator(strokeWidth: 2)),
          title: Text('...',
              style: TextStyle(fontSize: 12, color: Colors.white38)));
    }
    final flags = FieldFlags.describe(entry.meta.flags);
    final valueText = formatValue(entry.meta.dataType, entry.value);
    // Keyed entries are user-defined (no fixed schema): the key is the identity.
    return ListTile(
      dense: true,
      contentPadding: const EdgeInsets.only(left: 64, right: 12),
      title: Row(children: [
        SizedBox(
            width: 40,
            child: Text(keyLabel(entry.key),
                style: const TextStyle(color: kOrange, fontSize: 11))),
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
      subtitle: Text(dataTypeLabel(entry.meta.dataType),
          style: const TextStyle(fontSize: 11)),
      onTap: !entry.readOnly
          ? () => _editValue(block, dict, entry)
          : null,
      trailing: _backupView
        ? IconButton(
            icon: const Icon(Icons.restore_outlined,
                size: 17, color: Colors.white38),
            tooltip: 'Recall entry',
            onPressed: () async {
              final ok = await _client.recall(block: block.index);
              _snack(ok ? 'Recalled' : 'Recall failed');
              if (!ok) return;
              final fresh = block.dicts[dict.index];
              if (fresh != null) await _loadDictEntries(block, fresh);
              if (mounted) setState(() {});
            },
          )
        : IconButton(
            icon: const Icon(Icons.delete_outline,
                size: 17, color: Colors.white38),
            tooltip: 'Delete entry (marks None in place)',
            onPressed: () => _deleteEntry(block, dict, entry),
          ),
    );
  }

  Future<void> _deleteEntry(
      KeyedBlock block, KeyedDict dict, KeyedEntry entry) async {
    if (!mounted) return;
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text('Delete entry ${keyLabel(entry.key)}?'),
        content: const Text(
            'The key is marked None in place - other keys keep their values. '
            'Re-add the same key to restore it.'),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(context, false),
              child: const Text('Cancel')),
          FilledButton(
              onPressed: () => Navigator.pop(context, true),
              child: const Text('Delete')),
        ],
      ),
    );
    if (confirmed != true) return;
    // Real delete (CID 1): the firmware marks the key's meta None in place -
    // keys are content-addressed, so nothing shifts (Docs/Data Formats.md).
    final ok = await _client.delete(block: block.index, dict: dict.index, key: entry.key);
    if (!ok) {
      _snack('Delete failed');
      return;
    }
    // Re-read the dict so keys list and entries refresh (the deleted key
    // disappears, placeholders stay invisible).
    await _client.readDict(block, dict.index);
    final fresh = block.dicts[dict.index];
    if (fresh != null) {
      await _loadDictEntries(block, fresh);
    }
    if (mounted) setState(() {});
    _snack('Entry marked None (save to reclaim space)');
  }

  static String keyLabel(int key) =>
      '0x${key.toRadixString(16).padLeft(2, '0').toUpperCase()}';

  Future<void> _editValue(
      KeyedBlock block, KeyedDict dict, KeyedEntry entry) async {
    if (!mounted) return;
    final newValue =
        await showValueEditor(context, entry.meta.dataType, entry.value);
    if (newValue == null) return;
    final confirmed =
        await _client.writeEntry(block, dict.index, entry, newValue);
    _snack(confirmed != null ? 'Value written' : 'Write failed');
    if (confirmed != null) {
      setState(() => entry.value = confirmed);
    }
  }
}
