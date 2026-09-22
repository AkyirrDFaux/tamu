import 'dart:async';
import 'dart:io';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';

import '../core/backup.dart' show readPlatformFile;
import '../core/connection.dart';
import '../core/device_db.dart';
import '../core/register_client.dart';
import '../core/storage_client.dart';
import '../core/types.dart';
import 'theme.dart';
import 'file_viewers.dart';
import 'widgets.dart';

/// Storage service view (Docs/App/Service views/Storage.md): file table with
/// download, delete, rename, create and formatted viewers for known file
/// types. The file table itself is always read-only.
class StoragePage extends StatefulWidget {
  final int deviceId;

  const StoragePage({super.key, required this.deviceId});

  @override
  State<StoragePage> createState() => _StoragePageState();
}

class _StoragePageState extends State<StoragePage>
    with AutoRefreshMixin<StoragePage> {
  late final StorageClient _client = StorageClient(deviceId: widget.deviceId);

  List<FileRecord>? _files;
  String? _error;
  bool _refreshing = false;
  // Devices without the StorageFiles capability (e.g. USE_FIXED_STORAGE nodes like the
  // DAS) have a read-only const file table; create/upload/rename/delete are hidden.
  late final bool _reduced = !_hasStorageFiles();

  @override
  void initState() {
    super.initState();
    _refresh();
  }

  @override
  Future<void> onAutoRefresh() => _refresh();



  void _snack(String message) {
    if (!mounted) return;
    showSnack(context, message);
  }

  bool _hasStorageFiles() {
    final dev = DeviceDatabase.instance.byId(widget.deviceId);
    return dev != null && (dev.capabilities & Capability.storageFiles) != 0;
  }

  Future<void> _refresh() async {
    if (!ConnectionManager.instance.isConnected || _refreshing) return;
    setState(() => _refreshing = true);
    final files = await _client.readFileTable();
    if (!mounted) return;
    setState(() {
      _refreshing = false;
      // readFileTable() reads the ".TABLE  " file directly using CID 5. A reduced
      // (USE_FIXED_STORAGE) device serves the fixed filetable through the same read.
      _error = null;
      _files = files ?? [];
    });
  }

  String _formatSize(int bytes) {
    if (bytes < 1024) return '$bytes B';
    if (bytes < 1024 * 1024) return '${(bytes / 1024).toStringAsFixed(1)} kB';
    return '${(bytes / (1024 * 1024)).toStringAsFixed(2)} MB';
  }

  Future<bool> _confirm(String title, String body) async =>
      confirmDialog(context, title: title, body: body);

  Future<String?> _promptText(String title, String label, String initial,
      {int maxChars = 8}) async {
    final controller = TextEditingController(text: initial);
    return await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text(title),
        content: TextField(
            controller: controller,
            autofocus: true,
            maxLength: maxChars,
            decoration: InputDecoration(labelText: label)),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(context),
              child: const Text('Cancel')),
          FilledButton(
              onPressed: () =>
                  Navigator.pop(context, controller.text.trim()),
              child: const Text('OK')),
        ],
      ),
    );
  }

  Future<void> _createFile() async {
    final name = await _promptText(
        'New file', 'Name (max 8 chars)', '', maxChars: 8);
    if (name == null || name.isEmpty) return;
    final sizeText = await _promptText('Size of "$name"', 'Bytes', '4096',
        maxChars: 7);
    final size = sizeText == null ? null : int.tryParse(sizeText);
    if (size == null || size <= 0) return;
    final ok = await _client.createFile(name, size);
    _snack(ok ? 'File created' : 'Create failed');
    await _refresh();
  }

  /// Uploads a host file to the device (CID 7 write stream, Docs/App/Service
  /// views/Storage.md "Allow uploading ... files from the app host OS").
  Future<void> _uploadFile() async {
    final result = await FilePicker.pickFiles(withData: true);
    final file = result?.files.singleOrNull;
    if (file == null) return;
    final bytes = file.bytes ?? readPlatformFile(file.path ?? '');
    if (bytes.isEmpty) {
      _snack('Empty file');
      return;
    }
    // Device file names are 8 bytes: base name without extension, trimmed.
    var name = file.name;
    final dot = name.lastIndexOf('.');
    if (dot > 0) name = name.substring(0, dot);
    if (name.length > StorageClient.nameLength) {
      name = name.substring(0, StorageClient.nameLength);
    }
    if (name.isEmpty) name = 'UPFILE';
    _snack('Uploading "$name"...');
    final ok = await _client.writeFile(name, bytes);
    _snack(ok ? 'Uploaded "$name"' : 'Upload failed');
    await _refresh();
  }

  Future<void> _deleteFile(FileRecord file) async {
    if (!await _confirm(
        'Delete "${file.name}"?',
        'The file record is invalidated; the space is reused on the next '
        'matching allocation.')) {
      return;
    }
    final ok = await _client.deleteFile(file.name);
    _snack(ok ? 'File deleted' : 'Delete failed');
    await _refresh();
  }

  Future<void> _renameFile(FileRecord file) async {
    final newName = await _promptText(
        'Rename "${file.name}"', 'New name (max 8 chars)', file.name,
        maxChars: 8);
    if (newName == null || newName.isEmpty || newName == file.name) return;
    final ok = await _client.renameFile(file.name, newName);
    _snack(ok ? 'File renamed' : 'Rename failed');
    await _refresh();
  }

  /// Downloads the whole file to the host (~/Downloads by default).
  Future<void> _downloadFile(FileRecord file) async {
    _snack('Downloading ${file.name}...');
    final data = await _client.readFile(file.name, size: file.size);
    if (data == null) {
      _snack('Download failed');
      return;
    }
    final dir = Directory('${Platform.environment['HOME']}/Downloads');
    if (!await dir.exists()) await dir.create(recursive: true);
    final target = '${dir.path}/${file.name}_download.bin';
    await File(target).writeAsBytes(data, flush: true);
    _snack('Saved to $target');
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text('Storage - ${idToString(widget.deviceId)}'),
        actions: [
          if (!_reduced) ...[
            IconButton(
                onPressed: _uploadFile,
                tooltip: 'Upload file',
                icon: const Icon(Icons.upload_outlined)),
            IconButton(
                onPressed: _createFile,
                tooltip: 'Create file',
                icon: const Icon(Icons.create_new_folder_outlined)),
          ],
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
    final files = _files;
    if (files == null) return Center(child: Text(_error ?? 'Loading...'));
    final realFiles = files.toList();
    if (files.isEmpty) {
      return const Center(child: Text('No file table'));
    }
    return ListView.builder(
      itemCount: realFiles.length,
      itemBuilder: (context, index) {
        final file = realFiles[index];
        final isTable = file.isFiletable;
        return ListTile(
          // The file table is read-only but still openable: tapping shows the
          // decoded directory (Docs/Services/Storage.md). Read-only-ness is
          // signalled by the lock icon, not by disabling the row.
leading: Icon(isTable
              ? Icons.table_rows_outlined
              : storageFileIcon(file.name)),
          title: Text(isTable
              ? '${normalizeFileName(file.name)} (file table)'
              : normalizeFileName(file.name),
              style: isTable ? const TextStyle(color: Colors.white38) : null),
          subtitle:
              Text('${isTable ? "Internal directory" : fileTypeLabel(file.name)}   '
                  '${_formatSize(file.size)}   '
                  'offset ${hex32(file.offset)}'),
          trailing: isTable
              ? const Icon(Icons.lock_outline,
                  size: 16, color: Colors.white24)
              : PopupMenuButton<String>(
            tooltip: 'Actions',
            onSelected: (action) {
              switch (action) {
                case 'view':
                  _showFile(file);
                case 'download':
                  _downloadFile(file);
                case 'rename':
                  if (!_reduced) _renameFile(file);
                case 'delete':
                  if (!_reduced) _deleteFile(file);
              }
            },
            itemBuilder: (_) => [
              PopupMenuItem(value: 'view', child: Text('View')),
              PopupMenuItem(value: 'download', child: Text('Download')),
              if (!_reduced) ...[
                PopupMenuItem(value: 'rename', child: Text('Rename')),
                PopupMenuItem(value: 'delete', child: Text('Delete')),
              ],
            ],
          ),
          onTap: () => isTable ? _showTable(file) : _showFile(file),
        );
      },
    );
  }

  /// The file table itself, decoded: one row per Filerecord.
  Future<void> _showTable(FileRecord table) async {
    _snack('Reading file table...');
    // Read the table file directly using CID 5 (read file) instead of CID 7.
    final data = await _client.readFile(table.name, size: table.size);
    if (!mounted) return;
    Navigator.of(context).push(MaterialPageRoute(
        builder: (_) => FileTableViewPage(
            deviceId: widget.deviceId, size: table.size, data: data,
            fixed: _reduced)));
  }

  Future<void> _showFile(FileRecord file) async {
    _snack('Reading ${file.name}...');
    final data =
        await _client.readFile(file.name, size: file.size);
    // STATLOG entries are addressed by the static registry index; fetch the
    // device's blocks so the decoder can name the blocks and fields.
    List<({int type, int inst, BlockMeta meta, String name})?>? blocks;
    if (normalizeFileName(file.name).toUpperCase() == 'STATLOG') {
      final all = await RegisterClient(deviceId: widget.deviceId).readBlocks();
      blocks = all?.where((b) => b != null && b.type != 0).toList();
    }
    if (!mounted) return;
    Navigator.of(context).push(MaterialPageRoute(
        builder: (_) => FileViewPage(deviceId: widget.deviceId,
            name: file.name, size: file.size, data: data, blocks: blocks)));
  }
}

/// Decoded file-table view: every 16-byte Filerecord as offset/size/name.
class FileTableViewPage extends StatelessWidget {
  final int deviceId;
  final int size;
  final List<int>? data;
  final bool fixed;

  const FileTableViewPage(
      {super.key,
      required this.deviceId,
      required this.size,
      required this.data,
      this.fixed = false});

  @override
  Widget build(BuildContext context) {
    final body = data == null
        ? const Center(child: Text('Read failed'))
        : _TableBody(data: data!, size: size, fixed: fixed);
    return Scaffold(
      appBar: AppBar(title: const Text('File table')),
      body: body,
    );
  }
}

/// The decoded table: live records first (what is actually stored), then a
/// collapsible "invalidated" section for old generations so the view agrees
/// with the file list instead of drowning in stale records.
class _TableBody extends StatefulWidget {
  final List<int> data;
  final int size;
  final bool fixed;

  const _TableBody({
    required this.data,
    required this.size,
    this.fixed = false,
  });

  @override
  State<_TableBody> createState() => _TableBodyState();
}

class _TableBodyState extends State<_TableBody> {
  bool _showInvalidated = false;

  @override
  Widget build(BuildContext context) {
    final data = widget.data;
    final live = <Widget>[];
    final invalidated = <Widget>[];
    for (var off = 0; off + 16 <= data.length; off += 16) {
      final recOffset = uint32FromBytes(data, off);
      final fileSize = uint32FromBytes(data, off + 4);
      final unwritten = recOffset == 0xFFFFFFFF && fileSize == 0xFFFFFFFF;
      // Offset 0 marks a superseded/invalidated record on the full file system (the
      // device zeroes the 4-byte offset, leaving the size); on the fixed (reduced)
      // storage offset-0 records are the real files.
      final isInvalidated = !widget.fixed && !unwritten && recOffset == 0;
      String name() => String.fromCharCodes(data.sublist(off + 8, off + 16)).trim();
      final row = ListTile(
        dense: true,
        leading: Icon(
            isInvalidated
                ? Icons.delete_outline
                : Icons.table_rows_outlined,
            size: 18,
            color: isInvalidated ? Colors.white24 : Colors.white38),
        title: Text(
            '#${off ~/ 16}  '
            '${isInvalidated ? "(invalidated)" : name()}'
            '   ${!isInvalidated ? "${(fileSize / 1024).toStringAsFixed(1)} kB" : ""}',
            style: TextStyle(
                fontFamily: 'monospace',
                fontSize: 12,
                color: isInvalidated ? Colors.white24 : Colors.white70)),
        subtitle: isInvalidated
            ? null
            : Text('offset ${hex32(recOffset)}   $fileSize B',
                style: const TextStyle(fontSize: 11)),
      );
      if (isInvalidated) {
        invalidated.add(row);
      } else if (!unwritten) {
        live.add(row);
      }
    }
    return ListView(padding: const EdgeInsets.all(12), children: [
      Padding(
        padding: const EdgeInsets.only(bottom: 8),
        child: Text('File table (${widget.size} B) - '
            '${live.length} live, ${invalidated.length} invalidated',
            style: const TextStyle(color: kOrange)),
      ),
      ...live,
      if (invalidated.isNotEmpty) ...[
        const Divider(height: 20),
        ListTile(
          dense: true,
          leading: Icon(_showInvalidated
              ? Icons.expand_less
              : Icons.expand_more, size: 18, color: Colors.white38),
          title: Text('${invalidated.length} invalidated (old) records',
              style: const TextStyle(fontSize: 12, color: Colors.white54)),
          onTap: () => setState(() => _showInvalidated = !_showInvalidated),
        ),
        if (_showInvalidated) ...invalidated,
      ],
    ]);
  }
}
