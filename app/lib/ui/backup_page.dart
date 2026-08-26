import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';

import '../core/backup.dart';
import '../core/connection.dart';
import '../core/device_db.dart';
import '../core/notifications.dart';
import '../core/types.dart';
import 'device_icons.dart';

/// Backup page (Docs/App/Backup.md): unified whole-network backup and restore
/// as a zipfile of per-device JSON files, with per-device selection.
class BackupPage extends StatefulWidget {
  const BackupPage({super.key});

  @override
  State<BackupPage> createState() => _BackupPageState();
}

class _BackupPageState extends State<BackupPage> {
  final _db = DeviceDatabase.instance;
  final _selected = <int>{};
  bool _busy = false;

  void _snack(String message) {
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(message)));
  }

  Future<void> _createBackup() async {
    if (_selected.isEmpty) return;
    setState(() => _busy = true);
    try {
      final devices = <BackupDevice>[];
      for (final id in _selected) {
        final captured = await captureDevice(id);
        if (captured != null) devices.add(captured);
      }
      if (devices.isEmpty) {
        _snack('No device responded');
        return;
      }
      final zip = buildBackupZip(devices);
      final target = await FilePicker.saveFile(
        fileName:
            'tamu_backup_${DateTime.now().toIso8601String().substring(0, 10)}.zip',
        type: FileType.custom,
        allowedExtensions: ['zip'],
      );
      if (target == null) return;
      // saveFile with bytes only works on web/desktop via the bytes parameter;
      // write manually for reliability.
      await writePlatformFile(target, zip);
      _snack('Backup saved (${devices.length} device(s))');
      notifyAppEvent('Backup finished', 'Backup saved (${devices.length} device(s))');
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
    try {
      // withData:true puts the content in `bytes`; fall back to reading the
      // path only when the picker did not return in-memory data.
      final bytes = file.bytes ?? readPlatformFile(file.path ?? '');
      final backups = parseBackupZip(bytes);
      var totalFields = 0;
      var restoredDevices = 0;
      for (final backup in backups) {
        final written = await restoreDevice(backup);
        if (written > 0) restoredDevices++;
        totalFields += written;
      }
      _snack('Restored $totalFields field(s) on $restoredDevices device(s)');
    } catch (error) {
      _snack('Restore failed: $error');
    } finally {
      if (mounted) setState(() => _busy = false);
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
          appBar: AppBar(title: const Text('Backup')),
          body: !connected
              ? const Center(child: Text('Not connected'))
              : Column(children: [
                  Padding(
                    padding: const EdgeInsets.all(12),
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
