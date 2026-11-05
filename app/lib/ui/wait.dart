/// One wait, as the app shows it.
///
/// The owner's rule (2026-11-05): *no blank piece and no frozen frame.* A wait that has nothing to
/// show is a defect, so every wait in the app is described by a [WaitProgress] - a line naming the
/// operation, and a step counter whenever the operation can count its units - and drawn by
/// [WaitView] in `loading.dart`.
///
/// This file is the VALUE (no widgets), so a page and a controller can share it without importing
/// the toolkit: `widgets.dart` reads it for [StateMessage], the pages read it for their own bands.
library;

import 'package:flutter/foundation.dart';

/// What a wait is doing, and how far it has got.
@immutable
class WaitProgress {
  const WaitProgress(
    this.label, {
    this.completed,
    this.total,
    this.note,
    this.cancelLabel = 'Cancel',
    this.cancelling = false,
  });

  /// One line naming the operation, in the page's own words: `Reading SCR_004`,
  /// `Scanning Core #1`, `Applying 12 of 40 items`.
  final String label;

  /// Units done. Null whenever the operation cannot count them (a connect, a roster sweep).
  final int? completed;

  /// Units expected. Null whenever the operation cannot know the total.
  final int? total;

  /// A second, quieter line: why a read is being retried, which fragment is missing, the device a
  /// phase is on. Never the only thing that says what is happening.
  final String? note;

  /// The cancel control's label, or empty for a wait that cannot be stopped.
  final String cancelLabel;

  /// True once the user asked to stop and the operation has not yet reached a point where it can.
  final bool cancelling;

  /// Progress in 0..1 for the bar, or null for an indeterminate one.
  double? get value {
    final done = completed;
    final of = total;
    if (done == null || of == null || of <= 0) return null;
    return (done / of).clamp(0.0, 1.0).toDouble();
  }

  /// True when a real total is known, i.e. the bar can be determinate.
  bool get determinate => value != null;

  /// `3/12` beside the label, or null when there is no total to count against.
  String? get counter {
    final of = total;
    if (of == null) return null;
    return '${completed ?? 0}/$of';
  }

  /// True when this wait offers a way out.
  bool get cancellable => cancelLabel.isNotEmpty;

  WaitProgress copyWith({
    String? label,
    int? completed,
    int? total,
    String? note,
    String? cancelLabel,
    bool? cancelling,
  }) => WaitProgress(
    label ?? this.label,
    completed: completed ?? this.completed,
    total: total ?? this.total,
    note: note ?? this.note,
    cancelLabel: cancelLabel ?? this.cancelLabel,
    cancelling: cancelling ?? this.cancelling,
  );

  @override
  String toString() =>
      'WaitProgress($label${counter == null ? '' : ' $counter'})';
}

/// Drives one [WaitProgress] for a page.
///
/// The page calls [begin] when the work starts, [step] as it makes progress and [end] when it is
/// over; a [WaitView] listening to this controller draws whatever is current, and draws NOTHING
/// when [active] is false - so a page never has to keep a second "which wait is this" flag of its
/// own.
class WaitController extends ChangeNotifier {
  WaitProgress? _progress;
  bool _cancelRequested = false;

  /// True once [dispose] ran. A page disposes its controller with itself, but the read it started
  /// keeps running and calls back when it ends - a 16 kB table read over BLE outlives a page the
  /// user popped. `ChangeNotifier.notifyListeners` asserts not-disposed in debug builds, so every
  /// mutator here has to be inert after the page is gone: the wait has no screen left to draw.
  bool _disposed = false;

  /// The wait as it stands, or null when nothing is being waited for.
  WaitProgress? get progress => _progress;

  @override
  void dispose() {
    _disposed = true;
    super.dispose();
  }

  /// Repaints the listening views - unless the page that owns this controller is gone.
  void _notify() {
    if (_disposed) return;
    notifyListeners();
  }

  bool get active => _progress != null;

  /// True once [cancel] was called - the operation checks this at its own safe points.
  bool get cancelled => _cancelRequested;

  /// Starts (or replaces) the wait. [total] makes the bar determinate; omit it and the bar is
  /// indeterminate, which is the honest shape when the operation cannot know how much is left.
  void begin(
    String label, {
    int? total,
    int? completed,
    String? note,
    String cancelLabel = '',
  }) {
    if (_disposed) return;
    _cancelRequested = false;
    _progress = WaitProgress(
      label,
      completed: completed,
      total: total,
      note: note,
      cancelLabel: cancelLabel,
    );
    _notify();
  }

  /// Reports a step. A no-op when nothing is being waited for, so a stale callback from an
  /// abandoned read cannot re-open a wait the page already closed.
  void step({String? label, int? completed, int? total, String? note}) {
    if (_disposed) return;
    final current = _progress;
    if (current == null) return;
    if (label == null && completed == null && total == null && note == null) {
      return;
    }
    _progress = WaitProgress(
      label ?? current.label,
      completed: completed ?? current.completed,
      total: total ?? current.total,
      note: note ?? current.note,
      cancelLabel: _cancelRequested ? '' : current.cancelLabel,
      cancelling: _cancelRequested,
    );
    _notify();
  }

  /// The user's way out. The operation sees [cancelled] at its next safe point; the view says it is
  /// stopping so the screen is not still claiming to be working.
  void cancel() {
    if (_disposed) return;
    final current = _progress;
    if (current == null || _cancelRequested) return;
    _cancelRequested = true;
    _progress = current.copyWith(cancelling: true, cancelLabel: '');
    _notify();
  }

  /// Ends the wait: the screen stops drawing it.
  void end() {
    if (_disposed || _progress == null) return;
    _progress = null;
    _cancelRequested = false;
    _notify();
  }
}
