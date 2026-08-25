import 'dart:async';

import 'package:flutter/material.dart';

import '../core/connection.dart';
import '../core/device_db.dart';
import '../core/types.dart';
import 'device_icons.dart';
import 'widgets.dart';

/// SNDB viewer page (Docs/App/Device view.md, core only): serial number
/// registry with refresh and ID (re-)assignment via SNDB Write.
class SndbPage extends StatefulWidget {
  const SndbPage({super.key});

  @override
  State<SndbPage> createState() => _SndbPageState();
}

class _SndbPageState extends State<SndbPage>
    with AutoRefreshMixin<SndbPage> {
  final _db = DeviceDatabase.instance;
  List<(int, String)>? _entries;
  String? _error;
  bool _refreshing = false;

  @override
  void initState() {
    super.initState();
    _refresh();
  }

  @override
  Future<void> onAutoRefresh() => _refresh();



  Future<void> _refresh() async {
    if (!ConnectionManager.instance.isConnected || _refreshing) return;
    setState(() => _refreshing = true);
    final rows = await _db.sndbEntries();
    if (!mounted) return;
    setState(() {
      _refreshing = false;
      if (rows.isEmpty && !ConnectionManager.instance.isConnected) {
        _error = 'Not connected';
      } else {
        _error = null;
        _entries = rows;
      }
    });
  }

  /// Assigns an existing or new serial number a short ID (SNDB Write CID 14).
  Future<void> _assignId() async {
    final snController = TextEditingController();
    final idController = TextEditingController();
    final ok = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Assign device ID'),
        content: Column(mainAxisSize: MainAxisSize.min, children: [
          TextField(
            controller: snController,
            decoration:
                const InputDecoration(labelText: 'Serial number (28 hex chars)'),
            maxLength: 28,
          ),
          TextField(
            controller: idController,
            decoration:
                const InputDecoration(labelText: 'Short ID (decimal)'),
            keyboardType: TextInputType.number,
            maxLength: 5,
          ),
        ]),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(context, false),
              child: const Text('Cancel')),
          FilledButton(
              onPressed: () => Navigator.pop(context, true),
              child: const Text('Write')),
        ],
      ),
    );
    if (ok != true) return;
    final hex = snController.text.trim().toUpperCase();
    final id = int.tryParse(idController.text.trim());
    if (hex.length != 28 || int.tryParse(hex, radix: 16) == null) {
      _snack('Invalid serial number');
      return;
    }
    if (id == null || id < 0 || id > 0xFFF) {
      _snack('ID must be 0..4095');
      return;
    }
    final snBytes = [
      for (var i = 0; i < 14; i++)
        int.parse(hex.substring(i * 2, i * 2 + 2), radix: 16)
    ];
    final success = await _db.sndbWrite(snBytes, id);
    _snack(success ? 'Registry updated' : 'Write failed');
    await _refresh();
  }

  Future<void> _removeEntry(int id, String snHex) async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text('Remove ${idToString(id)}?'),
        content:
            const Text('The registry entry is tombstoned and the ID becomes reusable.'),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(context, false),
              child: const Text('Cancel')),
          FilledButton(
              onPressed: () => Navigator.pop(context, true),
              child: const Text('Remove')),
        ],
      ),
    );
    if (confirmed != true) return;
    final sn = [
      for (var i = 0; i < snHex.length; i += 2)
        int.parse(snHex.substring(i, i + 2), radix: 16)
    ];
    final ok = await _db.sndbDelete(sn);
    _snack(ok ? 'Entry removed' : 'Delete failed');
    await _refresh();
  }

  void _snack(String message) {
    if (!mounted) return;
    ScaffoldMessenger.of(context)
        .showSnackBar(SnackBar(content: Text(message)));
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('SN Database'),
        actions: [
          IconButton(
              onPressed: _assignId,
              tooltip: 'Assign / update an entry',
              icon: const Icon(Icons.person_add_alt_1_outlined)),
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
    final entries = _entries;
    if (entries == null) {
      return Center(child: Text(_error ?? 'Loading...'));
    }
    if (entries.isEmpty) return const Center(child: Text('Database empty'));
    return ListView.separated(
      itemCount: entries.length,
      separatorBuilder: (_, _) => const Divider(height: 1),
      itemBuilder: (context, index) {
        final (id, sn) = entries[index];
        final entry = _db.byId(id);
        return ListTile(
          dense: true,
          leading: Text(idToString(id)),
          title: Text(entry?.displayName ?? '(unknown device)',
              style: const TextStyle(fontSize: 13)),
          subtitle: Text(sn,
              style: const TextStyle(fontFamily: 'monospace', fontSize: 12)),
          trailing: Row(mainAxisSize: MainAxisSize.min, children: [
            IconButton(
              icon: const Icon(Icons.delete_outline, size: 18),
              tooltip: 'Remove from registry',
              onPressed: () => _removeEntry(id, sn),
            ),
            if (entry != null) Icon(deviceTypeIcon(entry.type), size: 18),
          ]),
        );
      },
    );
  }
}
