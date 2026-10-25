import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../core/backup.dart';
import 'dialogs.dart';
import 'theme.dart';
import 'viewer_kit.dart';
import 'widgets.dart';

/// The restore result screen: what the run did, and - the part a count cannot carry - WHY every
/// failed item failed, one section per device, copyable as text.
///
/// The owner reads this on a phone (Akyirr, 2026-10-25: "17 failures, no reason"), so the reasons
/// come first and are written to be pasted into a card: a device heading, then per row the item
/// kind, the address as the PLAN names it, the code the device answered with (or the explicit "no
/// code from the device" of a client-side refusal) and one sentence saying what that means. Nothing
/// about the successes is listed; their count is the first line.
///
/// Layout rule - the structural decision, not a pixel width: the body is ONE scrolling list in both
/// orientations. Portrait stacks the failure cards and lets the long addresses wrap inside the
/// dialog's width; landscape shows the same cards on a wider, shorter viewport and reaches them by
/// scrolling. No control is placed beside a neighbouring one in a fixed row, so a narrow window can
/// never push one past the edge - the shared [DialogBody] clamps the box and the [ListView] scrolls
/// inside it. Measured at 360x640 and 800x360 in `restore_result_test.dart`; those numbers are the
/// evidence, not the rule.
class RestoreResultView extends StatelessWidget {
  const RestoreResultView({super.key, required this.report});

  final RestoreReport report;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final groups = report.byDevice;
    final removed = report.removals;
    final kept = report.keptBySelection;
    // A content-sized scroll body, not a `ListView`: the shared [DialogBody] hands a scroll view the
    // box it has (`fitToBox`), and a `Column` inside one measures itself honestly - the same shape
    // the diagnostics dialog uses. It also means a short landscape window scrolls the same rows a
    // portrait one does, instead of laying them out differently.
    return SingleChildScrollView(
      padding: Insets.page,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Text(report.counts, style: theme.textTheme.titleSmall),
          const SizedBox(height: Spacing.hairline),
          Text(
            report.reasons.isEmpty
                ? 'Every selected item landed.'
                : 'Grouped by the device each item was being written to.',
            style: theme.textTheme.bodySmall,
          ),
          for (final group in groups) ...[
            const SizedBox(height: Spacing.md),
            Text(
              '${group.device} · ${group.failures.length} '
              'failure${group.failures.length == 1 ? '' : 's'}',
              style: theme.textTheme.titleSmall?.copyWith(color: kError),
            ),
            for (final failure in group.failures)
              _failureCard(context, failure),
          ],
          // A structural skip is neither a success nor a failure: its own heading, with the reason
          // each row was left alone, so "0 failed" cannot read as "nothing was left out".
          if (report.skips.isNotEmpty)
            _listSection(context, 'Skipped as structural', report.skips),
          // A deletion is neither a success nor a failure, so it gets its own heading rather than
          // being folded into either count.
          if (removed.isNotEmpty)
            _listSection(context, 'Removed from the device', removed),
          if (kept.isNotEmpty)
            _listSection(context, 'Left alone by selection', kept),
        ],
      ),
    );
  }

  /// A heading and one line per row: the run's deletions, and the rows it was told to leave.
  Widget _listSection(BuildContext context, String heading, List<String> rows) {
    final theme = Theme.of(context);
    return Padding(
      padding: const EdgeInsets.only(top: Spacing.md),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(
            '$heading (${rows.length})',
            style: theme.textTheme.titleSmall?.copyWith(color: kWarning),
          ),
          for (final row in rows)
            Padding(
              padding: const EdgeInsets.only(top: Spacing.hairline),
              child: Text(row, style: theme.textTheme.bodySmall),
            ),
        ],
      ),
    );
  }

  /// One failed item: its kind, the address the plan names it by, the code, and the meaning.
  ///
  /// The kind is a chip and the address is its own line, so a long register path wraps instead of
  /// being ellipsized - a truncated address is not something a card can quote.
  Widget _failureCard(BuildContext context, RestoreFailure failure) {
    final theme = Theme.of(context);
    return Card(
      margin: const EdgeInsets.only(top: Spacing.xs),
      child: Padding(
        padding: Insets.row,
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Wrap(
              spacing: Spacing.sm,
              runSpacing: Spacing.xs,
              crossAxisAlignment: WrapCrossAlignment.center,
              children: [
                TamuChip(
                  failure.kindWord.toUpperCase(),
                  tone: failure.placed ? ChipTone.error : ChipTone.warning,
                ),
                if (failure.placed)
                  Text(
                    'from ${failure.archive}',
                    style: theme.textTheme.labelSmall?.copyWith(
                      color: kTextMuted,
                    ),
                  ),
              ],
            ),
            const SizedBox(height: Spacing.xs),
            Text(failure.subject, style: theme.textTheme.bodyMedium),
            const SizedBox(height: Spacing.hairline),
            Text(
              'code: ${failure.code}',
              style: theme.textTheme.bodySmall?.copyWith(color: kWarning),
            ),
            Text(failure.meaning, style: theme.textTheme.bodySmall),
          ],
        ),
      ),
    );
  }
}

/// Makes [text] the clipboard's content and says so. Nothing is copied silently.
void copyRestoreReport(BuildContext context, String text, String snack) {
  Clipboard.setData(ClipboardData(text: text));
  showSnack(context, snack);
}

/// Shows the run's result: the counts, the reasons grouped by device, and a Copy that puts the
/// whole report on the clipboard as text.
Future<void> showRestoreResult(
  BuildContext context,
  RestoreReport report,
) async {
  await showTamuDialog<void>(
    context,
    title: report.reasons.isEmpty ? 'Restore complete' : 'Restore result',
    maxWidth: 560,
    height: 440,
    body: RestoreResultView(report: report),
    actions: [
      TextButton(
        onPressed: () =>
            copyRestoreReport(context, report.text, 'Restore report copied'),
        child: const Text('Copy as text'),
      ),
      FilledButton(
        onPressed: () => Navigator.pop(context),
        child: const Text('Close'),
      ),
    ],
  );
}
