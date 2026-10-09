// Formatted viewers for known file types (Docs/App/Service views/Storage.md:
// "the viewer/editor of files formats the files based on the file name").
//
// - FileViewPage: previews one file's content (SNREG registry, LAY LED-index
//   grid, text, hex).
// - MemoryBackupView: decodes the `.SV` / SUBREQ registry backups and the
//   .DT_ dynamic block table.
library;

import 'package:flutter/material.dart';

import '../core/block_registry.dart' show blockInfoFor;
import '../core/storage_client.dart' show normalizeFileName;
import '../core/device_backup.dart';
import '../core/system_schema.dart' show hasNetIdFor;
import '../core/register_client.dart';
import '../core/types.dart';
import 'theme.dart' show kOrange, kSurfaceAlt;
import 'value_editor.dart' show dataTypeLabel, formatValue;

// ---------------------------------------------------------------------------
// Known file types (Docs/Services/Storage.md: "the viewer/editor of files
// formats the files based on the file name")
// ---------------------------------------------------------------------------

enum StorageFileType {
  snreg,
  layout,
  text,
  binary,
  dynamicTable,
  dynamicValues,
  backup,
}

StorageFileType storageFileType(String name) {
  final upper = normalizeFileName(name).toUpperCase();
  if (upper == '.SNREG') return StorageFileType.snreg;
  if (upper == '.SV' || upper == '.SUBREQ' || upper == 'SUBREQ') return StorageFileType.backup;
  // Per-block dynamic persistence (Docs/Services/Register.md: .DT_XXX table / .DV_XXX values).
  if (upper.startsWith('.DT_')) return StorageFileType.dynamicTable;
  if (upper.startsWith('.DV_')) return StorageFileType.dynamicValues;
  if (upper.startsWith('LAY') || upper.endsWith('.LAY')) {
    return StorageFileType.layout;
  }
  if (upper.endsWith('.TXT') || upper.endsWith('.LOG')) {
    return StorageFileType.text;
  }
  return StorageFileType.binary;
}

IconData storageFileIcon(String name) => switch (storageFileType(name)) {
      StorageFileType.snreg => Icons.badge_outlined,
      StorageFileType.layout => Icons.grid_on_outlined,
      StorageFileType.text => Icons.description_outlined,
      StorageFileType.binary => Icons.insert_drive_file_outlined,
      StorageFileType.dynamicTable => Icons.table_chart_outlined,
      StorageFileType.dynamicValues => Icons.storage_outlined,
      StorageFileType.backup => Icons.storage_outlined,
    };

String fileTypeLabel(String name) => switch (storageFileType(name)) {
      StorageFileType.snreg => 'Serial registry',
      StorageFileType.layout => 'LED layout',
      StorageFileType.text => 'Text',
      StorageFileType.binary => 'Binary',
      StorageFileType.dynamicTable => 'Dynamic block table',
      StorageFileType.dynamicValues => 'Dynamic values',
      StorageFileType.backup => 'Registry backup',
    };

/// 32-bit hex formatting shared by the storage page rows and the backup decoder.
String hex32(int value) =>
    '0x${value.toRadixString(16).padLeft(8, '0').toUpperCase()}';

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

  const FileViewPage(
      {super.key,
      required this.deviceId,
      required this.name,
      required this.size,
      required this.data,
      this.blocks,
      this.staticFields = const {}});

  @override
  State<FileViewPage> createState() => _FileViewPageState();
}

class _FileViewPageState extends State<FileViewPage> {
  bool _showRaw = false;
  int _bytesPerLine = 16;
  StaticFieldLayout _staticFields = const {};

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
  }

  Future<void> _loadStaticFields() async {
    final fields = await RegisterClient(deviceId: widget.deviceId).readStaticFieldLayout();
    if (mounted) setState(() => _staticFields = fields);
  }

  static final List<int> bytesPerLineOptions = [8, 16, 32, 64];

  bool get _hasFormatted =>
      storageFileType(widget.name) != StorageFileType.binary;

  bool get _looksTextual {
    final data = widget.data;
    if (data == null || data.isEmpty) return false;
    var printable = 0;
    for (final b in data) {
      final isPrintable = (b >= 0x20 && b < 0x7F) ||
          b == 0x0A || // \n
          b == 0x0D; // \r
      if (isPrintable) printable++;
    }
    return printable / data.length > 0.9;
  }

  Widget _mono(String text) => SingleChildScrollView(
      padding: const EdgeInsets.all(12),
      child: SelectableText(text,
          style: const TextStyle(fontFamily: 'monospace', fontSize: 12)));

  /// Standard raw view: one lazily-built row per N bytes with offset, grouped hex and ASCII.
  /// Each row scrolls horizontally so 32/64-byte widths scroll sideways instead of overflowing.
  Widget _hexView() {
    final data = widget.data!;
    if (data.isEmpty) return const Center(child: Text('(empty file)'));
    final per = _bytesPerLine;
    final lines = (data.length + per - 1) ~/ per;
    return ListView.builder(
      padding: const EdgeInsets.all(12),
      itemCount: lines,
      itemBuilder: (context, line) {
        final off = line * per;
        final end = (off + per) < data.length ? off + per : data.length;
        final chunk = data.sublist(off, end);
        final hex = [
          for (var i = 0; i < chunk.length; i++)
            chunk[i].toRadixString(16).padLeft(2, '0')
        ].join(' ');
        final ascii = chunk
            .map((b) => (b >= 0x20 && b < 0x7F) ? String.fromCharCode(b) : '.')
            .join();
        return SingleChildScrollView(
          scrollDirection: Axis.horizontal,
          child: Row(mainAxisSize: MainAxisSize.min, children: [
            SizedBox(
                width: 82,
                child: Text('0x${off.toRadixString(16).padLeft(8, '0')}',
                    style: const TextStyle(
                        fontFamily: 'monospace',
                        fontSize: 11,
                        color: Colors.white38))),
            const SizedBox(width: 8),
            Text(hex,
                style: const TextStyle(fontFamily: 'monospace', fontSize: 11)),
            const SizedBox(width: 12),
            Text(ascii,
                style: const TextStyle(
                    fontFamily: 'monospace',
                    fontSize: 11,
                    color: Colors.white54)),
          ]),
        );
      },
    );
  }

  /// SNREG: 32-byte RegistryEntry records (SNDB.h): u16 valid marker, u16 device ID,
  /// u8 owning net, 11 reserved bytes, 14-byte serial number. The stored pair is
  /// net.device: local-net devices carry net 0, other cores their NetID with device 1.
  /// Markers: 0x55AA valid, 0x0000 removed, 0xFFFF unwritten.
  Widget _snregView() {
    final data = widget.data!;
    final rows = <Widget>[];
    for (var off = 0; off + 32 <= data.length; off += 32) {
      final valid = data[off] | (data[off + 1] << 8);
      if (valid == 0xFFFF) break; // unwritten slot
      final device = data[off + 2] | (data[off + 3] << 8);
      final net = data[off + 4] & 0x3F;
      final id = (net << 10) | (device & 0x3FF);
      final removed = valid == 0x0000;
      final sn = serialNumberToHex(data.sublist(off + 16, off + 30));
      rows.add(Padding(
        padding: const EdgeInsets.symmetric(vertical: 2),
        child: Row(children: [
          SizedBox(
              width: 88,
              child: Text(removed ? 'id $id (removed)' : idToString(id),
                  style: TextStyle(
                      color: removed ? Colors.white24 : kOrange,
                      fontSize: 12))),
          Expanded(
              child: Text(sn,
                  style: TextStyle(
                      fontFamily: 'monospace',
                      fontSize: 12,
                      color: removed ? Colors.white24 : Colors.white70))),
        ]),
      ));
    }
    return ListView(padding: const EdgeInsets.all(12), children: rows);
  }

  /// Layout file: row-first W x H uint16 LED indexes, 0xFFFF = missing
  /// (Docs/Modules and blocks/LED display.md). Header is u8 brightness limit (0-255 as a
  /// percentage) + u8 width + u8 height (3 bytes), then W*H little-endian uint16 indexes.
  Widget _layoutView() {
    final data = widget.data!;
    if (data.length < 3) return _mono('(empty)');
    final limit = data[0];
    final w = data[1];
    final h = data[2];
    if (w == 0 || h == 0 || w > 128 || h > 128 || 3 + w * h * 2 > data.length) {
      return _mono('Invalid layout header (${w}x$h)');
    }
    final limitPct = (limit * 100 + 127) ~/ 255;
    return SingleChildScrollView(
      padding: const EdgeInsets.all(12),
      scrollDirection: Axis.horizontal,
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Text('${w}x$h LEDs · limit $limit ($limitPct%)',
            style: const TextStyle(color: Colors.white38, fontSize: 11)),
        const SizedBox(height: 6),
        for (var r = 0; r < h; r++)
          Row(children: [
            for (var c = 0; c < w; c++)
              Container(
                width: 44,
                height: 22,
                margin: const EdgeInsets.all(1),
                color: kSurfaceAlt,
                alignment: Alignment.center,
                child: Text(() {
                  final i = 3 + (r * w + c) * 2;
                  final v = data[i] | (data[i + 1] << 8);
                  return v == 0xFFFF ? '-' : '$v';
                }(),
                    style: TextStyle(
                        fontSize: 10,
                        color:
                            data[3 + (r * w + c) * 2] == 0xFF &&
                                    data[3 + (r * w + c) * 2 + 1] == 0xFF
                                ? Colors.white24
                                : Colors.white)),
              ),
          ]),
      ]),
    );
  }

  Widget _formattedBody() {
    final data = widget.data!;
    switch (storageFileType(widget.name)) {
      case StorageFileType.snreg:
        return _snregView();
      case StorageFileType.layout:
        return _layoutView();
      case StorageFileType.dynamicTable:
      case StorageFileType.backup: {
        // The System segment is 20 B on a core (Name + NetID, padded) and 16 B on a node; the
        // System block's field count (>= 8 includes NetID) tells them apart.
        final hasNetId = hasNetIdFor(widget.blocks);
        return MemoryBackupView(
            fileName: widget.name,
            data: data,
            blocks: widget.blocks,
            staticFields: _staticFields,
            hasNetId: hasNetId);
      }
      case StorageFileType.dynamicValues:
        // The persistent value space, addressable only together with its DT table.
        return _hexView();
      case StorageFileType.text when _looksTextual:
        final text = String.fromCharCodes(data).replaceAll(RegExp(r' +'), ' ');
        return _mono(text);
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
          body: const Center(child: Text('Read failed')));
    }
    final raw = _showRaw || !_hasFormatted;
    return Scaffold(
      appBar: AppBar(
        title: Text('${normalizeFileName(widget.name)}'
            ' - ${fileTypeLabel(widget.name)} (${widget.size} B)'),
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
                for (final n in bytesPerLineOptions)
                  PopupMenuItem(value: n, child: Text('$n bytes per line')),
              ],
            ),
        ],
      ),
      body: raw ? _hexView() : _formattedBody(),
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

  const MemoryBackupView(
      {super.key,
      required this.fileName,
      required this.data,
      this.blocks,
      this.staticFields = const {},
      this.hasNetId = true});

  @override
  State<MemoryBackupView> createState() => _MemoryBackupViewState();
}

class _MemoryBackupViewState extends State<MemoryBackupView> {
  // Convenience accessors so the decoders below read as before.
  String get fileName => widget.fileName;
  List<int> get data => widget.data;
  List<({int type, int inst, ValueInfo meta, String name})?>? get blocks => widget.blocks;
  StaticFieldLayout get staticFields => widget.staticFields;
  bool get hasNetId => widget.hasNetId;

  /// The `.SV` layout is computed once per opened file instead of on every rebuild.
  late final StaticSpaceLayout _svLayout = StaticSpaceLayout.fromRegistry(
      staticRegistryOf(blocks), staticFields,
      hasNetId: hasNetId);

  Widget _blockCard(String title, String subtitle, List<Widget> children) {
    return Card(
      color: kSurfaceAlt,
      margin: const EdgeInsets.only(bottom: 6),
      clipBehavior: Clip.antiAlias,
      child: Column(children: [
        ListTile(
          dense: true,
          title: Text(title, style: const TextStyle(fontWeight: FontWeight.w600)),
          subtitle: Text(subtitle, style: const TextStyle(fontSize: 10)),
        ),
        if (children.isNotEmpty)
          Material(color: Colors.black26, child: Column(children: children)),
      ]),
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

  List<Widget> _parseDynamicTable() {
    final table = decodeDynamicTable(data);
    if (table == null) return [const Text('(corrupt table)')];

    final children = <Widget>[];
    for (final e in table.entries) {
      final flags = ValueFlags.describe(e.flags);
      children.add(ListTile(
        dense: true,
        contentPadding: const EdgeInsets.only(left: 40, right: 12),
        title: Text(
            'f${e.field}.k${e.key}: ${dataTypeLabel(e.meta.dataType)}  ${e.size} B',
            style: const TextStyle(fontFamily: 'monospace', fontSize: 12)),
        subtitle: Text(flags.isEmpty ? '(no flags)' : flags.join(' · '),
            style: const TextStyle(fontSize: 10, color: Colors.white38)),
      ));
    }
    final idx = _dynamicIndex();
    final typeLabel = idx == null ? '' : ' (${blockTypeLabel(dynamicTypeForIndex(idx))})';
    return [
      _blockCard(
          '${table.name.isEmpty ? 'Block' : table.name}$typeLabel',
          '${table.entries.length} entries',
          children)
    ];
  }

  // -------------------------------------------------------------------------
  // `.SV` - the static-block + System backup, decoded by device_backup.dart.
  // -------------------------------------------------------------------------
  List<Widget> _parseSv() {
    final registry = staticRegistryOf(blocks);
    final rows = <Widget>[];
    for (final e in decodeSv(data, _svLayout, registry)) {
      String title;
      String subtitle;
      if (e.blockType == systemBlockTypeValue) {
        title = e.field == systemNameField
            ? 'System Name'
            : (e.field == systemNetIdField ? 'NetID' : 'System field ${e.field}');
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
        title = '${(blk?.name ?? 'Block ${e.blockType}').trim()}'
            '${info?.field(e.field) == null ? '' : ' · ${info!.field(e.field)!.name}'}';
        subtitle = BlockType.fromValue(e.blockType).label;
      }
      rows.add(ListTile(
        dense: true,
        contentPadding: const EdgeInsets.only(left: 16, right: 12),
        title: Row(children: [
          Expanded(child: Text('$title: ${_formatBytes(e.meta, e.value)}',
              style: const TextStyle(fontFamily: 'monospace', fontSize: 12))),
        ]),
        subtitle: Text(subtitle, style: const TextStyle(fontSize: 10, color: Colors.white38)),
      ));
    }
    if (rows.isEmpty) return [const Text('(no entries)')];
    return rows;
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

    final rows = <Widget>[];
    final entries = decodeSubreq(data);
    for (var i = 0; i < entries.length; i++) {
      final e = entries[i];
      final triggerLabel = TriggerType.fromValue(e.trigger).label;
      final dz = e.deadzone == 0 ? '' : '  ·  deadzone ${e.deadzone}';
      rows.add(ListTile(
        dense: true,
        leading: const Icon(Icons.sync_alt, size: 20, color: kOrange),
        title: Text('#${i + 1}  ${_regLabel(e.targetReg)}  <-  ${_regLabel(e.sourceReg)}',
            style: const TextStyle(fontFamily: 'monospace', fontSize: 12)),
        subtitle: Text('$triggerLabel  ·  every ${e.periodMs} ms  ·  '
            'min ${e.minTimeMs} ms$dz  ·  provider ${idToString(e.providerAddr)}',
            style: const TextStyle(fontSize: 11)),
      ));
    }
    return rows;
  }

  @override
  Widget build(BuildContext context) {
    final upper = normalizeFileName(fileName).toUpperCase();
    final List<Widget> rows = switch (upper) {
          '.SV' => _parseSv(),
          '.SUBREQ' => _parseSubreq(),
          'SUBREQ' => _parseSubreq(),
          _ when upper.startsWith('.DT_') => _parseDynamicTable(),
          _ => [const Text('(unknown registry file)')],
        };
    if (rows.isEmpty) return const Center(child: Text('(empty backup)'));
    return ListView(
        padding: const EdgeInsets.all(12),
        children: [for (final r in rows) r]);
  }
}
