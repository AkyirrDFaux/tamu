import 'dart:convert';
import 'dart:io' as io;

import 'package:archive/archive_io.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/block_registry.dart';
import 'package:tamuapp/core/types.dart';

BackupEntry _entry(String field, int fieldIndex, String type, Object? value,
        {int size = 4, String key = 'Key 0', int keyIndex = 0, List<String> flags = const []}) =>
    BackupEntry(
      field: field,
      fieldIndex: fieldIndex,
      key: key,
      keyIndex: keyIndex,
      type: type,
      flags: flags,
      value: value,
      size: size,
    );

LiveEntry _live(String field, int fieldIndex, DataType type,
        {int size = 4, FieldInfo? info, String key = 'Key 0', int keyIndex = 0}) =>
    LiveEntry(
      field: fieldIndex,
      key: keyIndex,
      fieldName: field,
      keyName: key,
      meta: BlockMeta(flagsAndType: type.value, size: size),
      info: info,
    );

BackupDevice _device() => BackupDevice(
      type: 'Tamu v2.0A',
      typeId: 1,
      id: 1,
      name: 'Tamu',
      blocks: [
        BackupBlock(
          type: 'PWM output',
          typeIndex: BlockType.pwm.value,
          instance: 0,
          name: 'Fan',
          isDynamic: false,
          entries: [_entry('Frequency', 0, 'Number', 25000.0)],
        ),
      ],
    );

void main() {
  test('resolveEntryBytes matches compatible target types', () {
    final number = _entry('Frequency', 0, 'Number', 25000.0);
    expect(resolveEntryBytes(number, _live('Frequency', 0, DataType.number)), isNotNull);
    expect(resolveEntryBytes(number, _live('State', 0, DataType.bool_)), isNull);
    expect(resolveEntryBytes(_entry('State', 0, 'Bool', true), _live('State', 0, DataType.bool_)),
        [1]);
  });

  test('string and filename are cross-compatible and padded', () {
    final src = _entry('Layout File Name', 3, 'String', 'hello', size: 5);
    final bytes =
        resolveEntryBytes(src, _live('Layout File Name', 3, DataType.filename, size: 8));
    expect(bytes, 'hello'.codeUnits + List.filled(3, 0x20));
  });

  test('enum names resolve against the target option set', () {
    const info = FieldInfo('Sensor Type', enumValues: {3: 'LDR 10K', 5: 'NTC100K'});
    final src = _entry('Sensor Type', 1, 'Enum', 'NTC100K', size: 1);
    expect(resolveEntryBytes(src, _live('Sensor Type', 1, DataType.enum_, size: 1, info: info)),
        [5]);
    expect(
        resolveEntryBytes(_entry('Sensor Type', 1, 'Enum', 'Nope', size: 1),
            _live('Sensor Type', 1, DataType.enum_, size: 1, info: info)),
        isNull);
  });

  test('plan resolves by name (reordering), then by index', () {
    final device = _device();
    final live = LiveDevice(id: 1, name: 'Tamu', capabilities: Capability.scripts, blocks: [
      LiveBlock(
          typeValue: BlockType.pwm.value,
          instance: 0,
          name: 'Fan',
          isDynamic: false,
          entries: [
            _live('Duty', 0, DataType.number),
            _live('Frequency', 1, DataType.number),
          ]),
    ]);
    final item = RestoreItem(
        kind: RestoreKind.entry,
        device: device,
        block: device.blocks.first,
        entry: device.blocks.first.entries.first);
    final plan = RestorePlan(
      devices: [device],
      liveDevices: [live],
      items: [item],
      deviceTargets: {1: live},
      blockTargets: const {},
    );
    plan.resolve();
    expect(item.issue, isNull);
    expect(item.targetEntry!.field, 1, reason: 'matched Frequency by name, not index');
    expect(plan.selectedCount, 1);
  });

  test('read-only entries are captured but not restorable', () {
    final device = BackupDevice(
      type: 'Tamu v2.0A',
      typeId: 1,
      id: 1,
      name: 'Tamu',
      blocks: [
        BackupBlock(
          type: 'Button',
          typeIndex: BlockType.button.value,
          instance: 0,
          name: 'Button',
          isDynamic: false,
          entries: [
            _entry('Button Raw State', 0, 'Bool', false, size: 1, flags: const ['Read Only']),
          ],
        ),
      ],
    );
    final live = LiveDevice(id: 1, name: 'Tamu', blocks: [
      LiveBlock(typeValue: BlockType.button.value, instance: 0, name: 'Button', isDynamic: false, entries: [
        LiveEntry(
            field: 0,
            key: 0,
            fieldName: 'Button Raw State',
            keyName: 'Key 0',
            meta: BlockMeta(flagsAndType: DataType.bool_.value | FieldFlags.readOnly, size: 1)),
      ]),
    ]);
    final item = RestoreItem(
        kind: RestoreKind.entry, device: device, block: device.blocks.first, entry: device.blocks.first.entries.first);
    final plan = RestorePlan(
      devices: [device],
      liveDevices: [live],
      items: [item],
      deviceTargets: {1: live},
      blockTargets: const {},
    );
    plan.resolve();
    expect(item.issue, 'Read Only');
    expect(plan.selectedCount, 0);
  });

  test('plan flags a missing target device', () {
    final device = _device();
    final item = RestoreItem(
        kind: RestoreKind.entry, device: device, block: device.blocks.first, entry: device.blocks.first.entries.first);
    final plan = RestorePlan(
      devices: [device],
      liveDevices: const [],
      items: [item],
      deviceTargets: const {},
      blockTargets: const {},
    );
    plan.resolve();
    expect(item.issue, 'No target device');
    expect(plan.selectedCount, 0);
    expect(plan.issueCount, 1);
  });

  test('backup zip is per device with no aggregate manifest', () {
    final zip = buildBackupZip([_device()]);
    final decoded = ZipDecoder().decodeBytes(zip);
    expect(decoded.files.map((f) => f.name).toList(), ['1_Tamu.json']);
    expect(decoded.files.any((f) => f.name == 'backup.json'), isFalse);

    final parsed = parseBackupZip(zip);
    expect(parsed.single.name, 'Tamu');
    expect(parsed.single.blocks.single.entries.single.value, 25000.0);
  });

  test('zip declares correct sizes for non-ASCII content', () {
    // "±16 g" is why a real backup broke: ArchiveFile.string sizes by UTF-16 code
    // units but stores UTF-8, so the declared uncompressed size was short by one
    // byte per non-ASCII character and strict unzippers failed with a CRC error.
    final device = BackupDevice(
      type: 'Tamu v2.0A',
      typeId: 1,
      id: 1,
      name: 'Tamu',
      blocks: [
        BackupBlock(
          type: 'Accelerometer/Gyroscope',
          typeIndex: BlockType.accGyr.value,
          instance: 0,
          name: 'Accel',
          isDynamic: false,
          entries: [_entry('Range Acceleration', 1, 'Enum', '±16 g', size: 1)],
        ),
      ],
    );
    final zip = buildBackupZip([device]);

    var off = 0;
    var checked = 0;
    final contents = <String, List<int>>{};
    while (off + 30 <= zip.length &&
        zip[off] == 0x50 &&
        zip[off + 1] == 0x4B &&
        zip[off + 2] == 0x03 &&
        zip[off + 3] == 0x04) {
      final method = zip[off + 8] | (zip[off + 9] << 8);
      final csize = zip[off + 18] | (zip[off + 19] << 8) | (zip[off + 20] << 16) | (zip[off + 21] << 24);
      final usize = zip[off + 22] | (zip[off + 23] << 8) | (zip[off + 24] << 16) | (zip[off + 25] << 24);
      final nlen = zip[off + 26] | (zip[off + 27] << 8);
      final elen = zip[off + 28] | (zip[off + 29] << 8);
      final name = String.fromCharCodes(zip.sublist(off + 30, off + 30 + nlen));
      final start = off + 30 + nlen + elen;
      final comp = zip.sublist(start, start + csize);
      final raw = method == 8 ? io.ZLibDecoder(raw: true).convert(comp) : comp;
      expect(usize, raw.length, reason: '$name declared $usize, actual ${raw.length}');
      contents[name] = raw;
      checked++;
      off = start + csize;
    }
    expect(checked, 1);
    expect(utf8.decode(contents['1_Tamu.json']!), contains('±16 g'));
    expect(parseBackupZip(zip).single.name, 'Tamu');
  });
}
