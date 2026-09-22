// Formatted viewers for known file types (Docs/App/Service views/Storage.md:
// "the viewer/editor of files formats the files based on the file name").
//
// - FileViewPage: previews one file's content (SNREG registry, LAY LED-index
//   grid, text, hex).
// - MemoryBackupView: decodes the STATLOG / SUBREQ registry backups and the
//   DT_ dynamic block table.
library;

import 'package:flutter/material.dart';

import '../core/block_registry.dart' show blockInfoFor;
import '../core/storage_client.dart' show normalizeFileName;
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
  if (upper == 'SNREG') return StorageFileType.snreg;
  if (upper == 'STATLOG' || upper == 'SUBREQ') return StorageFileType.backup;
  // Per-block dynamic persistence (Docs/Services/Register.md: DT_XXX table / DV_XXX values).
  if (upper.startsWith('DT_')) return StorageFileType.dynamicTable;
  if (upper.startsWith('DV_')) return StorageFileType.dynamicValues;
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

  /// The device's static blocks in registry order (STATLOG decoder maps its
  /// block indexes onto these). null when unknown.
  final List<({int type, int inst, BlockMeta meta, String name})?>? blocks;

  const FileViewPage(
      {super.key,
      required this.deviceId,
      required this.name,
      required this.size,
      required this.data,
      this.blocks});

  @override
  State<FileViewPage> createState() => _FileViewPageState();
}

class _FileViewPageState extends State<FileViewPage> {
  bool _showRaw = false;
  int _bytesPerLine = 16;

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

  /// Standard raw view: one row per N bytes with offset, grouped hex and ASCII.
  Widget _hexView() {
    final data = widget.data!;
    if (data.isEmpty) return const Center(child: Text('(empty file)'));
    final per = _bytesPerLine;
    final rows = <Widget>[];
    for (var off = 0; off < data.length; off += per) {
      final end = (off + per) < data.length ? off + per : data.length;
      final chunk = data.sublist(off, end);
      final hex = [
        for (var i = 0; i < chunk.length; i++)
          chunk[i].toRadixString(16).padLeft(2, '0')
      ].join(' ');
      final ascii = chunk.map((b) =>
          (b >= 0x20 && b < 0x7F) ? String.fromCharCode(b) : '.').join();
      rows.add(Row(mainAxisSize: MainAxisSize.min, children: [
        SizedBox(
            width: 82,
            child: Text('0x${off.toRadixString(16).padLeft(8, '0')}',
                style: const TextStyle(
                    fontFamily: 'monospace', fontSize: 11, color: Colors.white38))),
        const SizedBox(width: 8),
        Text(hex,
            style: const TextStyle(fontFamily: 'monospace', fontSize: 11)),
        const SizedBox(width: 12),
        Text(ascii,
            style: const TextStyle(
                fontFamily: 'monospace', fontSize: 11, color: Colors.white54)),
      ]));
    }
    // Rows shrink-wrap so the 32/64-byte widths scroll horizontally instead of
    // being squeezed into the viewport width.
    return SingleChildScrollView(
      padding: const EdgeInsets.all(12),
      scrollDirection: Axis.horizontal,
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: rows),
    );
  }

  /// SNREG: 32-byte RegistryEntry records (SNDB.h): u16 valid marker, u16 short
  /// ID, 12 reserved bytes, 14-byte serial number. Markers: 0x55AA valid,
  /// 0x0000 removed, 0xFFFF unwritten.
  Widget _snregView() {
    final data = widget.data!;
    final rows = <Widget>[];
    for (var off = 0; off + 32 <= data.length; off += 32) {
      final valid = data[off] | (data[off + 1] << 8);
      if (valid == 0xFFFF) break; // unwritten slot
      final id = data[off + 2] | (data[off + 3] << 8);
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
  /// (Docs/Modules and blocks/LED display.md). Header is u8 width + u8 height (2 bytes), then
  /// W*H little-endian uint16 indexes.
  Widget _layoutView() {
    final data = widget.data!;
    if (data.length < 2) return _mono('(empty)');
    final w = data[0];
    final h = data[1];
    if (w == 0 || h == 0 || w > 128 || h > 128 || 2 + w * h * 2 > data.length) {
      return _mono('Invalid layout header (${w}x$h)');
    }
    return SingleChildScrollView(
      padding: const EdgeInsets.all(12),
      scrollDirection: Axis.horizontal,
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Text('${w}x$h LEDs',
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
                  final i = 2 + (r * w + c) * 2;
                  final v = data[i] | (data[i + 1] << 8);
                  return v == 0xFFFF ? '-' : '$v';
                }(),
                    style: TextStyle(
                        fontSize: 10,
                        color:
                            data[2 + (r * w + c) * 2] == 0xFF &&
                                    data[2 + (r * w + c) * 2 + 1] == 0xFF
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
      case StorageFileType.backup:
        return MemoryBackupView(
            fileName: widget.name, data: data, blocks: widget.blocks);
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
//   STATLOG (StaticMemory.h): u8 block_idx, u8 field, u16 pad, BlockMeta[4],
//     value[4-aligned]; block_idx 0xFF ends the log, 0xFE marks a System field.
//   SUBREQ (Subscriptions.h SaveRequesterTable): u8 count, then 26 B per entry
//     (target, source, provider, trigger + pad, period, min, deadzone).
//   DT_<hex2> (Memory.h SaveDynamicBlockFiles): u8 name_len, name, u16 type,
//     u16 entry_count, then fieldKey/flagsAndType/size/pad per entry.
// ---------------------------------------------------------------------------

class MemoryBackupView extends StatelessWidget {
  final String fileName;
  final List<int> data;

  /// The device's static blocks in registry order (used by the STATLOG decoder).
  final List<({int type, int inst, BlockMeta meta, String name})?>? blocks;

  const MemoryBackupView({super.key, required this.fileName, required this.data, this.blocks});

  static int _u16(List<int> b, int o) =>
      o + 1 < b.length ? (b[o] | (b[o + 1] << 8)) : 0;
  static int _align4(int v) => (v + 3) & ~3;

  // -------------------------------------------------------------------------
  // Dynamic block table (firmware Memory.h SaveDynamicBlockFiles): DT_<hex2>
  //   u8 name_len, name[], u16 type, u16 entry_count, then per entry
  //   u16 fieldKey (field<<8|key), u16 flagsAndType, u8 size, u8 pad.
  // The block's persistent value space lives in the sibling DV_<hex2> file.
  // -------------------------------------------------------------------------
  List<Widget> _parseDynamicTable() {
    final b = data;
    if (b.isEmpty) return [const Text('(corrupt table)')];
    var c = 0;
    final nameLen = b[c++];
    if (c + nameLen + 4 > b.length) return [const Text('(corrupt table)')];
    final name = String.fromCharCodes(b.sublist(c, c + nameLen));
    c += nameLen;
    final typeValue = _u16(b, c);
    final entryCount = _u16(b, c + 2);
    c += 4;

    final children = <Widget>[];
    for (var i = 0; i < entryCount; i++) {
      if (c + 6 > b.length) break;
      final fieldKey = _u16(b, c);
      final flagsAndType = _u16(b, c + 2);
      final size = b[c + 4];
      c += 6;
      final meta = BlockMeta(
          flagsAndType: flagsAndType, key: fieldKey & 0xFF, size: size);
      final flags = FieldFlags.describe(flagsAndType);
      children.add(ListTile(
        dense: true,
        contentPadding: const EdgeInsets.only(left: 40, right: 12),
        title: Text(
            'f${fieldKey >> 8}.k${fieldKey & 0xFF}: ${dataTypeLabel(meta.dataType)}'
            '  $size B',
            style: const TextStyle(fontFamily: 'monospace', fontSize: 12)),
        subtitle: Text(flags.isEmpty ? '(no flags)' : flags.join(' · '),
            style: const TextStyle(fontSize: 10, color: Colors.white38)),
      ));
    }
    return [
      _blockCard('${name.isEmpty ? 'Block' : name} (${blockTypeLabel(typeValue)})',
          '$entryCount entries', children)
    ];
  }


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

  String _formatBytes(BlockMeta meta, List<int> bytes) {
    if (meta.dataType == DataType.none) return '∅';
    return formatValue(meta.dataType, bytes);
  }

  // ---------------------------------------------------------------------------
  // STATLOG decoder - the static-block backup (firmware StaticMemory.h): a
  // sequential log of BlockIndex[4] + BlockMeta[4] + value[4-aligned] entries.
  // BlockIndex.block == 0xFF ends the log, 0xFE marks a System-block entry
  // (field 6 = Name, 7 = NetID); other indexes address the static registry.
  // ---------------------------------------------------------------------------

  static const int _systemBackup = 0xFE;
  static const int _systemNameField = 6;
  static const int _systemNetIdField = 7;

  List<Widget> _parseStatlog() {
    final rows = <Widget>[];
    final b = data;
    if (b.length < 8) return [const Text('(corrupt backup)')];
    var c = 0;
    while (c + 8 <= b.length) {
      final blockIdx = b[c];
      if (blockIdx == 0xFF) break; // end of log
      final field = b[c + 1];
      final meta = BlockMeta.fromBytes(b, c + 4);
      final el = 8 + _align4(meta.size);
      if (c + el > b.length) break;
      final value = b.sublist(c + 8, c + 8 + meta.size);
      c += el;

      String title;
      String subtitle;
      if (blockIdx == _systemBackup) {
        title = field == _systemNameField
            ? 'System Name'
            : (field == _systemNetIdField ? 'NetID' : 'System field $field');
        subtitle = 'System block';
      } else {
        final blk = (blocks != null && blockIdx < blocks!.length)
            ? blocks![blockIdx]
            : null;
        final info = blk == null
            ? null
            : blockInfoFor(BlockType.fromValue(blk.type));
        title = '${(blk?.name ?? 'Block $blockIdx').trim()}'
            '${info?.field(field) == null ? '' : ' · ${info!.field(field)!.name}'}';
        subtitle = blk == null
            ? 'static block $blockIdx'
            : BlockType.fromValue(blk.type).label;
      }
      rows.add(ListTile(
        dense: true,
        contentPadding: const EdgeInsets.only(left: 16, right: 12),
        title: Row(children: [
          Expanded(child: Text('$title: ${_formatBytes(meta, value)}',
              style: const TextStyle(fontFamily: 'monospace', fontSize: 12))),
        ]),
        subtitle: Text(subtitle, style: const TextStyle(fontSize: 10, color: Colors.white38)),
      ));
    }
    if (rows.isEmpty) return [const Text('(no entries)')];
    return rows;
  }

  // ---------------------------------------------------------------------------
  // SUBREQ decoder - the requester-subscription backup (firmware
  // Subscriptions.h SaveRequesterTable): u8 count, then per entry (26 B)
  // targetReg u32, sourceReg u32, providerAddr u16, trigger u8 + 3 pad,
  // periodMs u32, minTimeMs u32, deadzone Number (16.16).
  // ---------------------------------------------------------------------------

  static String _regLabel(int reg) =>
      '${blockTypeLabel(blockInfoType(reg))}[${blockInfoInstance(reg)}]'
      '.f${blockInfoField(reg)}.k${blockInfoKey(reg)}';

  List<Widget> _parseSubreq() {
    final b = data;
    if (b.isEmpty) return [const Text('(corrupt backup)')];
    final count = b[0];
    if (count > 16) return [Text('(corrupt: $count entries claimed)')];
    if (count == 0) return [const Text('(no subscriptions)')];
    final rows = <Widget>[];
    var c = 1;
    for (var i = 0; i < count && c + 26 <= b.length; i++) {
      final targetReg = uint32FromBytes(b, c); c += 4;
      final sourceReg = uint32FromBytes(b, c); c += 4;
      final providerAddr = b[c] | (b[c + 1] << 8); c += 2;
      final trigger = b[c]; c += 4; // trigger + 3 pad bytes
      final periodMs = uint32FromBytes(b, c); c += 4;
      final minTimeMs = uint32FromBytes(b, c); c += 4;
      final deadzone = numberFromBytes(b, c); c += 4;

      final triggerLabel = TriggerType.fromValue(trigger).label;
      final dz = deadzone == 0 ? '' : '  ·  deadzone $deadzone';
      rows.add(ListTile(
        dense: true,
        leading: const Icon(Icons.sync_alt, size: 20, color: kOrange),
        title: Text('#${i + 1}  ${_regLabel(targetReg)}  <-  ${_regLabel(sourceReg)}',
            style: const TextStyle(fontFamily: 'monospace', fontSize: 12)),
        subtitle: Text('$triggerLabel  ·  every $periodMs ms  ·  '
            'min $minTimeMs ms$dz  ·  provider ${idToString(providerAddr)}',
            style: const TextStyle(fontSize: 11)),
      ));
    }
    return rows;
  }

  @override
  Widget build(BuildContext context) {
    final upper = normalizeFileName(fileName).toUpperCase();
    final List<Widget> rows = switch (upper) {
          'STATLOG' => _parseStatlog(),
          'SUBREQ' => _parseSubreq(),
          _ when upper.startsWith('DT_') => _parseDynamicTable(),
          _ => [const Text('(unknown registry file)')],
        };
    if (rows.isEmpty) return const Center(child: Text('(empty backup)'));
    return ListView(
        padding: const EdgeInsets.all(12),
        children: [for (final r in rows) r]);
  }
}