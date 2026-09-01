import 'package:flutter/material.dart';

import 'core/connection.dart';
import 'core/notifications.dart';
import 'core/settings.dart';
import 'ui/backup_page.dart';
import 'ui/bootloader_page.dart';
import 'ui/connection_page.dart';
import 'ui/devices_page.dart';
import 'ui/settings_page.dart';
import 'ui/theme.dart';
import 'ui/widgets.dart';

void main() {
  // Load persisted settings (autoconnect target etc.) before the UI starts.
  AppSettings.instance.load();
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
    BootloaderPage(),
    SettingsPage(),
  ];

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Row(
        children: [
          ListenableBuilder(
            listenable: ConnectionManager.instance,
            builder: (context, _) {
              final connected = ConnectionManager.instance.isConnected;
              return NavigationRail(
                selectedIndex: _index,
                onDestinationSelected: (i) {
                  setState(() => _index = i);
                  ShellTabs.instance.update(i);
                },
                labelType: NavigationRailLabelType.all,
                leading: Padding(
                  padding: const EdgeInsets.symmetric(vertical: 12),
                  child: Column(
                    children: [
                      Icon(Icons.developer_board,
                          color: connected ? kOrange : Colors.white38),
                      Text(connected ? 'LINK' : 'OFF',
                          style: TextStyle(
                              fontSize: 10,
                              color: connected ? kOrange : Colors.white38)),
                    ],
                  ),
                ),
                destinations: const [
                  NavigationRailDestination(
                      icon: Icon(Icons.link), label: Text('Connection')),
                  NavigationRailDestination(
                      icon: Icon(Icons.device_hub), label: Text('Devices')),
                  NavigationRailDestination(
                      icon: Icon(Icons.settings_backup_restore),
                      label: Text('Backup')),
                  NavigationRailDestination(
                      icon: Icon(Icons.system_update),
                      label: Text('Bootloader')),
                  NavigationRailDestination(
                      icon: Icon(Icons.settings), label: Text('Settings')),
                ],
              );
            },
          ),
          const VerticalDivider(width: 1),
          Expanded(child: IndexedStack(index: _index, children: _pages)),
        ],
      ),
    );
  }
}
