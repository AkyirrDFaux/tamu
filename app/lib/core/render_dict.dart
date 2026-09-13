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
  'Operation', // 0
  'Shape', // 1
  'Position', // 2 (Matrix 2x3)
  'Size', // 3 (Vector2 / Number)
  'Fade', // 4 (Number, px)
  'Alpha', // 5 (Number, 0..1)
  'Rounding', // 6 (Number, px)
  'Angles', // 7
  'Point Number', // 8
  'Point Coordinates', // 9
  'Noise Seed', // 10
];

const List<String> _textureKeys = [
  'Type', // 0
  'Position', // 1 (Matrix 2x3)
  'Size', // 2
  'Colour 1', // 3 (RGBA)
  'Colour 2', // 4 (RGBA)
  'Colour 3', // 5 (RGBA)
  'Amount', // 6 (Number)
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

/// Field metadata for a dict key, so the value editor shows labels/ranges. The
/// Position keys (2x3 affine) open the transformation editor.
FieldInfo renderKeyFieldInfo(int typeValue, int key) {
  final name = renderDictKeyName(typeValue, key);
  final isPosition = (typeValue == geometryDictType && key == 2) ||
      (typeValue == textureDictType && key == 1);
  Map<int, String>? enums;
  if (typeValue == geometryDictType) {
    if (key == 0) enums = renderOperations;
    if (key == 1) enums = renderShapes;
  } else {
    if (key == 0) enums = renderTextures;
  }
  return FieldInfo(name, enumValues: enums, transform: isPosition);
}