import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/device_backup.dart';
import 'package:tamuapp/core/types.dart';

/// The device backup decoders, against synthetic bytes matching the firmware layouts
/// (`StaticMemory.h` STATLOG, `Subscriptions.h` SUBREQ, `Memory.h` DT_/DV_).
void main() {
  List<int> u16(int v) => [v & 0xFF, (v >> 8) & 0xFF];
  List<int> u32(int v) => [v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF];

  /// BlockIndex[4] + BlockMeta[4] + value[4-aligned].
  /// BlockMeta is u16 FlagsAndType, u8 Key, u8 Size.
  List<int> statlogEntry(int blockIdx, int field, DataType type, int flags, List<int> value) {
    final flagsAndType = type.value | flags;
    final out = <int>[
      blockIdx, field, 0xFF, 0, // BlockIndex
      flagsAndType & 0xFF, (flagsAndType >> 8) & 0xFF, // FlagsAndType
      0xFF, // key
      value.length, // size
      ...value,
    ];
    while (out.length % 4 != 0) {
      out.add(0);
    }
    return out;
  }

  /// DT_ table: u8 name_len, name, u16 type, u16 entry_count, then 6 B per entry.
  List<int> dynamicTable(String name, int typeValue, List<(int, int, int, int)> entries) {
    final out = <int>[
      name.length, ...name.codeUnits,
      ...u16(typeValue),
      ...u16(entries.length),
    ];
    for (final e in entries) {
      out.addAll([...u16(e.$1), ...u16(e.$2), e.$3, e.$4]);
    }
    return out;
  }

  group('STATLOG', () {
    test('decodes a System entry and a static entry, stopping at the terminator', () {
      final name = 'DAS v0.1'.codeUnits;
      final data = <int>[
        ...statlogEntry(statlogSystemBlock, systemNameField, DataType.string,
            FieldFlags.persistent, name),
        ...statlogEntry(0, 0, DataType.number, FieldFlags.persistent, numberToBytes(10)),
        0xFF,
      ];
      final records = decodeStatlog(data);
      expect(records, hasLength(2));
      expect(records[0].isSystem, isTrue);
      expect(records[0].field, 6);
      expect(String.fromCharCodes(records[0].value), 'DAS v0.1');
      expect(records[1].blockIdx, 0);
      expect(records[1].value, numberToBytes(10));
    });

    test('a truncated trailing record is dropped, not read past the end', () {
      final data = <int>[
        ...statlogEntry(0, 0, DataType.number, 0, numberToBytes(1)),
        // A record header claiming a 16-byte value but supplying only 2.
        0, 1, 0, 0, // BlockIndex
        0x06, 0x00, 0xFF, 16, // FlagsAndType = number, key 0xFF, size 16
        1, 2,
      ];
      final records = decodeStatlog(data);
      expect(records, hasLength(1));
    });

    test('a clear STATLOG decodes to nothing', () {
      expect(decodeStatlog([0xFF]), isEmpty);
      expect(decodeStatlog(const []), isEmpty);
    });
  });

  group('SUBREQ', () {
    test('decodes every entry including the 16.16 deadzone', () {
      final target = makeBlockInfo(0, 0, 0, 0);
      final source = makeBlockInfo(8, 0, 4, 0);
      List<int> entry(int provider, double deadzone) => [
            ...u32(target), ...u32(source),
            ...u16(provider),
            1, 0, 0, 0, // trigger + pad
            ...u32(1000), ...u32(100),
            ...numberToBytes(deadzone),
          ];
      final entries = decodeSubreq([2, ...entry(2, 0), ...entry(3, 1.5)]);
      expect(entries, hasLength(2));
      expect(entries[0].providerAddr, 2);
      expect(entries[0].periodMs, 1000);
      expect(entries[0].minTimeMs, 100);
      expect(entries[0].trigger, 1);
      expect(entries[1].providerAddr, 3);
      expect(entries[1].deadzone, closeTo(1.5, 1e-4));
    });

    test('an implausible count decodes to nothing rather than reading garbage', () {
      expect(decodeSubreq([200, 1, 2, 3]), isEmpty);
      expect(decodeSubreq(const []), isEmpty);
      expect(decodeSubreq([0]), isEmpty);
    });
  });

  group('DT_ / DV_', () {
    DynamicTable table() => decodeDynamicTable(dynamicTable(
          'Box',
          BlockType.dynamic.value,
          [
            // fieldKey, flagsAndType, size, pad
            ((0 << 8) | 0, DataType.number.value | FieldFlags.persistent, 4, 0),
            ((1 << 8) | 0, DataType.number.value, 4, 0), // volatile
            ((2 << 8) | 3, DataType.number.value | FieldFlags.persistent, 2, 0),
          ],
        ))!;

    test('the table decodes name, type and entries', () {
      final t = table();
      expect(t.name, 'Box');
      expect(t.typeValue, BlockType.dynamic.value);
      expect(t.entries, hasLength(3));
      expect(t.entries[0].field, 0);
      expect(t.entries[2].field, 2);
      expect(t.entries[2].key, 3);
      expect(t.entries[1].persistent, isFalse);
      // Only the persistent entries occupy DV space: 4 + 2.
      expect(t.persistentSize, 6);
    });

    test('values pair with the persistent entries in table order, skipping volatile', () {
      final entries = decodeDynamicValues(0x0A, table(), [
        ...numberToBytes(7),
        0x34, 0x12, // 2-byte field 2/key 3
      ]);
      expect(entries, hasLength(2));
      expect(entries[0].blockType, BlockType.dynamic.value);
      expect(entries[0].inst, 0x0A);
      expect(entries[0].field, 0);
      expect(entries[0].key, 0);
      expect(entries[0].value, numberToBytes(7));
      expect(entries[1].field, 2);
      expect(entries[1].key, 3);
      expect(entries[1].value, [0x34, 0x12]);
    });

    test('a DV length that does not match the persistent size yields nothing', () {
      // Firmware rule: the DV length must equal the table's persistent size.
      expect(decodeDynamicValues(0, table(), const []), isEmpty);
      expect(decodeDynamicValues(0, table(), List.filled(8, 0)), isEmpty);
    });

    test('a corrupt or empty table decodes to null', () {
      expect(decodeDynamicTable(const []), isNull);
      expect(decodeDynamicTable([9, 1, 2]), isNull); // name length past the end
      // Entry count past the supplied bytes.
      expect(decodeDynamicTable([0, 0, 0, 5, 0, 0, 0, 0]), isNull);
    });
  });

  group('DeviceBackup', () {
    test('maps STATLOG registry indexes onto the static block list', () {
      final data = <int>[
        ...statlogEntry(0, 0, DataType.number, 0, numberToBytes(10)),
        ...statlogEntry(1, 2, DataType.number, 0, numberToBytes(20)),
        ...statlogEntry(statlogSystemBlock, systemNameField, DataType.string, 0,
            'Eye'.codeUnits),
        0xFF,
      ];
      final backup = DeviceBackup.decode(
        statlog: data,
        staticRegistry: const [
          (type: 0x08, inst: 0), // resistive measure
          (type: 0x06, inst: 1), // display
        ],
      );
      expect(backup.hasAny, isTrue);
      expect(backup.staticField(0x08, 0, 0)?.value, numberToBytes(10));
      expect(backup.staticField(0x06, 1, 2)?.value, numberToBytes(20));
      // The System block is virtual (type 0, inst 0) and carries Name/NetID.
      expect(String.fromCharCodes(backup.staticField(0, 0, systemNameField)!.value), 'Eye');
      // A field that was never saved has no entry.
      expect(backup.staticField(0x08, 0, 1), isNull);
    });

    test('an out-of-range registry index is skipped, not misattributed', () {
      final data = <int>[
        ...statlogEntry(7, 0, DataType.number, 0, numberToBytes(1)),
        0xFF,
      ];
      final backup = DeviceBackup.decode(
          statlog: data, staticRegistry: const [(type: 0x08, inst: 0)]);
      expect(backup.staticField(0x08, 0, 0), isNull);
      expect(backup.hasAny, isFalse);
    });

    test('only static blocks may occupy a STATLOG registry index', () {
      // `readBlocks()` appends the Script (0x3FE) and Dynamic (0x3FF) memories after the
      // statics, so an unfiltered list *happens* to index STATLOG correctly. This shows why
      // the filter is required rather than incidental: list the memories first and the same
      // STATLOG bytes name a different block entirely.
      final withMemoriesFirst = <({int type, int inst})>[
        (type: BlockType.dynamic.value, inst: 0),
        (type: BlockType.script.value, inst: 0),
        (type: BlockType.ledButton.value, inst: 0),
        (type: BlockType.pwm.value, inst: 0),
      ];
      final registry =
          withMemoriesFirst.where((b) => isStaticRegistryType(b.type)).toList();
      expect(registry, hasLength(2));

      // Registry index 0 means the first *static* entry (LEDButton) to the firmware.
      final data = <int>[
        ...statlogEntry(0, 0, DataType.number, 0, numberToBytes(5)),
        0xFF,
      ];
      final right = DeviceBackup.decode(statlog: data, staticRegistry: registry);
      expect(right.staticField(BlockType.ledButton.value, 0, 0)?.value,
          numberToBytes(5));

      // Unfiltered, the same index lands on the dynamic memory instead.
      final wrong =
          DeviceBackup.decode(statlog: data, staticRegistry: withMemoriesFirst);
      expect(wrong.staticField(BlockType.ledButton.value, 0, 0), isNull);
      expect(wrong.staticField(BlockType.dynamic.value, 0, 0)?.value,
          numberToBytes(5));

      // The classifier: System, Script and Dynamic are excluded; static types are not.
      expect(isStaticRegistryType(systemBlockTypeValue), isFalse);
      expect(isStaticRegistryType(BlockType.script.value), isFalse);
      expect(isStaticRegistryType(BlockType.dynamic.value), isFalse);
      expect(isStaticRegistryType(BlockType.pwm.value), isTrue);
      expect(isStaticRegistryType(BlockType.resistiveMeasure.value), isTrue);
    });

    test('dynamic entries are looked up by field and key', () {
      final backup = DeviceBackup.decode(dynamic: {
        3: (
          table: dynamicTable('Box', BlockType.dynamic.value, [
            ((0 << 8) | 1, DataType.number.value | FieldFlags.persistent, 4, 0),
          ]),
          values: numberToBytes(42),
        ),
      });
      expect(backup.dynamicField(3, 0, 1)?.value, numberToBytes(42));
      // The same field at another key is a different entry.
      expect(backup.dynamicField(3, 0, 0), isNull);
      expect(backup.dynamicField(4, 0, 1), isNull);
      // A dynamic entry is not reachable through the static lookup.
      expect(backup.staticField(BlockType.dynamic.value, 3, 0), isNull);
    });

    test('an empty or missing backup is empty, not an error', () {
      expect(DeviceBackup.empty.hasAny, isFalse);
      expect(DeviceBackup.empty.staticField(0, 0, 0), isNull);
      expect(DeviceBackup.empty.dynamicField(0, 0, 0), isNull);
      expect(DeviceBackup.decode().hasAny, isFalse);
      expect(DeviceBackup.decode(statlog: const [], dynamic: const {}).hasAny, isFalse);
    });

    test('a DV file that does not match its DT table contributes no values', () {
      final backup = DeviceBackup.decode(dynamic: {
        1: (
          table: dynamicTable('Box', BlockType.dynamic.value, [
            ((0 << 8) | 0, DataType.number.value | FieldFlags.persistent, 4, 0),
          ]),
          values: const [1, 2], // wrong length
        ),
      });
      // No value can be trusted, but the stored *table* is still known, which is what lets
      // the Backup view say "not backed up" instead of showing the field as absent.
      expect(backup.dynamicField(1, 0, 0), isNull);
      expect(backup.dynamicFieldsFor(1), [0]);
      expect(backup.hasAny, isTrue);
    });
  });

  group('DeviceBackup table structure (for the Backup view)', () {
    DeviceBackup build() => DeviceBackup.decode(dynamic: {
          2: (
            table: dynamicTable('Panel', BlockType.dynamic.value, [
              ((3 << 8) | 1, DataType.number.value | FieldFlags.persistent, 4, 0),
              ((3 << 8) | 2, DataType.number.value, 4, 0), // volatile
              ((1 << 8) | 0, DataType.colour.value | FieldFlags.persistent, 4, 0),
            ]),
            values: numberToBytes(5) + [1, 2, 3, 4],
          ),
        });

    test('fields and keys come from the stored table, ascending', () {
      final b = build();
      expect(b.dynamicFieldsFor(2), [1, 3]);
      expect(b.dynamicKeysFor(2, 3), [1, 2]);
      expect(b.dynamicKeysFor(2, 1), [0]);
      // An unknown slot or field has nothing.
      expect(b.dynamicFieldsFor(9), isEmpty);
      expect(b.dynamicKeysFor(2, 7), isEmpty);
    });

    test('a volatile table entry is reported without a stored value', () {
      final b = build();
      final e = b.dynamicTableEntry(2, 3, 2);
      expect(e, isNotNull);
      expect(e!.persistent, isFalse);
      expect(b.dynamicField(2, 3, 2), isNull); // nothing persisted for it
      // The persistent ones do have values.
      expect(b.dynamicField(2, 3, 1)?.value, numberToBytes(5));
      expect(b.dynamicField(2, 1, 0)?.value, [1, 2, 3, 4]);
    });
  });

  test('align4 rounds up to the 4-byte boundary', () {
    expect(align4(0), 0);
    expect(align4(1), 4);
    expect(align4(4), 4);
    expect(align4(5), 8);
  });
}
