/// Shared dialog chrome: the one way a modal dialog is built and shown, and
/// the shell the picker dialogs share.
///
/// Why it exists (TODO.md, "App UX pass", phase 1b): the app hand-rolls
/// `showDialog` in about 35 places across 15 files, each one repeating the
/// `AlertDialog` surface, the title role and the Cancel/confirm action row, so
/// the dialog look drifts from file to file. A dialog here takes the standard
/// chrome ([TamuDialog]) or presents a widget that already renders its own
/// chrome through [showTamuPicker], and the pickers share [PickerShell].
///
/// Everything reads the theme and the tokens in [theme.dart], so a caller
/// passes content, not styling.
library;

import 'package:flutter/material.dart';

import 'theme.dart';
import 'widgets.dart' show DialogBody, fitToBox;

/// The standard dialog: the raised surface, the title role, a [DialogBody]-
/// bounded child and the action row. Built by [showTamuDialog], and the chrome
/// a [PickerShell] renders.
class TamuDialog extends StatelessWidget {
  const TamuDialog({
    super.key,
    this.title,
    this.titleWidget,
    required this.body,
    this.actions = const [],
    this.maxWidth = 400,
    this.height,
  });

  /// The title line; ignored when [titleWidget] is given.
  final String? title;

  /// A title that needs more than text, e.g. a back button row.
  final Widget? titleWidget;

  /// The dialog body.
  final Widget body;

  /// The action row, e.g. from [tamuDialogActions].
  final List<Widget> actions;

  /// Upper bound on the body width; [DialogBody] shrinks it to fit a phone.
  final double maxWidth;

  /// A fixed body height, clamped by [DialogBody] to the window.
  final double? height;

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: titleWidget ?? (title != null ? Text(title!) : null),
      content: DialogBody(maxWidth: maxWidth, height: height, child: body),
      actions: actions,
    );
  }
}

/// Shows a modal dialog and returns its result. The chrome (surface, title,
/// width bound) comes from [TamuDialog]; the caller passes only content and
/// actions.
Future<T?> showTamuDialog<T>(
  BuildContext context, {
  String? title,
  Widget? titleWidget,
  required Widget body,
  List<Widget> actions = const [],
  double maxWidth = 400,
  double? height,
  bool barrierDismissible = true,
}) {
  return showDialog<T>(
    context: context,
    barrierDismissible: barrierDismissible,
    builder: (context) => TamuDialog(
      title: title,
      titleWidget: titleWidget,
      body: body,
      actions: actions,
      maxWidth: maxWidth,
      height: height,
    ),
  );
}

/// Shows [dialog] (a widget that renders its own dialog chrome, e.g. a picker
/// or a value editor) and returns its result. The companion to
/// [showTamuDialog] for the flows whose body is a whole widget.
Future<T?> showTamuPicker<T>(
  BuildContext context,
  Widget dialog, {
  bool barrierDismissible = true,
}) {
  return showDialog<T>(
    context: context,
    barrierDismissible: barrierDismissible,
    builder: (_) => dialog,
  );
}

/// The standard Cancel + confirm action row, so a dialog does not rebuild its
/// two buttons by hand.
List<Widget> tamuDialogActions({
  required VoidCallback? onCancel,
  required VoidCallback? onConfirm,
  String cancelLabel = 'Cancel',
  String confirmLabel = 'OK',
}) => [
  TextButton(onPressed: onCancel, child: Text(cancelLabel)),
  FilledButton(onPressed: onConfirm, child: Text(confirmLabel)),
];

/// The items a dialog's value dropdown shows: [values] in order, with [current]
/// first when the list no longer offers it. A value read from a device or an
/// older file has to keep rendering, so the dropdown never asserts on a value the
/// list does not contain - the guard every such dropdown used to write by hand.
List<DropdownMenuItem<T>> dialogValueItems<T>(
  Iterable<T> values,
  T current,
  String Function(T value) label,
) {
  final list = values.toList();
  return [
    if (!list.contains(current))
      DropdownMenuItem(value: current, child: Text(label(current))),
    for (final value in list)
      DropdownMenuItem(value: value, child: Text(label(value))),
  ];
}

/// The chrome every picker shares: a title (with an optional back affordance
/// when the picker drills into a group), a [DialogBody]-bounded list, an
/// optional hint line and Cancel plus any extra actions. The block-info,
/// symbol, instruction and subscription pickers all render through it.
class PickerShell extends StatelessWidget {
  const PickerShell({
    super.key,
    required this.title,
    required this.body,
    this.onBack,
    this.backTooltip = 'Back',
    this.actions = const [],
    this.maxWidth = 360,
    this.height,
    this.hint,
  });

  final String title;

  /// The picker's body, normally a scrollable list.
  final Widget body;

  /// When given, a back button appears before the title that returns to the
  /// parent level.
  final VoidCallback? onBack;
  final String backTooltip;

  /// Extra actions after Cancel (a confirm button, usually).
  final List<Widget> actions;

  /// Upper bound on the body width.
  final double maxWidth;

  /// A fixed body height, clamped by [DialogBody] to the window.
  final double? height;

  /// An optional note above the list, in the accent colour.
  final String? hint;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return AlertDialog(
      title: onBack == null
          ? Text(title)
          : Row(
              children: [
                IconButton(
                  visualDensity: VisualDensity.compact,
                  icon: const Icon(Icons.arrow_back, size: IconSize.md),
                  tooltip: backTooltip,
                  onPressed: onBack,
                ),
                Expanded(child: Text(title)),
              ],
            ),
      content: DialogBody(
        maxWidth: maxWidth,
        height: height,
        child: hint == null ? body : _hintAndBody(theme),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(context),
          child: const Text('Cancel'),
        ),
        ...actions,
      ],
    );
  }

  /// The hint line above the body. The body keeps its own fit rule inside the
  /// flexible row ([fitToBox]), so a content-sized body scrolls in the leftover
  /// height instead of being stretched to it: a bare `Expanded` would force the
  /// body to that height and overflow by everything above it (measured: 903 px
  /// for a 20-row column in the 500x158 box of a 640x360 window).
  Widget _hintAndBody(ThemeData theme) => Column(
    children: [
      Padding(
        padding: const EdgeInsets.only(bottom: Spacing.xs),
        child: Text(
          hint!,
          style: theme.textTheme.labelSmall?.copyWith(color: kOrange),
        ),
      ),
      Expanded(child: fitToBox(body)),
    ],
  );
}
