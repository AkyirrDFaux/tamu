import 'dart:async';

import 'package:flutter/material.dart';

import '../core/connection.dart';
import '../core/device_db.dart';
import '../core/settings.dart';
import 'theme.dart';
import 'widgets.dart';

/// Connection page (Docs/App/Connection.md).
class ConnectionPage extends StatefulWidget {
  const ConnectionPage({super.key});

  @override
  State<ConnectionPage> createState() => _ConnectionPageState();
}

class _ConnectionPageState extends State<ConnectionPage> {
  final _manager = ConnectionManager.instance;
  bool _refreshing = false;
  Duration? _autoInterval = const Duration(seconds: 1); // docs default: on

  @override
  void initState() {
    super.initState();
    ShellTabs.instance.addListener(_onTabChanged);
    _refreshOnce();
    // Autorefresh is "automatically on" (docs, 1 s period): start the periodic
    // timer now, not only on the first tab switch.
    _onTabChanged();
  }

  @override
  void dispose() {
    ShellTabs.instance.removeListener(_onTabChanged);
    // Leaving the page (app exit) always stops autorefresh.
    if (ShellTabs.instance.index == 0) _manager.setAutoRefresh(false);
    super.dispose();
  }

  /// Autorefresh only runs while the Connection tab is visible.
  void _onTabChanged() async {
    final visible = ShellTabs.instance.index == 0;
    await _manager.setAutoRefresh(visible && _autoInterval != null,
        interval: _autoInterval);
  }

  bool get _autoTargetIsCurrent =>
      AppSettings.instance.autoConnect &&
      _manager.connectedName != null &&
      (AppSettings.instance.autoConnectDeviceId == _manager.connectedId ||
       AppSettings.instance.autoConnectDeviceId == _manager.connectedName);

  Future<void> _setAutoConnectTarget() async {
    final settings = AppSettings.instance;
    await settings.load();
    if (_autoTargetIsCurrent) {
      settings.update(() {
        settings.autoConnect = false;
        settings.autoConnectDeviceId = '';
      });
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('Autoconnect disabled')));
      return;
    }
    settings.update(() {
      settings.autoConnect = true;
      settings.autoConnectDeviceId =
          _manager.connectedId ?? _manager.connectedName ?? '';
    });
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text('Autoconnect target set: '
            '${_manager.connectedName}')));
  }

  Future<void> _setAutoConnectFor(DiscoveredLink link) async {
    final settings = AppSettings.instance;
    await settings.load();
    settings.update(() {
      settings.autoConnect = true;
      settings.autoConnectDeviceId = link.id;
    });
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text('Autoconnect target set: ${link.name}')));
  }

  Future<void> _refreshOnce() async {
    if (_refreshing) return;
    setState(() => _refreshing = true);
    try {
      // While connected the manager pauses scanning; a manual refresh then means
      // re-pulling the live network instead of re-scanning the air.
      if (_manager.isConnected) {
        await DeviceDatabase.instance.refreshNetwork();
      } else {
        await _manager.refresh();
      }
    } finally {
      if (mounted) setState(() => _refreshing = false);
    }
  }

  void _selectAuto(Duration? interval) {
    setState(() =>
        _autoInterval = interval == Duration.zero ? null : interval);
    _onTabChanged();
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
              RefreshButton(
                onRefresh: _refreshOnce,
                autoActive: _manager.autoRefresh,
                refreshing: _refreshing,
                error: _manager.refreshError,
                selectedInterval:
                    _manager.autoRefresh ? _autoInterval : null,
                onSelectAuto: _selectAuto,
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
                  trailing: IconButton(
                    icon: const Icon(Icons.close),
                    tooltip: 'Cancel',
                    onPressed: () =>
                        unawaited(_manager.disconnect(manual: true)),
                  ),
                ),
              if (_manager.isConnected)
                ListTile(
                  leading: const Icon(Icons.link, color: kOrange),
                  // The device's REPORTED name (from the Device service), with
                  // the link as the subtitle - not two copies of the same text.
                  title: Text(DeviceDatabase.instance.byId(coreId)?.name.isNotEmpty ==
                              true
                          ? DeviceDatabase.instance.byId(coreId)!.name
                          : _manager.connectedName ??
                              ''),
                  subtitle: Text(_manager.connectedName ?? 'Connected'),
                  trailing: Row(mainAxisSize: MainAxisSize.min, children: [
                    IconButton(
                      icon: Icon(
                          _autoTargetIsCurrent
                              ? Icons.autorenew
                              : Icons.autorenew_outlined,
                          color: _autoTargetIsCurrent
                              ? Colors.greenAccent
                              : null),
                      tooltip: _autoTargetIsCurrent
                          ? 'Autoconnect enabled for this device'
                          : 'Autoconnect to this device from now on',
                      onPressed: () => _setAutoConnectTarget(),
                    ),
                    IconButton(
                      icon: const Icon(Icons.link_off),
                      tooltip: 'Disconnect',
                      onPressed: () =>
                          unawaited(_manager.disconnect(manual: true)),
                    ),
                  ]),
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
        if (_manager.isConnecting) return;
        // While connected, tapping a (different) device switches the session:
        // disconnect the current one, then connect to this one.
        if (_manager.isConnected) {
          await _manager.disconnect(manual: true);
        }
        final error = await _manager.connectTo(link);
        if (!context.mounted) return;
        if (error != null) {
          ScaffoldMessenger.of(context).showSnackBar(
              SnackBar(content: Text('Connect failed: $error')));
          return;
        }
        // Populate the Devices page as soon as a session is up.
        unawaited(DeviceDatabase.instance.refreshNetwork());
      },
      // Long-press picks this device as the autoconnect target (Settings page
      // documents "Long-press a device on the Connection page to set it").
      onLongPress: () => _setAutoConnectFor(link),
    );
  }
}
