import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';

import '../core/backup.dart';
import '../core/backup_format.dart';
import '../core/connection.dart';
import '../core/device_db.dart';
import '../core/host_files.dart';
import '../core/notifications.dart';
import '../core/types.dart';
import 'widgets.dart';

/// Backup page (Docs/App/Backup.md): unified whole-network backup and restore
/// as a zipfile of per-device JSON files, with per-part selection of synced
/// items and target remapping when compatible.
class BackupPage extends StatefulWidget {
  const BackupPage({super.key});

  @override
  State<BackupPage> createState() => _BackupPageState();
}

class _BackupPageState extends State<BackupPage> {
  final _db = DeviceDatabase.instance;
  final _selected = <int>{};
  bool _busy = false;
  bool _includeFiles = true;

  void _snack(String message) => showSnack(context, message);

  Future<void> _createBackup() async {
    if (_selected.isEmpty) return;
    setState(() => _busy = true);
    try {
      final devices = <BackupDevice>[];
      for (final id in _selected) {
        final captured = await captureDevice(id, includeFiles: _includeFiles);
        if (captured != null) devices.add(captured);
      }
      if (devices.isEmpty) {
        _snack('No device responded');
        return;
      }
      final zip = buildBackupZip(devices);
      final target = await saveBytesWithPicker(
        fileName:
            'tamu_backup_${DateTime.now().toIso8601String().substring(0, 10)}.zip',
        bytes: zip,
        allowedExtensions: ['zip'],
      );
      if (target == null) return; // cancelled
      final items = devices.fold<int>(
          0, (sum, d) => sum + d.blocks.fold<int>(0, (s, b) => s + b.entries.length));
      _snack('Backup saved (${devices.length} device(s), $items register entries)');
      notifyAppEvent('Backup finished',
          'Backup saved (${devices.length} device(s), $items register entries)');
    } catch (error) {
      _snack('Backup failed: $error');
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _restoreBackup() async {
    final result = await FilePicker.pickFiles(
      type: FileType.custom,
      allowedExtensions: ['zip'],
      withData: true,
    );
    final file = result?.files.singleOrNull;
    if (file == null) return;
    setState(() => _busy = true);
    List<BackupDevice> devices;
    try {
      final bytes = file.bytes ?? readPlatformFile(file.path ?? '');
      devices = parseBackupZip(bytes);
    } catch (error) {
      _snack('Restore failed: $error');
      if (mounted) setState(() => _busy = false);
      return;
    }

    RestorePlan plan;
    try {
      plan = await buildRestorePlan(devices);
    } catch (error) {
      _snack('Could not build the restore plan: $error');
      if (mounted) setState(() => _busy = false);
      return;
    }
    if (mounted) setState(() => _busy = false);
    if (!mounted) return;

    final applied = await Navigator.of(context).push<bool>(
      MaterialPageRoute(builder: (_) => RestorePlanPage(plan: plan)),
    );
    if (applied == true && mounted) {
      _snack('Restore applied');
    }
  }

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: Listenable.merge([_db, ConnectionManager.instance]),
      builder: (context, _) {
        final connected = ConnectionManager.instance.isConnected;
        final devices = _db.all;
        return Scaffold(
          appBar: AppBar(
            title: const Text('Backup'),
            leading: const ShellDrawerButton(),
          ),
          body: !connected
              ? const Center(child: Text('Not connected'))
              : Column(children: [
                  Padding(
                    padding: const EdgeInsets.fromLTRB(12, 12, 12, 0),
                    child: Row(children: [
                      Text(
                          '${_selected.length} of ${devices.length} selected',
                          style: Theme.of(context).textTheme.bodySmall),
                      const Spacer(),
                      TextButton(
                        onPressed: () => setState(() =>
                            _selected.length == devices.length
                                ? _selected.clear()
                                : _selected
                                    .addAll(devices.map((d) => d.id))),
                        child: Text(_selected.length == devices.length &&
                                devices.isNotEmpty
                            ? 'Deselect all'
                            : 'Select all'),
                      ),
                    ]),
                  ),
                  SwitchListTile(
                    dense: true,
                    title: const Text('Include device files'),
                    value: _includeFiles,
                    onChanged: (v) => setState(() => _includeFiles = v),
                  ),
                  const Divider(height: 1),
                  Expanded(
                    child: ListView.builder(
                      itemCount: devices.length,
                      itemBuilder: (context, index) {
                        final device = devices[index];
                        return CheckboxListTile(
                          secondary: Icon(deviceTypeIcon(device.type)),
                          title: Text(device.name),
                          subtitle:
                              Text('${idToString(device.id)} - ${device.type.label}'),
                          value: _selected.contains(device.id),
                          onChanged: device.stale
                              ? null
                              : (value) => setState(() {
                                    value == true
                                        ? _selected.add(device.id)
                                        : _selected.remove(device.id);
                                  }),
                        );
                      },
                    ),
                  ),
                ]),
          bottomNavigationBar: SafeArea(
            child: Padding(
              padding: const EdgeInsets.all(12),
              child: Row(children: [
                Expanded(
                  child: FilledButton.icon(
                    onPressed: _busy || _selected.isEmpty ? null : _createBackup,
                    icon: const Icon(Icons.archive),
                    label: const Text('Create backup'),
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: FilledButton.icon(
                    onPressed: _busy || !connected ? null : _restoreBackup,
                    icon: const Icon(Icons.restore),
                    label: const Text('Restore'),
                  ),
                ),
              ]),
            ),
          ),
        );
      },
    );
  }
}

/// Per-part restore: pick which items to write and (optionally) remap a source
/// device/block to a different live one when compatible.
class RestorePlanPage extends StatefulWidget {
  final RestorePlan plan;

  const RestorePlanPage({super.key, required this.plan});

  @override
  State<RestorePlanPage> createState() => _RestorePlanPageState();
}

class _RestorePlanPageState extends State<RestorePlanPage> {
  RestorePlan get plan => widget.plan;
  bool _busy = false;

  void _reselectDevice(int sourceId, LiveDevice device) {
    setState(() {
      plan.deviceTargets[sourceId] = device;
      plan.blockTargets.removeWhere((key, _) => key.startsWith('$sourceId:'));
      plan.resolve();
    });
  }

  void _reselectBlock(int sourceDeviceId, BackupBlock block, LiveBlock target) {
    setState(() {
      plan.blockTargets[RestorePlan.blockKey(sourceDeviceId, block)] = target;
      plan.resolve();
    });
  }

  Future<void> _apply() async {
    setState(() => _busy = true);
    try {
      final result = await applyRestorePlan(plan);
      if (mounted) {
        showSnack(
            context,
            'Restored ${result.written} item(s)'
            '${result.failed > 0 ? ', ${result.failed} failed' : ''}');
        Navigator.of(context).pop(true);
      }
    } catch (error) {
      if (mounted) showSnack(context, 'Restore failed: $error');
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final devices = plan.devices;
    return Scaffold(
      appBar: AppBar(title: const Text('Restore plan')),
      body: ListView(
        children: [
          Padding(
            padding: const EdgeInsets.all(12),
            child: Text(
              'Select the items to restore. Items are matched by name; remap a '
              'device or block to sync into a different target when compatible.',
              style: Theme.of(context).textTheme.bodySmall,
            ),
          ),
          for (final device in devices) _deviceSection(device),
        ],
      ),
      bottomNavigationBar: SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: Row(children: [
            Expanded(
              child: Text(
                '${plan.selectedCount} selected'
                '${plan.issueCount > 0 ? '\n${plan.issueCount} unavailable' : ''}',
                style: Theme.of(context).textTheme.bodySmall,
              ),
            ),
            FilledButton.icon(
              onPressed: _busy || plan.selectedCount == 0 ? null : _apply,
              icon: const Icon(Icons.restore),
              label: const Text('Restore'),
            ),
          ]),
        ),
      ),
    );
  }

  Widget _deviceSection(BackupDevice device) {
    final target = plan.deviceTargets[device.id];
    final items = plan.items.where((i) => i.device.id == device.id).toList();
    final entries = items.where((i) => i.kind == RestoreKind.entry).toList();
    final blocks = <BackupBlock>{for (final i in entries) i.block!}.toList();
    final scripts = items.where((i) => i.kind == RestoreKind.script).toList();
    final subs = items.where((i) => i.kind == RestoreKind.subscription).toList();
    final sndb = items.where((i) => i.kind == RestoreKind.sndb).toList();
    final files = items.where((i) => i.kind == RestoreKind.file).toList();
    return Card(
      margin: const EdgeInsets.fromLTRB(12, 4, 12, 8),
      child: ExpansionTile(
        initiallyExpanded: true,
        title: Text('${device.name} (${device.type})'),
        subtitle: Padding(
          padding: const EdgeInsets.only(top: 4),
          child: _deviceTargetPicker(device, target),
        ),
        children: [
          for (final block in blocks) _blockSection(device, block),
          if (scripts.isNotEmpty) _group('Scripts', scripts),
          if (subs.isNotEmpty) _group('Subscriptions', subs),
          if (sndb.isNotEmpty) _group('SNDB', sndb),
          if (files.isNotEmpty) _group('Files', files),
        ],
      ),
    );
  }

  Widget _deviceTargetPicker(BackupDevice device, LiveDevice? target) {
    return DropdownButton<int>(
      isExpanded: true,
      value: target?.id,
      hint: const Text('No target'),
      items: [
        for (final live in plan.liveDevices)
          DropdownMenuItem(
            value: live.id,
            child:
                Text('${live.name} (${idToString(live.id)})', overflow: TextOverflow.ellipsis),
          ),
      ],
      onChanged: (id) {
        final live = plan.liveDevices.where((d) => d.id == id).firstOrNull;
        if (live != null) _reselectDevice(device.id, live);
      },
    );
  }

  Widget _group(String title, List<RestoreItem> items) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(12, 0, 12, 0),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Padding(
            padding: const EdgeInsets.only(top: 8, bottom: 2),
            child: Text(title, style: Theme.of(context).textTheme.titleSmall),
          ),
          for (final item in items) _itemTile(item),
        ],
      ),
    );
  }

  Widget _blockSection(BackupDevice device, BackupBlock block) {
    final items = plan.items
        .where((i) => i.device.id == device.id && i.kind == RestoreKind.entry && identical(i.block, block))
        .toList();
    final head = items.isNotEmpty ? items.first : null;
    final targetDevice = plan.deviceTargets[device.id];
    final options = targetDevice == null
        ? <LiveBlock>[]
        : plan.compatibleBlocks(targetDevice, block);
    return Padding(
      padding: const EdgeInsets.fromLTRB(12, 0, 12, 0),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(children: [
            Expanded(
              child: Text('${block.type} "${block.name}" #${block.instance}',
                  style: Theme.of(context).textTheme.titleSmall),
            ),
            if (options.length > 1)
              DropdownButton<LiveBlock>(
                value: head?.targetBlock,
                hint: const Text('Target'),
                items: [
                  for (final live in options)
                    DropdownMenuItem(
                      value: live,
                      child: Text('${live.name} #${live.instance}',
                          overflow: TextOverflow.ellipsis),
                    ),
                ],
                onChanged: (live) {
                  if (live != null) _reselectBlock(device.id, block, live);
                },
              ),
          ]),
          for (final item in items) _itemTile(item),
        ],
      ),
    );
  }

  Widget _itemTile(RestoreItem item) {
    return CheckboxListTile(
      dense: true,
      value: item.selected,
      onChanged: item.ready
          ? (v) => setState(() => item.selected = v == true)
          : null,
      title: Text(_title(item), overflow: TextOverflow.ellipsis),
      subtitle: Text(_subtitle(item),
          overflow: TextOverflow.ellipsis,
          style: item.issue != null
              ? const TextStyle(color: Colors.orangeAccent)
              : null),
    );
  }

  String _title(RestoreItem item) {
    switch (item.kind) {
      case RestoreKind.entry:
        return '${item.entry!.field} / ${item.entry!.key}';
      case RestoreKind.script:
        return '${item.script!.functionName.isEmpty ? 'Script' : item.script!.functionName} '
            '(SCR_${item.script!.slot.toRadixString(16).padLeft(2, '0').toUpperCase()})';
      case RestoreKind.subscription:
        return '${item.subscription!.trigger} from ${item.subscription!.provider}';
      case RestoreKind.sndb:
        return 'SNDB ${item.sndbEntry!.address}';
      case RestoreKind.file:
        return item.file!.name;
    }
  }

  String _subtitle(RestoreItem item) {
    if (item.issue != null) return item.issue!;
    switch (item.kind) {
      case RestoreKind.entry:
        final e = item.entry!;
        final unit = (e.unit ?? '').isEmpty ? '' : ' ${e.unit}';
        return '${e.type} = ${_preview(e.value)}$unit';
      case RestoreKind.script:
        return '${item.script!.inputs.length} in, ${item.script!.outputs.length} out, '
            '${item.script!.lines.length} lines';
      case RestoreKind.subscription:
        final s = item.subscription!;
        return '${s.source.block} / ${s.source.field} -> ${s.target.block} / ${s.target.field}';
      case RestoreKind.sndb:
        return item.sndbEntry!.serial;
      case RestoreKind.file:
        return '${item.file!.kind} (${item.file!.bytes.length} B)';
    }
  }

  String _preview(Object? value) {
    final text = value?.toString() ?? 'null';
    return text.length > 50 ? '${text.substring(0, 47)}...' : text;
  }
}
