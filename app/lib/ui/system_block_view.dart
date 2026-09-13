/// System block (type 0, inst 0) model: field/key structure and value formatting
/// (extracted from register_page.dart so the memory views share one definition).
library;

import '../core/types.dart';
import 'value_editor.dart' show formatValue;

/// Field display names for the system block (Register service fields 0-8).
String systemFieldName(int field) => switch (field) {
      0 => 'Device Type',
      1 => 'Serial Number',
      2 => 'Short Address',
      3 => 'Time',
      4 => 'RAM',
      5 => 'Storage',
      6 => 'Name',
      7 => 'NetID',
      8 => 'App/CLI Active',
      _ => 'Field $field',
    };

/// Sub-structure member names for a keyed system field (e.g. field 3 -> Time).
String systemStructMemberName(int field, int key) {
  switch (field) {
    case 0:
      switch (key) {
        case 0: return '.deviceType';
        case 1: return '.capabilities';
        case 2: return '.softwareVersion';
      }
    case 3:
      switch (key) {
        case 0: return '.uptime';
        case 1: return '.currentTime';
        case 2: return '.timeOffsetMs';
        case 3: return '.avgLoopTimeMs';
        case 4: return '.maxLoopTimeMs';
      }
    case 4:
      switch (key) {
        case 0: return '.usedRAM';
        case 1: return '.totalRAM';
      }
    case 5:
      switch (key) {
        case 0: return '.usedFlash';
        case 1: return '.totalFlash';
      }
    case 8:
      switch (key) {
        case 0: return '.appActive';
        case 1: return '.cliActive';
      }
  }
  return 'Key $key';
}

/// The key set of one system-block field (0xFF = scalar field, no keys).
List<int> systemKeysForField(int field) => switch (field) {
      0 => [0, 1, 2],
      1 => [0xFF],
      2 => [0],
      3 => [0, 1, 2, 3, 4],
      4 => [0, 1],
      5 => [0, 1],
      6 => [0xFF],
      7 => [0],
      8 => [0, 1],
      _ => [0],
    };

/// Formats one system-block value (special-cases the device type enum, serial
/// number, addresses, timestamps and the 4-byte software version). `field`/`key`
/// let field-specific values (the capabilities bitmask under Device Type) decode.
String formatSystemValue(DataType type, List<int> value, [int field = -1, int key = -1]) {
  switch (type) {
    case DataType.enum_:
      // Device type is a 32-bit enum
      if (value.length >= 4) {
        final val = value[0] | (value[1] << 8) | (value[2] << 16) | (value[3] << 24);
        return DeviceType.fromValue(val).label;
      }
      if (value.isNotEmpty) return '0x${value[0].toRadixString(16).padLeft(2, '0')}';
      return '-';
    case DataType.sn:
      return serialNumberToHex(value);
    case DataType.id:
      return value.length >= 2 ? idToString(value[0] | (value[1] << 8)) : '-';
    case DataType.integer:
      // Device Type -> capabilities (field 0, key 1) is a bitmask of enabled services.
      if (field == 0 && key == 1 && value.length >= 4) {
        return formatCapabilities(int32FromBytes(value));
      }
      // System block fields like Time offset are signed 32-bit: sign-extend.
      if (value.length >= 4) return int32FromBytes(value).toString();
      if (value.length >= 2) {
        final v = value[0] | (value[1] << 8);
        return (v >= 0x8000 ? v - 0x10000 : v).toString();
      }
      return '-';
    case DataType.string:
      // Software version is 4 bytes (YY, MM, DD, iteration)
      if (value.length == 4) {
        return '${value[0]}.${value[1]}.${value[2]}.${value[3]}';
      }
      return String.fromCharCodes(value).replaceAll('\x00', '');
    case DataType.bool_:
      return value.isNotEmpty && value[0] != 0 ? 'true' : 'false';
    default:
      return formatValue(type, value);
  }
}

/// Decodes the capability bitmask (Device Type field, key 1) into readable names.
String formatCapabilities(int bits) {
  final names = Capability.describe(bits);
  return names.isEmpty ? 'none' : names.join(', ');
}