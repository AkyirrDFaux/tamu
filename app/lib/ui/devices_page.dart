import 'package:flutter/material.dart';

import '../core/connection.dart';
import '../core/device_db.dart';
import '../core/types.dart';
import 'device_icons.dart';
import 'device_view_page.dart';
import 'theme.dart';

/// Devices page (Docs/App/Devices.md): main interaction layer with list and
/// graph views.
class DevicesPage extends StatefulWidget {
  const DevicesPage({super.key});

  @override
  State<DevicesPage> createState() => _DevicesPageState();
}

enum _ViewMode { list, graph }

class _DevicesPageState extends State<DevicesPage> {
  final _db = DeviceDatabase.instance;
  _ViewMode _mode = _ViewMode.list;

  // Filters (Docs/App/Devices.md bottom of screen).
  int? _netFilter;
  DeviceType? _typeFilter;
  _DeviceSort _sort = _DeviceSort.id;

  List<DeviceEntry> get _filtered {
    var devices = _db.all.where((d) {
      if (_netFilter != null && d.net != _netFilter) return false;
      if (_typeFilter != null && d.type != _typeFilter) return false;
      return true;
    }).toList();
    switch (_sort) {
      case _DeviceSort.id:
        devices.sort((a, b) => a.id.compareTo(b.id));
      case _DeviceSort.name:
        devices.sort((a, b) => a.displayName.toLowerCase().compareTo(b.displayName.toLowerCase()));
      case _DeviceSort.deviceType:
        devices.sort((a, b) => a.type.value.compareTo(b.type.value));
    }
    return devices;
  }

  @override
  void initState() {
    super.initState();
    _refresh();
  }

  Future<void> _refresh() async {
    await _db.refreshNetwork();
  }

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: _db,
      builder: (context, _) => Scaffold(
        appBar: AppBar(
          title: const Text('Devices'),
          leading: PopupMenuButton<_ViewMode>(
            icon: Icon(_mode == _ViewMode.list ? Icons.view_list : Icons.account_tree),
            tooltip: 'View',
            initialValue: _mode,
            onSelected: (mode) => setState(() => _mode = mode),
            itemBuilder: (_) => const [
              PopupMenuItem(value: _ViewMode.list, child: Text('List view')),
              PopupMenuItem(value: _ViewMode.graph, child: Text('Graph view')),
            ],
          ),
          actions: [
            IconButton(
              icon: _db.isRefreshing
                  ? const SizedBox(
                      width: 18,
                      height: 18,
                      child: CircularProgressIndicator(strokeWidth: 2))
                  : const Icon(Icons.refresh),
              onPressed: _db.isRefreshing ? null : _refresh,
            ),
          ],
        ),
        body: Column(
          children: [
            Expanded(child: _buildBody()),
            const Divider(height: 1),
            _buildFilterBar(),
          ],
        ),
      ),
    );
  }

  Widget _buildBody() {
    if (!ConnectionManager.instance.isConnected) {
      return const Center(child: Text('Not connected'));
    }
    final devices = _filtered;
    if (devices.isEmpty) {
      return Center(child: Text(_db.lastError ?? 'No devices discovered yet'));
    }
    return AnimatedSwitcher(
      duration: const Duration(milliseconds: 150),
      child: _mode == _ViewMode.list
          ? ListView.builder(
              key: const ValueKey('list'),
              itemCount: devices.length,
              itemBuilder: (context, index) =>
                  _deviceTile(context, devices[index]),
            )
          : GraphView(key: const ValueKey('graph'), devices: devices),
    );
  }

  Widget _deviceTile(BuildContext context, DeviceEntry device) {
    return ListTile(
      leading: Icon(deviceTypeIcon(device.type)),
      title: Text(device.displayName + (device.stale ? ' (stale)' : '')),
      subtitle: Text('${idToString(device.id)} - ${device.type.label}'),
      trailing: device.isCore ? const Icon(Icons.star, color: kOrange, size: 18) : null,
      onTap: () => Navigator.of(context).push(MaterialPageRoute(
          builder: (_) => DeviceViewPage(deviceId: device.id))),
    );
  }

  Widget _buildFilterBar() {
    final nets = _db.all.map((d) => d.net).toSet().toList()..sort();
    final types = _db.all.map((d) => d.type).toSet().toList();
    return SizedBox(
      height: 56,
      child: Row(
        children: [
          FilterChip(
            label: Text(_netFilter == null ? 'Net: all' : 'Net $_netFilter'),
            selected: _netFilter != null,
            onSelected: (_) => _cycleNet(nets),
          ),
          const SizedBox(width: 8),
          FilterChip(
            label: Text(_typeFilter?.label ?? 'Type: all'),
            selected: _typeFilter != null,
            onSelected: (_) => _cycleType(types),
          ),
          const Spacer(),
          PopupMenuButton<_DeviceSort>(
            tooltip: 'Sorting',
            initialValue: _sort,
            onSelected: (s) => setState(() => _sort = s),
            itemBuilder: (_) => const [
              PopupMenuItem(value: _DeviceSort.id, child: Text('Sort: ID')),
              PopupMenuItem(value: _DeviceSort.name, child: Text('Sort: Name')),
              PopupMenuItem(
                  value: _DeviceSort.deviceType, child: Text('Sort: Type')),
            ],
          ),
        ],
      ),
    );
  }

  void _cycleNet(List<int> nets) {
    setState(() {
      if (_netFilter == null) {
        _netFilter = nets.isNotEmpty ? nets.first : null;
      } else {
        final index = nets.indexOf(_netFilter!);
        _netFilter = index >= 0 && index < nets.length - 1 ? nets[index + 1] : null;
      }
    });
  }

  void _cycleType(List<DeviceType> types) {
    setState(() {
      if (_typeFilter == null) {
        _typeFilter = types.isNotEmpty ? types.first : null;
      } else {
        final index =
            types.indexWhere((t) => t.value == _typeFilter!.value);
        _typeFilter = index >= 0 && index < types.length - 1
            ? types[index + 1]
            : null;
      }
    });
  }
}

enum _DeviceSort { id, name, deviceType }

/// Pannable tree view (Docs/App/Devices.md): cores topmost, then routers
/// (none implemented yet), then nodes stacked vertically under their parent.
class GraphView extends StatelessWidget {
  final List<DeviceEntry> devices;

  const GraphView({super.key, required this.devices});

  @override
  Widget build(BuildContext context) {
    final cores = devices.where((d) => d.isCore).toList();
    final nodes = devices.where((d) => !d.isCore).toList();

    return InteractiveViewer(
      constrained: false,
      maxScale: 4,
      child: Padding(
        padding: const EdgeInsets.all(24),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.center,
          children: [
            if (cores.isEmpty)
              const Text('No core discovered')
            else
              ...[for (final core in cores) _deviceBlock(context, core)],
            if (nodes.isNotEmpty) ...[
              const SizedBox(height: 32),
              for (var i = 0; i < nodes.length; i += 3)
                Row(
                  mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                  children: [
                    for (var j = i; j < i + 3 && j < nodes.length; j++)
                      _deviceBlock(context, nodes[j]),
                  ],
                ),
            ],
          ],
        ),
      ),
    );
  }

  Widget _deviceBlock(BuildContext context, DeviceEntry device) {
    return Card(
      color: kSurfaceAlt,
      child: InkWell(
        borderRadius: BorderRadius.circular(12),
        onTap: () => Navigator.of(context).push(MaterialPageRoute(
            builder: (_) => DeviceViewPage(deviceId: device.id))),
        child: Container(
          width: 160,
          padding: const EdgeInsets.all(12),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              Icon(deviceTypeIcon(device.type), color: kOrange),
              const SizedBox(height: 6),
              Text(device.displayName,
                  maxLines: 1, overflow: TextOverflow.ellipsis),
              Text('${idToString(device.id)} - ${device.type.label}',
                  style: Theme.of(context).textTheme.bodySmall),
            ],
          ),
        ),
      ),
    );
  }
}
