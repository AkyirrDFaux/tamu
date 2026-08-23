import 'package:flutter/material.dart';

import '../core/connection.dart';
import '../core/protocol.dart';
import '../core/types.dart';
import 'theme.dart';

/// Log viewer: fetches the connected device's RAM log database via the
/// Log Handler GetLogs CID (streamed LogRecords).
class LogViewerPage extends StatefulWidget {
  final int deviceId;

  const LogViewerPage({super.key, required this.deviceId});

  @override
  State<LogViewerPage> createState() => _LogViewerPageState();
}

class _LogViewerPageState extends State<LogViewerPage> {
  List<LogEntry>? _logs;
  String? _error;
  bool _loading = false;

  @override
  void initState() {
    super.initState();
    _fetch();
  }

  Future<void> _fetch() async {
    setState(() {
      _loading = true;
      _error = null;
    });
    List<int>? reply;
    try {
      reply = await ConnectionManager.instance
          .request(widget.deviceId, ServiceType.logHandler, 1,
              timeout: const Duration(seconds: 5));
    } catch (_) {}
    if (!mounted) return;
    setState(() {
      _loading = false;
      if (reply == null) {
        _error = 'Device did not respond';
      } else if (reply.isEmpty) {
        _logs = [];
      } else {
        _logs = [
          for (var offset = 0; offset + 12 <= reply.length; offset += 12)
            LogEntry.fromBytes(reply.sublist(offset, offset + 12))
        ];
      }
    });
  }

  static String _hex(int value, int digits) =>
      value.toRadixString(16).padLeft(digits, '0').toUpperCase();

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text('Logs - ${idToString(widget.deviceId)}'),
        actions: [
          IconButton(
              onPressed: _loading ? null : _fetch,
              tooltip: 'Refresh',
              icon: const Icon(Icons.refresh)),
        ],
      ),
      body: _buildBody(),
    );
  }

  Widget _buildBody() {
    if (_loading) return const Center(child: CircularProgressIndicator());
    if (_error != null) return Center(child: Text(_error!));
    final logs = _logs;
    if (logs == null) return const SizedBox.shrink();
    if (logs.isEmpty) return const Center(child: Text('No logs recorded'));
    return ListView.separated(
      itemCount: logs.length,
      separatorBuilder: (_, _) => const Divider(height: 1),
      itemBuilder: (context, index) {
        final log = logs[index];
        return ListTile(
          dense: true,
          leading: Icon(
              log.isBlock ? Icons.widgets_outlined : Icons.settings_suggest,
              size: 20,
              color: kOrange),
          title: Text(
              '${log.isBlock ? 'Block' : 'Service'} 0x${_hex(log.sourceId, 4)}  Code 0x${_hex(log.code, 4)}'),
          subtitle: Text(
              'Device ${log.deviceId}   x${log.count}   t=${log.timestampMs} ms'),
        );
      },
    );
  }
}

class LogEntry {
  final int deviceId;
  final int count;
  final int srcAndCode;
  final int timestampMs;

  LogEntry(
      {required this.deviceId,
      required this.count,
      required this.srcAndCode,
      required this.timestampMs});

  factory LogEntry.fromBytes(List<int> bytes) => LogEntry(
        deviceId: bytes[0] | (bytes[1] << 8),
        count: bytes[2] | (bytes[3] << 8),
        srcAndCode: uint32FromBytes(bytes, 4),
        timestampMs: uint32FromBytes(bytes, 8),
      );

  bool get isBlock => srcAndCode & 0x1 != 0;
  int get sourceId => (srcAndCode >> 1) & 0x7FFF;
  int get code => (srcAndCode >> 16) & 0xFFFF;
}
