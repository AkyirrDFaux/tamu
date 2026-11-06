// Card t_ec99cf85: a restore over Bluetooth writes the `BLE Settings` rows.
//
// The firmware's write rule for the block is `BleWriteAllowed(AppFrameCameOverUsb(),
// AppFrameCameOverBle(), AppBLELinkSecured())` (`Blocks/BleWritePolicy.h`, asked by
// `Devices/Tamu_v2.0A/BleSettingsGlue.h`): USB always, a BLE frame only on a secured session. The
// app only holds secured sessions, so a restore planned over BLE must write those rows instead of
// marking them unplaceable (which is what it did while the rule was USB-only).
//
// This test drives the real stack over the bonded BLE link: capture the core, plan a restore with
// `linkWriteRefusals(overUsb: false)`, apply it, and read the device's OWN reply flags off the
// transport for every register write the run sends to block 0x0B. `0x0E` is START|STOP|TYPE|ACK -
// the accepted write (the same flags the firmware card measured for a bonded-BLE write).
//
// Only the `BLE Settings` rows are applied: every other item is deselected, which the restore reads
// as "not selected", never "not in the archive" - so nothing is unloaded, deleted or rewritten.
// The archive is a capture of this device, and its `Window` row is set to 0 first: that field is a
// COMMAND (a write sets the countdown), so writing the captured remaining-ms would open a new
// pairing window. 0 closes nothing and still exercises the write.
//
//   TAMU_HIL=ble flutter test --tags hil test/hil_ble_settings_restore_test.dart
@Timeout(Duration(minutes: 15))
@Tags(['hil'])
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/ble_security.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/types.dart';

import 'hil/hil_env.dart';

String _hex(int value) => '0x${value.toRadixString(16).padLeft(2, '0')}';

void main() {
  final env = HilEnv();
  setUpAll(env.connectAndScan);
  tearDownAll(env.finish);

  test('a restore planned and applied over BLE writes the BLE Settings rows', () async {
    if (!env.hilRequested) {
      markTestSkipped('TAMU_HIL not set');
      return;
    }
    final core = DeviceDatabase.instance.byId(coreId)?.id ?? coreId;

    // ---- the archive: a capture of this device --------------------------------------------
    final captured = await captureDevice(core, includeFiles: true);
    expect(
      captured,
      isNotNull,
      reason: 'the core is capturable over this link',
    );
    final archive = captured!;
    final blockAt = archive.blocks.indexWhere(
      (b) => b.typeIndex == BlockType.bleSettings.value,
    );
    expect(blockAt, isNot(-1), reason: 'the archive carries BLE Settings');
    final source = archive.blocks[blockAt];
    // The archive carries the block's PERSISTENT rows only (`backupBlockFromVisited`): `Enabled`
    // and `Display name` are the two the block persists, and they are exactly the two rows a
    // restore writes - the pair the earlier card measured failing over BLE. The trigger fields
    // (Passkey, Bonds, Window) are volatile state, not archive rows, so a restore never sends a
    // `Window` write that would open a pairing window.
    // ignore: avoid_print
    print(
      'ARCHIVE BLE Settings rows: '
      '${source.entries.map((e) => '${e.field}=${e.value}').join(', ')}',
    );
    expect(source.entries, hasLength(2));

    // ---- the plan, built the way the Backup page builds it over Bluetooth ------------------
    final plan = await buildRestorePlan([
      archive,
    ], linkRefusedBlocks: linkWriteRefusals(overUsb: false));
    expect(
      plan.linkRefusedBlocks,
      isEmpty,
      reason: 'a secured BLE link can write the block',
    );
    final bleItems = [
      for (final item in plan.items)
        if (item.block?.typeIndex == BlockType.bleSettings.value) item,
    ];
    final ready = bleItems.where((i) => i.ready).length;
    final refused = bleItems.where((i) => !i.ready).length;
    // ignore: avoid_print
    print(
      'PLAN BLE Settings: ${bleItems.length} rows, $ready ready, $refused unplaceable '
      '(${plan.items.length} items in the whole plan)',
    );
    for (final item in bleItems) {
      // ignore: avoid_print
      print(
        'PLAN  ${item.entry?.field} (field ${item.entry?.fieldIndex}): '
        'ready=${item.ready} selected=${item.selected} '
        'issue=${item.issue ?? '-'}',
      );
    }
    expect(
      refused,
      0,
      reason: 'no BLE Settings row is refused over the app\'s own link',
    );
    expect(ready, bleItems.length);

    // Only the block's rows are applied - the measurement is their write, and a deselected item
    // is "not selected", not "not in the archive" (so nothing is unloaded or deleted).
    for (final item in plan.items) {
      item.selected = item.block?.typeIndex == BlockType.bleSettings.value;
    }

    // ---- the device's own reply flags for those writes ------------------------------------
    final transport = ConnectionManager.instance.transport;
    expect(transport, isNotNull, reason: 'the BLE link is up');
    final replies = <String>[];
    final subscription = transport!.packetStream.listen((bytes) {
      try {
        final frame = PacketFrame.tryParse(bytes, 0);
        if (frame == null || !frame.isResponse || frame.isFrag) return;
        if (frame.srvTarget !=
            makeService(ServiceType.register, RegisterCid.write)) {
          return;
        }
        if (frame.payload.length < 4) return;
        final info = uint32FromBytes(frame.payload);
        if (blockInfoType(info) != BlockType.bleSettings.value) return;
        replies.add(
          'field ${blockInfoField(info)}: flags=${_hex(frame.flags)} '
          '(START|STOP|TYPE${frame.isFail ? '|FAIL' : ', FAIL clear'}, '
          '${frame.payload.length} B echoed)',
        );
      } on FormatException {
        // A frame the link handed over with a bad CRC is not a reply this run sent for.
      }
    });

    final report = await applyRestorePlan(plan);
    await Future<void>.delayed(const Duration(milliseconds: 300));
    await subscription.cancel();

    // ignore: avoid_print
    print('REPORT ${report.counts}');
    // ignore: avoid_print
    print(
      'APPLIED ${report.written} / SKIPPED ${report.skipped.length} / '
      'FAILED ${report.failed}',
    );
    for (final reply in replies) {
      // ignore: avoid_print
      print('DEVICE $reply');
    }
    for (final failure in report.failures) {
      // ignore: avoid_print
      print('FAILURE $failure');
    }

    expect(report.failed, 0, reason: report.text);
    expect(report.written, ready, reason: report.text);
    expect(
      replies,
      isNotEmpty,
      reason: 'the device answered every block write',
    );
    for (final reply in replies) {
      expect(
        reply,
        contains('flags=0x0e'),
        reason: 'the device accepted the write',
      );
    }

    // ---- read back: the rows the run wrote are the rows the device reports ----------------
    final after = await captureDevice(core, includeFiles: false);
    expect(after, isNotNull);
    final block = after!.blocks.firstWhere(
      (b) => b.typeIndex == BlockType.bleSettings.value,
    );
    // ignore: avoid_print
    print(
      'READ BACK ${block.entries.map((e) => '${e.field}=${e.value}').join(', ')}',
    );
    expect(block.entries, hasLength(2));
    expect(
      {for (final e in block.entries) e.fieldIndex: e.value},
      {for (final e in source.entries) e.fieldIndex: e.value},
      reason: 'the device holds the values the run wrote',
    );
  });
}
