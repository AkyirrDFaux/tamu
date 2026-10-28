import 'package:flutter/material.dart';

import '../core/ble_security.dart' show hasBleSettingsBlock;
import '../core/device_db.dart';
import '../core/register_client.dart';
import '../core/types.dart';
import '../core/wifi_settings.dart' show hasWifiSettingsBlock;
import 'ble_security_page.dart';
import 'formatters.dart';
import 'log_page.dart';
import 'register_page.dart';
import 'scripts_page.dart';
import 'sndb_page.dart';
import 'storage_page.dart';
import 'subscriptions_page.dart';
import 'theme.dart';
import 'viewer_kit.dart';
import 'widgets.dart';
import 'wifi_page.dart';

/// Declared size of the rename mark in the app-bar title row: an affordance, not
/// text, so it keeps this slot whatever the text scale. The gap before it is
/// [Spacing.sm].
const double _editIconSize = 16;

/// Device view (Docs/App/Device view.md): known facts about one device.
class DeviceViewPage extends StatefulWidget {
  final int deviceId;

  const DeviceViewPage({super.key, required this.deviceId});

  @override
  State<DeviceViewPage> createState() => _DeviceViewPageState();
}

class _DeviceViewPageState extends State<DeviceViewPage>
    with AutoRefreshMixin<DeviceViewPage> {
  final _db = DeviceDatabase.instance;
  bool _renaming = false;
  bool _refreshing = false;

  /// The device's own block list, asked once per page: only the Bluetooth and WiFi tiles are
  /// gated on it (WiFi's block is a compile-in option, and a non-core device carries neither).
  /// The client caches the enumeration, so an autorefresh does not re-ask.
  late final RegisterClient _register = RegisterClient(
    deviceId: widget.deviceId,
  );
  bool _hasWifiBlock = false;
  bool _hasBleBlock = false;

  @override
  void initState() {
    super.initState();
    _refresh();
  }

  @override
  Future<void> onAutoRefresh() => _refresh();

  @override
  void onAutoRefreshStarted() {
    _refresh();
  }

  Future<void> _refresh() async {
    if (_refreshing) return;
    setState(() => _refreshing = true);
    try {
      await _db.refreshDevice(widget.deviceId);
      await _db.refreshRuntime(widget.deviceId);
      final types = await _register.enumerateBlockTypes();
      // A null answer means the device did not say: keep what was known instead of dropping the
      // tile off a page that is merely mid-reconnect.
      if (types != null) {
        _hasWifiBlock = hasWifiSettingsBlock(types);
        _hasBleBlock = hasBleSettingsBlock(types);
      }
    } finally {
      if (mounted) setState(() => _refreshing = false);
    }
  }

  String _formatUptime(int? ms) => ms == null ? '-' : formatUptimeMs(ms);

  @override
  Widget build(BuildContext context) {
    final entry = _db.byId(widget.deviceId);
    return ListenableBuilder(
      listenable: _db,
      builder: (context, _) => Scaffold(
        appBar: AppBar(
          // The name takes the width the bar has and ellipsizes; the edit mark
          // beside it keeps its own slot, so a 16-byte (the docs' limit) device
          // name cannot push the title row past the bar.
          title: DeviceTitle(
            name: entry?.displayName ?? 'Device',
            onTap: () => setState(() => _renaming = true),
          ),
          actions: [
            IconButton(
              tooltip: 'Identify device (blink its LED)',
              icon: const Icon(Icons.visibility_outlined),
              onPressed: () async {
                final ok = await _db.identify(widget.deviceId);
                if (!ok && context.mounted) {
                  showSnack(context, 'Identify request failed');
                }
              },
            ),
            RefreshButton(
              onRefresh: _refresh,
              autoActive: autoRefreshActive,
              refreshing: _refreshing,
              error: false,
              selectedInterval: selectedInterval,
              onSelectAuto: applyAuto,
            ),
          ],
        ),
        body: entry == null
            ? const Center(child: CircularProgressIndicator())
            : ListView(
                padding: Insets.page,
                children: [
                  if (_renaming) ...[
                    RenameField(
                      currentName:
                          entry.name, // rename flow edits the REPORTED name
                      onDone: (name) async {
                        setState(() => _renaming = false);
                        if (name != null && name.isNotEmpty) {
                          await _db.setName(widget.deviceId, name);
                        }
                      },
                    ),
                    const SizedBox(height: Spacing.lg),
                  ],
                  // The connection badge: which app interfaces THIS DEVICE reports live, read
                  // from the System block's bit field (USB/BLE/WiFi can be live together). The
                  // shell's own badge says whether this app has a link at all; this one is the
                  // device's answer.
                  Padding(
                    padding: const EdgeInsets.only(bottom: Spacing.md),
                    child: Wrap(
                      spacing: Spacing.sm,
                      runSpacing: Spacing.xs,
                      crossAxisAlignment: WrapCrossAlignment.center,
                      children: [
                        const SectionLabel('App link'),
                        AppInterfaceBadges(entry.appActiveBits),
                      ],
                    ),
                  ),
                  InfoCard(
                    title: 'Device info',
                    children: [
                      InfoRow('ID', idToString(entry.id)),
                      InfoRow('Net', '${entry.net}'),
                      InfoRow('Device type', entry.type.label),
                      InfoRow('Serial number', entry.serialNumber ?? '-'),
                      InfoRow('Software version', entry.softwareVersion ?? '-'),
                      InfoRow('Uptime', _formatUptime(entry.uptimeMs)),
                      InfoRow(
                        'Avg loop time',
                        entry.avgLoopTimeMs?.toStringAsFixed(2) ?? '-',
                      ),
                      InfoRow(
                        'Max loop time',
                        entry.maxLoopTimeMs?.toStringAsFixed(2) ?? '-',
                      ),
                      InfoRow(
                        'Time offset',
                        entry.timeOffsetMs == null
                            ? '-'
                            : formatOffsetMs(entry.timeOffsetMs!),
                      ),
                      InfoRow(
                        'Capabilities',
                        Capability.describe(entry.capabilities).isEmpty
                            ? '-'
                            : Capability.describe(
                                entry.capabilities,
                              ).join(', '),
                      ),
                    ],
                  ),
                  const Divider(height: Spacing.xxl),
                  InfoCard(
                    title: 'Services',
                    children: [
                      // Per Docs/App/Device view.md: hide unavailable services
                      // based on the device's capability field. Register (covers the
                      // System block, static blocks and dynamic/keyed memory), Storage
                      // and Logs are mandatory; SNDB is core-only.
                      // Router table awaits its firmware service.
                      _serviceTile(
                        context,
                        Icons.table_chart,
                        'Register',
                        () => RegisterPage(
                          deviceId: widget.deviceId,
                          hasDynamicMemory:
                              entry.capabilities & Capability.dynamicMemory !=
                              0,
                        ),
                      ),
                      _serviceTile(
                        context,
                        Icons.save_outlined,
                        'Storage',
                        () => StoragePage(deviceId: widget.deviceId),
                      ),
                      if (entry.capabilities &
                              (Capability.subscriptionRequest |
                                  Capability.subscriptionProvide) !=
                          0)
                        _serviceTile(
                          context,
                          Icons.sync_alt,
                          'Subscriptions',
                          () => SubscriptionsPage(
                            deviceId: widget.deviceId,
                            deviceName: entry.displayName,
                          ),
                        ),
                      if (entry.capabilities & Capability.scripts != 0)
                        _serviceTile(
                          context,
                          Icons.code,
                          'Scripts',
                          () => ScriptsPage(
                            deviceId: widget.deviceId,
                            deviceName: entry.displayName,
                          ),
                        ),
                      // The Bluetooth panel: the BLE Settings block (0x0B). Like WiFi it is
                      // gated on the device's own enumeration - a non-core device carries no
                      // such block, and a tile for one would open an empty page.
                      if (_hasBleBlock)
                        _serviceTile(
                          context,
                          Icons.bluetooth,
                          'Bluetooth',
                          () => BleSecurityPage(deviceId: widget.deviceId),
                        ),
                      // WiFi Settings is a compile-in firmware option, off by default: the
                      // tile exists only for a device that actually enumerates the block, so a
                      // stock core shows no WiFi entry (and no empty panel behind one).
                      if (_hasWifiBlock)
                        _serviceTile(
                          context,
                          Icons.wifi,
                          'WiFi',
                          () => WifiPage(deviceId: widget.deviceId),
                        ),
                      if (entry.isCore) ...[
                        _serviceTile(
                          context,
                          Icons.format_list_numbered,
                          'SN Database',
                          () => const SndbPage(),
                        ),
                        // The log DATABASE lives on cores only (Docs/Services/
                        // Log Handler.md); non-core views get no logs entry.
                        _serviceTile(
                          context,
                          Icons.article_outlined,
                          'Logs',
                          () => LogViewerPage(deviceId: widget.deviceId),
                        ),
                      ],
                    ],
                  ),
                ],
              ),
      ),
    );
  }

  Widget _serviceTile(
    BuildContext context,
    IconData icon,
    String title,
    Widget Function() page,
  ) {
    return ListTile(
      dense: true,
      leading: Icon(icon),
      title: Text(title),
      trailing: const Icon(Icons.chevron_right),
      onTap: () =>
          Navigator.of(context).push(MaterialPageRoute(builder: (_) => page())),
    );
  }
}

/// The Device view's app-bar title (Docs/App/Device view.md): the device's
/// reported name with the rename mark beside it, the whole thing tappable.
///
/// The name is the part that gives way: it takes whatever width the bar has left
/// and ellipsizes, and the mark keeps its [_editIconSize] slot, so a long name
/// ellipsizes instead of overflowing the title row.
class DeviceTitle extends StatelessWidget {
  const DeviceTitle({super.key, required this.name, this.onTap});

  final String name;
  final VoidCallback? onTap;

  @override
  Widget build(BuildContext context) {
    return InkWell(
      onTap: onTap,
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Flexible(
            child: Text(name, maxLines: 1, overflow: TextOverflow.ellipsis),
          ),
          const SizedBox(width: Spacing.sm),
          const Icon(Icons.edit, size: _editIconSize),
        ],
      ),
    );
  }
}

class RenameField extends StatefulWidget {
  final String currentName;
  final ValueChanged<String?> onDone;

  const RenameField({
    super.key,
    required this.currentName,
    required this.onDone,
  });

  @override
  State<RenameField> createState() => _RenameFieldState();
}

class _RenameFieldState extends State<RenameField> {
  late final TextEditingController _controller = TextEditingController(
    text: widget.currentName,
  );

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Expanded(
          child: TextField(
            controller: _controller,
            autofocus: true,
            maxLength: 16, // docs: the System Name field is 16 bytes
            decoration: const InputDecoration(hintText: 'Device name'),
            onSubmitted: widget.onDone,
          ),
        ),
        IconButton(
          icon: const Icon(Icons.check, color: kSuccess),
          onPressed: () => widget.onDone(_controller.text.trim()),
        ),
        IconButton(
          icon: const Icon(Icons.close, color: kError),
          onPressed: () => widget.onDone(null),
        ),
      ],
    );
  }
}
