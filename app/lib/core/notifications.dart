/// In-app notifications (Docs/App/Settings.md): "Allow notifications (When app
/// open) - per event selection". `notifyAppEvent` honours the settings; the line it
/// raises is drawn by the app's one notice strip (`NoticeHost`), not by a snack bar,
/// so it can be closed, swept away and cannot cover a control.
library;

import 'notice.dart';
import 'settings.dart';

/// Shows an in-app notification for [event] when the settings allow it
/// (notifications enabled AND the event is selected). Silently returns otherwise.
///
/// [sequence] is the event's own occurrence number, when the producer has one: the same
/// number twice is one notice, a new number is a new line (see [NoticeCenter.raise]).
/// A producer that reports one occurrence once passes neither and every call shows.
void notifyAppEvent(String event, String message, {int? sequence}) {
  final s = AppSettings.instance;
  if (!s.notifyInApp || !s.inAppEvents.contains(event)) return;
  showNotice(message, event: event, sequence: sequence);
}
