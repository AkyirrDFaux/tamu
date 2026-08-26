/// In-app notifications (Docs/App/Settings.md): "Allow notifications (When app
/// open) - per event selection". A global messenger key lets any service show a
/// SnackBar without a BuildContext; `notifyAppEvent` honours the settings.
library;

import 'package:flutter/material.dart';

import 'settings.dart';

/// Global messenger key wired into `MaterialApp` so non-widget code (device
/// discovery, backup completion) can surface a SnackBar.
final GlobalKey<ScaffoldMessengerState> appMessengerKey =
    GlobalKey<ScaffoldMessengerState>();

/// Shows an in-app notification for [event] when the settings allow it
/// (notifications enabled AND the event is selected). Silently returns otherwise.
void notifyAppEvent(String event, String message) {
  final s = AppSettings.instance;
  if (!s.notifyInApp || !s.inAppEvents.contains(event)) return;
  final messenger = appMessengerKey.currentState;
  if (messenger == null) return;
  messenger
    ..clearSnackBars()
    ..showSnackBar(SnackBar(content: Text(message)));
}