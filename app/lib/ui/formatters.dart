/// Turning wire values into text: the one home for display formatting.
///
/// Nothing here talks to a device or holds state - it converts a value, a
/// timestamp or a file name into the string a page shows.
///
/// Why it exists (TODO.md, "App UX pass", phase 1b): these functions were spread
/// across `widgets.dart`, four `part` files of the `value_editor.dart` library and
/// both viewers, so **importing the value editor was the way to format a value**,
/// and the app's number formatter was a private `_num3` inside one of its parts.
/// A page that only wants to print a value should not pull in an editor.
///
/// Anything carrying no display decision stays in `core/types.dart`
/// ([idToString], [dataTypeWord], [formatSoftwareVersion], the byte readers),
/// because the core uses them too.
library;

import '../core/storage_client.dart' show normalizeFileName;
import '../core/types.dart';

/// `Hh Mm Ss` for an uptime in milliseconds (Device view, Log entry timestamps).
String formatUptimeMs(int ms) {
  final seconds = ms ~/ 1000;
  final h = seconds ~/ 3600;
  final m = (seconds % 3600) ~/ 60;
  final s = seconds % 60;
  return '${h}h ${m}m ${s}s';
}

/// Signed millisecond offset as `+hh:mm:ss.SSS` (the Time offset row).
String formatOffsetMs(int ms) {
  final sign = ms >= 0 ? '+' : '-';
  final abs = ms.abs();
  final msPart = abs % 1000;
  final totalSec = abs ~/ 1000;
  final s = totalSec % 60;
  final totalMin = totalSec ~/ 60;
  final m = totalMin % 60;
  final h = totalMin ~/ 60;
  return '$sign${h.toString().padLeft(2, '0')}:'
      '${m.toString().padLeft(2, '0')}:'
      '${s.toString().padLeft(2, '0')}.'
      '${msPart.toString().padLeft(3, '0')}';
}

/// A fixed-point number for display or for a text field: whole values lose their
/// decimals, everything else keeps three. Used by the scalar formatter and by
/// every numeric field in the transform editor.
String formatNumber3(double v) =>
    v == v.roundToDouble() ? v.toStringAsFixed(0) : v.toStringAsFixed(3);

/// Space-separated lowercase hex, two digits per byte (the fallback for any
/// unknown type).
String formatHexBytes(List<int> bytes) =>
    bytes.map((int b) => b.toRadixString(16).padLeft(2, '0')).join(' ');

/// The printable ASCII gutter of a byte dump: a printable byte as its own
/// character, anything else as `.` (the hex/ASCII viewer's right-hand column).
String formatAsciiBytes(List<int> bytes) => bytes
    .map((int b) => (b >= 0x20 && b < 0x7F) ? String.fromCharCode(b) : '.')
    .join();

/// `0x` + eight uppercase hex digits: a storage offset or record address.
String hex32(int value) =>
    '0x${value.toRadixString(16).padLeft(8, '0').toUpperCase()}';

/// Human-readable data-type name (shared with the semantic backup format, so a
/// label and a stored type word never drift apart).
String dataTypeLabel(DataType type) => dataTypeWord(type);

/// Formats a raw value of [type] for display. Absent or short buffers render
/// `-` rather than throwing: a half-written field is a normal sight on the bus.
String formatValue(DataType type, List<int> bytes) {
  switch (type) {
    case DataType.number:
      if (bytes.length < 4) return '-';
      final v = numberFromBytes(bytes);
      return v == v.roundToDouble()
          ? v.toStringAsFixed(1)
          : v.toStringAsFixed(3);
    case DataType.bool_:
      if (bytes.isEmpty) return '-';
      return bytes[0] != 0 ? 'true' : 'false';
    case DataType.uint32:
      if (bytes.length < 4) return '-';
      return uint32FromBytes(bytes).toString();
    case DataType.integer:
      if (bytes.isEmpty) return '-';
      if (bytes.length < 4) {
        // Short (1-3 byte) integer values, e.g. the uint8 edge counter.
        return bytesToInt(bytes).toString();
      }
      return int32FromBytes(bytes).toString();
    case DataType.text:
    case DataType.name:
    case DataType.char:
      return bytes.isEmpty ? '-' : String.fromCharCodes(bytes).trimRight();
    case DataType.devType:
      if (bytes.length < 2) return '-';
      return DeviceType.fromValue(bytes[0] | (bytes[1] << 8)).label;
    case DataType.id:
      if (bytes.length >= 2) return idToString(bytes[0] | (bytes[1] << 8));
      if (bytes.length == 1) {
        return bytes[0].toString(); // 1-byte Id (System NetID)
      }
      return '-';
    case DataType.colour:
      if (bytes.length < 4) return '-';
      return '#${bytes.sublist(0, 4).map((b) => b.toRadixString(16).padLeft(2, '0')).join()}';
    case DataType.sn:
      if (bytes.length < 14) return '-';
      return serialNumberToHex(bytes.sublist(0, 14));
    case DataType.vector:
      // Size-flexible: format however many 4-byte Numbers are present (Vector2/3/N).
      if (bytes.length < 4) return '-';
      final n = bytes.length ~/ 4;
      return '[${List.generate(n, (i) => formatNumber3(numberFromBytes(bytes, i * 4))).join(', ')}]';
    case DataType.matrix:
      // Matrix buffer: uint16 height, uint16 width, then height*width Numbers.
      if (bytes.length < 8) return '-';
      final h = bytes[0] | (bytes[1] << 8);
      final w = bytes[2] | (bytes[3] << 8);
      if (h * w * 4 + 4 > bytes.length || h == 0 || w == 0) {
        return 'matrix ${h}x$w';
      }
      String cell(int r, int c) =>
          formatNumber3(numberFromBytes(bytes, 4 + (r * w + c) * 4));
      return [
        for (var r = 0; r < h; r++)
          '[${[for (var c = 0; c < w; c++) cell(r, c)].join(' ')}]',
      ].join(' ');
    case DataType.none:
      // Placeholder/deleted slot.
      return '∅';
    default:
      return bytes.isEmpty ? '-' : formatHexBytes(bytes);
  }
}

/// Formats a System-block field, which needs a few overrides before the shared
/// scalar formatter applies: App Active is a 1-byte enum, Capabilities is a
/// bitmask, and the software version is packed YY:MM:DD:II.
String formatSystemValue(
  DataType type,
  List<int> value, [
  int field = -1,
  int key = -1,
]) {
  // System-specific overrides first, then the shared scalar formatter for the rest.
  if (field == 8 && type == DataType.enum_ && value.isNotEmpty) {
    // App Active: 1-byte enum No / USB / BLE.
    return const {0: 'No', 1: 'USB', 2: 'BLE'}[value[0]] ?? '?';
  }
  if (field == 0 && key == 1 && value.length >= 4) {
    // Device Type -> capabilities bitmask.
    return formatCapabilities(int32FromBytes(value));
  }
  if (field == 0 && key == 2 && value.length >= 4) {
    // Software version YY:MM:DD:II (7+4+5+16).
    return formatSoftwareVersion(value);
  }
  if (type == DataType.enum_ && value.length >= 4) {
    // The Device Type is a 32-bit enum; formatValue expects a 16-bit devType.
    return DeviceType.fromValue(uint32FromBytes(value)).label;
  }
  return formatValue(type, value);
}

/// The capability names a Device Type bitmask sets, or `none`.
String formatCapabilities(int bits) {
  final names = Capability.describe(bits);
  return names.isEmpty ? 'none' : names.join(', ');
}

/// How a stored file is classified, from its name alone.
enum StorageFileType {
  snreg,
  layout,
  text,
  binary,
  dynamicTable,
  dynamicValues,
  backup,
}

/// Classifies a stored file name (Docs/Services/Storage.md naming).
StorageFileType storageFileType(String name) {
  final upper = normalizeFileName(name).toUpperCase();
  if (upper == '.SNREG') return StorageFileType.snreg;
  if (upper == '.SV' || upper == '.SUBREQ' || upper == 'SUBREQ') {
    return StorageFileType.backup;
  }
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

/// The human-readable name of a stored file's type.
String fileTypeLabel(String name) => switch (storageFileType(name)) {
  StorageFileType.snreg => 'Serial registry',
  StorageFileType.layout => 'LED layout',
  StorageFileType.text => 'Text',
  StorageFileType.binary => 'Binary',
  StorageFileType.dynamicTable => 'Dynamic block table',
  StorageFileType.dynamicValues => 'Dynamic values',
  StorageFileType.backup => 'Registry backup',
};
