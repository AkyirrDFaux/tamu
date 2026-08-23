import 'package:flutter/material.dart';

import '../core/connection.dart';
import '../core/storage_client.dart';
import '../core/types.dart';


/// Storage service view (Docs/App/Service views/Storage.md): read-only file
/// table with file content preview.
class StoragePage extends StatefulWidget {
  final int deviceId;

  const StoragePage({super.key, required this.deviceId});

  @override
  State<StoragePage> createState() => _StoragePageState();
}

class _StoragePageState extends State<StoragePage> {
  late final StorageClient _client = StorageClient(deviceId: widget.deviceId);

  List<FileRecord>? _files;
  String? _error;

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
    final files = await _client.readFileTable();
    if (!mounted) return;
    setState(() {
      _error = files == null ? 'Device did not respond' : null;
      if (files != null) _files = files;
    });
  }

  String _formatSize(int bytes) {
    if (bytes < 1024) return '$bytes B';
    if (bytes < 1024 * 1024) return '${(bytes / 1024).toStringAsFixed(1)} kB';
    return '${(bytes / (1024 * 1024)).toStringAsFixed(2)} MB';
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text('Storage - ${idToString(widget.deviceId)}'),
        actions: [
          IconButton(
              onPressed: _refresh,
              tooltip: 'Refresh',
              icon: const Icon(Icons.refresh)),
        ],
      ),
      body: _buildBody(),
    );
  }

  Widget _buildBody() {
    final files = _files;
    if (files == null) return Center(child: Text(_error ?? 'Loading...'));
    // First record is the file table itself; list actual files only.
    final realFiles = files.where((f) => !f.isFiletable).toList();
    if (realFiles.isEmpty) {
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
        return ListTile(
          leading: const Icon(Icons.insert_drive_file_outlined),
          title: Text(file.name),
          subtitle:
              Text('Offset ${_hex32(file.offset)}   ${_formatSize(file.size)}'),
          trailing: const Icon(Icons.chevron_right),
          onTap: () => _showFile(file),
        );
      },
    );
  }

  static String _hex32(int value) =>
      '0x${value.toRadixString(16).padLeft(8, '0').toUpperCase()}';

  Future<void> _showFile(FileRecord file) async {
    _snack('Reading ${file.name}...');
    final data =
        await _client.readFile(file.name, maxBytes: 4096);
    if (!mounted) return;
    Navigator.of(context).push(MaterialPageRoute(
        builder: (_) => FileViewPage(deviceId: widget.deviceId,
            name: file.name, size: file.size, data: data)));
  }
}

/// Simple formatted preview of one file's content. Layout files (char[8]
/// space-padded LED index tables) and text render as text; anything else as hex.
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

  @override
  Widget build(BuildContext context) {
    Widget body;
    if (data == null) {
      body = const Center(child: Text('Read failed'));
    } else if (_looksTextual) {
      final text =
          String.fromCharCodes(data!).replaceAll(RegExp(r' +'), ' ');
      body = SingleChildScrollView(
          padding: const EdgeInsets.all(12),
          child: SelectableText(text,
              style: const TextStyle(fontFamily: 'monospace', fontSize: 12)));
    } else {
      final hex = data!
          .map((b) => b.toRadixString(16).padLeft(2, '0'))
          .join(' ');
      body = SingleChildScrollView(
          padding: const EdgeInsets.all(12),
          child: SelectableText(hex,
              style: const TextStyle(fontFamily: 'monospace', fontSize: 12)));
    }
    return Scaffold(
      appBar: AppBar(title: Text('$name ($size B)')),
      body: body,
    );
  }
}
