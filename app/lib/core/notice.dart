/// The app's in-app notices: one short line of feedback, shown one at a time in a
/// strip of its own below the page rather than painted over it (Docs/App/Settings.md
/// "Notifications (app open)").
///
/// Two rules the layer exists for:
///
///  * **A notice never sits on the page.** It is laid out beside the content (see
///    `NoticeHost` in `ui/notice_host.dart`), so it can neither cover the control
///    the user is reaching for nor take a pointer event outside its own box.
///  * **A notice is shown once per event occurrence, and dedupe is by sequence, not
///    by object identity.** Dart canonicalises `const`, so two cancels in a row hand
///    a listener the very same `ConnectNotice` object; `identical()` would remember
///    the first and silently drop the second. A producer therefore stamps each
///    occurrence with a number it owns ([AppNotice.sequence]), and the same number
///    twice is one notice while a new number with the same text is a new line.
library;

import 'package:flutter/foundation.dart';

/// What a notice is reporting. The tone picks the strip's icon and colour only; the
/// close affordance, the swipe and the place it occupies are identical for every tone.
enum NoticeTone { info, success, warning, error }

/// One in-app notice, as [NoticeCenter] holds it while it is on screen.
@immutable
class AppNotice {
  const AppNotice({
    required this.id,
    required this.event,
    required this.sequence,
    required this.message,
    this.tone = NoticeTone.info,
    this.duration = defaultDuration,
  });

  /// How long a notice stays up on its own. The user never has to wait it out: nothing
  /// is underneath it, and the strip carries a close control.
  static const Duration defaultDuration = Duration(seconds: 4);

  /// Monotonic, assigned by [NoticeCenter]. Two notices never share one, so the strip
  /// can key its dismiss timer on it - never on the message, and never on identity.
  final int id;

  /// The event family this notice belongs to: `connect` for a connect attempt ending,
  /// the settings' event name for a discovery, or `app` for a page's own line.
  final String event;

  /// The occurrence number within [event]. [NoticeCenter] shows each `(event, sequence)`
  /// pair once, so a listener that fires twice for one occurrence shows one line while
  /// the same text raised by a *new* occurrence still shows.
  final int sequence;

  final String message;
  final NoticeTone tone;
  final Duration duration;

  @override
  String toString() =>
      'AppNotice($event#$sequence "$message", id $id, ${tone.name})';
}

/// The one place in-app notices are raised and dismissed.
///
/// A notice belongs to the window, not to the page that raised it: the page may be gone
/// (or may be a dialog route) while the line still matters, and the strip outlives every
/// route. [NoticeHost] renders the current one; nothing else may.
class NoticeCenter extends ChangeNotifier {
  NoticeCenter._();

  static final NoticeCenter instance = NoticeCenter._();

  /// A centre of its own, for tests of the rules themselves: no host, no state shared with
  /// the running app (or with the next test).
  @visibleForTesting
  factory NoticeCenter.forTest() => NoticeCenter._();

  /// The last occurrence shown per event, so a repeated report of the SAME occurrence
  /// (a listener called twice, a rebuild) is one notice.
  final Map<String, int> _lastSequence = <String, int>{};

  /// Numbers handed out for producers that have no occurrence of their own; every
  /// `showNotice` call is one occurrence, so it always shows.
  final Map<String, int> _autoSequence = <String, int>{};

  AppNotice? _current;
  int _nextId = 0;
  int _hosts = 0;

  /// The notice on screen, or null.
  AppNotice? get current => _current;

  /// How many [NoticeHost]s are mounted. Zero means nothing can render a notice, and
  /// raising one then would only replay stale feedback the first time a host appears.
  int get hosts => _hosts;

  /// Raises [message] as [event]'s [sequence] occurrence.
  ///
  /// Returns whether it was shown. It is *not* shown when:
  ///  * [event] already showed [sequence] or a later occurrence (a replay, not a new line), or
  ///  * no host is mounted (nobody could see it; do not keep it for later).
  ///
  /// A notice replaces the one on screen rather than queueing behind it: at most one is
  /// ever visible, and the newest line is the one that matters.
  bool raise({
    required String event,
    required String message,
    int? sequence,
    NoticeTone tone = NoticeTone.info,
    Duration duration = AppNotice.defaultDuration,
  }) {
    final seq =
        sequence ?? (_autoSequence[event] = (_autoSequence[event] ?? 0) + 1);
    // Only a LATER occurrence is a new line: replaying one already shown (a listener called
    // twice, an older notice object resurfacing) is not.
    final last = _lastSequence[event];
    if (last != null && seq <= last) return false;
    _lastSequence[event] = seq;
    if (_hosts == 0) return false;
    _current = AppNotice(
      id: ++_nextId,
      event: event,
      sequence: seq,
      message: message,
      tone: tone,
      duration: duration,
    );
    notifyListeners();
    return true;
  }

  /// Dismisses [notice] (the close control, the swipe, or its own timer running out).
  ///
  /// A [notice] that is no longer the current one is ignored: two dismissals for the same
  /// line (the user tapped close just as the timer fired) are one dismissal.
  void dismiss([AppNotice? notice]) {
    final current = _current;
    if (current == null) return;
    if (notice != null && notice.id != current.id) return;
    _current = null;
    notifyListeners();
  }

  /// A host has mounted and can render.
  void attachHost() => _hosts++;

  /// The host is going away. When the last one goes, the notice goes with it: a line no
  /// host can draw is not kept (a test that raises one without a host would otherwise
  /// hand it to the next test's host).
  void detachHost() {
    if (_hosts > 0) _hosts--;
    if (_hosts == 0) _current = null;
  }

  /// Forgets every occurrence number and the current notice. Tests only: a suite shares
  /// this process, and a sequence left behind would look like a repeat.
  @visibleForTesting
  void reset() {
    _lastSequence.clear();
    _autoSequence.clear();
    _current = null;
  }
}

/// Raises one in-app notice; the app's `NoticeHost` renders it.
///
/// No [BuildContext]: a notice belongs to the window, not to the page that raised it.
/// [event] names the family of events the notice belongs to and [sequence] the occurrence
/// inside it, for producers whose event can be reported more than once (see
/// [NoticeCenter.raise]); a page reporting the result of one user action passes neither.
void showNotice(
  String message, {
  String event = 'app',
  int? sequence,
  NoticeTone tone = NoticeTone.info,
}) {
  NoticeCenter.instance.raise(
    event: event,
    message: message,
    sequence: sequence,
    tone: tone,
  );
}
