/// Runtime settings store (Docs/App/Settings.md).
///
/// Persisted as a simple JSON file under the user's config directory when the
/// platform provides one; otherwise kept in RAM only (Android/Windows targets
/// are not implemented yet).
library;

import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';

const String appVersion = '1.0.0';
const String appBuildDate =
    String.fromEnvironment('APP_BUILD_DATE', defaultValue: 'unknown');

/// Notification events offered for per-event selection.
const List<String> notificationEvents = [
  'Device discovered',
  'Device lost',
  'Backup finished',
];

class AppSettings extends ChangeNotifier {
  AppSettings._();

  static final AppSettings instance = AppSettings._();

  bool autoConnect = false;
  String autoConnectDeviceId = '';

  /// Notifications while the app is open.
  bool notifyInApp = true;
  final Set<String> inAppEvents = {...notificationEvents};

  /// "If app open do not notify OS" (true/false).
  bool suppressOsWhenOpen = true;

  /// Notifications handed to the operating system.
  bool notifyOs = false;
  final Set<String> osEvents = {...notificationEvents};

  Future<void> load() async {
    final file = _settingsFile();
    if (file == null || !file.existsSync()) return;
    try {
      final data = jsonDecode(file.readAsStringSync()) as Map<String, dynamic>;
      autoConnect = data['autoConnect'] as bool? ?? false;
      autoConnectDeviceId = data['autoConnectDeviceId'] as String? ?? '';
      notifyInApp = data['notifyInApp'] as bool? ?? true;
      inAppEvents
        ..clear()
        ..addAll((data['inAppEvents'] as List<dynamic>? ?? notificationEvents)
            .cast<String>());
      suppressOsWhenOpen = data['suppressOsWhenOpen'] as bool? ?? true;
      notifyOs = data['notifyOs'] as bool? ?? false;
      osEvents
        ..clear()
        ..addAll((data['osEvents'] as List<dynamic>? ?? notificationEvents)
            .cast<String>());
      notifyListeners();
    } catch (_) {
      // Corrupt settings are ignored; defaults stay in effect.
    }
  }

  Future<void> save() async {
    final file = _settingsFile();
    if (file == null) return;
    try {
      file.createSync(recursive: true);
      file.writeAsStringSync(jsonEncode({
        'autoConnect': autoConnect,
        'autoConnectDeviceId': autoConnectDeviceId,
        'notifyInApp': notifyInApp,
        'inAppEvents': inAppEvents.toList(),
        'suppressOsWhenOpen': suppressOsWhenOpen,
        'notifyOs': notifyOs,
        'osEvents': osEvents.toList(),
      }));
    } catch (_) {}
  }

  void update(void Function() change) {
    change();
    notifyListeners();
    save();
  }

  static File? _settingsFile() {
    try {
      final home = Platform.environment['HOME'] ??
          Platform.environment['USERPROFILE'];
      if (home == null || home.isEmpty) return null;
      return File('$home/.config/tamuapp/settings.json');
    } catch (_) {
      return null;
    }
  }
}
