/// System block (type 0, inst 0) model: field/key structure and value formatting
/// (extracted from register_page.dart so the memory views share one definition).
library;

import '../core/types.dart';
import 'value_editor.dart' show formatValue;

// The field/key names live in core/system_schema.dart; re-export so existing
// `system_block_view.dart` importers keep working.
export '../core/system_schema.dart';

/// Formats one system-block value (special-cases the device type enum, serial
/// number, addresses, timestamps and the 4-byte software version). `field`/`key`
/// let field-specific values (the capabilities bitmask under Device Type) decode.
String formatSystemValue(DataType type, List<int> value, [int field = -1, int key = -1]) {
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

/// Decodes the capability bitmask (Device Type field, key 1) into readable names.
String formatCapabilities(int bits) {
  final names = Capability.describe(bits);
  return names.isEmpty ? 'none' : names.join(', ');
}