import 'package:flutter/material.dart';

import '../core/settings.dart';
import 'theme.dart';

/// Settings screen (Docs/App/Settings.md).
class SettingsPage extends StatefulWidget {
  const SettingsPage({super.key});

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  final _settings = AppSettings.instance;

  @override
  void initState() {
    super.initState();
    _settings.load();
  }

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: _settings,
      builder: (context, _) => Scaffold(
        appBar: AppBar(title: const Text('Settings')),
        body: ListView(
          children: [
            const SectionHeader('Connection'),
            SwitchListTile(
              title: Text(_settings.autoConnectDeviceId.isEmpty
                  ? 'Autoconnect to specified device'
                  : 'Autoconnect: ${_settings.autoConnectDeviceId}'),
              value: _settings.autoConnect,
              onChanged: (value) => _settings.update(() {
                _settings.autoConnect = value;
              }),
            ),
            ListTile(
              enabled: _settings.autoConnect,
              title: const Text('Autoconnect device'),
              subtitle: Text(_settings.autoConnectDeviceId.isEmpty
                  ? 'Long-press a device on the Connection page to set it'
                  : 'Target: ${_settings.autoConnectDeviceId}'),
              trailing: TextButton(
                onPressed: _settings.autoConnect && _settings.autoConnectDeviceId.isNotEmpty
                    ? () => _settings.update(() {
                          _settings.autoConnect = false;
                          _settings.autoConnectDeviceId = '';
                        })
                    : null,
                child: const Text('Clear'),
              ),
            ),
            const Divider(),
            const SectionHeader('Notifications (app open)'),
            SwitchListTile(
              title: const Text('Allow notifications'),
              value: _settings.notifyInApp,
              onChanged: (value) => _settings.update(() {
                _settings.notifyInApp = value;
              }),
            ),
            if (_settings.notifyInApp)
              for (final event in notificationEvents)
                CheckboxListTile(
                  dense: true,
                  title: Text(event),
                  value: _settings.inAppEvents.contains(event),
                  onChanged: (value) => _settings.update(() {
                    value == true
                        ? _settings.inAppEvents.add(event)
                        : _settings.inAppEvents.remove(event);
                  }),
                ),
            const Divider(),
            const SectionHeader('Operating system notifications'),
            CheckboxListTile(
              title: const Text('If app open do not notify OS'),
              value: _settings.suppressOsWhenOpen,
              onChanged: (value) => _settings.update(() {
                _settings.suppressOsWhenOpen = value ?? true;
              }),
            ),
            SwitchListTile(
              title: const Text('Allow notifications (to OS)'),
              value: _settings.notifyOs,
              onChanged: (value) => _settings.update(() {
                _settings.notifyOs = value;
              }),
            ),
            if (_settings.notifyOs)
              for (final event in notificationEvents)
                CheckboxListTile(
                  dense: true,
                  title: Text(event),
                  value: _settings.osEvents.contains(event),
                  onChanged: (value) => _settings.update(() {
                    value == true
                        ? _settings.osEvents.add(event)
                        : _settings.osEvents.remove(event);
                  }),
                ),
            const Divider(),
            const SectionHeader('About'),
            const ListTile(title: Text('App version'), trailing: Text(appVersion)),
            ListTile(
                title: const Text('App compile date'), trailing: Text(appBuildDate)),
          ],
        ),
      ),
    );
  }
}

class SectionHeader extends StatelessWidget {
  final String text;

  const SectionHeader(this.text, {super.key});

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 16, 16, 4),
      child: Text(text.toUpperCase(),
          style: TextStyle(color: kOrange, fontSize: 12, letterSpacing: 1)),
    );
  }
}
