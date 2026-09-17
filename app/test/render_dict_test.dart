@Tags(['led-dict'])
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/render_dict.dart';
import 'package:tamuapp/core/types.dart';

/// Keyed-dictionary helpers for the LED display render block: parsing a field value
/// into entries and re-serializing (4-byte entry alignment) round-trips exactly.
List<int> entry(int type, int key, List<int> value) {
  final pad = (4 - ((4 + value.length) % 4)) % 4;
  return [
    ...BlockMeta(flagsAndType: type, key: key, size: value.length).toBytes(),
    ...value,
    ...List<int>.filled(pad, 0),
  ];
}

void main() {
  final geometry = [
    ...entry(DataType.enum_.value, 0, [0]), // Operation: Replace
    ...entry(DataType.enum_.value, 1, [3]), // Shape: Square
    ...entry(DataType.matrix.value, 2, [2, 0, 3, 0, for (var i = 0; i < 24; i++) 0]), // identity 2x3
    ...entry(DataType.number.value, 3, numberToBytes(4.0)), // Size
    ...entry(DataType.number.value, 4, numberToBytes(0.0)), // Fade
    ...entry(DataType.number.value, 5, numberToBytes(1.0)), // Alpha
  ];

  test('parse keyed dict field into entries', () {
    final parsed = parseKeyedDict(geometry)!;
    expect(parsed.length, 6);
    expect(parsed[0].key, 0);
    expect(parsed[1].key, 1);
    expect(parsed[2].key, 2);
    expect(parsed[2].meta.dataType, DataType.matrix);
    expect(parsed[2].value.length, 28);
    expect(parsed[3].key, 3);
    expect(numberFromBytes(parsed[3].value), 4.0);
    expect(parsed[5].key, 5);
    expect(numberFromBytes(parsed[5].value), 1.0);
  });

  test('build round-trips byte-identically', () {
    final rebuilt = buildKeyedDict(parseKeyedDict(geometry)!);
    expect(rebuilt, geometry);
  });

  test('editing one entry keeps the others intact', () {
    final parsed = parseKeyedDict(geometry)!;
    parsed[3] = KeyedEntry(
        key: 3, meta: parsed[3].meta, value: numberToBytes(8.0));
    final rebuilt = buildKeyedDict(parsed);
    final reparsed = parseKeyedDict(rebuilt)!;
    expect(numberFromBytes(reparsed[3].value), 8.0);
    expect(numberFromBytes(reparsed[5].value), 1.0);
    expect(reparsed[1].value.first, 3); // Square unchanged
  });

  test('render dict key names and enum labels', () {
    // DataType must cover the dictionary marker types (0x101/0x102) so the app can
    // create a dictionary field and render_dict constants stay in sync.
    expect(DataType.geometry.value, geometryDictType);
    expect(DataType.texture.value, textureDictType);
    expect(isRenderDictType(DataType.geometry.value), isTrue);
    expect(isRenderDictType(DataType.texture.value), isTrue);
    expect(isRenderDictType(0x06), isFalse);
    expect(renderDictKeyName(0x101, 0), 'Dictionary');
    expect(renderDictKeyName(0x101, 3), 'Position');
    expect(renderDictKeyName(0x102, 4), 'Colour 1');
    expect(renderKeyFieldInfo(0x101, 1).enumValues?[3], 'Square');
    expect(renderKeyFieldInfo(0x102, 1).enumValues?[1], 'Fill');
    expect(renderKeyFieldInfo(0x101, 2).enumValues?[0], 'Replace');
  });
}