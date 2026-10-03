import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/device_backup.dart';
import 'package:tamuapp/core/types.dart';

/// The device backup decoders, against synthetic bytes matching the firmware layouts
/// (the static `.SV` space, `Subscriptions.h` SUBREQ, `Memory.h` DT_/DV_).
void main() {
  List<int> u16(int v) => [v & 0xFF, (v >> 8) & 0xFF];
  List<int> u32(int v) => [v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF];

  /// The static blocks' persistent fields (the values the device reports via CID 1/2).
  final staticFields = <int, List<({int field, int size, int type})>>{
    0x04: [(field: 0, size: 4, type: DataType.uint32.value)],
    0x05: [
      (field: 0, size: 1, type: DataType.enum_.value),
      (field: 1, size: 1, type: DataType.enum_.value),
      (field: 2, size: 1, type: DataType.enum_.value),
      (field: 3, size: 4, type: DataType.number.value),
      (field: 4, size: 4, type: DataType.number.value),
    ],
    0x06: [
      (field: 1, size: 24, type: DataType.matrix.value),
      (field: 2, size: 4, type: DataType.integer.value),
      (field: 3, size: 8, type: DataType.filename.value),
    ],
    0x08: [
      (field: 0, size: 4, type: DataType.number.value),
      (field: 1, size: 1, type: DataType.enum_.value),
      (field: 2, size: 4, type: DataType.number.value),
    ],
  };

  /// DT_ table: Name (16 chars, space-padded), u16 entry_count, u16 reserved, then
  /// 8 B per entry (Field&Key, MemoryOffset, ValueInfo). The offset is the entry's position
  /// in its (compacted) value space.
  List<int> dynamicTable(String name, List<(int, int, int, int)> entries) {
    final nameBytes = name.codeUnits.take(16).toList();
    while (nameBytes.length < 16) {
      nameBytes.add(0x20);
    }
    final out = <int>[
      ...nameBytes,
      ...u16(entries.length),
      0, 0, // reserved padding
    ];
    var po = 0, vo = 0;
    for (final e in entries) {
      final type = e.$2, size = e.$3, flags = e.$4;
      final persistent = flags & ValueFlags.persistent != 0;
      final offset = persistent ? po : vo;
      if (persistent) {
        po += size;
      } else {
        vo += size;
      }
      out.addAll([...u16(e.$1), ...u16(offset), ...u16(type), size, flags]);
    }
    return out;
  }

  group('.SV', () {
    // One Vysi1 display (0x06) and one ResistiveMeasure (0x08), ascending - the space is stacked
    // in block-type order. System (20 B) + 0x06 (36 B: Offset@0, RenderBlock@24, LayoutFile@28)
    // + 0x08 (12 B: SamplingRate@0, SensorType@4, FilterCoeff@8).
    const registry = [(type: 0x06, inst: 0), (type: 0x08, inst: 0)];

    List<int> svBytes() {
      final b = List<int>.filled(20 + 36 + 12, 0);
      b.setRange(0, 3, 'Eye'.codeUnits); // System Name @ 0
      b[16] = 3; // System NetID @ 16
      b[44] = 5; // 0x06 RenderBlock @ 20 + 24 = 44
      b.setRange(56, 60, numberToBytes(10)); // 0x08 SamplingRate @ 20 + 36 = 56
      b[60] = 2; // 0x08 SensorType @ 60
      return b;
    }

    test('computes each field offset from the field sizes + alignment', () {
      final layout = StaticSpaceLayout.fromRegistry(registry, staticFields);
      expect(layout.offsetOf(0x06, 0, 1), 20);
      expect(layout.offsetOf(0x06, 0, 2), 44);
      expect(layout.offsetOf(0x06, 0, 3), 48);
      expect(layout.offsetOf(0x08, 0, 0), 56);
      expect(layout.offsetOf(0x08, 0, 1), 60); // SensorType (1 B) follows the 4 B Number
      expect(layout.offsetOf(0x08, 0, 2), 64); // FilterCoeff re-aligns to 4
      // A volatile field and an absent type have no offset.
      expect(layout.offsetOf(0x08, 0, 3), isNull);
      expect(layout.offsetOf(0x05, 0, 0), isNull);
    });

    test('a node (no NetID) has a 16 B System segment and no NetID field', () {
      final layout = StaticSpaceLayout.fromRegistry(registry, staticFields, hasNetId: false);
      expect(layout.offsetOf(0, 0, systemNameField), 0);
      expect(layout.offsetOf(0, 0, systemNetIdField), isNull);
      expect(layout.offsetOf(0x06, 0, 1), 16); // the first static block starts at 16, not 20
      expect(layout.offsetOf(0x08, 0, 0), 52);
    });

    test('decodes the System segment and every static persistent field', () {
      final layout = StaticSpaceLayout.fromRegistry(registry, staticFields);
      final entries = decodeSv(svBytes(), layout, registry);
      // System Name + NetID + 0x06 x3 + 0x08 x3.
      expect(entries, hasLength(8));
      final byKey = {
        for (final e in entries) '${e.blockType}.${e.inst}.${e.field}': e,
      };
      expect(byKey['0.0.6']!.value.sublist(0, 3), 'Eye'.codeUnits);
      expect(byKey['0.0.7']!.value, [3]);
      expect(byKey['6.0.2']!.value, [5, 0, 0, 0]);
      expect(byKey['8.0.0']!.value, numberToBytes(10));
      expect(byKey['8.0.1']!.value, [2]);
    });

    test('a truncated space drops the fields past its end', () {
      final layout = StaticSpaceLayout.fromRegistry(registry, staticFields);
      // Only the System segment is present.
      expect(decodeSv(List.filled(20, 0), layout, registry), hasLength(2));
      expect(decodeSv(const [], layout, registry), isEmpty);
    });
  });

  group('SUBREQ', () {
    test('decodes every entry including the 16.16 deadzone', () {
      final target = makeBlockInfo(0, 0, 0, 0);
      final source = makeBlockInfo(8, 0, 4, 0);
      // provider(2) trid(2) source(4) trigger(1) min(3) period(4) deadzone(4) target(4)
      List<int> entry(int provider, int trid, double deadzone) => [
            ...u16(provider), ...u16(trid),
            ...u32(source),
            1, 100, 0, 0, // trigger + uint24 minTime = 100
            ...u32(1000),
            ...numberToBytes(deadzone),
            ...u32(target),
          ];
      final entries = decodeSubreq([2, ...entry(2, 0x1000, 0), ...entry(3, 0x1001, 1.5)]);
      expect(entries, hasLength(2));
      expect(entries[0].providerAddr, 2);
      expect(entries[0].trid, 0x1000);
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
          [
            // fieldKey, type, size, flags
            ((0 << 8) | 0, DataType.number.value, 4, ValueFlags.persistent),
            ((1 << 8) | 0, DataType.number.value, 4, 0), // volatile
            ((2 << 8) | 3, DataType.number.value, 2, ValueFlags.persistent),
          ],
        ))!;

    test('the table decodes name and entries', () {
      final t = table();
      expect(t.name, 'Box');
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
      expect(entries[0].blockType, dynamicTypeForIndex(0x0A));
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
      expect(decodeDynamicTable([9, 1, 2]), isNull); // shorter than the header
      // Entry count past the supplied bytes.
      expect(decodeDynamicTable([...List.filled(16, 0), 5, 0, 0, 0]), isNull);
    });
  });

  group('DeviceBackup', () {
    test('decodes the System segment and the static blocks from .SV', () {
      const registry = [(type: 0x08, inst: 0)];
      final sv = List<int>.filled(20 + 12, 0);
      sv.setRange(0, 3, 'Eye'.codeUnits);
      sv.setRange(20, 24, numberToBytes(10));
      final backup = DeviceBackup.decode(
          sv: sv, staticRegistry: registry, staticFields: staticFields);
      expect(backup.hasAny, isTrue);
      expect(backup.staticField(0x08, 0, 0)?.value, numberToBytes(10));
      // The System block is virtual (type 0, inst 0) and carries Name/NetID.
      final name = backup.staticField(0, 0, systemNameField)!.value;
      expect(name.sublist(0, 3), 'Eye'.codeUnits);
      // A persistent field the space carries is decoded...
      expect(backup.staticField(0x08, 0, 1)?.value, [0]);
      // ...but a volatile field has no stored entry.
      expect(backup.staticField(0x08, 0, 3), isNull);
    });

    test('a registry type with no persistent fields contributes nothing', () {
      final backup = DeviceBackup.decode(
          sv: List.filled(20, 0), staticRegistry: const [(type: 0x03, inst: 0)]);
      // LEDButton (0x03) has no persistent fields, so only the System fields are present.
      expect(backup.staticField(0x03, 0, 0), isNull);
      expect(backup.staticField(0, 0, systemNameField), isNotNull);
    });

    test('the static classifier excludes the System, Script and Dynamic memories', () {
      expect(isStaticRegistryType(systemBlockTypeValue), isFalse);
      // The banked script range (0x3F4-0x3F7) and dynamic range (0x3F0-0x3F3).
      expect(isStaticRegistryType(scriptTypeBase), isFalse);
      expect(isStaticRegistryType(scriptTypeBase + 3), isFalse);
      expect(isStaticRegistryType(dynamicTypeBase), isFalse);
      expect(isStaticRegistryType(dynamicTypeBase + 3), isFalse);
      expect(isStaticRegistryType(BlockType.pwm.value), isTrue);
      expect(isStaticRegistryType(BlockType.resistiveMeasure.value), isTrue);
    });

    test('dynamic entries are looked up by field and key', () {
      final backup = DeviceBackup.decode(dynamic: {
        3: (
          table: dynamicTable('Box', [
            ((0 << 8) | 1, DataType.number.value, 4, ValueFlags.persistent),
          ]),
          values: numberToBytes(42),
        ),
      });
      expect(backup.dynamicField(3, 0, 1)?.value, numberToBytes(42));
      // The same field at another key is a different entry.
      expect(backup.dynamicField(3, 0, 0), isNull);
      expect(backup.dynamicField(4, 0, 1), isNull);
      // A dynamic entry is not reachable through the static lookup.
      expect(backup.staticField(dynamicTypeForIndex(3), 3, 0), isNull);
    });

    test('an empty or missing backup is empty, not an error', () {
      expect(DeviceBackup.empty.hasAny, isFalse);
      expect(DeviceBackup.empty.staticField(0, 0, 0), isNull);
      expect(DeviceBackup.empty.dynamicField(0, 0, 0), isNull);
      expect(DeviceBackup.decode().hasAny, isFalse);
      expect(DeviceBackup.decode(sv: const [], dynamic: const {}).hasAny, isFalse);
    });

    test('a DV file that does not match its DT table contributes no values', () {
      final backup = DeviceBackup.decode(dynamic: {
        1: (
          table: dynamicTable('Box', [
            ((0 << 8) | 0, DataType.number.value, 4, ValueFlags.persistent),
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
            table: dynamicTable('Panel', [
              ((3 << 8) | 1, DataType.number.value, 4, ValueFlags.persistent),
              ((3 << 8) | 2, DataType.number.value, 4, 0), // volatile
              ((1 << 8) | 0, DataType.colour.value, 4, ValueFlags.persistent),
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

  // The per-field Save writes the file itself (the wire only carries Save All), so these two
  // writers must round-trip through the decoders above.
  group('field writers', () {
    test('svSaveField writes a persistent field at its computed offset', () {
      const registry = [(type: 0x08, inst: 0)];
      final layout = StaticSpaceLayout.fromRegistry(registry, staticFields);
      final sv = List<int>.filled(20 + 12, 0);
      sv.setRange(0, 3, 'Eye'.codeUnits);
      final out = svSaveField(sv, layout, 0x08, 0, 0, numberToBytes(30))!;
      expect(out.length, sv.length, reason: '.SV keeps its exact length');
      expect(out.sublist(20, 24), numberToBytes(30)); // SamplingRate @ 20
      expect(out.sublist(0, 3), 'Eye'.codeUnits); // nothing else moved
      // The patched space still decodes.
      final decoded = decodeSv(out, layout, registry)
          .firstWhere((e) => e.blockType == 0x08 && e.field == 0);
      expect(decoded.value, numberToBytes(30));

      // A wrong size and a volatile/absent field are refused rather than written.
      expect(svSaveField(sv, layout, 0x08, 0, 0, [1]), isNull);
      expect(svSaveField(sv, layout, 0x08, 0, 3, [1, 2, 3, 4]), isNull);
    });

    test('svSaveField grows a short space to reach the field', () {
      const registry = [(type: 0x08, inst: 0)];
      final layout = StaticSpaceLayout.fromRegistry(registry, staticFields);
      final out = svSaveField(const <int>[], layout, 0x08, 0, 0, numberToBytes(7))!;
      expect(out.length, 20 + 4);
      expect(out.sublist(20, 24), numberToBytes(7));
    });

    test('dvSaveField patches one persistent entry at its table offset', () {
      final table = decodeDynamicTable(dynamicTable('Box', [
        ((0 << 8) | 0, DataType.number.value, 4, ValueFlags.persistent),
        ((1 << 8) | 0, DataType.number.value, 2, ValueFlags.persistent),
      ]))!;
      final values = <int>[...numberToBytes(1.0), 5, 6];
      final patched = dvSaveField(table, values, 1, 0, [7, 8])!;
      expect(patched.length, values.length, reason: 'DV_ keeps its exact length');
      expect(patched.sublist(0, 4), numberToBytes(1.0)); // the first entry is untouched
      expect(patched.sublist(4), [7, 8]);
      // The patched file still pairs with its table.
      expect(decodeDynamicValues(0, table, patched).first.value, numberToBytes(1.0));

      // A wrong size and a non-persistent entry are refused rather than written.
      expect(dvSaveField(table, values, 1, 0, [1]), isNull);
      expect(dvSaveField(table, values, 9, 0, [1, 2]), isNull);
    });
  });
}
