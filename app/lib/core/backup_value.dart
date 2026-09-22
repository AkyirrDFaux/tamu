/// Semantic value codec for the backup format (Docs/App/Backup.md).
///
/// "The storage format is semantic (types, keys, enums, etc. are described in
/// words), numbers are not used unless it's the literal value or index."
///
/// Pure functions: raw wire bytes <-> semantic JSON. Used by the archive model
/// (`backup_format.dart`) and the capture/restore I/O (`backup.dart`).
library;

import 'dart:typed_data';

import 'block_registry.dart';
import 'render_dict.dart';
import 'types.dart';

// ---------------------------------------------------------------------------
// Flags
// ---------------------------------------------------------------------------

/// Passive value flags described in words (Docs/Services/Register.md).
List<String> flagWords(int flagsAndType) {
  final flags = flagsAndType & FieldFlags.mask;
  return [
    if (flags & FieldFlags.readOnly != 0) 'Read Only',
    if (flags & FieldFlags.persistent != 0) 'Persistent',
    if (flags & FieldFlags.trigger != 0) 'Trigger',
  ];
}

/// Rebuilds the passive flag bits from their words.
int flagsFromWords(List<String> words) {
  var flags = 0;
  for (final w in words) {
    switch (w) {
      case 'Read Only':
        flags |= FieldFlags.readOnly;
      case 'Persistent':
        flags |= FieldFlags.persistent;
      case 'Trigger':
        flags |= FieldFlags.trigger;
    }
  }
  return flags;
}

/// Semantic kind of a device file, derived from its 8-char name
/// (Docs/App/Service views/Storage.md: "formats the files based on the file name").
String backupFileKind(String name) {
  final upper = name.replaceAll('\x00', '').trim().toUpperCase();
  if (upper.startsWith('SCR_')) return 'Script';
  if (upper == 'SNREG') return 'Serial registry';
  if (upper == 'DYNMEM') return 'Dynamic memory backup';
  if (upper == 'STATLOG' || upper == 'SUBREQ') return 'Registry backup';
  if (upper.startsWith('DV_') || upper.startsWith('DT_')) return 'Registry backup';
  if (upper.startsWith('LAY') || upper.endsWith('.LAY')) return 'LED layout';
  if (upper.endsWith('.TXT') || upper.endsWith('.LOG')) return 'Text';
  return 'Binary';
}

// ---------------------------------------------------------------------------
// Semantic value codec
// ---------------------------------------------------------------------------

String hexBytes(List<int> bytes) =>
    bytes.map((b) => b.toRadixString(16).padLeft(2, '0')).join();

List<int> unhexBytes(String hex) {
  final clean = hex.replaceAll(RegExp(r'[^0-9a-fA-F]'), '');
  return [
    for (var i = 0; i + 1 < clean.length; i += 2)
      int.parse(clean.substring(i, i + 2), radix: 16)
  ];
}

/// Encodes raw wire [bytes] of [type] into a semantic JSON value. Enum and
/// device-type values become their name; numbers stay literal; unknown types fall
/// back to a `{hex: ...}` map so nothing is lost.
Object? encodeSemantic(DataType type, List<int> bytes, {FieldInfo? info}) {
  switch (type) {
    case DataType.none:
      return null;
    case DataType.bool_:
      return bytes.isNotEmpty && bytes[0] != 0;
    case DataType.number:
      return bytes.length >= 4 ? numberFromBytes(bytes) : bytesToInt(bytes);
    case DataType.integer:
      if (bytes.isEmpty) return null;
      return bytes.length < 4 ? bytesToInt(bytes) : int32FromBytes(bytes);
    case DataType.uint32:
    case DataType.idx:
      if (bytes.isEmpty) return null;
      return bytes.length < 4 ? bytesToInt(bytes) : uint32FromBytes(bytes);
    case DataType.enum_:
      if (bytes.isEmpty) return null;
      final raw = bytes.length < 4 ? bytesToInt(bytes)! : uint32FromBytes(bytes);
      return info?.enumValues?[raw] ?? raw;
    case DataType.devType:
      if (bytes.length < 2) return null;
      return DeviceType.fromValue(bytes[0] | (bytes[1] << 8)).label;
    case DataType.sn:
      return bytes.length >= 14 ? serialNumberToHex(bytes.sublist(0, 14)) : hexBytes(bytes);
    case DataType.id:
    case DataType.netAddr:
      if (bytes.length >= 2) return idToString(bytes[0] | (bytes[1] << 8));
      return bytes.isEmpty ? null : bytes[0];
    case DataType.string:
    case DataType.filename:
      return String.fromCharCodes(bytes).replaceAll('\x00', '').trimRight();
    case DataType.vector:
      if (bytes.length < 4) return hexBytes(bytes);
      final n = bytes.length ~/ 4;
      return [for (var i = 0; i < n; i++) numberFromBytes(bytes, i * 4)];
    case DataType.matrix:
      if (bytes.length < 4) return {'hex': hexBytes(bytes)};
      final h = bytes[0] | (bytes[1] << 8);
      final w = bytes[2] | (bytes[3] << 8);
      if (h == 0 || w == 0 || 4 + h * w * 4 > bytes.length) {
        return {'hex': hexBytes(bytes)};
      }
      return {
        'rows': h,
        'cols': w,
        'values': [
          for (var r = 0; r < h; r++)
            [for (var c = 0; c < w; c++) numberFromBytes(bytes, 4 + (r * w + c) * 4)]
        ],
      };
    case DataType.colour:
      if (bytes.length < 4) return hexBytes(bytes);
      return {'r': bytes[0], 'g': bytes[1], 'b': bytes[2], 'a': bytes[3]};
    case DataType.geometry:
    case DataType.texture:
      return encodeDictionary(type.value, bytes);
    case DataType.undefined:
    case DataType.deleted:
      return {'hex': hexBytes(bytes)};
  }
}

/// Decodes a semantic JSON [value] back into raw wire bytes for [type]. Returns
/// null when the value is missing or incompatible with the requested type/size.
List<int>? decodeSemantic(DataType type, Object? value,
    {FieldInfo? info, int? size}) {
  switch (type) {
    case DataType.none:
      return const <int>[];
    case DataType.bool_:
      return [value == true ? 1 : 0];
    case DataType.number:
      return value is num ? numberToBytes(value.toDouble()) : null;
    case DataType.integer:
      return value is num ? intToBytes(value.toInt(), size ?? 4) : null;
    case DataType.uint32:
    case DataType.idx:
      return value is num ? uint32ToBytes(value.toInt()) : null;
    case DataType.enum_:
      final raw = _enumRaw(value, info);
      return raw == null ? null : intToBytes(raw, size ?? 4);
    case DataType.devType:
      final raw = _deviceTypeRaw(value);
      return raw == null ? null : [raw & 0xFF, (raw >> 8) & 0xFF];
    case DataType.sn:
      return value is String && unhexBytes(value).length >= 14
          ? unhexBytes(value).sublist(0, 14)
          : null;
    case DataType.id:
    case DataType.netAddr:
      return _idBytes(value, size);
    case DataType.string:
    case DataType.filename:
      if (value is! String) return null;
      var out = value.codeUnits;
      if (size != null && out.length < size) {
        out = [...out, ...List<int>.filled(size - out.length, 0x20)];
      }
      return out;
    case DataType.vector:
      if (value is! List) return null;
      final out = BytesBuilder();
      for (final v in value) {
        if (v is! num) return null;
        out.add(numberToBytes(v.toDouble()));
      }
      return out.toBytes();
    case DataType.matrix:
      return _matrixBytes(value);
    case DataType.colour:
      if (value is Map) {
        final r = value['r'], g = value['g'], b = value['b'], a = value['a'];
        if (r is num && g is num && b is num && a is num) {
          return [r.toInt(), g.toInt(), b.toInt(), a.toInt()];
        }
      }
      return null;
    case DataType.geometry:
    case DataType.texture:
      return decodeDictionary(type.value, value);
    case DataType.undefined:
    case DataType.deleted:
      if (value is Map && value['hex'] is String) return unhexBytes(value['hex'] as String);
      return null;
  }
}

int? _enumRaw(Object? value, FieldInfo? info) {
  if (value is num) return value.toInt();
  if (value is String) {
    final options = info?.enumValues;
    if (options != null) {
      for (final entry in options.entries) {
        if (entry.value == value) return entry.key;
      }
    }
    return int.tryParse(value);
  }
  return null;
}

int? _deviceTypeRaw(Object? value) {
  if (value is num) return value.toInt();
  if (value is String) {
    for (final type in DeviceType.values) {
      if (type.label == value) return type.value;
    }
    return int.tryParse(value);
  }
  return null;
}

List<int>? _idBytes(Object? value, int? size) {
  if (value is num) return intToBytes(value.toInt(), size ?? 2);
  if (value is String) {
    final parts = value.split('.');
    if (parts.length != 2) return null;
    final net = int.tryParse(parts[0], radix: 16);
    final dev = int.tryParse(parts[1], radix: 16);
    if (net == null || dev == null) return null;
    final id = ((net & 0x3F) << 10) | (dev & 0x3FF);
    return [id & 0xFF, (id >> 8) & 0xFF];
  }
  return null;
}

List<int>? _matrixBytes(Object? value) {
  if (value is! Map) return null;
  final h = value['rows'], w = value['cols'], rows = value['values'];
  if (h is! num || w is! num || rows is! List) return null;
  final hh = h.toInt(), ww = w.toInt();
  if (hh <= 0 || ww <= 0 || rows.length != hh) return null;
  final out = BytesBuilder()..add([hh & 0xFF, hh >> 8, ww & 0xFF, ww >> 8]);
  for (final row in rows) {
    if (row is! List || row.length != ww) return null;
    for (final cell in row) {
      if (cell is! num) return null;
      out.add(numberToBytes(cell.toDouble()));
    }
  }
  return out.toBytes();
}

// ---------------------------------------------------------------------------
// Dictionaries (LED display geometry/texture)
// ---------------------------------------------------------------------------

/// Encodes a keyed-dictionary field value into a list of semantic entries.
List<Map<String, dynamic>> encodeDictionary(int dictType, List<int> bytes) {
  final entries = parseKeyedDict(bytes);
  if (entries == null) return [];
  return [
    for (final e in entries)
      {
        'key': renderDictKeyName(dictType, e.key),
        'keyIndex': e.key,
        'type': dataTypeWord(e.meta.dataType),
        'flags': flagWords(e.meta.flagsAndType),
        'value': encodeSemantic(e.meta.dataType, e.value,
            info: renderKeyFieldInfo(dictType, e.key)),
      }
  ];
}

/// Rebuilds a keyed-dictionary field value from its semantic entries.
List<int>? decodeDictionary(int dictType, Object? value) {
  if (value is! List) return null;
  final entries = <KeyedEntry>[];
  for (final raw in value) {
    if (raw is! Map) return null;
    final key = _dictKeyIndex(dictType, raw);
    final type = dataTypeFromWord(raw['type'] as String? ?? '');
    if (key == null || type == null) return null;
    final bytes = decodeSemantic(type, raw['value'],
        info: renderKeyFieldInfo(dictType, key));
    if (bytes == null) return null;
    entries.add(KeyedEntry(
        key: key,
        meta: BlockMeta(
            flagsAndType:
                flagsFromWords((raw['flags'] as List?)?.cast<String>() ?? []) | type.value,
            key: key,
            size: bytes.length),
        value: bytes));
  }
  return buildKeyedDict(entries);
}

int? _dictKeyIndex(int dictType, Map raw) {
  final keyIndex = raw['keyIndex'];
  if (keyIndex is num) return keyIndex.toInt();
  final name = raw['key'];
  if (name is String) {
    for (var k = 0; k < 32; k++) {
      if (renderDictKeyName(dictType, k) == name) return k;
    }
  }
  return null;
}
