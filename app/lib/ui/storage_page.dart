import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';

import '../core/connection.dart';
import '../core/block_registry.dart' show FieldInfo, blockInfoFor;
import '../core/storage_client.dart';
import '../core/types.dart';
import 'theme.dart';
import 'value_editor.dart' show dataTypeLabel, formatValue;
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
    ScaffoldMessenger.of(context)
        .showSnackBar(SnackBar(content: Text(message)));
  }

  Future<void> _refresh() async {
    if (!ConnectionManager.instance.isConnected || _refreshing) return;
    setState(() => _refreshing = true);
    final files = await _client.readFileTable();
    if (!mounted) return;
    setState(() {
      _refreshing = false;
      _error = files == null ? 'Device did not respond' : null;
      if (files != null) _files = files;
    });
  }

  String _formatSize(int bytes) {
    if (bytes < 1024) return '$bytes B';
    if (bytes < 1024 * 1024) return '${(bytes / 1024).toStringAsFixed(1)} kB';
    return '${(bytes / (1024 * 1024)).toStringAsFixed(2)} MB';
  }

  Future<bool> _confirm(String title, String body) async {
    return await showDialog<bool>(
          context: context,
          builder: (context) => AlertDialog(
            title: Text(title),
            content: Text(body),
            actions: [
              TextButton(
                  onPressed: () => Navigator.pop(context, false),
                  child: const Text('Cancel')),
              FilledButton(
                  onPressed: () => Navigator.pop(context, true),
                  child: const Text('Confirm')),
            ],
          ),
        ) ==
        true;
  }

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
          IconButton(
              onPressed: _createFile,
              tooltip: 'Create file',
              icon: const Icon(Icons.create_new_folder_outlined)),
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
    // First record is the file table itself; list actual files only.
    final realFiles = files.toList();
    if (files.isEmpty || (files.length == 1 && files.first.isFiletable)) {
      return Center(
          child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                const Text('No files stored'),
                const SizedBox(height: 4),
                if (files.isNotEmpty)
                  Text('File table: ${_formatSize(files.first.size)}',
                      style: const TextStyle(color: Colors.white38)),
              ]));
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
          title: Text(isTable ? '${file.name} (file table)' : file.name,
              style: isTable ? const TextStyle(color: Colors.white38) : null),
          subtitle:
              Text('${isTable ? "Internal directory" : fileTypeLabel(file.name)}   '
                  '${_formatSize(file.size)}   '
                  'offset ${_hex32(file.offset)}'),
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
                  _renameFile(file);
                case 'delete':
                  _deleteFile(file);
              }
            },
            itemBuilder: (_) => const [
              PopupMenuItem(value: 'view', child: Text('View')),
              PopupMenuItem(value: 'download', child: Text('Download')),
              PopupMenuItem(value: 'rename', child: Text('Rename')),
              PopupMenuItem(value: 'delete', child: Text('Delete')),
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
    // The table can span several pages; read it all so the decoded view agrees
    // with the CID-0 file list (a 4096-byte cap truncates >256-record tables).
    final data = await _client.readFile(table.name, size: table.size);
    if (!mounted) return;
    Navigator.of(context).push(MaterialPageRoute(
        builder: (_) => FileTableViewPage(
            deviceId: widget.deviceId, size: table.size, data: data)));
  }

  Future<void> _showFile(FileRecord file) async {
    _snack('Reading ${file.name}...');
    final data =
        await _client.readFile(file.name, size: file.size);
    if (!mounted) return;
    Navigator.of(context).push(MaterialPageRoute(
        builder: (_) => FileViewPage(deviceId: widget.deviceId,
            name: file.name, size: file.size, data: data)));
  }
}

// ---------------------------------------------------------------------------
// Known file types (Docs/Services/Storage.md: "the viewer/editor of files
// formats the files based on the file name")
// ---------------------------------------------------------------------------

enum StorageFileType { snreg, layout, text, binary, sysmem, dynmem, keymem }

StorageFileType storageFileType(String name) {
  final upper = name.toUpperCase().trim();
  if (upper == 'SNREG') return StorageFileType.snreg;
  if (upper == 'SYSMEM') return StorageFileType.sysmem;
  if (upper == 'DYNMEM') return StorageFileType.dynmem;
  if (upper == 'KEYMEM') return StorageFileType.keymem;
  if (upper.startsWith('LAY') || upper.endsWith('.LAY')) {
    return StorageFileType.layout;
  }
  if (upper.endsWith('.TXT') || upper.endsWith('.LOG')) {
    return StorageFileType.text;
  }
  return StorageFileType.binary;
}

IconData storageFileIcon(String name) => switch (storageFileType(name)) {
      StorageFileType.snreg => Icons.badge_outlined,
      StorageFileType.layout => Icons.grid_on_outlined,
      StorageFileType.text => Icons.description_outlined,
      StorageFileType.binary => Icons.insert_drive_file_outlined,
      StorageFileType.sysmem => Icons.memory,
      StorageFileType.dynmem => Icons.storage_outlined,
      StorageFileType.keymem => Icons.key_outlined,
    };

String fileTypeLabel(String name) => switch (storageFileType(name)) {
      StorageFileType.snreg => 'Serial registry',
      StorageFileType.layout => 'LED layout',
      StorageFileType.text => 'Text',
      StorageFileType.binary => 'Binary',
      StorageFileType.sysmem => 'System memory backup',
      StorageFileType.dynmem => 'Dynamic memory backup',
      StorageFileType.keymem => 'Keyed memory backup',
    };

/// Decoded file-table view: every 16-byte Filerecord as offset/size/name.
class FileTableViewPage extends StatelessWidget {
  final int deviceId;
  final int size;
  final List<int>? data;

  const FileTableViewPage(
      {super.key,
      required this.deviceId,
      required this.size,
      required this.data});

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
      final isInvalidated = !unwritten && recOffset == 0;
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
            : Text('offset ${_hex32(recOffset)}   $fileSize B',
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

/// Formatted preview of one file's content. Known types render decoded:
/// SNREG as a registry table, LAY files as an LED-index grid, textual data as
/// text; anything else as hex.
class FileViewPage extends StatelessWidget {
  final int deviceId;
  final String name;
  final int size;
  final List<int>? data;

  const FileViewPage(
      {super.key,
      required this.deviceId,
      required this.name,
      required this.size,
      required this.data});

  bool get _looksTextual {
    if (data == null || data!.isEmpty) return false;
    var printable = 0;
    for (final b in data!) {
      final isPrintable = (b >= 0x20 && b < 0x7F) ||
          b == 0x0A || // \n
          b == 0x0D; // \r
      if (isPrintable) printable++;
    }
    return printable / data!.length > 0.9;
  }

  Widget _mono(String text) => SingleChildScrollView(
      padding: const EdgeInsets.all(12),
      child: SelectableText(text,
          style: const TextStyle(fontFamily: 'monospace', fontSize: 12)));

  /// SNREG: 32-byte RegistryEntry records (SNDB.h): u16 valid marker, u16 short
  /// ID, 12 reserved bytes, 14-byte serial number. Markers: 0x55AA valid,
  /// 0x0000 removed, 0xFFFF unwritten.
  Widget _snregView() {
    final rows = <Widget>[];
    for (var off = 0; off + 32 <= data!.length; off += 32) {
      final valid = data![off] | (data![off + 1] << 8);
      if (valid == 0xFFFF) break; // unwritten slot
      final id = data![off + 2] | (data![off + 3] << 8);
      final removed = valid == 0x0000;
      final sn = serialNumberToHex(data!.sublist(off + 16, off + 30));
      rows.add(Padding(
        padding: const EdgeInsets.symmetric(vertical: 2),
        child: Row(children: [
          SizedBox(
              width: 88,
              child: Text(removed ? 'id $id (removed)' : idToString(id),
                  style: TextStyle(
                      color: removed ? Colors.white24 : kOrange,
                      fontSize: 12))),
          Expanded(
              child: Text(sn,
                  style: TextStyle(
                      fontFamily: 'monospace',
                      fontSize: 12,
                      color: removed ? Colors.white24 : Colors.white70))),
        ]),
      ));
    }
    return ListView(padding: const EdgeInsets.all(12), children: rows);
  }

  /// Layout file: row-first W x H uint16 LED indexes, 0xFFFF = missing
  /// (Docs/Modules/LED display.md). Header: uint16 width, uint16 height.
  Widget _layoutView() {
    if (data!.length < 4) return _mono('(empty)');
    final w = data![0] | (data![1] << 8);
    final h = data![2] | (data![3] << 8);
    if (w == 0 || h == 0 || w > 128 || h > 128 || 4 + w * h * 2 > data!.length) {
      return _mono('Invalid layout header (${w}x$h)');
    }
    return SingleChildScrollView(
      padding: const EdgeInsets.all(12),
      scrollDirection: Axis.horizontal,
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Text('${w}x$h LEDs',
            style: const TextStyle(color: Colors.white38, fontSize: 11)),
        const SizedBox(height: 6),
        for (var r = 0; r < h; r++)
          Row(children: [
            for (var c = 0; c < w; c++)
              Container(
                width: 44,
                height: 22,
                margin: const EdgeInsets.all(1),
                color: kSurfaceAlt,
                alignment: Alignment.center,
                child: Text(() {
                  final i = 4 + (r * w + c) * 2;
                  final v = data![i] | (data![i + 1] << 8);
                  return v == 0xFFFF ? '-' : '$v';
                }(),
                    style: TextStyle(
                        fontSize: 10,
                        color:
                            data![4 + (r * w + c) * 2] == 0xFF &&
                                    data![4 + (r * w + c) * 2 + 1] == 0xFF
                                ? Colors.white24
                                : Colors.white)),
              ),
          ]),
      ]),
    );
  }

  @override
  Widget build(BuildContext context) {
    Widget body;
    if (data == null) {
      body = const Center(child: Text('Read failed'));
    } else {
      switch (storageFileType(name)) {
        case StorageFileType.snreg:
          body = _snregView();
        case StorageFileType.layout:
          body = _layoutView();
        case StorageFileType.sysmem:
        case StorageFileType.dynmem:
        case StorageFileType.keymem:
          body = MemoryBackupView(fileName: name, data: data!);
        case StorageFileType.text when _looksTextual:
          final text =
              String.fromCharCodes(data!).replaceAll(RegExp(r' +'), ' ');
          body = _mono(text);
        case StorageFileType.binary when _looksTextual:
          final text =
              String.fromCharCodes(data!).replaceAll(RegExp(r' +'), ' ');
          body = _mono(text);
        default:
          final hex = data!
              .map((b) => b.toRadixString(16).padLeft(2, '0'))
              .join(' ');
          body = _mono(hex);
      }
    }
    return Scaffold(
      appBar: AppBar(title:
          Text('$name - ${fileTypeLabel(name)} ($size B)')),
      body: body,
    );
  }
}

// ---------------------------------------------------------------------------
// Memory backup file decoders (Docs/Services/System|Dynamic|Keyed Memory.md:
// "the backup file is the serialised registry").
//
// Dynamic/Keyed format (SerializeRegistry):  u16 block_count, then per block
//   u8 name_len + name, u16 type, u16 map_count, map (BlockMeta x map_count,
//   4 B each), u16 data_len + data. Field/dict data sits at aligned offsets
//   (GetOffset sums AlignTo4(Size)).
// System format (SerializeSystemBlocks):   u16 writable_blocks, then per block
//   u16 block_index, u16 field_count, then per field u16 index, u16 vlen, value.
// ---------------------------------------------------------------------------

class MemoryBackupView extends StatelessWidget {
  final String fileName;
  final List<int> data;

  const MemoryBackupView({super.key, required this.fileName, required this.data});

  static int _u16(List<int> b, int o) =>
      o + 1 < b.length ? (b[o] | (b[o + 1] << 8)) : 0;
  static int _align4(int v) => (v + 3) & ~3;

  List<Widget> _parseSystem() {
    final rows = <Widget>[];
    final b = data;
    if (b.length < 2) return [const Text('(corrupt backup)')];
    final blockCount = _u16(b, 0);
    var c = 2;
    for (var w = 0; w < blockCount; w++) {
      if (c + 4 > b.length) break;
      final blockIndex = _u16(b, c);
      final fieldCount = _u16(b, c + 2);
      c += 4;
      final type = _systemBlockType(blockIndex);
      final info = type == null ? null : blockInfoFor(type);
      final fields = <Widget>[];
      for (var f = 0; f < fieldCount; f++) {
        if (c + 4 > b.length) break;
        final fi = _u16(b, c);
        final vlen = _u16(b, c + 2);
        c += 4;
        if (c + vlen > b.length) break;
        final value = b.sublist(c, c + vlen);
        c += vlen;
        final fname = info?.field(fi)?.name ?? 'Field $fi';
        final meta = BlockMeta(flagsAndType: info?.field(fi) == null ? 0 : _systemFieldType(type!, fi), size: vlen);
        fields.add(ListTile(
          dense: true,
          contentPadding: const EdgeInsets.only(left: 40, right: 12),
          title: Row(children: [
            Expanded(child: Text('$fname: ${_formatBytes(meta, value)}',
                style: const TextStyle(fontFamily: 'monospace', fontSize: 12))),
          ]),
        ));
      }
      rows.add(_blockCard('Block $blockIndex (${type?.label ?? 'unknown'})',
          info?.typeName ?? '', fields));
    }
    return rows;
  }

  BlockType? _systemBlockType(int index) => switch (index) {
        0 => BlockType.ledButton,
        1 => BlockType.pwm,
        2 => BlockType.pwm,
        3 => BlockType.accGyr,
        4 => BlockType.vysiDisplay,
        5 => BlockType.vysiDisplay,
        _ => null,
      };

  int _systemFieldType(BlockType type, int index) {
    final info = blockInfoFor(type);
    final f = info?.field(index);
    if (f == null) return 0;
    // infer data type from the field name registry (Number for numbers, etc.)
    return _inferDataType(f).value;
  }

  DataType _inferDataType(FieldInfo f) {
    final n = f.name.toLowerCase();
    if (n.contains('bool') || n == 'led' || n == 'button') return DataType.bool_;
    if (n.contains('percent') || n.contains('value') || n.contains('range') ||
        n.contains('voltage') || n.contains('resistance') || n.contains('lux') ||
        n.contains('temp') || n.contains('brightness') || n.contains('rate')) {
      return DataType.number;
    }
    return DataType.uint32;
  }

  List<Widget> _parseRegistry() {
    final rows = <Widget>[];
    final isKeyed = fileName.toUpperCase() == 'KEYMEM';
    final b = data;
    if (b.length < 2) return [const Text('(corrupt backup)')];
    final blockCount = _u16(b, 0);
    var c = 2;
    for (var i = 0; i < blockCount; i++) {
      if (c + 1 > b.length) break;
      final nameLen = b[c++];
      if (c + nameLen > b.length) break;
      final name = String.fromCharCodes(b.sublist(c, c + nameLen));
      c += nameLen;
      if (c + 6 > b.length) break;
      final typeValue = _u16(b, c);
      final mapCount = _u16(b, c + 2);
      c += 4;
      if (c + mapCount * 4 > b.length) break;
      final metas = <BlockMeta>[];
      for (var m = 0; m < mapCount; m++) {
        metas.add(BlockMeta.fromBytes(b, c + m * 4));
      }
      c += mapCount * 4;
      if (c + 2 > b.length) break;
      final dataLen = _u16(b, c);
      c += 2;
      if (c + dataLen > b.length) break;
      final blob = b.sublist(c, c + dataLen);
      c += dataLen;

      final children = <Widget>[];
      final blockType = BlockType.fromValue(typeValue);
      final info = blockInfoFor(blockType);
      if (isKeyed) {
        // each dict: its data starts at AlignTo4(sum of previous dict sizes)
        var off = 0;
        for (var d = 0; d < mapCount; d++) {
          final dictMeta = metas[d];
          final dictSize = dictMeta.size;
          final entries = <Widget>[];
          var e = off;
          while (e + 4 <= off + dictSize) {
            final em = BlockMeta.fromBytes(blob, e);
            final esize = em.size;
            if (e + 4 + esize > off + dictSize || esize > 255) break;
            final v = blob.sublist(e + 4, e + 4 + esize);
            entries.add(ListTile(
              dense: true,
              contentPadding: const EdgeInsets.only(left: 60, right: 12),
              title: Row(children: [
                SizedBox(width: 56,
                    child: Text('k${em.key.toRadixString(16).padLeft(2, '0').toUpperCase()}',
                        style: const TextStyle(color: kOrange, fontSize: 11))),
                Expanded(child: Text(_formatBytes(em, v),
                    style: const TextStyle(fontFamily: 'monospace', fontSize: 12))),
              ]),
              subtitle: Text(dataTypeLabel(em.dataType),
                  style: const TextStyle(fontSize: 10, color: Colors.white38)),
            ));
            e += _align4(4 + esize);
          }
          children.add(_blockCard('Dictionary $d (${dataTypeLabel(dictMeta.dataType)})',
              '${entries.length} entries', entries));
          off += _align4(dictSize);
        }
      } else {
        var off = 0;
        for (var f = 0; f < mapCount; f++) {
          final meta = metas[f];
          final size = meta.size;
          if (off + size > blob.length) break;
          final v = blob.sublist(off, off + size);
          final fname = info?.field(f)?.name ?? 'Entry $f';
          children.add(ListTile(
            dense: true,
            contentPadding: const EdgeInsets.only(left: 60, right: 12),
            title: Row(children: [
              Expanded(child: Text('$fname: ${_formatBytes(meta, v)}',
                  style: const TextStyle(fontFamily: 'monospace', fontSize: 12))),
            ]),
            subtitle: Text(dataTypeLabel(meta.dataType),
                style: const TextStyle(fontSize: 10, color: Colors.white38)),
          ));
          off += _align4(size);
        }
      }
      rows.add(_blockCard(name, blockType.label, children));
    }
    return rows;
  }

  Widget _blockCard(String title, String subtitle, List<Widget> children) {
    return Card(
      color: kSurfaceAlt,
      margin: const EdgeInsets.only(bottom: 6),
      clipBehavior: Clip.antiAlias,
      child: Column(children: [
        ListTile(
          dense: true,
          title: Text(title, style: const TextStyle(fontWeight: FontWeight.w600)),
          subtitle: Text(subtitle, style: const TextStyle(fontSize: 10)),
        ),
        if (children.isNotEmpty)
          Material(color: Colors.black26, child: Column(children: children)),
      ]),
    );
  }

  String _formatBytes(BlockMeta meta, List<int> bytes) {
    if (meta.dataType == DataType.none) return '∅';
    return formatValue(meta.dataType, bytes);
  }

  @override
  Widget build(BuildContext context) {
    final isSystem = fileName.toUpperCase() == 'SYSMEM';
    final rows = isSystem ? _parseSystem() : _parseRegistry();
    if (rows.isEmpty) return const Center(child: Text('(empty backup)'));
    return ListView(
        padding: const EdgeInsets.all(12),
        children: [for (final r in rows) r]);
  }
}

/// 32-bit hex formatting shared by the storage page rows and backup decoder.
String _hex32(int value) =>
    '0x${value.toRadixString(16).padLeft(8, '0').toUpperCase()}';
