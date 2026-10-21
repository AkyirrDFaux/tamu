import 'dart:async';

import 'package:flutter/material.dart';

import '../core/connection.dart';
import '../core/device_db.dart';
import '../core/types.dart';
import 'theme.dart';
import 'viewer_kit.dart';
import 'widgets.dart';

/// SNDB viewer page (Docs/App/Device view.md, core only): serial number
/// registry with refresh and ID (re-)assignment via SNDB Write.
class SndbPage extends StatefulWidget {
  const SndbPage({super.key});

  @override
  State<SndbPage> createState() => _SndbPageState();
}

class _SndbPageState extends State<SndbPage> with AutoRefreshMixin<SndbPage> {
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
        content: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            TextField(
              controller: snController,
              decoration: const InputDecoration(
                labelText: 'Serial number (28 hex chars)',
              ),
              maxLength: 28,
            ),
            TextField(
              controller: idController,
              decoration: const InputDecoration(
                labelText: 'Short ID (decimal)',
              ),
              keyboardType: TextInputType.number,
              maxLength: 5,
            ),
          ],
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context, false),
            child: const Text('Cancel'),
          ),
          FilledButton(
            onPressed: () => Navigator.pop(context, true),
            child: const Text('Write'),
          ),
        ],
      ),
    );
    final hex = snController.text.trim().toUpperCase();
    final id = int.tryParse(idController.text.trim());
    snController.dispose();
    idController.dispose();
    if (ok != true) return;
    // A 14-byte serial is 28 hex chars (112 bits); int.tryParse would overflow int64, so
    // validate the whole string with a regex (like the value editor does).
    if (!RegExp(r'^[0-9A-F]{28}$').hasMatch(hex)) {
      _snack('Invalid serial number');
      return;
    }
    if (id == null || id < 0 || id > 0xFFF) {
      _snack('ID must be 0..4095');
      return;
    }
    final snBytes = [
      for (var i = 0; i < 14; i++)
        int.parse(hex.substring(i * 2, i * 2 + 2), radix: 16),
    ];
    final success = await _db.sndbWrite(snBytes, id);
    _snack(success ? 'Registry updated' : 'Write failed');
    await _refresh();
  }

  Future<void> _removeEntry(int id, String snHex) async {
    final confirmed = await confirmDialog(
      context,
      title: 'Remove ${idToString(id)}?',
      body: 'The registry entry is tombstoned and the ID becomes reusable.',
      confirmLabel: 'Remove',
    );
    if (confirmed != true) return;
    final sn = [
      for (var i = 0; i < snHex.length; i += 2)
        int.parse(snHex.substring(i, i + 2), radix: 16),
    ];
    final ok = await _db.sndbDelete(sn);
    _snack(ok ? 'Entry removed' : 'Delete failed');
    await _refresh();
  }

  void _snack(String message) {
    if (!mounted) return;
    showSnack(context, message);
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
            icon: const Icon(Icons.person_add_alt_1_outlined),
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
      body: _buildBody(),
    );
  }

  Widget _buildBody() {
    final entries = _entries;
    if (entries == null) {
      return Center(child: Text(_error ?? 'Loading...'));
    }
    // The registry is the toolkit's table shell: the device's type icon leads, the
    // short ID, name and serial print as columns, and the row's delete action trails.
    return TamuTable<(int, String)>(
      columns: [
        TamuColumn<(int, String)>(
          label: 'ID',
          text: (r) => idToString(r.$1),
          mono: true,
          flex: 1,
        ),
        TamuColumn<(int, String)>(
          label: 'Device',
          text: (r) => _db.byId(r.$1)?.displayName ?? '(unknown device)',
          flex: 3,
        ),
        TamuColumn<(int, String)>(
          label: 'Serial number',
          text: (r) => r.$2,
          mono: true,
          muted: true,
          flex: 4,
        ),
      ],
      rows: entries,
      leading: (context, r) {
        final entry = _db.byId(r.$1);
        if (entry == null) return const SizedBox.shrink();
        return Icon(
          deviceTypeIcon(entry.type),
          size: IconSize.md,
          color: kTextSecondary,
        );
      },
      trailing: (context, r) => IconButton(
        icon: const Icon(Icons.delete_outline, size: IconSize.md),
        tooltip: 'Remove from registry',
        onPressed: () => _removeEntry(r.$1, r.$2),
      ),
      emptyMessage: 'Database empty',
    );
  }
}
