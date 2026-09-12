@Tags(['hil'])
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/value_editor.dart' show dataTypeLabel;

import 'hil_helpers.dart';

/// Reproduces the dynamic memory page flow: create block, open, add entries
/// (append and at an explicit index), edit, delete, refresh.
Future<void> runTests() async {
  final dyn = RegisterClient(deviceId: 1);
  // Delete every dynamic block. Indices SHIFT when a block is removed, so re-read
  // the list after each delete instead of iterating a stale index list.
  for (;;) {
    final current = await dyn.readDynamicBlocks() ?? <DynBlock>[];
    if (current.isEmpty) break;
    await dyn.deleteDynamic(block: current.first.index);
  }
  await dyn.saveDynamic();

  final _ = await dyn.createDynamicBlock(BlockType.dynamic, 'DPROBE');
  var b = (await dyn.readDynamicBlocks())!.first;
  // ignore: avoid_print
  print('[F] fresh fieldCount=${b.fieldCount}');

  final e1 = await dyn.appendDynamicEntry(
      b, BlockMeta(flagsAndType: DataType.number.value, size: 4), numberToBytes(1.0));
  // ignore: avoid_print
  print('[F] append entry -> ${e1 == null ? "FAIL" : "ok"}');
  b = (await dyn.readDynamicBlocks())!.first;
  // ignore: avoid_print
  print('[F] after append fieldCount=${b.fieldCount}');

  final bOpen = (await dyn.readDynamicBlockMeta(b.index))!;
  final f0 = await dyn.readDynamicField(bOpen, 0);
  // ignore: avoid_print
  print('[F] field0 read: type=${f0 == null ? "NULL" : dataTypeLabel(f0.meta.dataType)} '
      'value=${f0 == null ? "?" : numberFromBytes(f0.value)}');
  final edit = await dyn.writeDynamicField(
      bOpen, f0!, numberToBytes(2.5));
  // ignore: avoid_print
  print('[F] edit field0 -> ${edit == null ? "FAIL" : numberFromBytes(edit)}');

  final del = await dyn.deleteDynamic(block: bOpen.index, field: 0);
  b = (await dyn.readDynamicBlocks())!.first;
  // ignore: avoid_print
  print('[F] delete field0: ok=$del fieldCount=${b.fieldCount}');
  final f0b = await dyn.readDynamicField(b, 0);
  // ignore: avoid_print
  print('[F] field0 after delete: '
      '${f0b == null ? "NULL" : dataTypeLabel(f0b.meta.dataType)}');

  final e2 = await dyn.appendDynamicEntry(
      b, BlockMeta(flagsAndType: DataType.number.value, size: 4), numberToBytes(3.0),
      index: 0);
  // ignore: avoid_print
  print('[F] fill None slot 0 -> ${e2 == null ? "FAIL" : "ok"}');
  b = (await dyn.readDynamicBlocks())!.first;
  final f0c = await dyn.readDynamicField(b, 0);
  // ignore: avoid_print
  print('[F] field0 after fill: '
      '${f0c == null ? "NULL" : dataTypeLabel(f0c.meta.dataType)} = '
      '${f0c == null ? "?" : numberFromBytes(f0c.value)}');

  await dyn.deleteDynamic(block: b.index);
  await dyn.saveDynamic();
  // ignore: avoid_print
  print('[F] done');
}

void main() async {
  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  setUpAll(() async => await connectHil());
  tearDownAll(disconnectHil);

  test('dynamic page flow', () async {
    final link = ConnectionManager.instance;
    final payload = [0, 0, 0, 1]; // field 0, key 1 (capabilities)
    final capReply = await link.request(1, ServiceType.register, 1, payload: payload);
    if (capReply != null && capReply.length >= 12) {
      final caps = capReply[8] | (capReply[9] << 8) | (capReply[10] << 16) | (capReply[11] << 24);
      if ((caps & Capability.dynamicMemory) == 0) {
        print('Skipping: device does not have dynamic memory capability (caps=0x${caps.toRadixString(16)})');
        return;
      }
    }
    
    final dyn = RegisterClient(deviceId: 1);
    // Delete every dynamic block; indices shift on removal, so re-read each time.
    for (;;) {
      final current = await dyn.readDynamicBlocks() ?? <DynBlock>[];
      if (current.isEmpty) break;
      await dyn.deleteDynamic(block: current.first.index);
    }
    await dyn.saveDynamic();

final _ = await dyn.createDynamicBlock(BlockType.dynamic, 'DPROBE');
    var b = (await dyn.readDynamicBlocks())!.first;
    // ignore: avoid_print
    print('[F] fresh fieldCount=${b.fieldCount}');

    final e1 = await dyn.appendDynamicEntry(
        b, BlockMeta(flagsAndType: DataType.number.value, size: 4), numberToBytes(1.0));
    // ignore: avoid_print
    print('[F] append entry -> ${e1 == null ? "FAIL" : "ok"}');
    b = (await dyn.readDynamicBlocks())!.first;
    // ignore: avoid_print
    print('[F] after append fieldCount=${b.fieldCount}');

    final bOpen = (await dyn.readDynamicBlockMeta(b.index))!;
    final f0 = await dyn.readDynamicField(bOpen, 0);
    // ignore: avoid_print
    print('[F] field0 read: type=${f0 == null ? "NULL" : dataTypeLabel(f0.meta.dataType)} '
        'value=${f0 == null ? "?" : numberFromBytes(f0.value)}');
    final edit = await dyn.writeDynamicField(
        bOpen, f0!, numberToBytes(2.5));
    // ignore: avoid_print
    print('[F] edit field0 -> ${edit == null ? "FAIL" : numberFromBytes(edit)}');

    final del = await dyn.deleteDynamic(block: bOpen.index, field: 0);
    b = (await dyn.readDynamicBlocks())!.first;
    // ignore: avoid_print
    print('[F] delete field0: ok=$del fieldCount=${b.fieldCount}');
    final f0b = await dyn.readDynamicField(b, 0);
    // ignore: avoid_print
    print('[F] field0 after delete: '
        '${f0b == null ? "NULL" : dataTypeLabel(f0b.meta.dataType)}');

    final e2 = await dyn.appendDynamicEntry(
        b, BlockMeta(flagsAndType: DataType.number.value, size: 4), numberToBytes(3.0),
        index: 0);
    // ignore: avoid_print
    print('[F] fill None slot 0 -> ${e2 == null ? "FAIL" : "ok"}');
    b = (await dyn.readDynamicBlocks())!.first;
    final f0c = await dyn.readDynamicField(b, 0);
    // ignore: avoid_print
    print('[F] field0 after fill: '
        '${f0c == null ? "NULL" : dataTypeLabel(f0c.meta.dataType)} = '
        '${f0c == null ? "?" : numberFromBytes(f0c.value)}');

    await dyn.deleteDynamic(block: b.index);
    await dyn.saveDynamic();
    // ignore: avoid_print
    print('[F] done');
  }, timeout: const Timeout(Duration(minutes: 3)), skip: skipReason);
}
