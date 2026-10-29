import 'dart:async';

import 'package:flutter/material.dart';

import '../core/ble_security.dart'
    show BleLinkSymptom, bleLinkGuidance, classifyBleLinkFailure;
import '../core/connection.dart';
import '../core/device_db.dart';
import '../core/platform_caps.dart';
import '../core/settings.dart';
import 'ble_security_page.dart' show showBleLinkGuidance;
import 'theme.dart';
import 'widgets.dart';

/// Size of the BLE signal icon inside a device row. A declared size, not a
/// spacing step: the row gives the icon this slot, and the gap beside it is
/// [Spacing.xs].
const double _signalIconSize = 18;

/// Connection page (Docs/App/Connection.md).
class ConnectionPage extends StatefulWidget {
  const ConnectionPage({super.key, this.manager});

  /// The connection to drive. Defaults to the app's [ConnectionManager.instance]; a test passes
  /// its own so the pending path can be exercised against a fake transport.
  final ConnectionManager? manager;

  @override
  State<ConnectionPage> createState() => _ConnectionPageState();
}

class _ConnectionPageState extends State<ConnectionPage>
    with WidgetsBindingObserver {
  late final ConnectionManager _manager =
      widget.manager ?? ConnectionManager.instance;
  bool _refreshing = false;
  Duration? _autoInterval = const Duration(seconds: 1); // docs default: on

  /// Cached once: the getter maps a fresh availability stream on every access, which would
  /// make the StreamBuilder resubscribe on every rebuild.
  late final Stream<String?> _bluetoothWarning = _manager.bluetoothWarning;

  /// The link loss this page has already explained, so one drop is not explained twice.
  LinkTrouble? _explainedTrouble;

  /// The attempt outcome this page has already shown, so one failure is not shown twice (the
  /// manager keeps the notice for whoever asks, this page asks once).
  ConnectNotice? _shownNotice;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    ShellTabs.instance.addListener(_onTabChanged);
    _manager.addListener(_onManagerEvent);
    _refreshOnce();
    // Autorefresh is "automatically on" (docs, 1 s period): start the periodic
    // timer now, not only on the first tab switch.
    _onTabChanged();
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    ShellTabs.instance.removeListener(_onTabChanged);
    _manager.removeListener(_onManagerEvent);
    // Leaving the page (app exit) always stops autorefresh.
    if (ShellTabs.instance.index == 0) _manager.setAutoRefresh(false);
    super.dispose();
  }

  /// A link that ended by itself is explained once: a Bluetooth session that came up and died is
  /// almost always the device refusing a pairing, which the user fixes on the phone (see
  /// `bleLinkGuidance`). A link that merely went away gets the plain notice instead.
  ///
  /// A pending attempt that ended without a session is explained from the manager's one
  /// [ConnectNotice], so Cancel, the timeout and a refusal all leave the panel the same way.
  void _onManagerEvent() {
    _showConnectNotice();
    _explainLinkTrouble();
  }

  void _showConnectNotice() {
    final notice = _manager.lastConnectNotice;
    if (notice == null || identical(notice, _shownNotice)) return;
    _shownNotice = notice;
    if (!mounted) return;
    showSnack(context, notice.message);
  }

  void _explainLinkTrouble() {
    final trouble = _manager.lastLinkTrouble;
    if (trouble == null || identical(trouble, _explainedTrouble)) return;
    _explainedTrouble = trouble;
    _manager.clearLinkTrouble();
    if (!mounted || !trouble.isBle) return;
    final symptom = classifyBleLinkFailure(
      errorText: trouble.errorText,
      droppedWhileSecuring: trouble.droppedDuringSecuring,
    );
    if (symptom == BleLinkSymptom.unknown) {
      // The session outlived both securing budgets, so the device is not refusing a pairing it
      // never got to finish: the link itself went away (range, a reset, the user's own settings).
      // A snack is the honest amount of alarm; the two-cause guidance would be a diagnosis the
      // evidence does not support.
      showSnack(context, 'Bluetooth link lost');
      return;
    }
    unawaited(
      _explainBleFailure(trouble.errorText, trouble.droppedDuringSecuring),
    );
  }

  Future<void> _explainBleFailure(
    String? errorText,
    bool droppedWhileSecuring,
  ) async {
    if (!mounted) return;
    await showBleLinkGuidance(
      context,
      bleLinkGuidance(
        errorText: errorText,
        droppedWhileSecuring: droppedWhileSecuring,
      ),
      linkError: errorText,
    );
  }

  /// Coming back from the system settings page (permission fix-up) re-checks.
  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed && _manager.blePermissionBlocked) {
      _manager.retryBlePermissions();
    }
  }

  /// Autorefresh only runs while the Connection tab is visible.
  void _onTabChanged() async {
    final visible = ShellTabs.instance.index == 0;
    await _manager.setAutoRefresh(
      visible && _autoInterval != null,
      interval: _autoInterval,
    );
  }

  bool get _autoTargetIsCurrent =>
      AppSettings.instance.autoConnect &&
      _manager.connectedName != null &&
      (AppSettings.instance.autoConnectDeviceId == _manager.connectedId ||
          AppSettings.instance.autoConnectDeviceId == _manager.connectedName);

  Future<void> _setAutoConnectTarget() async {
    final settings = AppSettings.instance;
    if (_autoTargetIsCurrent) {
      settings.update(() {
        settings.autoConnect = false;
        settings.autoConnectDeviceId = '';
      });
      if (!mounted) return;
      showSnack(context, 'Autoconnect disabled');
      return;
    }
    settings.update(() {
      settings.autoConnect = true;
      settings.autoConnectDeviceId =
          _manager.connectedId ?? _manager.connectedName ?? '';
    });
    if (!mounted) return;
    showSnack(context, 'Autoconnect target set: ${_manager.connectedName}');
  }

  bool _isAutoTarget(DiscoveredLink link) =>
      AppSettings.instance.autoConnect &&
      AppSettings.instance.autoConnectDeviceId == link.id;

  /// The star button on a device row: sets this device as the autoconnect target, or clears it.
  Future<void> _toggleAutoConnectFor(DiscoveredLink link) async {
    final settings = AppSettings.instance;
    final isTarget = _isAutoTarget(link);
    settings.update(() {
      settings.autoConnect = !isTarget;
      settings.autoConnectDeviceId = isTarget ? '' : link.id;
    });
    if (!mounted) return;
    showSnack(
      context,
      isTarget
          ? 'Autoconnect disabled'
          : 'Autoconnect target set: ${link.name}',
    );
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
    setState(() => _autoInterval = interval == Duration.zero ? null : interval);
    _onTabChanged();
  }

  /// Signal strength colour for the RSSI readout. The tokens are the ones that
  /// read as text at 13-14 px (TODO.md's contrast audit: `white38` measured 3.41
  /// and `redAccent` 3.58 on the page), so a weak link is [kError] rather than
  /// Material's `redAccent`.
  Color _rssiColor(int? rssi) {
    if (rssi == null) return kTextMuted;
    if (rssi >= -60) return kSuccess;
    if (rssi >= -75) return kOrange;
    return kError;
  }

  /// Source selector. USB is a desktop-only link (Docs/App/General info.md), so
  /// Android only offers BLE.
  Widget _sourceMenu() {
    return PopupMenuButton<LinkSource>(
      icon: const Icon(Icons.source),
      tooltip: 'Source',
      initialValue: _manager.source,
      onSelected: (source) => _manager.setSource(source),
      itemBuilder: (_) => [
        const PopupMenuItem(value: LinkSource.ble, child: Text('BLE devices')),
        if (supportsUsb)
          const PopupMenuItem(
            value: LinkSource.usb,
            child: Text('USB devices'),
          ),
        if (supportsUsb)
          const PopupMenuItem(
            value: LinkSource.all,
            child: Text('All devices'),
          ),
      ],
    );
  }

  /// A Bluetooth availability / permission banner, or nothing when usable.
  Widget _statusBanner() {
    if (_manager.blePermissionBlocked) {
      return _Banner(
        icon: Icons.bluetooth_disabled,
        text: 'Bluetooth permission denied',
        actionLabel: 'Open settings',
        onAction: () => _manager.openPermissionSettings(),
      );
    }
    return StreamBuilder<String?>(
      stream: _bluetoothWarning,
      builder: (context, snapshot) {
        final warning = snapshot.data;
        if (warning == null) return const SizedBox.shrink();
        return _Banner(icon: Icons.bluetooth_disabled, text: warning);
      },
    );
  }

  @override
  Widget build(BuildContext context) {
    final compact = ShellLayout.isCompact(context);
    return ListenableBuilder(
      listenable: _manager,
      builder: (context, _) {
        final links = _manager.discoveredLinks;
        return Scaffold(
          appBar: AppBar(
            title: const Text('Connection'),
            leading: compact ? const ShellDrawerButton() : _sourceMenu(),
            actions: [
              if (compact && supportsUsb) _sourceMenu(),
              RefreshButton(
                onRefresh: _refreshOnce,
                autoActive: _manager.autoRefresh,
                refreshing: _refreshing,
                error: _manager.refreshError,
                selectedInterval: _manager.autoRefresh ? _autoInterval : null,
                onSelectAuto: _selectAuto,
              ),
              PopupMenuButton<DeviceSort>(
                icon: const Icon(Icons.sort),
                tooltip: 'Sorting',
                initialValue: _manager.sort,
                onSelected: _manager.setSort,
                itemBuilder: (_) => const [
                  PopupMenuItem(
                    value: DeviceSort.signal,
                    child: Text('Signal strength'),
                  ),
                  PopupMenuItem(
                    value: DeviceSort.alphabetical,
                    child: Text('Alphabetical'),
                  ),
                ],
              ),
            ],
          ),
          body: Column(
            children: [
              _statusBanner(),
              if (_manager.isConnecting)
                ListTile(
                  leading: const SizedBox(
                    width: 22,
                    height: 22,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  ),
                  title: Text(
                    'Connecting to ${_manager.connectingTarget ?? ''}',
                  ),
                  subtitle: const Text('Establishing session'),
                  // The pending state's explicit way out: it abandons the attempt (see
                  // ConnectionManager.cancelConnect), and the same row is what the 10 s
                  // timeout takes away when nobody answers.
                  trailing: TextButton.icon(
                    onPressed: () => unawaited(_manager.cancelConnect()),
                    icon: const Icon(Icons.close, size: 18),
                    label: const Text('Cancel'),
                  ),
                ),
              if (_manager.isConnected)
                ListTile(
                  leading: const Icon(Icons.link, color: kOrange),
                  // The device's REPORTED name (from the Register service), with
                  // the link as the subtitle - not two copies of the same text.
                  title: Text(
                    DeviceDatabase.instance.byId(coreId)?.displayName ??
                        _manager.connectedName ??
                        '',
                  ),
                  subtitle: Text(_manager.connectedName ?? 'Connected'),
                  trailing: Row(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      IconButton(
                        icon: Icon(
                          _autoTargetIsCurrent
                              ? Icons.autorenew
                              : Icons.autorenew_outlined,
                          color: _autoTargetIsCurrent
                              ? Colors.greenAccent
                              : null,
                        ),
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
                    ],
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

  /// One discovered link as a [DeviceTile]: the connection type drives the icon,
  /// the link's own id is the subtext (a MAC for BLE), and the trailing content
  /// is the BLE signal readout plus the autoconnect star.
  Widget _buildTile(BuildContext context, DiscoveredLink link) {
    final isBle = link.type == LinkType.ble;
    final rssi = link.rssi;
    return DeviceTile(
      icon: isBle ? Icons.bluetooth : Icons.usb,
      name: link.name,
      subtitle: isBle ? 'MAC ${link.id}' : link.id,
      trailing: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          // BLE only: a USB link reports no signal strength.
          if (isBle && rssi != null) ...[
            Icon(
              Icons.signal_cellular_alt,
              size: _signalIconSize,
              color: _rssiColor(rssi),
            ),
            const SizedBox(width: Spacing.xs),
            Text('$rssi dBm'),
          ],
          IconButton(
            icon: Icon(_isAutoTarget(link) ? Icons.star : Icons.star_border),
            tooltip: 'Set as autoconnect target',
            onPressed: () => _toggleAutoConnectFor(link),
          ),
        ],
      ),
      onTap: () async {
        if (_manager.isConnecting) return;
        // While connected, tapping a (different) device switches the session:
        // disconnect the current one, then connect to this one.
        if (_manager.isConnected) {
          await _manager.disconnect(manual: true);
        }
        final error = await _manager.connectTo(link);
        if (!context.mounted) return;
        // How the attempt ended - cancelled, timed out, refused - is the manager's
        // ConnectNotice, shown once by _showConnectNotice. Only a refusal that never became an
        // attempt (already connected/connecting, USB on a phone) has no notice to show.
        if (error != null && _manager.lastConnectNotice == null) {
          showSnack(context, error);
          return;
        }
        if (error == null) {
          // Populate the Devices page as soon as a session is up.
          unawaited(DeviceDatabase.instance.refreshNetwork());
        }
      },
    );
  }
}

/// A compact full-width warning strip used for Bluetooth/permission problems.
class _Banner extends StatelessWidget {
  const _Banner({
    required this.icon,
    required this.text,
    this.actionLabel,
    this.onAction,
  });

  final IconData icon;
  final String text;
  final String? actionLabel;
  final VoidCallback? onAction;

  @override
  Widget build(BuildContext context) {
    return Container(
      width: double.infinity,
      color: kOrange.withAlpha(38),
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
      child: Row(
        children: [
          Icon(icon, size: 18, color: kOrange),
          const SizedBox(width: Spacing.md),
          Expanded(child: Text(text)),
          if (actionLabel != null && onAction != null)
            TextButton(onPressed: onAction, child: Text(actionLabel!)),
        ],
      ),
    );
  }
}
