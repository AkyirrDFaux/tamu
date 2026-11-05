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
import 'loading.dart';
import 'theme.dart';
import 'file_viewers.dart';
import 'viewer_kit.dart';
import 'wait.dart';
import 'widgets.dart';

/// Storage service view (Docs/App/Service views/Storage.md): file table with
/// download, delete, rename, create and formatted viewers for known file
/// types. The file table itself is always read-only.
class StoragePage extends StatefulWidget {
  final int deviceId;

  /// Whether the link is up, as the page's own guard. Overridable so a widget test can drive the
  /// page without the app's connection singleton (the same seam [RegisterPage] has).
  final bool Function()? isConnected;

  const StoragePage({super.key, required this.deviceId, this.isConnected});

  bool get _connected =>
      (isConnected ?? () => ConnectionManager.instance.isConnected)();

  @override
  State<StoragePage> createState() => _StoragePageState();
}

class _StoragePageState extends State<StoragePage>
    with AutoRefreshMixin<StoragePage> {
  late final StorageClient _client = StorageClient(deviceId: widget.deviceId);

  List<FileRecord>? _files;
  String? _error;
  bool _refreshing = false;

  /// The wait the page is in: the table read for the list (drawn as a panel on the first load and
  /// as a band above the list on a refresh) and the whole-file read behind its modal dialog.
  final WaitController _listWait = WaitController();
  final WaitController _readWait = WaitController();

  @override
  void initState() {
    super.initState();
    _refresh();
  }

  @override
  void dispose() {
    _listWait.dispose();
    _readWait.dispose();
    super.dispose();
  }

  @override
  Future<void> onAutoRefresh() => _refresh();

  void _snack(String message) {
    if (!mounted) return;
    showSnack(context, message);
  }

  Future<void> _refresh() async {
    if (!widget._connected || _refreshing) return;
    setState(() => _refreshing = true);
    _listWait.begin(
      _files == null ? 'Reading the file table' : 'Refreshing the file table',
    );
    // The one shared whole-table reader: `null` is NOT the only failure shape any more - an absent
    // table (a device with no filesystem) is an empty directory, and a failed or partial read keeps
    // its reason so the page can say it instead of rendering an empty list.
    final table = await _client.readFileTableResult(
      onProgress: (received, total) => _listWait.step(
        completed: received,
        total: total,
        note: 'fragment $received of $total',
      ),
    );
    _listWait.end();
    if (!mounted) return;
    setState(() {
      _refreshing = false;
      _files = table.ok ? table.records : (table.absent ? const [] : null);
      _error = table.ok || table.absent
          ? null
          : (table.detail ??
                _client.lastReadError ??
                'The device did not answer.');
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
    final read = await runBehindWait<FileRead>(
      context,
      _readWait,
      label: 'Downloading ${normalizeFileName(file.name)}',
      cancelLabel: 'Cancel',
      task: () => _client.readFileWhole(
        file.name,
        size: file.size,
        onProgress: (received, total) => _readWait.step(
          completed: received,
          total: total,
          note: 'fragment $received of $total',
        ),
        isCancelled: () => _readWait.cancelled,
      ),
    );
    if (!mounted || read.cancelled) return;
    final data = read.value.bytes;
    if (data == null) {
      _snack(read.value.detail ?? 'Download failed');
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
    if (files == null) {
      final error = _error;
      if (error == null) {
        // Nothing is known yet: the wait names the read and counts its fragments as they land,
        // instead of a bare "Loading..." over an empty panel.
        return WaitView(controller: _listWait, shape: WaitShape.panel);
      }
      // A file-table read that could not be completed is a visible, actionable error: the page
      // says what the device answered and offers the retry that the client already bounded.
      return StateMessage.error(
        message: 'The file table could not be read',
        details: error,
        action: FilledButton(
          onPressed: _refresh,
          child: const Text('Try again'),
        ),
      );
    }
    // The list is the toolkit's table shell: a type icon, the name, the type/size/
    // offset detail and the per-row actions. The file table itself is read-only but
    // still openable: tapping shows the decoded directory
    // (Docs/Services/Storage.md). Read-only-ness is signalled by the lock icon, not
    // by disabling the row.
    final list = TamuTable<FileRecord>(
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
    // A refresh keeps the list on screen and puts the wait above it, so the page never goes blank
    // between two reads.
    return Column(
      children: [
        WaitView(controller: _listWait, shape: WaitShape.banner),
        Expanded(child: list),
      ],
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

  /// Reads one file behind the modal wait that names it and counts its fragments.
  ///
  /// The dialog is the "no blank piece" answer for a read the viewer cannot draw yet: it says what
  /// is being read, shows *fragments received of expected* while the stream lands, and its Cancel
  /// closes it at once - the page then drops the result and stops the retry policy.
  Future<FileRead> _readBehindDialog(FileRecord file) async {
    final read = await runBehindWait<FileRead>(
      context,
      _readWait,
      label: 'Reading ${normalizeFileName(file.name)}',
      cancelLabel: 'Cancel',
      task: () => _client.readFileWhole(
        file.name,
        size: file.size,
        onProgress: (received, total) => _readWait.step(
          completed: received,
          total: total,
          note: 'fragment $received of $total',
        ),
        isCancelled: () => _readWait.cancelled,
      ),
    );
    return read.value;
  }

  /// The file table itself, decoded: one row per file record.
  Future<void> _showTable(FileRecord table) async {
    // Read the table file directly using CID 5 (read file) instead of CID 7. The table is a
    // fixed 16 kB reservation whose reply can lose fragments and its STOP frame over BLE: the
    // client re-requests it whole rather than handing back a prefix, because a decoded prefix
    // shows 32 of N records as if it were the directory.
    final read = await runBehindWait<FileRead>(
      context,
      _readWait,
      label: 'Reading the file table (${formatFileSize(table.size)})',
      cancelLabel: 'Cancel',
      task: () => _client.readFileWhole(
        table.name,
        size: table.size,
        onProgress: (received, total) => _readWait.step(
          completed: received,
          total: total,
          note: 'fragment $received of $total',
        ),
        isCancelled: () => _readWait.cancelled,
      ),
    );
    if (!mounted || read.cancelled) return;
    final tableRead = read.value;
    Navigator.of(context).push(
      MaterialPageRoute(
        builder: (_) => FileTableViewPage(
          deviceId: widget.deviceId,
          size: table.size,
          data: tableRead.bytes,
          error: tableRead.detail,
        ),
      ),
    );
  }

  Future<void> _showFile(FileRecord file) async {
    final read = await runBehindWait<FileRead>(
      context,
      _readWait,
      label: 'Reading ${normalizeFileName(file.name)}',
      cancelLabel: 'Cancel',
      task: () => _client.readFileWhole(
        file.name,
        size: file.size,
        onProgress: (received, total) => _readWait.step(
          completed: received,
          total: total,
          note: 'fragment $received of $total',
        ),
        isCancelled: () => _readWait.cancelled,
      ),
    );
    if (!mounted || read.cancelled) return;
    final fileRead = read.value;
    final data = fileRead.bytes;
    // `.SV` is a raw static persistent-space mirror; fetch the device's blocks so
    // the decoder can compute the field layout and name the blocks and fields.
    List<({int type, int inst, ValueInfo meta, String name})?>? blocks;
    if (normalizeFileName(file.name).toUpperCase() == '.SV') {
      final all = (await RegisterClient(
        deviceId: widget.deviceId,
      ).readBlocks())?.blocks;
      blocks = all?.where((b) => isStaticRegistryType(b.type)).toList();
    }
    if (!mounted) return;
    Navigator.of(context).push(
      MaterialPageRoute(
        builder: (_) => FileViewPage(
          deviceId: widget.deviceId,
          name: file.name,
          size: file.size,
          data: data,
          error: fileRead.detail,
          onRetry: () async => (await _readBehindDialog(file)).bytes,
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

  /// Why [data] is null, as the storage client worded it.
  final String? error;

  const FileTableViewPage({
    super.key,
    required this.deviceId,
    required this.size,
    required this.data,
    this.error,
  });

  @override
  Widget build(BuildContext context) {
    // A table the client could not read whole is an error, never a decoded prefix: the records
    // past a hole are unknown, and printing the ones before it reads as the whole directory.
    final body = data == null
        ? StateMessage.error(
            message: 'Could not read the file table',
            details: error,
          )
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
