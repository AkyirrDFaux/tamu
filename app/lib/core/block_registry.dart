/// Block and field display metadata, mirroring the firmware block schemas
/// (firmware/src/Blocks/*.h) and Docs/Modules and blocks/Generic system blocks.md.
///
/// The wire protocol only carries numeric indexes; these tables resolve them
/// into human-readable field names, units, ranges and enum options - exactly
/// what the firmware code defines for each block type.
library;

import 'ble_security.dart' show bleModeLabels;
import 'types.dart';
import 'wifi_settings.dart' show wifiStateLabels;

class FieldInfo {
  final String name;
  final String? unit;
  final double? min;
  final double? max;
  final double? step;
  final Map<int, String>? enumValues;

  /// Edit a matrix field via the transformation editor (offset/rotation/scale/skew/
  /// mirror) instead of the raw grid.
  final bool transform;

  /// Max character count for string fields (wire format limit).
  final int maxChars;

  /// A secret string (a password): the UI masks it and an editor must not echo it.
  final bool secret;

  const FieldInfo(
    this.name, {
    this.unit,
    this.min,
    this.max,
    this.step,
    this.enumValues,
    this.transform = false,
    this.maxChars = 23,
    this.secret = false,
  });
}

class BlockInfo {
  final String typeName;
  final List<FieldInfo> fields;

  const BlockInfo(this.typeName, this.fields);

  FieldInfo? field(int index) =>
      index >= 0 && index < fields.length ? fields[index] : null;
}

const Map<int, String> _sensorTypes = {
  0: 'Raw Measurement',
  1: 'Raw Voltage',
  2: 'Raw Resistance',
  3: 'LDR 10K',
  4: 'NTC10K',
  5: 'NTC100K',
};

/// Units reported by each DAS sensor type (Docs/Modules and blocks/Generic system blocks.md).
/// Raw Measurement (0) is a raw ADC bit count and has no unit.
const Map<int, String> sensorUnits = {
  0: '',
  1: 'V',
  2: 'kΩ',
  3: 'lux',
  4: '°C',
  5: '°C',
};

/// Human-readable name for a DAS sensor type value (0..5), else the raw number.
String sensorTypeLabel(int value) => _sensorTypes[value] ?? 'Type $value';

/// Qualitative light level for the LDR (lux) - fuzzy descriptions spanning roughly
/// 1 lux (deep twilight) up to 10000 lux (full sunlight) and beyond.
String luxLevel(double lux) {
  if (lux < 1) return 'Dark';
  if (lux < 10) return 'Very dim';
  if (lux < 50) return 'Dim';
  if (lux < 200) return 'Low light';
  if (lux < 1000) return 'Moderate';
  if (lux < 5000) return 'Bright';
  if (lux < 10000) return 'Very bright';
  return 'Sunlight';
}

/// LSM6DS3TR output data rates (Hz), index into the ODR table (Docs/Modules and
/// blocks/Measurement.md; datasheet Table 52).
const Map<int, String> _odrOptions = {
  0: '12.5',
  1: '26',
  2: '52',
  3: '104',
  4: '208',
  5: '416',
  6: '833',
  7: '1660',
};

/// Accel full-scale options (datasheet Table 51: FS_XL 00/10/11/01 -> ±2/±4/±8/±16 g).
const Map<int, String> _accelRanges = {
  0: '±2 g',
  1: '±4 g',
  2: '±8 g',
  3: '±16 g',
};

/// Gyro full-scale options (datasheet Table 54: FS_G 00/01/10/11 + FS_125 -> ±250/±500/
/// ±1000/±2000 dps, ±125 with FS_125 set). No ±4000 on this part.
const Map<int, String> _gyroRanges = {
  0: '±125 dps',
  1: '±250 dps',
  2: '±500 dps',
  3: '±1000 dps',
  4: '±2000 dps',
};

const _buttonFields = [FieldInfo('Button Raw State')];

const _ledFields = [FieldInfo('LED State')];

// LED-Button (Docs/Modules and blocks/Buttons & LEDS.md): Button raw state (0) and
// LEDState (3); fields 1-2 are reserved so LEDState keeps its documented index.
const _ledButtonFields = [
  FieldInfo('Button Raw State'),
  FieldInfo('Reserved'),
  FieldInfo('Reserved'),
  FieldInfo('LED State'),
];

const _pwmFields = [
  FieldInfo('Frequency', unit: 'Hz', min: 1, max: 40000, step: 100),
  FieldInfo('Duty', unit: '%', min: 0, max: 100, step: 1),
];

const _accGyrFields = [
  FieldInfo('Sampling Rate', unit: 'Hz', enumValues: _odrOptions),
  FieldInfo('Range Acceleration', enumValues: _accelRanges),
  FieldInfo('Range Angular', enumValues: _gyroRanges),
  FieldInfo('Acceleration Filter', min: 0, max: 1, step: 0.05),
  FieldInfo('Angular Filter', min: 0, max: 1, step: 0.05),
  FieldInfo('Acceleration', unit: 'm/s^2'),
  FieldInfo('Angular Velocity', unit: 'rad/s'),
];

const _vysiDisplayFields = [
  FieldInfo('Brightness', unit: '%', min: 0, max: 100, step: 1),
  FieldInfo('Offset', transform: true), // 2x3 transformation
  FieldInfo('Render Block Index'),
  FieldInfo('Layout File Name', maxChars: 8), // char[8] on the wire
  FieldInfo('Refresh Rate', unit: 'FPS'),
];

const _resistiveMeasureFields = [
  FieldInfo('Sampling Rate', unit: 'Hz', min: 1, max: 1000),
  FieldInfo('Sensor Type', enumValues: _sensorTypes),
  FieldInfo('Filter Coefficient', min: 0, max: 1, step: 0.05),
  FieldInfo('Measured Value'),
  FieldInfo('Current Range', unit: 'kOhm'),
  // The two rail flags (firmware Core/Devices/Measuring.h, fields 5/6): read-only Bools
  // computed with every sample. They carry no unit or edit range - they are never written.
  FieldInfo('Open Circuit'),
  FieldInfo('Short Circuit'),
];

// WiFi Settings (firmware BlockType 0x0C, core/wifi_settings.dart): the WiFi link's
// settings and its reported state. The block is a compile-in option that is off by default,
// so a device normally reports none at all - its absence is not an error.
//
// Password is the one secret field in the app: the row masks it, the editor does not echo
// it, and the block is write-only (a read answers empty), so the app must never show what it
// once wrote. IP address is 16 raw `Undefined` bytes in network order - the panel formats it
// (core/wifi_settings.dart `wifiAddressText`); this entry only names it.
const _wifiSettingsFields = [
  FieldInfo('Enabled'),
  FieldInfo('SSID', maxChars: 32),
  FieldInfo('Password', maxChars: 63, secret: true),
  FieldInfo('State', enumValues: wifiStateLabels),
  FieldInfo('IP address'),
];

// BLE Settings (firmware BlockType 0x0B, `core/ble_security.dart`): the radio's settings, the
// pairing policy and the bond list as register fields. The labels are indexed by field number, so
// the order below is the firmware table order (Blocks/BleSettings.h) and
// `firmware_contract_test.dart` pins it.
//
// Six fields, and nothing else: `Action`, `Bond count`, the keyed `Bond 0..2` rows and the
// `Last peer`/`Last air`/`Last link` read-out were retired with the six-field redesign
// (2026-10-29). The bond list is ONE `Undefined` value (66 bytes) the panel decodes, and `Window`
// is a command as well as a read-out.
//
// Passkey is a secret in the app's sense - masked on the register row, and an editor never
// pre-fills it - but it READS BACK on purpose: it is the user's own code, and the panel reveals
// it on request. `000000` is the "no passkey" value, so the panel prints Just Works rather than
// the six zeros. `Bonds` is a packed array, not a text field: the register row shows it as bytes,
// and the Bluetooth panel is where it is decoded into addresses and names.
const _bleSettingsFields = [
  FieldInfo('Enabled'),
  FieldInfo('Passkey', maxChars: 6, secret: true),
  FieldInfo('Mode', enumValues: bleModeLabels),
  FieldInfo('Display name', maxChars: 16),
  FieldInfo('Bonds'),
  FieldInfo('Window', unit: 'ms'),
];

const Map<BlockType, BlockInfo> _blockRegistry = {
  BlockType.button: BlockInfo('Button', _buttonFields),
  BlockType.led: BlockInfo('LED', _ledFields),
  BlockType.ledButton: BlockInfo('LED/Button', _ledButtonFields),
  BlockType.pwm: BlockInfo('PWM output', _pwmFields),
  BlockType.accGyr: BlockInfo('Accelerometer/Gyroscope', _accGyrFields),
  BlockType.vysiDisplay: BlockInfo('LED Display', _vysiDisplayFields),
  BlockType.resistiveMeasure: BlockInfo(
    'Resistive measurement',
    _resistiveMeasureFields,
  ),
  BlockType.wifiSettings: BlockInfo('WiFi Settings', _wifiSettingsFields),
  BlockType.bleSettings: BlockInfo('BLE Settings', _bleSettingsFields),
};

/// Metadata for a block type, or null for unknown/dynamic types.
BlockInfo? blockInfoFor(BlockType type) => _blockRegistry[type];

/// Formats a value with its unit suffix ("25.0 Hz").
String valueWithUnit(String formatted, FieldInfo? info) {
  final unit = info?.unit;
  return unit == null ? formatted : '$formatted $unit';
}
