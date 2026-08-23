import 'dart:async';

import 'package:flutter/material.dart';

import '../core/connection.dart';
import '../core/dynmem.dart';
import '../core/types.dart';
import 'sysmem_page.dart' show formatValue, showValueEditor, dataTypeLabel;
import 'theme.dart';

/// Dynamic Memory service view (Docs/App/Service views/Dynamic Memory.md):
/// nested list of user blocks and entries with editing, save/recall, create/delete.
class DynamicMemoryPage extends StatefulWidget {
  final int deviceId;

  const DynamicMemoryPage({super.key, required this.deviceId});

  @override
  State<DynamicMemoryPage> createState() => _DynamicMemoryPageState();
}

class _DynamicMemoryPageState extends State<DynamicMemoryPage> {
  late final DynamicMemoryClient _client =
      DynamicMemoryClient(deviceId: widget.deviceId);

  bool _backupView = false;
  List<DynBlock>? _blocks;
  String? _error;
  final Set<int> _expanded = {};

  Timer? _autoTimer;
  Duration? _autoInterval;

  @override
  void initState() {
    super.initState();
    _refresh();
  }

  @override
  void dispose() {
    _autoTimer?.cancel();
    super.dispose();
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
      if (blocks != null) {
        final kept = <DynBlock>[];
        for (final fresh in blocks) {
          final old = _blocks?.where((b) => b.index == fresh.index).firstOrNull;
          if (old != null) fresh.fields.addAll(old.fields);
          kept.add(fresh);
        }
        _blocks = kept;
      }
    });
    if (_autoTimer == null) await _loadVisibleFields();
  }

  Future<void> _loadVisibleFields() async {
    final blocks = _blocks;
    if (blocks == null || _backupView) return;
    for (final block in blocks) {
      if (!_expanded.contains(block.index)) continue;
      for (var f = 0; f < block.fieldCount; f++) {
        await _client.readField(block, f);
      }
    }
    if (mounted) setState(() {});
  }

  void _setAutoRefresh(Duration? interval) {
    _autoTimer?.cancel();
    _autoTimer = null;
    _autoInterval = interval;
    if (interval != null) {
      _autoTimer = Timer.periodic(interval, (_) => _refresh());
    }
    setState(() {});
  }

  Future<void> _showAutorefreshDialog() async {
    final selected = await showDialog<Duration>(
      context: context,
      builder: (context) => SimpleDialog(
        title: const Text('Autorefresh'),
        children: [
          for (final (interval, label) in const [
            (null, 'Off'),
            (Duration(seconds: 1), '1 s'),
            (Duration(seconds: 2), '2 s'),
            (Duration(seconds: 5), '5 s'),
            (Duration(seconds: 10), '10 s'),
          ])
            SimpleDialogOption(
              onPressed: () =>
                  Navigator.pop(context, interval ?? Duration.zero),
              child: Row(children: [
                Text(label),
                const Spacer(),
                if (_autoInterval == interval ||
                    (interval == null && _autoInterval == null))
                  const Padding(
                      padding: EdgeInsets.only(left: 8),
                      child:
                          Icon(Icons.check, size: 16, color: kOrange)),
              ]),
            ),
        ],
      ),
    );
    if (!mounted) return;
    if (selected == null) {
      await _refresh();
    } else {
      _setAutoRefresh(selected == Duration.zero ? null : selected);
    }
  }

  Future<void> _createBlock() async {
    final name = await showDialog<String>(
      context: context,
      builder: (context) {
        final controller = TextEditingController();
        return AlertDialog(
          title: const Text('New block name'),
          content:
              TextField(controller: controller, autofocus: true, maxLength: 16),
          actions: [
            TextButton(
                onPressed: () => Navigator.pop(context),
                child: const Text('Cancel')),
            FilledButton(
                onPressed: () => Navigator.pop(context, controller.text.trim()),
                child: const Text('OK')),
          ],
        );
      },
    );
    if (name == null || name.isEmpty) return;
    final index = await _client.createBlock(BlockType.unknown, name);
    _snack(index != null ? 'Block created' : 'Create failed');
    await _refresh();
  }

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
          IconButton(
            onPressed: _showAutorefreshDialog,
            tooltip: 'Refresh',
            icon: Stack(
              clipBehavior: Clip.none,
              children: [
                const Icon(Icons.refresh),
                if (_autoInterval != null)
                  const Positioned(
                    right: -3,
                    bottom: -3,
                    child: Icon(Icons.circle,
                        size: 9, color: Colors.greenAccent),
                  ),
              ],
            ),
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
      return const Center(child: Text('No dynamic blocks'));
    }
    return ListView.builder(
      itemCount: _blocks!.length,
      itemBuilder: (context, index) {
        final block = _blocks![index];
        final isExpanded = _expanded.contains(block.index);
        return Column(children: [
          ListTile(
            leading: Icon(isExpanded ? Icons.folder_open : Icons.folder),
            title: Text(block.name),
            subtitle: Text(
                '${block.blockType.label}   ${FieldFlags.describe(block.meta.flags).join(" ")}'),
            trailing: Row(mainAxisSize: MainAxisSize.min, children: [
              IconButton(
                icon: const Icon(Icons.delete_outline, size: 20),
                tooltip: 'Delete block',
                onPressed: () async {
                  final ok = await _client.delete(block: block.index);
                  _snack(ok ? 'Block deleted (save to free)' : 'Delete failed');
                  await _refresh();
                },
              ),
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
                if (isExpanded) {
                  _expanded.remove(block.index);
                } else {
                  _expanded.add(block.index);
                }
              });
              if (_expanded.contains(block.index)) {
                for (var f = 0; f < block.fieldCount; f++) {
                  await _client.readField(block, f);
                }
                if (mounted) setState(() {});
              }
            },
          ),
          if (isExpanded)
            for (var f = 0; f < block.fieldCount; f++)
              _fieldTile(block, block.fields[f]),
          const Divider(height: 1),
        ]);
      },
    );
  }

  Widget _fieldTile(DynBlock block, DynField? field) {
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
    final valueText = formatValue(field.meta.dataType, field.value);
    return ListTile(
      dense: true,
      contentPadding: const EdgeInsets.only(left: 40, right: 12),
      title: Row(children: [
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
      subtitle: Text(dataTypeLabel(field.meta.dataType)),
      trailing: _backupView
          ? IconButton(
              icon: const Icon(Icons.restore_outlined, size: 18),
              tooltip: 'Recall entry',
              onPressed: () async {
                final ok = await _client.recall(block: block.index);
                if (!mounted) return;
                ScaffoldMessenger.of(context).showSnackBar(SnackBar(
                    content: Text(ok ? 'Recalled' : 'Recall failed')));
                await _client.readField(block, field.index);
                if (mounted) setState(() {});
              },
            )
          : IconButton(
              icon: const Icon(Icons.delete_outline, size: 18),
              tooltip: 'Delete entry',
              onPressed: () async {
                final ok =
                    await _client.delete(block: block.index, field: field.index);
                _snack(ok ? 'Entry deleted' : 'Delete failed');
                await _client.readField(block, field.index);
                if (mounted) setState(() {});
              },
            ),
      onTap: (!_backupView && !field.readOnly)
          ? () => _editValue(block, field)
          : null,
    );
  }

  Future<void> _editValue(DynBlock block, DynField field) async {
    if (!mounted) return;
    final newValue =
        await showValueEditor(context, field.meta.dataType, field.value);
    if (newValue == null) return;
    final confirmed = await _client.writeField(block, field, newValue);
    _snack(confirmed != null ? 'Value written' : 'Write failed');
    if (confirmed != null) {
      setState(() => field.value = confirmed);
    }
  }
}
