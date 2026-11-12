import 'package:flutter/material.dart';

import '../core/connection.dart';
import '../core/notice.dart';
import '../core/unsaved_settings.dart';

/// The app-wide voice for unsaved writes while no settings panel is open.
///
/// A panel says its own piece while it is on screen (`ui/unsaved_settings_panel.dart`). The state
/// belongs to the device, though, and the two moments it could otherwise be left silently both
/// happen with the panel closed: the link going away (the save can no longer be sent, so the next
/// reset is what the change is measured against) and the link coming back (the device may have
/// reset while it was down, so the writes the app still holds must not be forgotten).
///
/// The watcher only raises the notices. The warning itself is the record in
/// `core/unsaved_settings.dart`, which no notice can dismiss: it stays until a save is confirmed.
class UnsavedSettingsWatcher extends StatefulWidget {
  const UnsavedSettingsWatcher({
    super.key,
    this.link,
    this.isConnected,
    this.store,
    this.child,
  });

  /// Notifies when the link comes up or goes away. Defaults to the app's connection manager.
  final Listenable? link;

  /// Whether a link is up; defaults to the app's connection manager.
  final bool Function()? isConnected;

  /// The store to watch; defaults to the app's own.
  final UnsavedSettings? store;

  /// Drawn unchanged - the watcher is a wrapper, so mounting it costs no layout.
  final Widget? child;

  @override
  State<UnsavedSettingsWatcher> createState() => _UnsavedSettingsWatcherState();
}

class _UnsavedSettingsWatcherState extends State<UnsavedSettingsWatcher> {
  late final UnsavedSettings _store = widget.store ?? UnsavedSettings.instance;
  late final Listenable? _link =
      widget.link ??
      (widget.isConnected == null ? ConnectionManager.instance : null);
  late final bool Function() _isConnected =
      widget.isConnected ?? (() => ConnectionManager.instance.isConnected);
  late bool _linkUp = false;

  @override
  void initState() {
    super.initState();
    _linkUp = _isConnected();
    _link?.addListener(_onLinkChanged);
  }

  @override
  void dispose() {
    _link?.removeListener(_onLinkChanged);
    super.dispose();
  }

  /// Every unsaved field of every device, as one list of names - the notice names the settings,
  /// because "unsaved changes" alone tells the user nothing they can act on.
  List<String> get _names => [
    for (final device in _store.dirtyDevices)
      ..._store.labelsFor(_store.of(device)),
  ];

  void _onLinkChanged() {
    final up = _isConnected();
    if (up == _linkUp) return;
    _linkUp = up;
    final names = _names;
    if (names.isEmpty) return;
    showNotice(
      up ? unsavedLinkBackNotice(names) : unsavedLinkLostNotice(names),
      tone: NoticeTone.warning,
    );
  }

  @override
  Widget build(BuildContext context) => widget.child ?? const SizedBox.shrink();
}
