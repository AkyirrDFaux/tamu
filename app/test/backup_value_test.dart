import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/backup_value.dart';
import 'package:tamuapp/core/block_registry.dart';
import 'package:tamuapp/core/render_dict.dart';
import 'package:tamuapp/core/types.dart';

void main() {
  List<int> round(DataType type, List<int> bytes, {FieldInfo? info, int? size}) {
    final semantic = encodeSemantic(type, bytes, info: info);
    final back = decodeSemantic(type, semantic, info: info, size: size);
    expect(back, isNotNull, reason: 'decode failed for $type');
    return back!;
  }

  test('scalar types round-trip semantically', () {
    expect(encodeSemantic(DataType.bool_, [1]), true);
    expect(round(DataType.bool_, [1]), [1]);

    expect(encodeSemantic(DataType.number, numberToBytes(12.5)), 12.5);
    expect(numberFromBytes(round(DataType.number, numberToBytes(12.5))), 12.5);

    expect(encodeSemantic(DataType.integer, intToBytes(-7, 4)), -7);
    expect(int32FromBytes(round(DataType.integer, intToBytes(-7, 4))), -7);

    expect(encodeSemantic(DataType.uint32, uint32ToBytes(4000000000)), 4000000000);

    expect(encodeSemantic(DataType.string, 'hello'.codeUnits), 'hello');
    expect(encodeSemantic(DataType.filename, 'VYSIV1'.codeUnits), 'VYSIV1');
  });

  test('enum and device type use words', () {
    const sensors = FieldInfo('Sensor Type', enumValues: {3: 'LDR 10K', 5: 'NTC100K'});
    expect(encodeSemantic(DataType.enum_, [5], info: sensors), 'NTC100K');
    expect(round(DataType.enum_, [5], info: sensors, size: 1), [5]);
    expect(encodeSemantic(DataType.devType, [0x01, 0x00]), 'Tamu v2.0A');
    expect(round(DataType.devType, [0x01, 0x00]), [0x01, 0x00]);
  });

  test('serial number and id are words', () {
    final sn = List<int>.generate(14, (i) => i + 1);
    expect((encodeSemantic(DataType.sn, sn) as String).length, 28);
    expect(round(DataType.sn, sn), sn);

    final id = ((0x01 & 0x3F) << 10) | 8;
    expect(encodeSemantic(DataType.id, [id & 0xFF, id >> 8]), '1.8');
    expect(round(DataType.id, [id & 0xFF, id >> 8]), [id & 0xFF, id >> 8]);
  });

  test('vector, matrix and colour', () {
    final vec = BytesBuilder();
    for (final v in [1.0, -2.5, 3.25]) {
      vec.add(numberToBytes(v));
    }
    final vecBytes = vec.toBytes();
    expect(encodeSemantic(DataType.vector, vecBytes), [1.0, -2.5, 3.25]);
    expect(round(DataType.vector, vecBytes), vecBytes);

    final matrix = (BytesBuilder()..add([2, 0, 3, 0])).toBytes();
    final builder = BytesBuilder()..add(matrix);
    for (final v in [1.5, 2.5, 3.5, 4.5, 5.5, 6.5]) {
      builder.add(numberToBytes(v));
    }
    final matBytes = builder.toBytes();
    final matSemantic = encodeSemantic(DataType.matrix, matBytes) as Map;
    expect(matSemantic['rows'], 2);
    expect(matSemantic['cols'], 3);
    expect((matSemantic['values'] as List).first, [1.5, 2.5, 3.5]);
    expect(decodeSemantic(DataType.matrix, matSemantic), matBytes);

    expect(encodeSemantic(DataType.colour, [255, 0, 0, 128]),
        {'r': 255, 'g': 0, 'b': 0, 'a': 128});
    expect(round(DataType.colour, [255, 0, 0, 128]), [255, 0, 0, 128]);
  });

  test('dictionary encodes key/type names and round-trips', () {
    final dict = buildKeyedDict([
      KeyedEntry(
          key: 1,
          meta: BlockMeta(flagsAndType: DataType.enum_.value, key: 1, size: 4),
          value: intToBytes(1, 4)),
      KeyedEntry(
          key: 4,
          meta: BlockMeta(flagsAndType: DataType.colour.value, key: 4, size: 4),
          value: [255, 0, 0, 255]),
    ]);
    final semantic = encodeSemantic(DataType.texture, dict) as List;
    expect(semantic.first['key'], 'Type');
    expect(semantic.first['value'], 'Fill');
    expect(semantic[1]['key'], 'Colour 1');
    expect(decodeSemantic(DataType.texture, semantic), dict);
  });

  test('flag words round-trip', () {
    final flags = FieldFlags.readOnly | FieldFlags.persistent | FieldFlags.trigger;
    final words = flagWords(flags);
    expect(words, ['Read Only', 'Persistent', 'Trigger']);
    expect(flagsFromWords(words), flags);
    expect(flagsFromWords(const []), 0);
  });

  test('backup file kinds are semantic', () {
    expect(backupFileKind('SCR_00'), 'Script');
    expect(backupFileKind('LAY1    '), 'LED layout');
    expect(backupFileKind('SUBREQ'), 'Registry backup');
    expect(backupFileKind('whatever'), 'Binary');
  });

  test('device JSON round-trips semantically with all sections', () {
    final device = BackupDevice(
      created: '2026-09-22T00:00:00Z',
      type: 'Tamu v2.0A',
      typeId: 1,
      id: 1,
      name: 'Tamu',
      serial: 'AA' * 14,
      blocks: [
        BackupBlock(
          type: 'PWM output',
          typeIndex: 4,
          instance: 0,
          name: 'Fan',
          isDynamic: false,
          entries: [
            BackupEntry(
              field: 'Frequency',
              fieldIndex: 0,
              key: 'Key 0',
              keyIndex: 0,
              type: 'Number',
              flags: const ['Persistent'],
              unit: 'Hz',
              value: 25000.0,
              size: 4,
            ),
          ],
        ),
      ],
      requesterSubscriptions: [
        BackupRequesterSubscription(
          provider: '0.2',
          trigger: 'Periodic',
          periodMs: 200,
          minTimeMs: 50,
          deadzone: 0,
          source: const BackupBlockRef(
              block: 'Resistive measurement',
              typeIndex: 8,
              instance: 0,
              field: 'Measured Value',
              fieldIndex: 3,
              key: 'Key 0',
              keyIndex: 0),
          target: const BackupBlockRef(
              block: 'Dynamic',
              typeIndex: 0x3FF,
              instance: 0,
              field: 'Field 0',
              fieldIndex: 0,
              key: 'Key 0',
              keyIndex: 0),
        ),
      ],
      sndb: [BackupSndbEntry(serial: 'BB' * 14, address: '0.2')],
      files: [BackupFile(name: 'LAY1', kind: 'LED layout', data: 'AQID')],
    );
    final json = device.toJson();
    final decoded = BackupDevice.fromJson(json);
    expect(decoded.name, 'Tamu');
    expect(decoded.blocks.single.entries.single.value, 25000.0);
    expect(decoded.requesterSubscriptions.single.source.field, 'Measured Value');
    expect(decoded.sndb.single.address, '0.2');
    expect(decoded.files.single.bytes, [1, 2, 3]);
    expect(json.containsKey('semantic'), isTrue);
  });

  test('legacy numeric device JSON is rejected with a clear error', () {
    expect(
      () => BackupDevice.fromJson({'format': 1, 'blocks': []}),
      throwsA(isA<FormatException>()),
    );
  });
}
