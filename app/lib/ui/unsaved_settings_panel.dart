import 'dart:async';

import 'package:flutter/material.dart';

import '../core/notice.dart';
import '../core/register_client.dart';
import '../core/unsaved_settings.dart';
import 'theme.dart';

/// The panel half of the unsaved-writes state (`core/unsaved_settings.dart`).
///
/// A settings panel writes `Persistent` fields into the device's RAM; only Save All makes them
/// survive a reset. This file is the three things a panel owes the user because of that: a strip
/// that stays on screen while anything is unsaved, one save action, and a voice at each moment the
/// state could otherwise be left silently - leaving the page, the link going away, and the app
/// going to the background.
///
/// The strip is drawn OUTSIDE the panel's scroll view (see [unsavedStrip]), because a warning that
/// scrolls off the top is the same as no warning.

/// The strip a settings panel shows while the device holds writes it has not saved.
///
/// It names the fields in its first line, because "unsaved changes" is not actionable: the user has
/// to know which setting is at risk to judge whether leaving is safe.
///
/// The layout is one variant at every size: the mark and one text column, with the save action
/// under the text and flush with the column's end. That is a structural choice, not a measured one
/// - the strip's text wraps and the action never competes with it for a line, so the strip fits a
/// narrow portrait screen and a wide landscape one without a second variant.
class UnsavedSettingsBar extends StatelessWidget {
  const UnsavedSettingsBar({
    super.key,
    required this.names,
    required this.onSaveAll,
    this.busy = false,
    this.connected = true,
    this.report,
  });

  /// The fields the device holds, as the panel prints them.
  final List<String> names;

  /// The one action that makes them durable.
  final VoidCallback onSaveAll;

  /// True while a save is in flight: the action is disabled and says so.
  final bool busy;

  /// Whether a save could be sent at all. False means the link is down, and the strip says the
  /// change will be lost at the next reset rather than offering an action that cannot run.
  final bool connected;

  /// What the last save attempt said, when it did not end clean - the reason, naming the fields.
  final String? report;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final single = names.length == 1;
    final detail = connected
        ? 'The device holds ${single ? 'it' : 'them'} in RAM only: a reset brings the old '
              '${single ? 'value' : 'values'} back. Save all is the step that writes '
              '${single ? 'it' : 'them'} to the device.'
        : 'The link is down, so the save cannot be sent: a reset brings the old '
              '${single ? 'value' : 'values'} back. Save all becomes possible again when the '
              'link returns.';
    return Card(
      color: kSurfaceAlt,
      child: Padding(
        padding: const EdgeInsets.all(Spacing.lg),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const Icon(
              Icons.report_outlined,
              color: kWarning,
              size: IconSize.lg,
            ),
            const SizedBox(width: Spacing.md),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    connected ? 'Not saved' : 'Not saved - no link',
                    style: theme.textTheme.titleSmall?.copyWith(
                      color: kWarning,
                    ),
                  ),
                  const SizedBox(height: Spacing.xs),
                  Text(
                    'Not saved: ${unsavedNames(names)}.',
                    style: theme.textTheme.bodySmall,
                  ),
                  const SizedBox(height: Spacing.xs),
                  Text(detail, style: theme.textTheme.bodySmall),
                  if (report != null) ...[
                    const SizedBox(height: Spacing.xs),
                    Text(
                      report!,
                      style: theme.textTheme.bodySmall?.copyWith(color: kError),
                    ),
                  ],
                  const SizedBox(height: Spacing.sm),
                  Align(
                    alignment: AlignmentDirectional.centerEnd,
                    child: FilledButton(
                      onPressed: busy || !connected ? null : onSaveAll,
                      child: Text(busy ? 'Saving...' : 'Save all'),
                    ),
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}

/// What the user chose when leaving a page that holds unsaved writes.
enum UnsavedLeave {
  /// Save first, and stay on the page when the save does not land.
  saveAll,

  /// Leave: the writes stay in the store, so the next visit still says they are unsaved.
  leave,

  /// Stay on the page (also the answer when the dialog is dismissed).
  stay,
}

/// What leaving a page with unsaved writes says: the fields, the one step that saves them, and
/// which link could not send it.
Future<UnsavedLeave> showUnsavedLeaveDialog(
  BuildContext context, {
  required String names,
  required bool canSave,
}) async {
  final theme = Theme.of(context);
  final choice = await showDialog<UnsavedLeave>(
    context: context,
    builder: (context) => AlertDialog(
      title: const Text('Unsaved changes'),
      content: SingleChildScrollView(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(
              'This device holds changes that are not saved: $names.',
              style: theme.textTheme.bodyMedium,
            ),
            const SizedBox(height: Spacing.md),
            Text(
              'Only Save all writes them to the device. Until it does, the device keeps them in '
              'RAM only, and a reset brings the old values back.',
              style: theme.textTheme.bodySmall,
            ),
            if (!canSave) ...[
              const SizedBox(height: Spacing.md),
              Text(
                'The link is down, so the save cannot be sent now.',
                style: theme.textTheme.bodySmall?.copyWith(color: kWarning),
              ),
            ],
          ],
        ),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(context, UnsavedLeave.stay),
          child: const Text('Stay'),
        ),
        TextButton(
          onPressed: () => Navigator.pop(context, UnsavedLeave.leave),
          child: const Text('Leave without saving'),
        ),
        FilledButton(
          onPressed: canSave
              ? () => Navigator.pop(context, UnsavedLeave.saveAll)
              : null,
          child: const Text('Save all'),
        ),
      ],
    ),
  );
  // A dismissed dialog is not a decision: the page stays, and the writes stay unsaved.
  return choice ?? UnsavedLeave.stay;
}

/// The offer a reconnected panel makes: re-apply the writes it still holds and save them.
Future<bool> showUnsavedReconnectDialog(
  BuildContext context, {
  required String names,
}) async {
  final theme = Theme.of(context);
  final reapply = await showDialog<bool>(
    context: context,
    builder: (context) => AlertDialog(
      title: const Text('The link is back'),
      content: SingleChildScrollView(
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          mainAxisSize: MainAxisSize.min,
          children: [
            Text(
              'This device still holds changes that were never saved: $names.',
              style: theme.textTheme.bodyMedium,
            ),
            const SizedBox(height: Spacing.md),
            Text(
              'A reset while the link was down clears the RAM copy, so the device may hold the old '
              'values now. Re-applying writes them again and then saves them.',
              style: theme.textTheme.bodySmall,
            ),
          ],
        ),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(context, false),
          child: const Text('Not now'),
        ),
        FilledButton(
          onPressed: () => Navigator.pop(context, true),
          child: const Text('Re-apply and save'),
        ),
      ],
    ),
  );
  return reapply ?? false;
}

/// The behaviour every settings panel shares once it writes a `Persistent` field.
///
/// A panel supplies what it is (its store, its device, its client and its link) and gets: the
/// strip, the save action, the leave prompt, the background save, the disconnect line and the
/// reconnect offer. The state itself lives in `core/unsaved_settings.dart`, so it survives this
/// widget - which is the whole point of the mixin being here and not in the store.
mixin UnsavedSettingsPanel<W extends StatefulWidget> on State<W>
    implements WidgetsBindingObserver {
  /// The store this panel records into (the app's, or a test's own).
  UnsavedSettings get unsavedStore;

  /// The device the panel's fields live on.
  int get unsavedDeviceId;

  /// The client a save is sent through.
  RegisterClient get unsavedClient;

  /// Whether the panel is on a live link right now.
  bool get unsavedConnected;

  /// Notifies when the link comes up or goes away; null when the panel was given a fixed link
  /// state and has no live connection to follow.
  Listenable? get unsavedLink;

  /// Called when the link came back and the panel holds nothing unsaved - the panel reloads its
  /// reading then. A panel with unsaved writes is asked about them first.
  void unsavedLinkRestored() {}

  /// True while a save is in flight.
  bool saving = false;

  /// What the last save attempt said when it did not end clean, or null.
  String? saveReport;

  bool _linkUp = false;
  bool _offeredReapply = false;

  /// The fields the device holds unwritten, as the panel prints them.
  List<String> get unsavedLabels =>
      unsavedStore.labelsFor(unsavedStore.of(unsavedDeviceId));

  /// Whether the panel's device holds a write that is not durable.
  bool get unsavedDirty => unsavedStore.isDirty(unsavedDeviceId);

  /// The fields, as one readable list ("Display name and Enabled").
  String get unsavedDescription => unsavedStore.describe(unsavedDeviceId);

  /// Hooks the store, the link and the lifecycle up. Call from `initState`, first.
  void unsavedInit() {
    _linkUp = unsavedConnected;
    unsavedStore.addListener(_onUnsavedChanged);
    unsavedLink?.addListener(_onLinkChanged);
    WidgetsBinding.instance.addObserver(this);
  }

  /// The teardown for [unsavedInit]. Call from `dispose`, before `super.dispose()`.
  void unsavedDispose() {
    WidgetsBinding.instance.removeObserver(this);
    unsavedLink?.removeListener(_onLinkChanged);
    unsavedStore.removeListener(_onUnsavedChanged);
  }

  void _onUnsavedChanged() {
    if (mounted) setState(() {});
  }

  /// The strip, or null when nothing is unsaved: a panel draws it above its scroll view, so it is
  /// never scrolled away from the rows it is talking about.
  Widget? unsavedStrip() {
    if (!unsavedDirty) return null;
    return Padding(
      padding: const EdgeInsets.fromLTRB(Spacing.lg, Spacing.lg, Spacing.lg, 0),
      child: UnsavedSettingsBar(
        names: unsavedLabels,
        busy: saving,
        connected: unsavedConnected,
        report: saveReport,
        onSaveAll: () => unawaited(saveUnsaved()),
      ),
    );
  }

  /// Records a write that landed in the device's RAM only (`mark` takes the bytes that were sent,
  /// so a reconnect can re-apply them).
  void unsavedMark(PendingSetting setting) {
    saveReport = null;
    unsavedStore.mark(setting);
  }

  /// Re-applies every pending write, sends Save All and reads each field back - the one action
  /// that makes the state durable (`savePendingSettings` holds the order and the reasons).
  ///
  /// A save that does not land reports through the strip's own line, which is on screen anyway and
  /// cannot be missed the way a four-second notice can; only a clean save announces itself, because
  /// that is the moment the strip disappears.
  Future<void> saveUnsaved() async {
    if (saving) return;
    if (!unsavedDirty) return;
    if (!unsavedConnected) {
      // Nothing can be sent: say what that means instead of reporting a save that never ran.
      if (mounted) {
        setState(() => saveReport = unsavedOfflineMessage(unsavedLabels));
      }
      return;
    }
    if (unsavedLabels.isEmpty) return;
    setState(() {
      saving = true;
      saveReport = null;
    });
    final outcome = await savePendingSettings(
      unsavedClient,
      deviceId: unsavedDeviceId,
      store: unsavedStore,
    );
    if (!mounted) return;
    setState(() {
      saving = false;
      saveReport = outcome.clean ? null : outcome.message;
    });
    if (outcome.clean) {
      _offeredReapply = false;
      showNotice(outcome.message, tone: NoticeTone.success);
    }
  }

  /// Asks before the page pops. Returns true when the page may go: the user left without saving
  /// (the writes stay in the store, so the next visit still says they are unsaved), or the save
  /// landed. A failed save keeps the page open with the reason on the strip.
  Future<bool> unsavedConfirmLeave() async {
    if (!unsavedDirty) return true;
    final choice = await showUnsavedLeaveDialog(
      context,
      names: unsavedDescription,
      canSave: unsavedConnected,
    );
    if (!mounted) return false;
    switch (choice) {
      case UnsavedLeave.leave:
        return true;
      case UnsavedLeave.stay:
        return false;
      case UnsavedLeave.saveAll:
        await saveUnsaved();
        if (!mounted) return false;
        return !unsavedDirty;
    }
  }

  /// Nobody can answer a dialog in the background, so the app saves what it holds there; a save
  /// that does not land leaves the entries in the store and the strip names them on return.
  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state != AppLifecycleState.paused &&
        state != AppLifecycleState.hidden) {
      return;
    }
    if (unsavedDirty && unsavedConnected) {
      unawaited(saveUnsaved());
    }
  }

  /// The panel says it is waiting when the link arrives, and it never leaves a reconnect silent:
  /// unsaved writes are offered back to the device instead of being dropped.
  void _onLinkChanged() {
    final up = unsavedConnected;
    if (up == _linkUp) return;
    _linkUp = up;
    if (!mounted) return;
    setState(() {});
    if (!up) {
      if (unsavedDirty) {
        showNotice(
          unsavedLinkLostNotice(unsavedLabels),
          tone: NoticeTone.warning,
        );
      }
      return;
    }
    if (unsavedDirty) {
      if (!_offeredReapply) {
        _offeredReapply = true;
        unawaited(_offerReapply());
      }
      return;
    }
    _offeredReapply = false;
    unsavedLinkRestored();
  }

  Future<void> _offerReapply() async {
    final reapply = await showUnsavedReconnectDialog(
      context,
      names: unsavedDescription,
    );
    if (!mounted || !reapply) return;
    await saveUnsaved();
  }
}
