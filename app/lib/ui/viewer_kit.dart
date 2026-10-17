/// The viewer toolkit: the one table shell, hex/ASCII byte view, incremental
/// (paging) list and row chips a viewer is built from.
///
/// Why it exists (TODO.md, "App UX pass", phase 1b): four near-parallel
/// table/tree viewers (`file_viewers.dart`, `log_page.dart`, `storage_page.dart`,
/// `register_backup_view.dart`) each grew their own header row, byte view, paging
/// and chips, and with them the tree's worst styling debt. This file replaces
/// those four shapes with one implementation each, so a viewer supplies data and
/// columns and gets a consistent table.
///
/// Every string a cell prints comes through `formatters.dart`, and every colour,
/// size, gap and radius comes from `theme.dart` - a viewer carries neither.
library;

import 'dart:math' as math;

import 'package:flutter/material.dart';

import 'formatters.dart';
import 'theme.dart';
import 'widgets.dart' show StateMessage;

// ---------------------------------------------------------------------------
// Row chips / badges
// ---------------------------------------------------------------------------

/// The tone of a [TamuChip]: which token pair fills and colours it.
enum ChipTone {
  /// The notable datum (a block type, an active flag).
  accent,

  /// A plain tag (a count, a unit, a secondary label).
  subtle,

  /// A healthy / up state.
  success,

  /// A problem worth noticing.
  warning,

  /// A failure.
  error,
}

/// The one small label chip: a block type, a flag, a count, a status. Styled
/// from the tokens ([kAccentTint]/[kSubtleTint], the status colours, [Radii.tag],
/// [Spacing.sm]/[Spacing.hairline], the small label role) so a viewer never
/// builds a badge by hand.
class TamuChip extends StatelessWidget {
  const TamuChip(this.text, {super.key, this.tone = ChipTone.subtle});

  final String text;
  final ChipTone tone;

  @override
  Widget build(BuildContext context) {
    final (
      Color background,
      Color foreground,
      FontWeight weight,
    ) = switch (tone) {
      ChipTone.accent => (kAccentTint, kOrange, FontWeight.w600),
      ChipTone.subtle => (kSubtleTint, kTextMuted, FontWeight.w400),
      ChipTone.success => (kSuccess.withAlpha(46), kSuccess, FontWeight.w600),
      ChipTone.warning => (kWarning.withAlpha(46), kWarning, FontWeight.w600),
      ChipTone.error => (kError.withAlpha(46), kError, FontWeight.w600),
    };
    return Container(
      padding: const EdgeInsets.symmetric(
        horizontal: Spacing.sm,
        vertical: Spacing.hairline,
      ),
      decoration: BoxDecoration(color: background, borderRadius: Radii.tag),
      child: Text(
        text,
        style: Theme.of(
          context,
        ).textTheme.labelSmall?.copyWith(color: foreground, fontWeight: weight),
      ),
    );
  }
}

/// A wrap of [TamuChip]s: the row-chip / badge strip a viewer draws under a
/// table row or a tile. Renders nothing for an empty list.
class TamuChipRow extends StatelessWidget {
  const TamuChipRow({
    super.key,
    required this.chips,
    this.spacing = Spacing.sm,
    this.runSpacing = Spacing.hairline,
  });

  final List<TamuChip> chips;
  final double spacing;
  final double runSpacing;

  @override
  Widget build(BuildContext context) {
    if (chips.isEmpty) return const SizedBox.shrink();
    return Wrap(spacing: spacing, runSpacing: runSpacing, children: chips);
  }
}

// ---------------------------------------------------------------------------
// Monospace text
// ---------------------------------------------------------------------------

/// Monospace text drawn from the theme: the one place a viewer prints an offset,
/// a hex column, an id or an ASCII gutter. It reads the small body role (size
/// and family aside, no viewer sets a font of its own); [muted] drops it to the
/// subtext colour, and the text is clipped rather than wrapped so a fixed-width
/// token never breaks mid-value.
class MonoText extends StatelessWidget {
  const MonoText(
    this.text, {
    super.key,
    this.muted = false,
    this.align = TextAlign.start,
    this.overflow = TextOverflow.ellipsis,
  });

  final String text;
  final bool muted;
  final TextAlign align;
  final TextOverflow overflow;

  @override
  Widget build(BuildContext context) {
    return Text(
      text,
      textAlign: align,
      softWrap: false,
      overflow: overflow,
      style: Theme.of(context).textTheme.bodySmall?.copyWith(
        fontFamily: 'monospace',
        color: muted ? kTextMuted : kTextPrimary,
      ),
    );
  }
}

// ---------------------------------------------------------------------------
// Table shell
// ---------------------------------------------------------------------------

/// One column of a [TamuTable].
///
/// A cell's text is either [text] - a `formatters.dart` result, so the viewer
/// never formats a value itself - or a [cell] widget for anything richer (a
/// chip, a colour swatch). [width] pins the column; otherwise it shares the row
/// by [flex]. [mono] prints through [MonoText]; [muted] drops the text to the
/// subtext colour.
class TamuColumn<T> {
  const TamuColumn({
    required this.label,
    this.text,
    this.cell,
    this.width,
    this.flex = 1,
    this.mono = false,
    this.muted = false,
    this.align = TextAlign.start,
  }) : assert(text != null || cell != null, 'a column needs text or cell');

  /// Header text.
  final String label;

  /// The cell text for a row, normally a formatter result.
  final String Function(T row)? text;

  /// A full cell override; takes precedence over [text].
  final Widget Function(BuildContext context, T row)? cell;

  /// A fixed column width; null shares the row by [flex].
  final double? width;

  /// Share of the remaining width when [width] is null.
  final int flex;

  /// Print the cell through [MonoText].
  final bool mono;

  /// Draw the cell in the subtext colour.
  final bool muted;

  final TextAlign align;

  /// Renders this column's cell for [row].
  Widget buildCell(BuildContext context, T row) {
    final override = cell;
    if (override != null) return override(context, row);
    final value = text!(row);
    if (mono) return MonoText(value, muted: muted, align: align);
    return Text(
      value,
      textAlign: align,
      overflow: TextOverflow.ellipsis,
      style: Theme.of(
        context,
      ).textTheme.bodySmall?.copyWith(color: muted ? kTextMuted : kTextPrimary),
    );
  }
}

/// The one table shell: a header row, one row per item, an optional leading slot,
/// an optional trailing widget and an optional per-row chip strip, with
/// incremental loading.
///
/// A viewer supplies [columns] and [rows] (and formatters for the cell text);
/// the header, the row insets, the hairline, the mono font and the empty state
/// all come from the tokens. The table fills its parent, so it belongs directly
/// in a page body, not inside another scroll. Long tables reveal [pageSize] rows
/// at a time through [TamuPagedList].
class TamuTable<T> extends StatelessWidget {
  const TamuTable({
    super.key,
    required this.columns,
    required this.rows,
    this.leading,
    this.leadingWidth = Spacing.xxl,
    this.trailing,
    this.trailingWidth = Spacing.xxl * 2,
    this.chips,
    this.onTap,
    this.pageSize,
    this.initialRows,
    this.emptyMessage,
  });

  /// The columns, left to right.
  final List<TamuColumn<T>> columns;

  /// The rows to render.
  final List<T> rows;

  /// A fixed-width leading slot per row (a type icon); null for no leading slot.
  final Widget Function(BuildContext context, T row)? leading;
  final double leadingWidth;

  /// Trailing content per row (an action menu); null for no trailing slot.
  final Widget Function(BuildContext context, T row)? trailing;
  final double trailingWidth;

  /// A chip strip drawn under a row's cells (row chips/badges).
  final Widget Function(BuildContext context, T row)? chips;

  /// Tapping a row.
  final void Function(T row)? onTap;

  /// Rows revealed per step; null shows every row at once.
  final int? pageSize;

  /// Rows shown before the first "show more"; defaults to [pageSize].
  final int? initialRows;

  /// The empty state shown when [rows] is empty.
  final String? emptyMessage;

  @override
  Widget build(BuildContext context) {
    return TamuPagedList<T>(
      items: rows,
      pageSize: pageSize,
      initialCount: initialRows,
      emptyMessage: emptyMessage,
      header: _header(context),
      itemBuilder: (context, row, _) => _dataRow(context, row),
    );
  }

  /// The column boxes, shared verbatim by the header and the data rows so the
  /// two can never drift out of alignment.
  List<Widget> _columnBoxes(List<Widget> cells) {
    final boxes = <Widget>[];
    for (var i = 0; i < columns.length; i++) {
      if (i > 0) boxes.add(const SizedBox(width: Spacing.lg));
      final column = columns[i];
      boxes.add(
        column.width != null
            ? SizedBox(width: column.width, child: cells[i])
            : Expanded(flex: column.flex, child: cells[i]),
      );
    }
    return boxes;
  }

  Widget _header(BuildContext context) {
    final style = Theme.of(context).textTheme.labelSmall?.copyWith(
      color: kTextSecondary,
      fontWeight: FontWeight.w600,
    );
    final cells = <Widget>[
      for (final column in columns)
        Text(column.label, style: style, overflow: TextOverflow.ellipsis),
    ];
    return Container(
      padding: Insets.row,
      decoration: const BoxDecoration(
        border: Border(bottom: BorderSide(color: kLine)),
      ),
      child: Row(
        children: [
          if (leading != null) ...[
            SizedBox(width: leadingWidth),
            const SizedBox(width: Spacing.lg),
          ],
          ..._columnBoxes(cells),
          if (trailing != null) ...[
            const SizedBox(width: Spacing.lg),
            SizedBox(width: trailingWidth),
          ],
        ],
      ),
    );
  }

  Widget _dataRow(BuildContext context, T row) {
    final cells = <Widget>[
      for (final column in columns) column.buildCell(context, row),
    ];
    Widget content = Padding(
      padding: Insets.row,
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.center,
        children: [
          if (leading != null) ...[
            SizedBox(width: leadingWidth, child: leading!(context, row)),
            const SizedBox(width: Spacing.lg),
          ],
          ..._columnBoxes(cells),
          if (trailing != null) ...[
            const SizedBox(width: Spacing.lg),
            SizedBox(width: trailingWidth, child: trailing!(context, row)),
          ],
        ],
      ),
    );
    final chipStrip = chips;
    if (chipStrip != null) {
      content = Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          content,
          Padding(
            padding: const EdgeInsets.fromLTRB(
              Spacing.lg,
              0,
              Spacing.lg,
              Spacing.sm,
            ),
            child: chipStrip(context, row),
          ),
        ],
      );
    }
    final tap = onTap;
    if (tap == null) return content;
    return InkWell(onTap: () => tap(row), child: content);
  }
}

// ---------------------------------------------------------------------------
// Incremental loading
// ---------------------------------------------------------------------------

/// The one incremental list shell: render [items] through [itemBuilder], showing
/// [initialCount] first and revealing [pageSize] more per "Show more" step. Used
/// by [TamuTable], and usable on its own for a tile-based viewer (logs, files,
/// backups) that is not tabular. It fills its parent.
class TamuPagedList<T> extends StatefulWidget {
  const TamuPagedList({
    super.key,
    required this.items,
    required this.itemBuilder,
    this.pageSize,
    this.initialCount,
    this.header,
    this.padding,
    this.emptyMessage,
  });

  final List<T> items;

  /// Builds one row; `index` is its position in the visible list.
  final Widget Function(BuildContext context, T item, int index) itemBuilder;

  /// Items revealed per step; null shows every item at once.
  final int? pageSize;

  /// Items shown before the first step; defaults to [pageSize].
  final int? initialCount;

  /// A fixed header drawn above the scrolling list (a table's header row).
  final Widget? header;

  /// Padding around the list; defaults to [Insets.page].
  final EdgeInsets? padding;

  /// The empty state shown when [items] is empty.
  final String? emptyMessage;

  @override
  State<TamuPagedList<T>> createState() => _TamuPagedListState<T>();
}

class _TamuPagedListState<T> extends State<TamuPagedList<T>> {
  int _shown = 0;

  @override
  void initState() {
    super.initState();
    _shown = _initial();
  }

  @override
  void didUpdateWidget(TamuPagedList<T> oldWidget) {
    super.didUpdateWidget(oldWidget);
    // A shorter list must not leave the view claiming rows it no longer has.
    if (widget.items.length < _shown) _shown = _initial();
  }

  /// The count shown before any "show more" step.
  int _initial() {
    final page = widget.pageSize;
    if (page == null) return widget.items.length;
    return math.min(widget.initialCount ?? page, widget.items.length);
  }

  void _showMore() {
    final page = widget.pageSize;
    if (page == null) return;
    setState(() => _shown = math.min(_shown + page, widget.items.length));
  }

  @override
  Widget build(BuildContext context) {
    final items = widget.items;
    if (items.isEmpty) {
      return StateMessage.empty(message: widget.emptyMessage);
    }
    final page = widget.pageSize;
    final allShown = page == null || _shown >= items.length;
    final count = allShown ? items.length : _shown;
    final header = widget.header;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        ?header,
        Expanded(
          child: ListView.builder(
            padding: widget.padding ?? Insets.page,
            itemCount: count,
            itemBuilder: (context, index) =>
                widget.itemBuilder(context, items[index], index),
          ),
        ),
        if (!allShown) _footer(context, count, items.length),
      ],
    );
  }

  Widget _footer(BuildContext context, int shown, int total) {
    return Padding(
      padding: const EdgeInsets.symmetric(
        horizontal: Spacing.lg,
        vertical: Spacing.sm,
      ),
      child: Row(
        children: [
          Expanded(
            child: Text(
              'Showing $shown of $total',
              style: Theme.of(context).textTheme.bodySmall,
            ),
          ),
          TextButton(onPressed: _showMore, child: const Text('Show more')),
        ],
      ),
    );
  }
}

// ---------------------------------------------------------------------------
// Hex / ASCII byte view
// ---------------------------------------------------------------------------

/// The one hex/ASCII byte view: one lazy row per [bytesPerLine] bytes, with the
/// offset, the grouped hex and an ASCII gutter - all through [formatters.dart].
/// Use it for a raw file, a memory dump or any decoded fallback; [pageSize] adds
/// incremental loading for a large dump.
class HexView extends StatelessWidget {
  const HexView({
    super.key,
    required this.data,
    this.bytesPerLine = 16,
    this.showOffset = true,
    this.showAscii = true,
    this.pageSize,
    this.emptyMessage,
  });

  final List<int> data;

  /// Bytes per row (one of [widths]).
  final int bytesPerLine;

  /// Show the row's byte offset.
  final bool showOffset;

  /// Show the printable-ASCII gutter.
  final bool showAscii;

  /// Rows revealed per step; null renders the whole dump lazily.
  final int? pageSize;

  /// The empty state shown for an empty [data].
  final String? emptyMessage;

  /// The byte widths a "bytes per line" control offers.
  static const List<int> widths = [8, 16, 32, 64];

  int get _rowCount => (data.length + bytesPerLine - 1) ~/ bytesPerLine;

  @override
  Widget build(BuildContext context) {
    return TamuPagedList<int>(
      items: List<int>.generate(_rowCount, (i) => i),
      pageSize: pageSize,
      padding: Insets.page,
      emptyMessage: emptyMessage ?? 'No data',
      itemBuilder: (context, row, _) => _row(context, row),
    );
  }

  Widget _row(BuildContext context, int row) {
    final offset = row * bytesPerLine;
    final end = math.min(offset + bytesPerLine, data.length);
    final chunk = data.sublist(offset, end);
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: Spacing.hairline),
      child: SingleChildScrollView(
        scrollDirection: Axis.horizontal,
        child: Row(
          children: [
            if (showOffset) ...[
              MonoText(hex32(offset), muted: true),
              const SizedBox(width: Spacing.lg),
            ],
            MonoText(formatHexBytes(chunk)),
            if (showAscii) ...[
              const SizedBox(width: Spacing.lg),
              MonoText(formatAsciiBytes(chunk), muted: true),
            ],
          ],
        ),
      ),
    );
  }
}
