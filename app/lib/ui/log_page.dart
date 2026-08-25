import 'dart:async';

import 'package:flutter/material.dart';

import '../core/connection.dart';
import '../core/protocol.dart';
import '../core/types.dart';
import 'theme.dart';
import 'widgets.dart';

/// Log viewer for CORE devices (Docs/Services/Log Handler.md): fetches the RAM
/// log database (GetLogs CID 1) streamed as LogDatabase entries, decodes them
/// into readable text, offers a per-device filter and clearing.
class LogViewerPage extends StatefulWidget {
  final int deviceId;

  const LogViewerPage({super.key, required this.deviceId});

  @override
  State<LogViewerPage> createState() => _LogViewerPageState();
}

class _LogViewerPageState extends State<LogViewerPage>
    with AutoRefreshMixin<LogViewerPage> {
  List<LogEntry>? _logs;
  String? _error;
  bool _loading = false;

  /// null = show every device's entries.
  int? _deviceFilter;

  @override
  void initState() {
    super.initState();
    _fetch();
  }

  @override
  Future<void> onAutoRefresh() => _fetch();



  Future<void> _fetch() async {
    if (!ConnectionManager.instance.isConnected || _loading) return;
    setState(() => _loading = true);
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

  Future<void> _clearAll() async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Clear log database'),
        content: const Text('Remove all stored log entries from the core?'),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(context, false),
              child: const Text('Cancel')),
          FilledButton(
              onPressed: () => Navigator.pop(context, true),
              child: const Text('Clear')),
        ],
      ),
    );
    if (confirmed != true) return;
    // ClearReadLogs CID 2: count of oldest entries to drop; 0xFFFFFFFF = all.
    try {
      await ConnectionManager.instance.request(
          widget.deviceId, ServiceType.logHandler, 2,
          payload: [...uint32ToBytes(0xFFFFFFFF)],
          timeout: const Duration(seconds: 5));
    } catch (_) {}
    await _fetch();
  }

  List<LogEntry> get _filtered {
    final logs = _logs;
    if (logs == null) return const [];
    if (_deviceFilter == null) return logs;
    return logs.where((l) => l.deviceId == _deviceFilter).toList();
  }

  @override
  Widget build(BuildContext context) {
    final devices = _logs?.map((l) => l.deviceId).toSet().toList();
    devices?.sort();
    return Scaffold(
      appBar: AppBar(
        title: Text('Logs - ${idToString(widget.deviceId)}'),
        actions: [
          if (devices != null && devices.length > 1)
            PopupMenuButton<int?>(
              tooltip: 'Filter by device',
              icon: Badge(
                isLabelVisible: _deviceFilter != null,
                smallSize: 8,
                child:
                    const Icon(Icons.filter_alt_outlined),
              ),
              initialValue: _deviceFilter,
              onSelected: (id) => setState(() => _deviceFilter = id),
              itemBuilder: (_) => [
                const PopupMenuItem(value: null, child: Text('All devices')),
                for (final id in devices)
                  PopupMenuItem(
                      value: id, child: Text('Device ${idToString(id)}')),
              ],
            ),
          IconButton(
              onPressed: _clearAll,
              tooltip: 'Clear database',
              icon: const Icon(Icons.delete_sweep_outlined)),
          RefreshButton(
            onRefresh: _fetch,
            autoActive: autoRefreshActive,
            refreshing: _loading,
            error: _error != null,
            selectedInterval: selectedInterval,
            onSelectAuto: applyAuto,
          ),
        ],
      ),
      body: _buildBody(devices),
    );
  }

  Widget _buildBody(List<int>? devices) {
    if (_loading && _logs == null) {
      return const Center(child: CircularProgressIndicator());
    }
    if (_error != null && _logs == null) return Center(child: Text(_error!));
    final logs = _filtered;
    if (logs.isEmpty) {
      return Center(
          child: Text(_logs == null || _logs!.isEmpty
              ? 'No logs recorded'
              : 'No logs match the filter'));
    }
    return Column(children: [
      Expanded(
        child: ListView.separated(
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
              title: Text(log.describe()),
              subtitle: Text(log.detail()),
            );
          },
        ),
      ),
      if (devices != null)
        Padding(
          padding: const EdgeInsets.all(6),
          child: Text('${_logs!.length} entr${_logs!.length == 1 ? 'y' : 'ies'}'
                  ' from ${devices.length} device${devices.length == 1 ? '' : 's'}',
              style: const TextStyle(fontSize: 11, color: Colors.white38)),
        ),
    ]);
  }
}

/// One LogDatabase entry (Docs/Services/Log Handler.md):
/// Source Device u16 | Count u16 | Log Struct (src_and_code u32 + timestamp u32).
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

  /// Human-readable one-liner, resolved from the firmware's known sources.
  String describe() {
    final source = sourceName();
    final meaning = codeMeaning();
    return meaning ?? '$source reported code ${codeText()}';
  }

  String detail() =>
      '${isBlock ? 'Block' : 'Service'} $sourceName   '
      '${deviceId == 0xFFFF ? 'broadcast' : idToString(deviceId)}   '
      'x$count   ${formatUptime(timestampMs)}';

  String sourceName() {
    if (isBlock) {
      return BlockType.fromValue(sourceId).label;
    }
    final service = ServiceType.fromValue(sourceId & 0xFF);
    return service?.name ?? 'Service ${sourceId & 0xFF}';
  }

  /// Decoded code where the firmware defines one, else raw hex.
  String? codeMeaning() {
    if (!isBlock) {
      // Dispatcher reports MakeLog(false, service, cid, 0) when a request
      // could not be routed/handled; code 0 marks boot reports.
      if (code == 0) return 'Reported without a code';
      final service = ServiceType.fromValue(sourceId & 0xFF);
      return '${service == null ? 'Service' : service.name} CID $code failed';
    }
    switch (BlockType.fromValue(sourceId)) {
      case BlockType.accGyr:
        const errors = [
          'No error',
          'Bus transmit/receive failed',
          'Sensor not found (ACK/power)',
          'Init sequence failed',
          'Transaction timed out',
        ];
        return 'Acc/Gyr: ${code < errors.length ? errors[code] : codeText()}';
      default:
        return null;
    }
  }

  String codeText() => '0x${code.toRadixString(16).padLeft(4, '0').toUpperCase()}';

  static String formatUptime(int ms) => formatUptimeMs(ms);
}
