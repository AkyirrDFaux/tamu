import 'package:flutter/material.dart';

import '../core/connection.dart';
import '../core/device_db.dart';
import 'theme.dart';

/// Connection page (Docs/App/Connection.md).
class ConnectionPage extends StatefulWidget {
  const ConnectionPage({super.key});

  @override
  State<ConnectionPage> createState() => _ConnectionPageState();
}

class _ConnectionPageState extends State<ConnectionPage> {
  final _manager = ConnectionManager.instance;

  @override
  void initState() {
    super.initState();
    _manager.refresh();
  }

  Color _rssiColor(int? rssi) {
    if (rssi == null) return Colors.white38;
    if (rssi >= -60) return Colors.greenAccent;
    if (rssi >= -75) return kOrange;
    return Colors.redAccent;
  }

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: _manager,
      builder: (context, _) {
        final links = _manager.discoveredLinks;
        return Scaffold(
          appBar: AppBar(
            title: const Text('Connection'),
            leading: PopupMenuButton<LinkSource>(
              icon: const Icon(Icons.source),
              tooltip: 'Source',
              initialValue: _manager.source,
              onSelected: (source) => _manager.setSource(source),
              itemBuilder: (_) => const [
                PopupMenuItem(value: LinkSource.ble, child: Text('BLE devices')),
                PopupMenuItem(value: LinkSource.usb, child: Text('USB devices')),
                PopupMenuItem(value: LinkSource.all, child: Text('All devices')),
              ],
            ),
            actions: [
              IconButton(
                icon: Stack(
                  clipBehavior: Clip.none,
                  children: [
                    const Icon(Icons.refresh),
                    Positioned(
                      right: -2,
                      bottom: -2,
                      child: Icon(
                        Icons.circle,
                        size: 8,
                        color: _manager.refreshError
                            ? Colors.red
                            : (_manager.isRefreshing
                                ? Colors.greenAccent
                                : Colors.transparent),
                      ),
                    ),
                  ],
                ),
                tooltip: _manager.autoRefresh
                    ? 'Autorefresh active (tap to stop)'
                    : 'Refresh (hold for autorefresh)',
                onPressed: () async {
                  await _manager.setAutoRefresh(!_manager.autoRefresh);
                  if (!_manager.autoRefresh) await _manager.refresh();
                },
                onLongPress: () => _manager.setAutoRefresh(true),
              ),
              PopupMenuButton<DeviceSort>(
                icon: const Icon(Icons.sort),
                tooltip: 'Sorting',
                initialValue: _manager.sort,
                onSelected: _manager.setSort,
                itemBuilder: (_) => const [
                  PopupMenuItem(
                      value: DeviceSort.signal, child: Text('Signal strength')),
                  PopupMenuItem(
                      value: DeviceSort.alphabetical,
                      child: Text('Alphabetical')),
                ],
              ),
            ],
          ),
          body: Column(
            children: [
              if (_manager.isConnecting)
                ListTile(
                  leading: const SizedBox(
                      width: 22,
                      height: 22,
                      child: CircularProgressIndicator(strokeWidth: 2)),
                  title: Text('Connecting to ${_manager.connectingTarget ?? ''}'),
                  subtitle: const Text('Establishing session...'),
                ),
              if (_manager.isConnected)
                ListTile(
                  leading: const Icon(Icons.link, color: kOrange),
                  title: Text(DeviceDatabase.instance.byId(1)?.displayName ??
                      _manager.connectedName ??
                      ''),
                  subtitle: Text(_manager.connectedName ?? 'Connected'),
                  trailing: IconButton(
                    icon: const Icon(Icons.link_off),
                    tooltip: 'Disconnect',
                    onPressed: () => _manager.disconnect(),
                  ),
                ),
              const Divider(height: 1),
              Expanded(
                child: links.isEmpty
                    ? const Center(child: Text('No devices found'))
                    : ListView.builder(
                        itemCount: links.length,
                        itemBuilder: (context, index) =>
                            _buildTile(context, links[index]),
                      ),
              ),
            ],
          ),
        );
      },
    );
  }

  Widget _buildTile(BuildContext context, DiscoveredLink link) {
    final isBle = link.type == LinkType.ble;
    return ListTile(
      leading: Icon(isBle ? Icons.bluetooth : Icons.usb),
      title: Text(link.name),
      subtitle: Text(isBle ? 'MAC ${link.id}' : link.id),
      trailing: isBle && link.rssi != null
          ? Row(mainAxisSize: MainAxisSize.min, children: [
              Icon(Icons.signal_cellular_alt,
                  size: 18, color: _rssiColor(link.rssi)),
              const SizedBox(width: 4),
              Text('${link.rssi} dBm'),
            ])
          : null,
      onTap: () async {
        if (_manager.isConnecting || _manager.isConnected) return;
        final error = await _manager.connectTo(link);
        if (!context.mounted) return;
        if (error != null) {
          ScaffoldMessenger.of(context).showSnackBar(
              SnackBar(content: Text('Connect failed: $error')));
        }
      },
    );
  }
}
