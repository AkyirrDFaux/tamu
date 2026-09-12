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

  /// Max character count for string fields (wire format limit).
  final int maxChars;

  const FieldInfo(this.name,
      {this.unit, this.min, this.max, this.step, this.enumValues, this.maxChars = 23});
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

const _ledButtonFields = [
  FieldInfo('LED State'),
  FieldInfo('Button'),
];

const _pwmFields = [
  FieldInfo('Frequency', unit: 'Hz', min: 1, max: 40000, step: 100),
  FieldInfo('Duty', unit: '%', min: 0, max: 100, step: 1),
];

const _accGyrFields = [
  FieldInfo('Sampling Rate', unit: 'Hz', min: 1, max: 6600),
  FieldInfo('Acceleration', unit: 'm/s^2'),
  FieldInfo('Angular Velocity', unit: 'rad/s'),
  FieldInfo('Acc Filter', unit: 'samples', min: 0, max: 1000, step: 1),
  FieldInfo('Gyro Filter', unit: 'samples', min: 0, max: 1000, step: 1),
];

const _vysiDisplayFields = [
  FieldInfo('Brightness', unit: '%', min: 0, max: 100, step: 1),
  FieldInfo('Offset'),
  FieldInfo('Render Block Index'),
  FieldInfo('Layout File Name', maxChars: 8), // char[8] on the wire
  FieldInfo('Refresh Rate', unit: 'FPS'),
];

const _resistiveMeasureFields = [
  FieldInfo('Sampling Rate', unit: 'Hz', min: 1, max: 10000),
  FieldInfo('Filter Coefficient', unit: 'samples', min: 0, max: 1000, step: 1),
  FieldInfo('Sensor Type', enumValues: _sensorTypes),
  FieldInfo('Measured Value'),
  FieldInfo('Current Range', unit: 'kOhm'),
];

const Map<BlockType, BlockInfo> _blockRegistry = {
  BlockType.ledButton: BlockInfo('LED/Button', _ledButtonFields),
  BlockType.pwm: BlockInfo('PWM output', _pwmFields),
  BlockType.accGyr: BlockInfo('Accelerometer/Gyroscope', _accGyrFields),
  BlockType.vysiDisplay: BlockInfo('LED Display', _vysiDisplayFields),
  BlockType.resistiveMeasure:
      BlockInfo('Resistive measurement', _resistiveMeasureFields),
};

/// Metadata for a block type, or null for unknown/dynamic types.
BlockInfo? blockInfoFor(BlockType type) => _blockRegistry[type];

/// Resolves a field's display name: registry entry when the block type is
/// known, generic label otherwise.
String fieldNameFor(BlockType blockType, int index) {
  final info = blockInfoFor(blockType)?.field(index);
  return info?.name ?? 'Field $index';
}

/// Formats a value with its unit suffix ("25.0 Hz").
String valueWithUnit(String formatted, FieldInfo? info) {
  final unit = info?.unit;
  return unit == null ? formatted : '$formatted $unit';
}
