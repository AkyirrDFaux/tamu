import 'dart:async';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';

import '../core/connection.dart';
import '../core/host_files.dart';
import '../core/platform_caps.dart';
import '../core/register_client.dart';
import '../core/storage_client.dart';
import '../core/types.dart';
import 'formatters.dart';
import 'theme.dart';
import 'file_viewers.dart';
import 'viewer_kit.dart';
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

  Future<void> _refresh() async {
    if (!ConnectionManager.instance.isConnected || _refreshing) return;
    setState(() => _refreshing = true);
    final files = await _client.readFileTable();
    if (!mounted) return;
    setState(() {
      _refreshing = false;
      // readFileTable() reads the ".TABLE  " file directly using CID 5.
      _error = null;
      _files = files ?? [];
    });
  }

  Future<String?> _promptText(
    String title,
    String label,
    String initial, {
    int maxChars = 8,
  }) async {
    final controller = TextEditingController(text: initial);
    return await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text(title),
        content: TextField(
          controller: controller,
          autofocus: true,
          maxLength: maxChars,
          decoration: InputDecoration(labelText: label),
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context),
            child: const Text('Cancel'),
          ),
          FilledButton(
            onPressed: () => Navigator.pop(context, controller.text.trim()),
            child: const Text('OK'),
          ),
        ],
      ),
    );
  }

  Future<void> _createFile() async {
    final name = await _promptText(
      'New file',
      'Name (max 8 chars)',
      '',
      maxChars: 8,
    );
    if (name == null || name.isEmpty) return;
    final sizeText = await _promptText(
      'Size of "$name"',
      'Bytes',
      '4096',
      maxChars: 7,
    );
    final size = sizeText == null ? null : int.tryParse(sizeText);
    if (size == null || size <= 0) return;
    final ok = await _client.createFile(name, size);
    _snack(ok ? 'File created' : 'Create failed');
    await _refresh();
  }

  /// Uploads a host file to the device (CID 7 write stream, Docs/App/Service
  /// views/Storage.md "Allow uploading ... files from the app host OS").
  Future<void> _uploadFile() async {
    final result = await FilePicker.pickFiles();
    final file = result.singleOrNull;
    if (file == null) return;
    final bytes = await file.readAsBytes();
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
    if (!await confirmDialog(
      context,
      title: 'Delete "${file.name}"?',
      body:
          'The file record is invalidated; the space is reused on the next '
          'matching allocation.',
    )) {
      return;
    }
    final ok = await _client.deleteFile(file.name);
    _snack(ok ? 'File deleted' : 'Delete failed');
    await _refresh();
  }

  Future<void> _renameFile(FileRecord file) async {
    final newName = await _promptText(
      'Rename "${file.name}"',
      'New name (max 8 chars)',
      file.name,
      maxChars: 8,
    );
    if (newName == null || newName.isEmpty || newName == file.name) return;
    final ok = await _client.renameFile(file.name, newName);
    _snack(ok ? 'File renamed' : 'Rename failed');
    await _refresh();
  }

  /// Downloads the whole file to a user-chosen location.
  Future<void> _downloadFile(FileRecord file) async {
    _snack('Downloading ${file.name}...');
    final data = await _client.readFile(file.name, size: file.size);
    if (data == null) {
      _snack('Download failed');
      return;
    }
    final target = await saveBytesWithPicker(
      fileName: '${file.name}_download.bin',
      bytes: data,
    );
    if (target == null) {
      _snack('Download cancelled');
      return;
    }
    // Mobile pickers return a content URI, not a filesystem path.
    _snack(isMobile ? 'Saved' : 'Saved to $target');
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text('Storage - ${idToString(widget.deviceId)}'),
        actions: [
          IconButton(
            onPressed: _uploadFile,
            tooltip: 'Upload file',
            icon: const Icon(Icons.upload_outlined),
          ),
          IconButton(
            onPressed: _createFile,
            tooltip: 'Create file',
            icon: const Icon(Icons.create_new_folder_outlined),
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
      body: _buildBody(context),
    );
  }

  Widget _buildBody(BuildContext context) {
    final files = _files;
    if (files == null) return Center(child: Text(_error ?? 'Loading...'));
    // The list is the toolkit's table shell: a type icon, the name, the type/size/
    // offset detail and the per-row actions. The file table itself is read-only but
    // still openable: tapping shows the decoded directory
    // (Docs/Services/Storage.md). Read-only-ness is signalled by the lock icon, not
    // by disabling the row.
    return TamuTable<FileRecord>(
      columns: [
        TamuColumn<FileRecord>(
          label: 'File',
          flex: 4,
          cell: (context, file) => Text(
            _fileName(file),
            overflow: TextOverflow.ellipsis,
            style: Theme.of(context).textTheme.bodySmall?.copyWith(
              color: file.isFiletable ? kTextMuted : kTextPrimary,
            ),
          ),
        ),
        TamuColumn<FileRecord>(
          label: 'Type  ·  size  ·  offset',
          flex: 6,
          text: _fileDetail,
          muted: true,
        ),
      ],
      rows: files,
      leading: (context, file) => Icon(
        file.isFiletable
            ? Icons.table_rows_outlined
            : storageFileIcon(file.name),
        size: IconSize.md,
        color: kTextSecondary,
      ),
      trailing: (context, file) => file.isFiletable
          ? const Icon(
              Icons.lock_outline,
              size: IconSize.sm,
              color: kTextDisabled,
            )
          : PopupMenuButton<String>(
              tooltip: 'Actions',
              onSelected: (action) {
                switch (action) {
                  case 'view':
                    _showFile(file);
                  case 'download':
                    _downloadFile(file);
                  case 'rename':
                    _renameFile(file);
                  case 'delete':
                    _deleteFile(file);
                }
              },
              itemBuilder: (_) => [
                PopupMenuItem(value: 'view', child: Text('View')),
                PopupMenuItem(value: 'download', child: Text('Download')),
                PopupMenuItem(value: 'rename', child: Text('Rename')),
                PopupMenuItem(value: 'delete', child: Text('Delete')),
              ],
            ),
      onTap: (file) => file.isFiletable ? _showTable(file) : _showFile(file),
      emptyMessage: 'No file table',
    );
  }

  /// The name a row prints: the stored name, marked when it is the directory itself.
  String _fileName(FileRecord file) => file.isFiletable
      ? '${normalizeFileName(file.name)} (file table)'
      : normalizeFileName(file.name);

  /// The row's subtext: what the file is, how big it is and where it lives.
  String _fileDetail(FileRecord file) =>
      '${file.isFiletable ? "Internal directory" : fileTypeLabel(file.name)}   '
      '${formatFileSize(file.size)}   '
      'offset ${hex32(file.offset)}';

  /// The file table itself, decoded: one row per file record.
  Future<void> _showTable(FileRecord table) async {
    _snack('Reading file table...');
    // Read the table file directly using CID 5 (read file) instead of CID 7.
    final data = await _client.readFile(table.name, size: table.size);
    if (!mounted) return;
    Navigator.of(context).push(
      MaterialPageRoute(
        builder: (_) => FileTableViewPage(
          deviceId: widget.deviceId,
          size: table.size,
          data: data,
        ),
      ),
    );
  }

  Future<void> _showFile(FileRecord file) async {
    _snack('Reading ${file.name}...');
    final data = await _client.readFile(file.name, size: file.size);
    // `.SV` is a raw static persistent-space mirror; fetch the device's blocks so
    // the decoder can compute the field layout and name the blocks and fields.
    List<({int type, int inst, ValueInfo meta, String name})?>? blocks;
    if (normalizeFileName(file.name).toUpperCase() == '.SV') {
      final all = await RegisterClient(deviceId: widget.deviceId).readBlocks();
      blocks = all
          ?.where((b) => b != null && isStaticRegistryType(b.type))
          .toList();
    }
    if (!mounted) return;
    Navigator.of(context).push(
      MaterialPageRoute(
        builder: (_) => FileViewPage(
          deviceId: widget.deviceId,
          name: file.name,
          size: file.size,
          data: data,
          blocks: blocks,
        ),
      ),
    );
  }
}

/// Decoded file-table view: every 16-byte file record as offset/size/name.
class FileTableViewPage extends StatelessWidget {
  final int deviceId;
  final int size;
  final List<int>? data;

  const FileTableViewPage({
    super.key,
    required this.deviceId,
    required this.size,
    required this.data,
  });

  @override
  Widget build(BuildContext context) {
    final body = data == null
        ? const Center(child: Text('Read failed'))
        : _TableBody(data: data!, size: size);
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

  const _TableBody({required this.data, required this.size});

  @override
  State<_TableBody> createState() => _TableBodyState();
}

/// One decoded file-table record.
class _FileTableRow {
  const _FileTableRow({
    required this.index,
    required this.name,
    required this.size,
    required this.offset,
    this.invalidated = false,
  });

  final int index;
  final String name;
  final String size;
  final String offset;
  final bool invalidated;
}

class _TableBodyState extends State<_TableBody> {
  bool _showInvalidated = false;

  /// The record columns, shared by the live and the invalidated table so the two
  /// can never drift out of alignment.
  List<TamuColumn<_FileTableRow>> get _columns => [
    TamuColumn<_FileTableRow>(
      label: '#',
      text: (r) => '${r.index}',
      mono: true,
      muted: true,
      flex: 1,
    ),
    TamuColumn<_FileTableRow>(
      label: 'File',
      flex: 3,
      cell: (context, r) => MonoText(
        r.name,
        tone: r.invalidated ? kTextDisabled : kTextSecondary,
      ),
    ),
    TamuColumn<_FileTableRow>(
      label: 'Size',
      text: (r) => r.size,
      mono: true,
      muted: true,
      flex: 2,
    ),
    TamuColumn<_FileTableRow>(
      label: 'Offset',
      text: (r) => r.offset,
      mono: true,
      muted: true,
      flex: 2,
    ),
  ];

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final data = widget.data;
    final live = <_FileTableRow>[];
    final invalidated = <_FileTableRow>[];
    for (var off = 0; off + 16 <= data.length; off += 16) {
      final recOffset = uint32FromBytes(data, off);
      final fileSize = uint32FromBytes(data, off + 4);
      final unwritten = recOffset == 0xFFFFFFFF && fileSize == 0xFFFFFFFF;
      // Offset 0 marks a superseded/invalidated record (the device zeroes the 4-byte
      // offset, leaving the size).
      final isInvalidated = !unwritten && recOffset == 0;
      final row = _FileTableRow(
        index: off ~/ 16,
        name: isInvalidated
            ? '(invalidated)'
            : normalizeFileName(
                String.fromCharCodes(data.sublist(off + 8, off + 16)),
              ),
        size: isInvalidated ? '' : formatFileSize(fileSize),
        offset: isInvalidated ? '' : hex32(recOffset),
        invalidated: isInvalidated,
      );
      if (isInvalidated) {
        invalidated.add(row);
      } else if (!unwritten) {
        live.add(row);
      }
    }
    return ListView(
      padding: Insets.page,
      children: [
        Padding(
          padding: const EdgeInsets.only(bottom: Spacing.md),
          child: Text(
            'File table (${widget.size} B) - '
            '${live.length} live, ${invalidated.length} invalidated',
            style: theme.textTheme.bodySmall?.copyWith(color: kOrange),
          ),
        ),
        TamuTable<_FileTableRow>(
          embedded: true,
          columns: _columns,
          rows: live,
          emptyMessage: 'No live records',
        ),
        if (invalidated.isNotEmpty) ...[
          const Divider(height: Spacing.xl),
          ListTile(
            dense: true,
            leading: Icon(
              _showInvalidated ? Icons.expand_less : Icons.expand_more,
              size: IconSize.md,
              color: kTextDisabled,
            ),
            title: Text(
              '${invalidated.length} invalidated (old) records',
              style: theme.textTheme.bodySmall?.copyWith(color: kTextMuted),
            ),
            onTap: () => setState(() => _showInvalidated = !_showInvalidated),
          ),
          if (_showInvalidated)
            TamuTable<_FileTableRow>(
              embedded: true,
              columns: _columns,
              rows: invalidated,
            ),
        ],
      ],
    );
  }
}
