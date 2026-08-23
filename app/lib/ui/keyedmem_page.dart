import 'package:flutter/material.dart';

import '../core/connection.dart';
import '../core/keyedmem.dart';
import '../core/types.dart';
import 'sysmem_page.dart' show formatValue, showValueEditor, dataTypeLabel;
import 'theme.dart';

/// Keyed Memory service view (Docs/App/Service views/Keyed Memory.md):
/// blocks -> dictionaries -> keyed entries, with editing and save/recall.
class KeyedMemoryPage extends StatefulWidget {
  final int deviceId;

  const KeyedMemoryPage({super.key, required this.deviceId});

  @override
  State<KeyedMemoryPage> createState() => _KeyedMemoryPageState();
}

class _KeyedMemoryPageState extends State<KeyedMemoryPage> {
  late final KeyedMemoryClient _client =
      KeyedMemoryClient(deviceId: widget.deviceId);

  List<KeyedBlock>? _blocks;
  String? _error;
  final Set<int> _openBlocks = {};
  final Set<int> _openDicts = {};

  @override
  void initState() {
    super.initState();
    _refresh();
  }

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
      if (blocks != null) _blocks = blocks;
    });
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text('Keyed Memory - ${idToString(widget.deviceId)}'),
        actions: [
          IconButton(
            tooltip: 'Save everything to backup',
            icon: const Icon(Icons.save),
            onPressed: () async {
              final ok = await _client.save();
              _snack(ok ? 'Saved' : 'Operation failed');
            },
          ),
          IconButton(
            tooltip: 'Recall everything from backup',
            icon: const Icon(Icons.restore),
            onPressed: () async {
              final ok = await _client.recall();
              _snack(ok ? 'Recalled' : 'Operation failed');
              await _refresh();
            },
          ),
          IconButton(onPressed: _refresh, tooltip: 'Refresh',
              icon: const Icon(Icons.refresh)),
        ],
      ),
      body: _buildBody(),
    );
  }

  Widget _buildBody() {
    final blocks = _blocks;
    if (blocks == null) return Center(child: Text(_error ?? 'Loading...'));
    if (blocks.isEmpty) return const Center(child: Text('No keyed blocks'));
    return ListView.builder(
      itemCount: blocks.length,
      itemBuilder: (context, index) {
        final block = blocks[index];
        final blockOpen = _openBlocks.contains(block.index);
        return Column(children: [
          ListTile(
            leading: Icon(blockOpen ? Icons.folder_open : Icons.folder),
            title: Text(block.name),
            subtitle:
                Text('${block.blockType.label}   ${block.dictCount} dictionaries'),
            trailing: IconButton(
              icon: const Icon(Icons.delete_outline, size: 20),
              tooltip: 'Delete block',
              onPressed: () async {
                final ok = await _client.delete(block: block.index);
                _snack(ok ? 'Block deleted' : 'Delete failed');
                await _refresh();
              },
            ),
            onTap: () async {
              setState(() {
                if (blockOpen) {
                  _openBlocks.remove(block.index);
                } else {
                  _openBlocks.add(block.index);
                }
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
            for (var d = 0; d < block.dictCount; d++)
              _dictTile(block, block.dicts[d]),
          const Divider(height: 1),
        ]);
      },
    );
  }

  Widget _dictTile(KeyedBlock block, KeyedDict? dict) {
    if (dict == null) {
      return const ListTile(
          dense: true,
          contentPadding: EdgeInsets.only(left: 40, right: 12),
          leading: SizedBox(
              width: 16,
              height: 16,
              child: CircularProgressIndicator(strokeWidth: 2)),
          title: Text('...'));
    }
    final dictOpen = _openDicts.contains(dict.index);
    return Column(children: [
      ListTile(
        dense: true,
        contentPadding: const EdgeInsets.only(left: 40, right: 12),
        leading: Icon(dictOpen ? Icons.folder_open : Icons.folder, size: 20),
        title: Text('Dictionary ${dict.index}'),
        subtitle: Text('${dict.keys.length} keys'),
        trailing: Row(mainAxisSize: MainAxisSize.min, children: [
          IconButton(
            icon: const Icon(Icons.delete_outline, size: 18),
            tooltip: 'Delete dictionary',
            onPressed: () async {
              final ok =
                  await _client.delete(block: block.index, dict: dict.index);
              _snack(ok ? 'Dictionary deleted' : 'Delete failed');
              await _refresh();
            },
          ),
          Icon(dictOpen ? Icons.expand_less : Icons.expand_more, size: 20),
        ]),
        onTap: () async {
          setState(() {
            if (dictOpen) {
              _openDicts.remove(dict.index);
            } else {
              _openDicts.add(dict.index);
            }
          });
          if (_openDicts.contains(dict.index)) {
            for (final key in dict.keys) {
              await _client.readEntry(block, dict.index, key);
            }
            if (mounted) setState(() {});
          }
        },
      ),
      if (dictOpen)
        for (final key in dict.keys)
          _entryTile(block, dict, block.entries[dict.index]?[key]),
    ]);
  }

  Widget _entryTile(KeyedBlock block, KeyedDict dict, KeyedEntry? entry) {
    if (entry == null) {
      return const ListTile(
          dense: true,
          contentPadding: EdgeInsets.only(left: 64, right: 12),
          leading: SizedBox(
              width: 14,
              height: 14,
              child: CircularProgressIndicator(strokeWidth: 2)),
          title: Text('...'));
    }
    final flags = FieldFlags.describe(entry.meta.flags);
    final valueText = formatValue(entry.meta.dataType, entry.value);
    return ListTile(
      dense: true,
      contentPadding: const EdgeInsets.only(left: 64, right: 12),
      title: Row(children: [
        Text('[$keyLabel(entry.key)] ',
            style: const TextStyle(color: kOrange, fontSize: 12)),
        Expanded(child: Text(valueText)),
        for (final flag in flags)
          Padding(
            padding: const EdgeInsets.only(left: 4),
            child: Text(flag,
                style: TextStyle(
                    fontSize: 10,
                    color: flag == 'RO' ? kOrange : Colors.white54)),
          ),
      ]),
      subtitle: Text(dataTypeLabel(entry.meta.dataType)),
      onTap: !entry.readOnly
          ? () => _editValue(block, dict, entry)
          : null,
    );
  }

  static String keyLabel(int key) =>
      key.toRadixString(16).padLeft(2, '0').toUpperCase();

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
