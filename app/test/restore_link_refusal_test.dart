// A restore over Bluetooth cannot write the blocks the firmware only accepts over USB
// (card t_686176aa).
//
// Measured on the rig over BLE: the two writable `BLE Settings` rows were sent and answered
// `FLAG_FAIL` with no reason payload, while the same rows restored cleanly over USB - the report
// could only say "the device refused the write", so a link limit read as a broken restore. The
// plan now knows what the link cannot write and says so per row, before anything is sent.

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/ble_security.dart';
import 'package:tamuapp/core/types.dart';

BackupDevice _archive() => BackupDevice(
  type: 'Tamu v2.0A',
  typeId: 1,
  id: idFromString('1.1')!,
  name: 'Akyirr',
  blocks: [
    BackupBlock(
      type: 'BLE Settings',
      typeIndex: BlockType.bleSettings.value,
      instance: 0,
      name: 'BLE Settings',
      isDynamic: false,
      entries: [
        BackupEntry(
          field: 'Enabled',
          fieldIndex: 0,
          key: 'Key 0',
          keyIndex: 0,
          type: 'Bool',
          flags: const ['Persistent'],
          value: true,
          size: 1,
        ),
      ],
    ),
  ],
);

/// The live core, carrying the same block with the same field so the row RESOLVES: the refusal
/// under test is the link's, not a missing target.
LiveDevice _live() => LiveDevice(
  id: idFromString('1.1')!,
  name: 'Akyirr',
  capabilities: Capability.core,
  blocks: [
    LiveBlock(
      type: BlockType.bleSettings.value,
      instance: 0,
      name: 'BLE Settings',
      isDynamic: false,
      entries: [
        LiveEntry(
          field: 0,
          key: 0,
          fieldName: 'Enabled',
          keyName: 'Key 0',
          meta: ValueInfo(type: DataType.bool_.value, size: 1),
          value: const [1],
        ),
      ],
    ),
  ],
);

RestorePlan _plan({required bool overUsb}) => planFromSnapshot(
  devices: [_archive()],
  live: [_live()],
  inventories: {
    _live().id: const LiveInventory(
      fileNames: {},
      loadedSlots: {},
      requesters: [],
      sndb: [],
    ),
  },
  linkRefusedBlocks: linkWriteRefusals(overUsb: overUsb),
);

void main() {
  test(
    'a BLE Settings row is unplaceable on a Bluetooth link, and says why',
    () {
      final plan = _plan(overUsb: false);
      final item = plan.items.single;

      expect(
        item.ready,
        isFalse,
        reason: 'the firmware refuses this write before it looks at the value',
      );
      expect(
        item.selected,
        isFalse,
        reason: 'an unplaceable row is never sent',
      );
      expect(item.issue, bleWriteRefusal(overUsb: false));
      expect(item.issue, contains('only be written over USB'));
    },
  );

  test('the same row is an ordinary write over a USB link', () {
    final plan = _plan(overUsb: true);
    final item = plan.items.single;

    expect(item.issue, isNull);
    expect(item.ready, isTrue);
    expect(item.selected, isTrue);
    expect(plan.linkRefusedBlocks, isEmpty);
  });

  test('the refusal names only the block the firmware restricts', () {
    expect(linkWriteRefusals(overUsb: false).keys, [
      BlockType.bleSettings.value,
    ]);
  });
}
