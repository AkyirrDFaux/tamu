/// Block and field display metadata, mirroring the firmware block schemas
/// (tamu/src/Blocks/*.h) and Docs/Modules/Generic system blocks.md.
///
/// The wire protocol only carries numeric indexes; these tables resolve them
/// into human-readable field names, units, ranges and enum options - exactly
/// what the firmware code defines for each block type.
library;

import 'types.dart';

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

  const FieldInfo(this.name,
      {this.unit,
      this.min,
      this.max,
      this.step,
      this.enumValues,
      this.transform = false,
      this.maxChars = 23});
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

/// Units reported by each DAS sensor type (Docs/Modules/Generic system blocks.md).
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

/// Edge detection modes (Docs/Modules and blocks/Buttons & LEDS.md).
const Map<int, String> _buttonEdges = {
  0: 'None',
  1: 'Rising',
  2: 'Falling',
  3: 'Both',
};

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

const _buttonFields = [
  FieldInfo('Button Raw State'),
  FieldInfo('Edge Detection', enumValues: _buttonEdges),
  FieldInfo('Edge Counter'),
];

const _ledFields = [
  FieldInfo('LED State'),
];

const _ledButtonFields = [
  FieldInfo('Button Raw State'),
  FieldInfo('Edge Detection', enumValues: _buttonEdges),
  FieldInfo('Edge Counter'),
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
  FieldInfo('Deadzone Acceleration'),
  FieldInfo('Deadzone Angular'),
  FieldInfo('Acceleration', unit: 'm/s^2'),
  FieldInfo('Angular Velocity', unit: 'rad/s'),
];

const _vysiDisplayFields = [
  FieldInfo('Brightness', unit: '%', min: 0, max: 100, step: 1),
  FieldInfo('Offset'),
  FieldInfo('Render Block Index'),
  FieldInfo('Layout File Name', maxChars: 8), // char[8] on the wire
  FieldInfo('Refresh Rate', unit: 'FPS'),
];

const _resistiveMeasureFields = [
  FieldInfo('Sampling Rate', unit: 'Hz', min: 1, max: 1000),
  FieldInfo('Sensor Type', enumValues: _sensorTypes),
  FieldInfo('Filter Coefficient', min: 0, max: 1, step: 0.05),
  FieldInfo('Deadzone'),
  FieldInfo('Measured Value'),
  FieldInfo('Current Range', unit: 'kOhm'),
];

const Map<BlockType, BlockInfo> _blockRegistry = {
  BlockType.button: BlockInfo('Button', _buttonFields),
  BlockType.led: BlockInfo('LED', _ledFields),
  BlockType.ledButton: BlockInfo('LED/Button', _ledButtonFields),
  BlockType.pwm: BlockInfo('PWM output', _pwmFields),
  BlockType.accGyr: BlockInfo('Accelerometer/Gyroscope', _accGyrFields),
  BlockType.vysiDisplay: BlockInfo('LED Display', _vysiDisplayFields),
  BlockType.resistiveMeasure:
      BlockInfo('Resistive measurement', _resistiveMeasureFields),
};

/// Metadata for a block type, or null for unknown/dynamic types.
BlockInfo? blockInfoFor(BlockType type) => _blockRegistry[type];

/// Formats a value with its unit suffix ("25.0 Hz").
String valueWithUnit(String formatted, FieldInfo? info) {
  final unit = info?.unit;
  return unit == null ? formatted : '$formatted $unit';
}
