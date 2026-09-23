import 'package:flutter/material.dart';

import 'core/connection.dart';
import 'core/notifications.dart';
import 'core/settings.dart';
import 'ui/backup_page.dart';
import 'ui/connection_page.dart';
import 'ui/devices_page.dart';
import 'ui/settings_page.dart';
import 'ui/theme.dart';
import 'ui/widgets.dart';

void main() async {
  // path_provider (Android settings) and the plugin channels need the binding.
  WidgetsFlutterBinding.ensureInitialized();
  // Load persisted settings (autoconnect target etc.) before the UI starts.
  await AppSettings.instance.load();
  runApp(const TamuApp());
}

class TamuApp extends StatelessWidget {
  const TamuApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      title: 'Tamu',
      theme: buildTheme(),
      themeMode: ThemeMode.dark,
      scaffoldMessengerKey: appMessengerKey,
      home: const ShellPage(),
    );
  }
}

/// Top-level layout (Docs/App/General info.md):
/// Connection, Devices, Backup, Settings.
class ShellPage extends StatefulWidget {
  const ShellPage({super.key});

  @override
  State<ShellPage> createState() => _ShellPageState();
}

class _ShellPageState extends State<ShellPage> {
  int _index = 0;

  static const _pages = [
    ConnectionPage(),
    DevicesPage(),
    BackupPage(),
    SettingsPage(),
  ];

  static const _icons = [
    Icons.link,
    Icons.device_hub,
    Icons.settings_backup_restore,
    Icons.settings,
  ];

  static const _labels = ['Connection', 'Devices', 'Backup', 'Settings'];

  void _select(int i) {
    setState(() => _index = i);
    ShellTabs.instance.update(i);
    shellScaffoldKey.currentState?.closeDrawer();
  }

  Widget _linkBadge({double iconSize = 24, bool withLabel = false}) {
    final connected = ConnectionManager.instance.isConnected;
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        Icon(Icons.developer_board,
            size: iconSize, color: connected ? kOrange : Colors.white38),
        if (withLabel)
          Text(connected ? 'LINK' : 'OFF',
              style: TextStyle(
                  fontSize: 10, color: connected ? kOrange : Colors.white38)),
      ],
    );
  }

  @override
  Widget build(BuildContext context) {
    final compact = ShellLayout.isCompact(context);
    final content = IndexedStack(index: _index, children: _pages);
    return Scaffold(
      key: shellScaffoldKey,
      // Phones get a hamburger drawer; wide screens keep the visible rail.
      drawer: compact ? _buildDrawer() : null,
      body: compact
          ? SafeArea(child: content)
          : Row(
              children: [
                ListenableBuilder(
                  listenable: ConnectionManager.instance,
                  builder: (context, _) => NavigationRail(
                    selectedIndex: _index,
                    onDestinationSelected: (i) {
                      setState(() => _index = i);
                      ShellTabs.instance.update(i);
                    },
                    labelType: NavigationRailLabelType.all,
                    leading: Padding(
                      padding: const EdgeInsets.symmetric(vertical: 12),
                      child: _linkBadge(withLabel: true),
                    ),
                    destinations: const [
                      NavigationRailDestination(
                          icon: Icon(Icons.link), label: Text('Connection')),
                      NavigationRailDestination(
                          icon: Icon(Icons.device_hub),
                          label: Text('Devices')),
                      NavigationRailDestination(
                          icon: Icon(Icons.settings_backup_restore),
                          label: Text('Backup')),
                      NavigationRailDestination(
                          icon: Icon(Icons.settings),
                          label: Text('Settings')),
                    ],
                  ),
                ),
                const VerticalDivider(width: 1),
                Expanded(child: content),
              ],
            ),
    );
  }

  Widget _buildDrawer() {
    return Drawer(
      child: SafeArea(
        child: Column(
          children: [
            ListenableBuilder(
              listenable: ConnectionManager.instance,
              builder: (context, _) => Padding(
                padding: const EdgeInsets.fromLTRB(16, 20, 16, 12),
                child: Row(children: [
                  _linkBadge(iconSize: 30),
                  const SizedBox(width: 12),
                  Text(
                    ConnectionManager.instance.isConnected
                        ? 'Link established'
                        : 'Not connected',
                    style: Theme.of(context).textTheme.titleMedium,
                  ),
                ]),
              ),
            ),
            const Divider(height: 1),
            for (var i = 0; i < _pages.length; i++)
              ListTile(
                leading:
                    Icon(_icons[i], color: _index == i ? kOrange : null),
                title: Text(_labels[i]),
                selected: _index == i,
                onTap: () => _select(i),
              ),
          ],
        ),
      ),
    );
  }
}
