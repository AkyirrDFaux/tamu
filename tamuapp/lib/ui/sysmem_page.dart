import 'dart:async';

import 'package:flutter/material.dart';

import '../core/connection.dart';
import '../core/sysmem.dart';
import '../core/types.dart';
import 'theme.dart';

/// System Memory service view (Docs/App/Service views/System Memory.md):
/// nested list of blocks and their entries, current vs backup views, editing,
/// save/recall and refresh with selectable autorefresh.
class SystemMemoryPage extends StatefulWidget {
  final int deviceId;

  const SystemMemoryPage({super.key, required this.deviceId});

  @override
  State<SystemMemoryPage> createState() => _SystemMemoryPageState();
}

class _SystemMemoryPageState extends State<SystemMemoryPage> {
  late final SystemMemoryClient _client =
      SystemMemoryClient(deviceId: widget.deviceId);

  bool _backupView = false;
  List<SysBlock>? _blocks;
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
    if (_autoTimer == null) await _loadVisibleFields();
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
    if (_backupView) return; // backup values are read on demand per entry
    final count = block.meta.size; // block meta carries the field count
    for (var f = 0; f < count; f++) {
      await _client.readField(block, f);
    }
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
    // Returns a duration (Duration.zero = off), or null when cancelled.
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
                      child: Icon(Icons.check, size: 16, color: kOrange)),
              ]),
            ),
        ],
      ),
    );
    if (!mounted) return;
    if (selected == null) {
      await _refresh(); // cancelled -> plain refresh action
    } else {
      _setAutoRefresh(selected == Duration.zero ? null : selected);
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
                    child: Icon(Icons.circle, size: 9, color: Colors.greenAccent),
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
      return const Center(child: Text('No memory blocks'));
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
                await _loadFields(block);
                if (mounted) setState(() {});
              }
            },
          ),
          if (isExpanded)
            for (var f = 0; f < block.meta.size; f++)
              _fieldTile(block, block.fields[f]),
          const Divider(height: 1),
        ]);
      },
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
    final newValue = await showValueEditor(context, field.meta.dataType, field.value);
    if (newValue == null) return;
    final confirmed =
        await _client.writeField(block, field, newValue);
    _snack(confirmed != null ? 'Value written' : 'Write failed');
    if (confirmed != null) {
      setState(() => field.value = confirmed);
    }
  }
}

// =============================================================================
// Value display + editing helpers
// =============================================================================

String dataTypeLabel(DataType type) => switch (type) {
      DataType.unknown => 'Unknown',
      DataType.sn => 'Serial number',
      DataType.uint32 => 'Uint32',
      DataType.number => 'Number',
      DataType.devType => 'Device type',
      DataType.netAddr => 'Net address',
      DataType.bool_ => 'Bool',
      DataType.vector => 'Vector',
      DataType.matrix => 'Matrix',
      DataType.enum_ => 'Enum',
      DataType.colour => 'Colour',
      DataType.integer => 'Index',
      DataType.string => 'String',
      DataType.deleted => 'Deleted',
    };

String formatValue(DataType type, List<int> bytes) {
  switch (type) {
    case DataType.number:
      if (bytes.length < 4) return '-';
      return numberFromBytes(bytes).toStringAsFixed(3);
    case DataType.bool_:
      if (bytes.isEmpty) return '-';
      return bytes[0] != 0 ? 'true' : 'false';
    case DataType.uint32:
      if (bytes.length < 4) return '-';
      return uint32FromBytes(bytes).toString();
    case DataType.integer:
      if (bytes.length < 4) return '-';
      return int32FromBytes(bytes).toString();
    case DataType.string:
      return String.fromCharCodes(bytes);
    case DataType.devType:
      if (bytes.length < 2) return '-';
      return DeviceType.fromValue(bytes[0] | (bytes[1] << 8)).label;
    case DataType.netAddr:
      if (bytes.length < 2) return idToString(0);
      return idToString(bytes[0] | (bytes[1] << 8));
    case DataType.colour:
      if (bytes.length < 4) return '-';
      return '#${bytes.sublist(0, 4).map((b) => b.toRadixString(16).padLeft(2, '0')).join()}';
    case DataType.sn:
      if (bytes.length < 14) return '-';
      return serialNumberToHex(bytes.sublist(0, 14));
    case DataType.unknown:
    case DataType.deleted:
      return bytes.isEmpty ? '-' : '0x${bytes.map((b) => b.toRadixString(16).padLeft(2, '0')).join()}';
    default:
      return bytes.isEmpty
          ? '-'
          : bytes.map((b) => b.toRadixString(16).padLeft(2, '0')).join(' ');
  }
}

/// Returns the new encoded bytes, or null when cancelled.
Future<List<int>?> showValueEditor(
    BuildContext context, DataType type, List<int> current) async {
  switch (type) {
    case DataType.number:
      final text = TextEditingController(
          text: current.length >= 4 ? numberFromBytes(current).toString() : '');
      final result = await _textDialog(context, 'Number', text);
      if (result == null) return null;
      final value = double.tryParse(result);
      return value == null ? null : numberToBytes(value);
    case DataType.bool_:
      final result = await showDialog<bool>(
        context: context,
        builder: (context) => AlertDialog(
          title: const Text('Bool'),
          actions: [
            TextButton(
                onPressed: () => Navigator.pop(context, false),
                child: const Text('false')),
            FilledButton(
                onPressed: () => Navigator.pop(context, true),
                child: const Text('true')),
          ],
        ),
      );
      return result == null ? null : [result ? 1 : 0];
    case DataType.uint32:
    case DataType.integer:
      final text = TextEditingController(
          text: current.length >= 4
              ? (type == DataType.uint32
                      ? uint32FromBytes(current)
                      : int32FromBytes(current))
                  .toString()
              : '');
      final result = await _textDialog(context, dataTypeLabel(type), text);
      if (result == null) return null;
      final value = int.tryParse(result);
      if (value == null) return null;
      return uint32ToBytes(type == DataType.integer ? value & 0xFFFFFFFF : value);
    case DataType.string:
      final text = TextEditingController(text: String.fromCharCodes(current));
      final result = await _textDialog(context, 'String', text, multiline: true);
      return result?.codeUnits.toList();
    default:
      final text = TextEditingController(
          text: current
              .map((b) => b.toRadixString(16).padLeft(2, '0'))
              .join());
      final result = await _textDialog(
          context, '${dataTypeLabel(type)} (hex)', text);
      if (result == null) return null;
      final clean = result.replaceAll(RegExp(r'[^0-9a-fA-F]'), '');
      if (clean.length.isOdd) return null;
      return [
        for (var i = 0; i < clean.length; i += 2)
          int.parse(clean.substring(i, i + 2), radix: 16)
      ];
  }
}

Future<String?> _textDialog(BuildContext context, String title,
    TextEditingController controller,
    {bool multiline = false}) {
  return showDialog<String>(
    context: context,
    builder: (context) => AlertDialog(
      title: Text(title),
      content: TextField(
        controller: controller,
        autofocus: true,
        maxLines: multiline ? 3 : 1,
      ),
      actions: [
        TextButton(
            onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(
            onPressed: () => Navigator.pop(context, controller.text),
            child: const Text('OK')),
      ],
    ),
  );
}
