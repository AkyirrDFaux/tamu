/// The one wait display: a labelled progress bar, in whichever frame a screen needs.
///
/// Three shapes, one API (Akyirr, 2026-11-05: *"some places just wait and freeze up, some show a
/// blank piece"*):
///
///   * [WaitShape.panel]  - the whole panel is the wait: a first load, before anything is known.
///   * [WaitShape.banner] - a strip above content that is already on screen: a refresh.
///   * [WaitShape.dialog] - a modal wait that covers what it is working on (a read the page cannot
///                          draw until it is done), with the cancel that closes it at once.
///
/// The bar is DETERMINATE whenever the operation reports a total (`3/12`, `fragment 40 of 256`)
/// and indeterminate otherwise - never absent, and never a spinner over a blank area.
library;

import 'dart:async';

import 'package:flutter/material.dart';

import 'theme.dart';
import 'wait.dart';
import 'widgets.dart';

/// Which frame a [WaitView] is drawn in.
enum WaitShape { panel, banner, dialog }

/// Draws what a [WaitController] (or a plain [WaitProgress]) is waiting for, in the requested shape.
///
/// Draws nothing while idle, so a page can keep it permanently in its tree (above a list, at the
/// top of a panel) and only the waits it starts ever appear.
class WaitView extends StatelessWidget {
  const WaitView({
    super.key,
    this.controller,
    this.progress,
    this.shape = WaitShape.panel,
    this.onCancel,
  }) : assert(
         controller != null || progress != null,
         'a WaitView needs a controller or a progress',
       );

  /// The wait to draw, live: the view repaints as the controller steps.
  final WaitController? controller;

  /// A fixed wait, for a state a page already holds (a connect that is pending).
  final WaitProgress? progress;

  final WaitShape shape;

  /// Called when the user asks to stop. When null the wait offers no way out (the operation cannot
  /// be stopped, or the page has not wired one) and the view draws no control.
  final VoidCallback? onCancel;

  @override
  Widget build(BuildContext context) {
    final live = controller;
    if (live == null) return _content(context, progress);
    return ListenableBuilder(
      listenable: live,
      builder: (context, _) => _content(context, live.progress),
    );
  }

  Widget _content(BuildContext context, WaitProgress? progress) {
    if (progress == null) return const SizedBox.shrink();
    final canCancel =
        onCancel != null && progress.cancellable && !progress.cancelling;
    final body = TaskProgress(
      label: progress.label,
      value: progress.value,
      completed: progress.determinate ? progress.completed : null,
      total: progress.determinate ? progress.total : null,
      note: progress.note,
    );
    final cancel = canCancel
        ? TextButton.icon(
            onPressed: onCancel,
            icon: const Icon(Icons.close, size: IconSize.sm),
            label: Text(progress.cancelLabel),
          )
        : progress.cancelling
        ? Text(
            'Stopping...',
            style: Theme.of(
              context,
            ).textTheme.bodySmall?.copyWith(color: kTextMuted),
          )
        : null;
    switch (shape) {
      case WaitShape.panel:
        return Center(
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 420),
            child: Padding(
              padding: Insets.page,
              child: Column(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  body,
                  if (cancel != null) ...[
                    const SizedBox(height: Spacing.md),
                    Align(alignment: Alignment.centerRight, child: cancel),
                  ],
                ],
              ),
            ),
          ),
        );
      case WaitShape.banner:
        return Card(
          color: kSurfaceAlt,
          margin: const EdgeInsets.fromLTRB(8, 8, 8, 0),
          child: Padding(
            padding: Insets.row,
            child: Row(
              children: [
                Expanded(child: body),
                if (cancel != null) ...[
                  const SizedBox(width: Spacing.md),
                  cancel,
                ],
              ],
            ),
          ),
        );
      case WaitShape.dialog:
        return Dialog(
          child: Padding(
            padding: Insets.page,
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                body,
                if (cancel != null) ...[
                  const SizedBox(height: Spacing.md),
                  Align(alignment: Alignment.centerRight, child: cancel),
                ],
              ],
            ),
          ),
        );
    }
  }
}

/// Runs [task] behind a modal [WaitShape.dialog] that names the operation and reports its progress.
///
/// The dialog closes when the task ends - it is not a screen the user has to dismiss - and its
/// Cancel stops the wait at once: the dialog disappears and `controller.cancelled` tells the task
/// to stop at its next safe point. The result carries the task's own value and whether the user
/// stopped it, so a page can tell "the device answered nothing" from "the user walked away".
Future<({T value, bool cancelled})> runBehindWait<T>(
  BuildContext context,
  WaitController controller, {
  required String label,
  int? total,
  String cancelLabel = '',
  required Future<T> Function() task,
}) async {
  controller.begin(label, total: total, cancelLabel: cancelLabel);
  var closed = false;
  final navigator = Navigator.of(context, rootNavigator: true);
  void close() {
    if (closed) return;
    closed = true;
    if (navigator.canPop()) navigator.pop();
  }

  unawaited(
    showDialog<void>(
      context: context,
      barrierDismissible: false,
      builder: (dialogContext) => PopScope(
        canPop: false,
        child: WaitView(
          controller: controller,
          shape: WaitShape.dialog,
          onCancel: cancelLabel.isEmpty
              ? null
              : () {
                  controller.cancel();
                  close();
                },
        ),
      ),
    ).then((_) => closed = true),
  );
  try {
    final value = await task();
    return (value: value, cancelled: controller.cancelled);
  } finally {
    controller.end();
    close();
  }
}
