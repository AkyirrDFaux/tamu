import 'package:flutter/material.dart';

import '../core/device_db.dart';
import '../core/types.dart';
import 'sysmem_page.dart';
import 'theme.dart';

/// Device view (Docs/App/Device view.md): known facts about one device.
class DeviceViewPage extends StatefulWidget {
  final int deviceId;

  const DeviceViewPage({super.key, required this.deviceId});

  @override
  State<DeviceViewPage> createState() => _DeviceViewPageState();
}

class _DeviceViewPageState extends State<DeviceViewPage> {
  final _db = DeviceDatabase.instance;
  bool _renaming = false;

  @override
  void initState() {
    super.initState();
    _refresh();
  }

  Future<void> _refresh() async {
    await _db.refreshDevice(widget.deviceId);
    await _db.refreshRuntime(widget.deviceId);
  }

  String _formatUptime(int? ms) {
    if (ms == null) return '-';
    final seconds = ms ~/ 1000;
    final h = seconds ~/ 3600;
    final m = (seconds % 3600) ~/ 60;
    final s = seconds % 60;
    return '${h}h ${m}m ${s}s';
  }

  @override
  Widget build(BuildContext context) {
    final entry = _db.byId(widget.deviceId);
    return ListenableBuilder(
      listenable: _db,
      builder: (context, _) => Scaffold(
        appBar: AppBar(
          title: InkWell(
            onTap: () => setState(() => _renaming = true),
            child: Row(mainAxisSize: MainAxisSize.min, children: [
              Text(entry?.name ?? 'Device'),
              const SizedBox(width: 6),
              const Icon(Icons.edit, size: 16),
            ]),
          ),
          actions: [
            IconButton(icon: const Icon(Icons.refresh), onPressed: _refresh),
          ],
        ),
        body: entry == null
            ? const Center(child: CircularProgressIndicator())
            : ListView(
                padding: const EdgeInsets.all(12),
                children: [
                  if (_renaming) ...[
                    RenameField(
                        currentName: entry.name,
                        onDone: (name) async {
                          setState(() => _renaming = false);
                          if (name != null && name.isNotEmpty) {
                            await _db.setName(widget.deviceId, name);
                          }
                        }),
                    const SizedBox(height: 12),
                  ],
                  _card(context, 'Device info', [
                    _row('ID', idToString(entry.id)),
                    _row('Net', '${entry.net}'),
                    _row('Device type', entry.type.label),
                    _row('Serial number', entry.serialNumber ?? '-'),
                    _row('Software version', entry.softwareVersion ?? '-'),
                    _row('Uptime', _formatUptime(entry.uptimeMs)),
                    _row('Avg loop time',
                        entry.avgLoopTimeMs?.toStringAsFixed(2) ?? '-'),
                    _row('Max loop time',
                        entry.maxLoopTimeMs?.toStringAsFixed(2) ?? '-'),
                  ]),
                  const SizedBox(height: 4),
                  _row('Capabilities', Capability.describe(entry.capabilities).isEmpty
                      ? '-'
                      : Capability.describe(entry.capabilities).join(', ')),
                  const Divider(height: 24),
                  _card(context, 'Services', [
                    ListTile(
                      dense: true,
                      leading: const Icon(Icons.memory),
                      title: const Text('System Memory'),
                      trailing: const Icon(Icons.chevron_right),
                      onTap: () =>
                          Navigator.of(context).push(MaterialPageRoute(
                              builder: (_) => SystemMemoryPage(
                                  deviceId: widget.deviceId))),
                    ),
                  ]),
                ],
              ),
      ),
    );
  }

  Widget _card(BuildContext context, String title, List<Widget> children) {
    return Card(
      color: kSurfaceAlt,
      child: Padding(
        padding: const EdgeInsets.symmetric(vertical: 8),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 8, 16, 4),
              child: Text(title,
                  style: TextStyle(color: kOrange, fontWeight: FontWeight.w600)),
            ),
            ...children,
          ],
        ),
      ),
    );
  }

  Widget _row(String label, String value) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 3),
      child: Row(children: [
        SizedBox(width: 140, child: Text(label, style: Theme.of(context).textTheme.bodySmall)),
        Expanded(child: Text(value)),
      ]),
    );
  }
}

class RenameField extends StatefulWidget {
  final String currentName;
  final ValueChanged<String?> onDone;

  const RenameField({super.key, required this.currentName, required this.onDone});

  @override
  State<RenameField> createState() => _RenameFieldState();
}

class _RenameFieldState extends State<RenameField> {
  late final TextEditingController _controller =
      TextEditingController(text: widget.currentName);

  @override
  Widget build(BuildContext context) {
    return Row(children: [
      Expanded(
        child: TextField(
          controller: _controller,
          autofocus: true,
          maxLength: 23,
          decoration: const InputDecoration(hintText: 'Device name'),
          onSubmitted: widget.onDone,
        ),
      ),
      IconButton(
          icon: const Icon(Icons.check, color: Colors.greenAccent),
          onPressed: () => widget.onDone(_controller.text.trim())),
      IconButton(
          icon: const Icon(Icons.close, color: Colors.redAccent),
          onPressed: () => widget.onDone(null)),
    ]);
  }
}
