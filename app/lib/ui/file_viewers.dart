// Formatted viewers for known file types (Docs/App/Service views/Storage.md:
// "the viewer/editor of files formats the files based on the file name").
//
// - FileViewPage: previews one file's content (SNREG registry, LAY LED-index
//   grid, text, hex).
// - MemoryBackupView: decodes the `.SV` / SUBREQ registry backups and the
//   .DT_ dynamic block table.
//
// Both render through `viewer_kit.dart` - the one table shell ([TamuTable]), the
// one hex/ASCII view ([HexView]) and the one chip row ([TamuChipRow]) - with every
// string from `formatters.dart` and every colour, size and inset from `theme.dart`.
// What stays local is only what the toolkit does not cover, and each such shape says
// why in place: the LAY grid (a spatial LED map, not rows of columns), the selectable
// text preview and a monospace cell that prints a struck value.
library;

import 'package:flutter/material.dart';

import '../core/block_registry.dart' show blockInfoFor;
import '../core/device_backup.dart';
import '../core/register_client.dart';
import '../core/storage_client.dart' show StorageClient, normalizeFileName;
import '../core/system_schema.dart' show hasNetIdFor;
import '../core/types.dart';
import 'formatters.dart';
import 'theme.dart';
import 'viewer_kit.dart';
import 'widgets.dart' show BlockScrollBody;

// ---------------------------------------------------------------------------
// Known file types (Docs/Services/Storage.md: "the viewer/editor of files
// formats the files based on the file name")
// ---------------------------------------------------------------------------

/// The icon a stored file's type is shown with.
IconData storageFileIcon(String name) => switch (storageFileType(name)) {
  StorageFileType.snreg => Icons.badge_outlined,
  StorageFileType.layout => Icons.grid_on_outlined,
  StorageFileType.text => Icons.description_outlined,
  StorageFileType.binary => Icons.insert_drive_file_outlined,
  StorageFileType.dynamicTable => Icons.table_chart_outlined,
  StorageFileType.dynamicValues => Icons.storage_outlined,
  StorageFileType.backup => Icons.storage_outlined,
};

// ---------------------------------------------------------------------------
// Table rows the decoders below print
// ---------------------------------------------------------------------------

/// One `.SNREG` record: the id its 32-byte entry resolves to and the serial it
/// carries. A tombstoned entry keeps its row but is drawn as unavailable.
class _SnregRow {
  const _SnregRow(this.id, this.serial, {this.removed = false});

  final String id;
  final String serial;
  final bool removed;
}

/// One field/key row of a decoded table: what the entry is, its type, and the
/// value (a decoded value, a flag list or a block/type name).
class _FieldRow {
  const _FieldRow({
    required this.label,
    required this.type,
    required this.value,
  });

  final String label;
  final String type;
  final String value;
}

/// One requester-subscription row: the route it copies and how it is triggered.
class _SubreqRow {
  const _SubreqRow(this.route, this.transfer);

  final String route;
  final String transfer;
}

/// The one field/key table the file viewer and both backup decoders print.
Widget _fieldTable({
  required List<_FieldRow> rows,
  bool embedded = true,
  bool showHeader = false,
}) => TamuTable<_FieldRow>(
  embedded: embedded,
  showHeader: showHeader,
  columns: [
    TamuColumn<_FieldRow>(
      label: 'Field',
      text: (r) => r.label,
      mono: true,
      flex: 2,
    ),
    TamuColumn<_FieldRow>(
      label: 'Type',
      text: (r) => r.type,
      muted: true,
      flex: 2,
    ),
    TamuColumn<_FieldRow>(
      label: 'Value',
      text: (r) => r.value,
      mono: true,
      flex: 3,
    ),
  ],
  rows: rows,
);

/// Formatted preview of one file's content. Known types render decoded
/// (SNREG registry, LAY LED-index grid, backup registries, text); a raw hex
/// table (8/16/32/64 bytes per line) is one app-bar toggle away.
class FileViewPage extends StatefulWidget {
  final int deviceId;
  final String name;
  final int size;
  final List<int>? data;

  /// The device's static blocks in registry order (the `.SV` decoder computes its
  /// field layout from these). null when unknown.
  final List<({int type, int inst, ValueInfo meta, String name})?>? blocks;

  /// The static blocks' persistent fields (the `.SV` layout source). Read from the device
  /// when not supplied.
  final StaticFieldLayout staticFields;

  const FileViewPage({
    super.key,
    required this.deviceId,
    required this.name,
    required this.size,
    required this.data,
    this.blocks,
    this.staticFields = const {},
  });

  @override
  State<FileViewPage> createState() => _FileViewPageState();
}

class _FileViewPageState extends State<FileViewPage> {
  bool _showRaw = false;
  int _bytesPerLine = 16;
  StaticFieldLayout _staticFields = const {};
  List<int>?
  _dvTable; // sibling `.DT_<hex2>` bytes, for the `.DV_` value viewer

  // The LED grid's cell box: a fixed size so a 128-wide layout scrolls sideways by
  // pixels instead of reflowing. These are the grid's dimensions, not spacing steps.
  static const double _ledCellWidth = 44;
  static const double _ledCellHeight = 22;

  @override
  void initState() {
    super.initState();
    _staticFields = widget.staticFields;
    // Only the `.SV` decoder needs the static field layout; skip the read for the other
    // file types (it is a device round-trip per opened file).
    if (_staticFields.isEmpty &&
        storageFileType(widget.name) == StorageFileType.backup) {
      _loadStaticFields();
    }
    // The `.DV_` value viewer decodes against the sibling `.DT_` table.
    if (storageFileType(widget.name) == StorageFileType.dynamicValues) {
      _loadDynamicTable();
    }
  }

  Future<void> _loadStaticFields() async {
    final fields = await RegisterClient(
      deviceId: widget.deviceId,
    ).readStaticFieldLayout();
    if (mounted) setState(() => _staticFields = fields);
  }

  int? _dvIndex() {
    final n = normalizeFileName(widget.name).toUpperCase();
    if (!n.startsWith('.DV_')) return null;
    return int.tryParse(n.substring(4), radix: 16);
  }

  Future<void> _loadDynamicTable() async {
    final inst = _dvIndex();
    if (inst == null) return;
    final name = '.DT_${inst.toRadixString(16).padLeft(2, '0').toUpperCase()}';
    final bytes = await StorageClient(deviceId: widget.deviceId).readFile(name);
    if (mounted) setState(() => _dvTable = bytes);
  }

  bool get _hasFormatted =>
      storageFileType(widget.name) != StorageFileType.binary;

  bool get _looksTextual {
    final data = widget.data;
    if (data == null || data.isEmpty) return false;
    var printable = 0;
    for (final b in data) {
      final isPrintable =
          (b >= 0x20 && b < 0x7F) ||
          b == 0x0A || // \n
          b == 0x0D; // \r
      if (isPrintable) printable++;
    }
    return printable / data.length > 0.9;
  }

  /// A plain-text file's preview, or a decoder's one-line explanation. Kept local
  /// because the preview must stay **selectable** - a log or a text file is
  /// something you copy out of the app - and [MonoText] is a plain `Text`. The look
  /// still comes from the theme's body role, not from a literal size.
  Widget _textPreview(BuildContext context, String text) =>
      SingleChildScrollView(
        padding: Insets.page,
        child: SelectableText(
          text,
          style: Theme.of(context).textTheme.bodySmall?.copyWith(
            fontFamily: 'monospace',
            color: kTextPrimary,
          ),
        ),
      );

  /// The raw view: the toolkit's [HexView], one lazily-built row per N bytes with
  /// the offset, grouped hex and ASCII, each row scrolling sideways so 32/64-byte
  /// widths scroll instead of overflowing.
  Widget _hexView() => HexView(
    data: widget.data!,
    bytesPerLine: _bytesPerLine,
    emptyMessage: '(empty file)',
  );

  /// SNREG: 32-byte RegistryEntry records (SNDB.h): u16 valid marker, u16 device ID,
  /// u8 owning net, 11 reserved bytes, 14-byte serial number. The stored pair is
  /// net.device: local-net devices carry net 0, other cores their NetID with device 1.
  /// Markers: 0x55AA valid, 0x0000 removed, 0xFFFF unwritten.
  Widget _snregView() {
    final data = widget.data!;
    final rows = <_SnregRow>[];
    for (var off = 0; off + 32 <= data.length; off += 32) {
      final valid = data[off] | (data[off + 1] << 8);
      if (valid == 0xFFFF) break; // unwritten slot
      final device = data[off + 2] | (data[off + 3] << 8);
      final net = data[off + 4] & 0x3F;
      final id = (net << 10) | (device & 0x3FF);
      final removed = valid == 0x0000;
      rows.add(
        _SnregRow(
          removed ? 'id $id (removed)' : idToString(id),
          serialNumberToHex(data.sublist(off + 16, off + 30)),
          removed: removed,
        ),
      );
    }
    return TamuTable<_SnregRow>(
      columns: [
        TamuColumn<_SnregRow>(
          label: 'ID',
          flex: 2,
          cell: (context, r) => Text(
            r.id,
            overflow: TextOverflow.ellipsis,
            style: Theme.of(context).textTheme.labelSmall?.copyWith(
              color: r.removed ? kTextDisabled : kOrange,
            ),
          ),
        ),
        TamuColumn<_SnregRow>(
          label: 'Serial number',
          flex: 3,
          cell: (context, r) => MonoText(
            r.serial,
            tone: r.removed ? kTextDisabled : kTextSecondary,
          ),
        ),
      ],
      rows: rows,
      emptyMessage: '(no registry entries)',
    );
  }

  /// Layout file: row-first W x H uint16 LED indexes, 0xFFFF = missing
  /// (Docs/Modules and blocks/LED display.md). Header is u8 brightness limit (0-255 as a
  /// percentage) + u8 width + u8 height (3 bytes), then W*H little-endian uint16 indexes.
  ///
  /// Stays local: the cell's *position* is the datum, so this is a spatial map
  /// rather than a table of rows, which is the one shape [TamuTable] cannot print.
  Widget _layoutView(BuildContext context) {
    final data = widget.data!;
    if (data.length < 3) return _textPreview(context, '(empty)');
    final limit = data[0];
    final w = data[1];
    final h = data[2];
    if (w == 0 || h == 0 || w > 128 || h > 128 || 3 + w * h * 2 > data.length) {
      return _textPreview(context, 'Invalid layout header (${w}x$h)');
    }
    final theme = Theme.of(context);
    final limitPct = (limit * 100 + 127) ~/ 255;
    return SingleChildScrollView(
      padding: Insets.page,
      scrollDirection: Axis.horizontal,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(
            '${w}x$h LEDs · limit $limit ($limitPct%)',
            style: theme.textTheme.bodySmall?.copyWith(color: kTextMuted),
          ),
          const SizedBox(height: Spacing.sm),
          for (var r = 0; r < h; r++)
            Row(
              children: [
                for (var c = 0; c < w; c++)
                  _ledCell(theme.textTheme, data, 3 + (r * w + c) * 2),
              ],
            ),
        ],
      ),
    );
  }

  /// One LED cell: the index a layout file holds at [at], or `-` for a missing one.
  Widget _ledCell(TextTheme text, List<int> data, int at) {
    final value = data[at] | (data[at + 1] << 8);
    final missing = value == 0xFFFF;
    return Container(
      width: _ledCellWidth,
      height: _ledCellHeight,
      margin: const EdgeInsets.all(Spacing.hairline),
      color: kSurfaceAlt,
      alignment: Alignment.center,
      child: Text(
        missing ? '-' : '$value',
        style: text.labelSmall?.copyWith(
          color: missing ? kTextDisabled : kTextPrimary,
        ),
      ),
    );
  }

  /// `.DV_<hex2>`: the block's packed persistent values, decoded against the sibling
  /// `.DT_<hex2>` table (offsets and sizes) so each field/key shows its typed value.
  Widget _dvView(BuildContext context) {
    final tableBytes = _dvTable;
    final table = tableBytes == null ? null : decodeDynamicTable(tableBytes);
    final inst = _dvIndex();
    if (table == null || inst == null) {
      // Still loading the sibling table, or it is missing: fall back to raw hex.
      return _hexView();
    }
    final entries = decodeDynamicValues(inst, table, widget.data!);
    if (entries.isEmpty) return _hexView();
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Padding(
          padding: Insets.row,
          child: Text(
            '${table.name.isEmpty ? 'Block' : table.name}'
            ' (${blockTypeLabel(dynamicTypeForIndex(inst))})',
            style: Theme.of(
              context,
            ).textTheme.bodySmall?.copyWith(color: kTextMuted),
          ),
        ),
        Expanded(
          child: _fieldTable(
            embedded: false,
            rows: [
              for (final e in entries)
                _FieldRow(
                  label: 'f${e.field}.k${e.key}',
                  type: '${dataTypeLabel(e.meta.dataType)}  ${e.meta.size} B',
                  value: e.meta.dataType == DataType.none
                      ? '∅'
                      : formatValue(e.meta.dataType, e.value),
                ),
            ],
          ),
        ),
      ],
    );
  }

  Widget _formattedBody(BuildContext context) {
    final data = widget.data!;
    switch (storageFileType(widget.name)) {
      case StorageFileType.snreg:
        return _snregView();
      case StorageFileType.layout:
        return _layoutView(context);
      case StorageFileType.dynamicTable:
      case StorageFileType.backup:
        {
          // The System segment is 20 B on a core (Name + NetID, padded) and 16 B on a node; the
          // System block's field count (>= 8 includes NetID) tells them apart.
          final hasNetId = hasNetIdFor(widget.blocks);
          return MemoryBackupView(
            fileName: widget.name,
            data: data,
            blocks: widget.blocks,
            staticFields: _staticFields,
            hasNetId: hasNetId,
          );
        }
      case StorageFileType.dynamicValues:
        // The persistent value space, decoded against its sibling `.DT_` table.
        return _dvView(context);
      case StorageFileType.text when _looksTextual:
        final text = String.fromCharCodes(data).replaceAll(RegExp(r' +'), ' ');
        return _textPreview(context, text);
      default:
        return _hexView();
    }
  }

  @override
  Widget build(BuildContext context) {
    final data = widget.data;
    if (data == null) {
      return Scaffold(
        appBar: AppBar(title: Text(widget.name)),
        body: const Center(child: Text('Read failed')),
      );
    }
    final raw = _showRaw || !_hasFormatted;
    return Scaffold(
      appBar: AppBar(
        title: Text(
          '${normalizeFileName(widget.name)}'
          ' - ${fileTypeLabel(widget.name)} (${widget.size} B)',
        ),
        actions: [
          if (_hasFormatted)
            IconButton(
              tooltip: raw ? 'Show formatted' : 'Show raw hex',
              icon: Icon(raw ? Icons.article_outlined : Icons.code),
              onPressed: () => setState(() => _showRaw = !_showRaw),
            ),
          if (raw)
            PopupMenuButton<int>(
              tooltip: 'Bytes per line',
              icon: const Icon(Icons.more_vert),
              initialValue: _bytesPerLine,
              onSelected: (v) => setState(() => _bytesPerLine = v),
              itemBuilder: (_) => [
                for (final n in HexView.widths)
                  PopupMenuItem(value: n, child: Text('$n bytes per line')),
              ],
            ),
        ],
      ),
      body: raw ? _hexView() : _formattedBody(context),
    );
  }
}

// ---------------------------------------------------------------------------
// Registry backup decoders (firmware layouts, Docs/Services/Register.md):
//   `.SV` (StaticMemory.h + the board's StaticPersistent): a raw 1:1 mirror of the static
//     persistent space, decoded by device_backup.dart from the computed field layout.
//   .SUBREQ (SubscriptionsPersist.h SaveRequesterTable): u8 count, then 24 B per entry
//     (provider, trid, subscription table [source, trigger, min u24, period, deadzone],
//     target). The timeout is not persisted.
//   .DT_<hex2> (MemoryDynamic.h SaveDynamicBlockFiles): Name (16 chars, space-padded),
//     u16 entry_count, u16 reserved, then Field&Key + MemoryOffset + ValueInfo per entry.
// ---------------------------------------------------------------------------

class MemoryBackupView extends StatefulWidget {
  final String fileName;
  final List<int> data;

  /// The device's static blocks in registry order (used by the `.SV` decoder).
  final List<({int type, int inst, ValueInfo meta, String name})?>? blocks;

  /// The static blocks' persistent fields, read from the device (the `.SV` layout source).
  final StaticFieldLayout staticFields;

  /// Whether the device has the System NetID field (a core): it changes the System segment
  /// size in the `.SV` space.
  final bool hasNetId;

  const MemoryBackupView({
    super.key,
    required this.fileName,
    required this.data,
    this.blocks,
    this.staticFields = const {},
    this.hasNetId = true,
  });

  @override
  State<MemoryBackupView> createState() => _MemoryBackupViewState();
}

class _MemoryBackupViewState extends State<MemoryBackupView> {
  // Convenience accessors so the decoders below read as before.
  String get fileName => widget.fileName;
  List<int> get data => widget.data;
  List<({int type, int inst, ValueInfo meta, String name})?>? get blocks =>
      widget.blocks;
  StaticFieldLayout get staticFields => widget.staticFields;
  bool get hasNetId => widget.hasNetId;

  /// The `.SV` layout is computed once per opened file instead of on every rebuild.
  late final StaticSpaceLayout _svLayout = StaticSpaceLayout.fromRegistry(
    staticRegistryOf(blocks),
    staticFields,
    hasNetId: hasNetId,
  );

  /// One titled card holding a decoded table: the block's name and a summary chip
  /// over the rows, the shape `register_backup_view.dart` uses for the same data.
  Widget _blockCard(
    BuildContext context,
    String title,
    String summary,
    List<_FieldRow> rows,
  ) {
    return Card(
      margin: const EdgeInsets.only(bottom: Spacing.sm),
      clipBehavior: Clip.antiAlias,
      child: Column(
        children: [
          ListTile(
            title: Text(title, style: Theme.of(context).textTheme.titleSmall),
            subtitle: TamuChipRow(chips: [TamuChip(summary)]),
          ),
          if (rows.isNotEmpty) _fieldTable(rows: rows),
        ],
      ),
    );
  }

  String _formatBytes(ValueInfo meta, List<int> bytes) {
    if (meta.dataType == DataType.none) return '∅';
    return formatValue(meta.dataType, bytes);
  }

  // -------------------------------------------------------------------------
  // Dynamic block table (.DT_<hex2>, decoded by device_backup.dart): the block's
  // persistent value space lives in the sibling .DV_<hex2> file. The table no
  // longer stores the block's type; it is derived from the file's global index.
  // -------------------------------------------------------------------------
  int? _dynamicIndex() {
    final n = normalizeFileName(fileName).toUpperCase();
    if (!n.startsWith('.DT_')) return null;
    return int.tryParse(n.substring(4), radix: 16);
  }

  List<Widget> _parseDynamicTable(BuildContext context) {
    final table = decodeDynamicTable(data);
    if (table == null) return [const Text('(corrupt table)')];

    final rows = <_FieldRow>[
      for (final e in table.entries)
        _FieldRow(
          label: 'f${e.field}.k${e.key}',
          type: '${dataTypeLabel(e.meta.dataType)}  ${e.size} B',
          value: () {
            final flags = ValueFlags.describe(e.flags);
            return flags.isEmpty ? '(no flags)' : flags.join(' · ');
          }(),
        ),
    ];
    final idx = _dynamicIndex();
    final typeLabel = idx == null
        ? ''
        : ' (${blockTypeLabel(dynamicTypeForIndex(idx))})';
    return [
      _blockCard(
        context,
        '${table.name.isEmpty ? 'Block' : table.name}$typeLabel',
        '${table.entries.length} entries',
        rows,
      ),
    ];
  }

  // -------------------------------------------------------------------------
  // `.SV` - the static-block + System backup, decoded by device_backup.dart.
  // -------------------------------------------------------------------------
  List<Widget> _parseSv() {
    final registry = staticRegistryOf(blocks);
    final rows = <_FieldRow>[];
    for (final e in decodeSv(data, _svLayout, registry)) {
      String title;
      String subtitle;
      if (e.blockType == systemBlockTypeValue) {
        title = e.field == systemNameField
            ? 'System Name'
            : (e.field == systemNetIdField
                  ? 'NetID'
                  : 'System field ${e.field}');
        subtitle = 'System block';
      } else {
        ({int type, int inst, ValueInfo meta, String name})? blk;
        for (final b in blocks ?? const []) {
          if (b != null && b.type == e.blockType && b.inst == e.inst) {
            blk = b;
            break;
          }
        }
        final info = blockInfoFor(BlockType.fromValue(e.blockType));
        title =
            '${(blk?.name ?? 'Block ${e.blockType}').trim()}'
            '${info?.field(e.field) == null ? '' : ' · ${info!.field(e.field)!.name}'}';
        subtitle = BlockType.fromValue(e.blockType).label;
      }
      rows.add(
        _FieldRow(
          label: title,
          type: subtitle,
          value: _formatBytes(e.meta, e.value),
        ),
      );
    }
    if (rows.isEmpty) return [const Text('(no entries)')];
    return [_fieldTable(rows: rows)];
  }

  // -------------------------------------------------------------------------
  // SUBREQ - the requester-subscription backup, decoded by device_backup.dart.
  // -------------------------------------------------------------------------
  static String _regLabel(int reg) => blockInfoLabel(reg);

  List<Widget> _parseSubreq() {
    if (data.isEmpty) return [const Text('(corrupt backup)')];
    final claimed = data[0];
    if (claimed > maxSubreqEntries) {
      return [Text('(corrupt: $claimed entries claimed)')];
    }
    if (claimed == 0) return [const Text('(no subscriptions)')];

    final entries = decodeSubreq(data);
    final rows = <_SubreqRow>[
      for (final (i, e) in entries.indexed)
        _SubreqRow(
          '#${i + 1}  ${_regLabel(e.targetReg)}  <-  ${_regLabel(e.sourceReg)}',
          '${TriggerType.fromValue(e.trigger).label}  ·  '
              'every ${e.periodMs} ms  ·  min ${e.minTimeMs} ms'
              '${e.deadzone == 0 ? '' : '  ·  deadzone ${e.deadzone}'}  ·  '
              'provider ${idToString(e.providerAddr)}',
        ),
    ];
    return [
      TamuTable<_SubreqRow>(
        embedded: true,
        leading: (context, r) =>
            const Icon(Icons.sync_alt, size: IconSize.md, color: kOrange),
        columns: [
          TamuColumn<_SubreqRow>(
            label: 'Target <- source',
            text: (r) => r.route,
            mono: true,
            flex: 3,
          ),
          TamuColumn<_SubreqRow>(
            label: 'Trigger  ·  period  ·  provider',
            text: (r) => r.transfer,
            muted: true,
            flex: 4,
          ),
        ],
        rows: rows,
      ),
    ];
  }

  @override
  Widget build(BuildContext context) {
    final upper = normalizeFileName(fileName).toUpperCase();
    final List<Widget> rows = switch (upper) {
      '.SV' => _parseSv(),
      '.SUBREQ' => _parseSubreq(),
      'SUBREQ' => _parseSubreq(),
      _ when upper.startsWith('.DT_') => _parseDynamicTable(context),
      _ => [const Text('(unknown registry file)')],
    };
    if (rows.isEmpty) return const Center(child: Text('(empty backup)'));
    return BlockScrollBody(children: rows);
  }
}
