@Tags(['hil'])
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/types.dart';

import 'hil_helpers.dart';

/// Reproduces the dynamic memory page flow: create block, open, add entries
/// (append and at an explicit index), edit, delete, refresh. Shared by
/// `hil_test_suite.dart` and runnable standalone via `main`.
Future<void> runTests() async {
  final dyn = RegisterClient(deviceId: 1);
  // Delete every dynamic block; indices shift on removal, so re-read each time
  // instead of iterating a stale index list.
  for (;;) {
    final current = await dyn.readDynamicBlocks() ?? <DynBlock>[];
    if (current.isEmpty) break;
    await dyn.deleteDynamic(block: current.first.index);
  }
  await dyn.saveAll();

  expect(await dyn.createDynamicBlock('DPROBE'), isNotNull);
  var b = (await dyn.readDynamicBlocks())!.first;
  expect(b.fieldCount, 0, reason: 'a fresh block has no fields');

  final e1 = await dyn.appendDynamicEntry(
      b, ValueInfo(type: DataType.number.value, size: 4), numberToBytes(1.0));
  expect(e1, isNotNull, reason: 'append entry');
  b = (await dyn.readDynamicBlocks())!.first;
  expect(b.fieldCount, 1);

  final bOpen = (await dyn.readDynamicBlockMeta(b.index))!;
  final f0 = await dyn.readDynamicField(bOpen, 0);
  expect(f0, isNotNull);
  expect(numberFromBytes(f0!.value), closeTo(1.0, 0.001));

  final edit = await dyn.writeDynamicField(bOpen, f0, numberToBytes(2.5));
  expect(edit, isNotNull);
  expect(numberFromBytes(edit!), closeTo(2.5, 0.001));

  expect(await dyn.deleteDynamic(block: bOpen.index, field: 0), isTrue);
  b = (await dyn.readDynamicBlocks())!.first;
  expect(b.fieldCount, 0);
  expect(await dyn.readDynamicField(b, 0), isNull);

  final e2 = await dyn.appendDynamicEntry(
      b, ValueInfo(type: DataType.number.value, size: 4), numberToBytes(3.0),
      index: 0);
  expect(e2, isNotNull, reason: 'fill a None slot');
  b = (await dyn.readDynamicBlocks())!.first;
  final f0c = await dyn.readDynamicField(b, 0);
  expect(f0c, isNotNull);
  expect(numberFromBytes(f0c!.value), closeTo(3.0, 0.001));

  // A Read Only entry is non-writable from outside: both a value write and a delete fail.
  final e3 = await dyn.appendDynamicEntry(
      b, ValueInfo(type: DataType.number.value, flags: ValueFlags.readOnly, size: 4),
      numberToBytes(9.0), index: 1);
  expect(e3, isNotNull, reason: 'append a read-only entry');
  b = (await dyn.readDynamicBlocks())!.first;
  final ro = await dyn.readDynamicField(b, 1);
  expect(ro, isNotNull);
  expect(ro!.meta.readOnly, isTrue);
  expect(await dyn.writeDynamicField(b, ro, numberToBytes(10.0)), isNull,
      reason: 'a read-only entry rejects writes');
  expect(await dyn.deleteDynamic(block: b.index, field: 1), isFalse,
      reason: 'a read-only entry rejects deletes');

  await dyn.deleteDynamic(block: b.index);
  await dyn.saveAll();
}

void main() {
  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  setUpAll(() async {
    if (skipReason is String) return;
    await connectHil();
  });
  tearDownAll(disconnectHil);

  test('dynamic page flow', () async {
    // Skip quietly when the device has no dynamic memory capability.
    final link = ConnectionManager.instance;
    final payload = [0, 0, 0, 1]; // System capabilities (field 0, key 1)
    final capReply =
        await link.request(1, ServiceType.register, RegisterCid.read, payload: payload);
    if (capReply.length >= 12) {
      final caps = capReply[8] | (capReply[9] << 8) | (capReply[10] << 16) | (capReply[11] << 24);
      if ((caps & Capability.dynamicMemory) == 0) return;
    }
    await runTests();
  }, timeout: const Timeout(Duration(minutes: 3)), skip: skipReason);
}
