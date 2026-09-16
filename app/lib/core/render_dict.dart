/// Keyed dictionary support for the LED display render block
/// (Docs/Modules and blocks/LED display.md).
///
/// A render block is a dynamic block whose fields are keyed dictionaries:
/// Geometry fields (type 0x101) build the alpha mask, Texture fields (0x102) fill it.
/// Each field's value is a sequence of keyed entries:
///   BlockMeta (flagsAndType, key, size) + value (padded to a multiple of 4).
library;

import 'dart:typed_data';

import 'block_registry.dart' show FieldInfo;
import 'types.dart';

const int geometryDictType = 0x101;
const int textureDictType = 0x102;

bool isRenderDictType(int typeValue) =>
    typeValue == geometryDictType || typeValue == textureDictType;

class KeyedEntry {
  final int key;
  final BlockMeta meta;
  final List<int> value;

  KeyedEntry({required this.key, required this.meta, required this.value});
}

/// Parses a keyed-dictionary field value into its entries; null when malformed.
List<KeyedEntry>? parseKeyedDict(List<int> fieldValue) {
  if (fieldValue.isEmpty) return <KeyedEntry>[];
  final entries = <KeyedEntry>[];
  var offset = 0;
  while (offset + 4 <= fieldValue.length) {
    final meta = BlockMeta.fromBytes(fieldValue, offset);
    if (offset + 4 + meta.size > fieldValue.length) return null; // truncated
    entries.add(KeyedEntry(
      key: meta.key,
      meta: meta,
      value: fieldValue.sublist(offset + 4, offset + 4 + meta.size),
    ));
    final aligned = (4 + meta.size + 3) & ~3;
    if (aligned == 0) break;
    offset += aligned;
  }
  return entries;
}

/// Serializes keyed entries back into a field value (4-byte entry alignment,
/// matching the firmware's keyed-entry walker).
List<int> buildKeyedDict(List<KeyedEntry> entries) {
  final out = BytesBuilder();
  for (final e in entries) {
    out.add(BlockMeta(flagsAndType: e.meta.flagsAndType, key: e.key, size: e.value.length).toBytes());
    out.add(e.value);
    final aligned = (4 + e.value.length + 3) & ~3;
    final pad = aligned - 4 - e.value.length;
    if (pad > 0) out.add(List<int>.filled(pad, 0));
  }
  return out.toBytes();
}

const List<String> _geometryKeys = [
  'Dictionary', // 0 (reserved marker)
  'Shape', // 1
  'Operation', // 2
  'Position', // 3 (Matrix 2x3)
  'Size', // 4 (Vector2 / Number)
  'Fade', // 5 (Number, px)
  'Alpha', // 6 (Number, 0..1)
  'Rounding', // 7 (Number, px)
  'Angles', // 8
  'Point Number', // 9
  'Point Coordinates', // 10
  'Noise Seed', // 11
];

const List<String> _textureKeys = [
  'Dictionary', // 0 (reserved marker)
  'Type', // 1
  'Position', // 2 (Matrix 2x3)
  'Size', // 3
  'Colour 1', // 4 (RGBA)
  'Colour 2', // 5 (RGBA)
  'Colour 3', // 6 (RGBA)
  'Amount', // 7 (Number)
];

String renderDictKeyName(int typeValue, int key) {
  final names = typeValue == geometryDictType ? _geometryKeys : _textureKeys;
  return key >= 0 && key < names.length ? names[key] : 'Key $key';
}

const Map<int, String> renderOperations = {
  0: 'Replace',
  1: 'Add',
  2: 'Cut',
  3: 'Intersect',
  4: 'XOR',
};

const Map<int, String> renderShapes = {
  0: 'None',
  1: 'Fill',
  2: 'HalfFill',
  3: 'Square',
  4: 'Rectangle',
  5: 'Trapezoid',
  6: 'Circle',
  7: 'Ellipse',
  8: 'Double Parabola',
  9: 'Triangle',
  10: 'Polygon',
  11: 'Star',
  12: 'Mesh',
  13: 'Noise',
};

const Map<int, String> renderTextures = {
  0: 'None',
  1: 'Fill',
  2: 'Gradient Linear',
  3: 'Gradient Circular',
  4: 'Invert Colour',
  5: 'Hue Shift',
  6: 'Contrast',
  7: 'Brightness',
};

/// Geometry dict keys that are meaningful for a given shape value (the Shape enum
/// at key 1). Irrelevant keys are ignored by the renderer; the editor hides them.
Set<int> geometryKeysForShape(int shape) {
  final base = <int>{1, 2, 3, 6}; // Shape, Operation, Position, Alpha
  switch (shape) {
    case 1: // Fill
      return base;
    case 2: // HalfFill
      return {...base, 5}; // Fade
    case 3: // Square
    case 4: // Rectangle
      return {...base, 4, 5, 7}; // Size, Fade, Rounding
    case 5: // Trapezoid
      return {...base, 4, 5, 7, 8}; // Size, Fade, Rounding, Angles
    case 6: // Circle
    case 7: // Ellipse
    case 8: // DoubleParabola
      return {...base, 4, 5}; // Size, Fade
    case 9: // Triangle (equilateral / isosceles)
      return {...base, 4, 5, 8}; // Size, Fade, Angles
    case 10: // Polygon
      return {...base, 4, 5, 9}; // Size, Fade, PointNumber
    case 11: // Star
      return {...base, 4, 5, 8, 9}; // Size, Fade, Angles, PointNumber
    case 13: // Noise
      return {...base, 4, 11}; // Size (scale), NoiseSeed
    default:
      return base;
  }
}

/// Texture dict keys that are meaningful for a given texture/effect type (key 1).
Set<int> textureKeysForType(int type) {
  final base = <int>{1}; // Type
  switch (type) {
    case 1: // Fill
      return {...base, 4}; // Colour1
    case 2: // GradientLinear
    case 3: // GradientCircular
      return {...base, 2, 3, 4, 5}; // Position, Size, Colour1, Colour2
    case 5: // HueShift
    case 6: // Contrast
    case 7: // Brightness
      return {...base, 7}; // Amount
    default:
      return base; // InvertColour (4) has no extra keys
  }
}

/// Field metadata for a dict key, so the value editor shows labels/ranges. The
/// Position keys (2x3 affine) open the transformation editor.
FieldInfo renderKeyFieldInfo(int typeValue, int key) {
  final name = renderDictKeyName(typeValue, key);
  final isPosition = (typeValue == geometryDictType && key == 3) ||
      (typeValue == textureDictType && key == 2);
  Map<int, String>? enums;
  if (typeValue == geometryDictType) {
    if (key == 2) enums = renderOperations;
    if (key == 1) enums = renderShapes;
  } else {
    if (key == 1) enums = renderTextures;
  }
  return FieldInfo(name, enumValues: enums, transform: isPosition);
}