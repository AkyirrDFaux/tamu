@Tags(['hil'])
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/keyedmem.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/types.dart';

import 'hil_helpers.dart';

/// Reproduces the keyed page flow step by step, checking for a device reboot
/// (uptime going backwards / ping failure) after every operation.
Future<void> runTests() async {
  final db = DeviceDatabase.instance;
  final keyed = KeyedMemoryClient(deviceId: 1);

  Future<void> step(String label, Future<void> Function() op) async {
    final before = await db.pingCore();
    if (!before) {
      // ignore: avoid_print
      print('[C] DEAD before step: $label');
      fail('device dead before $label');
    }
    try {
      await op();
    } catch (e) {
      // ignore: avoid_print
      print('[C] step "$label" THREW: $e');
    }
    await Future<void>.delayed(const Duration(milliseconds: 150));
    final alive = await db.pingCore();
    // ignore: avoid_print
    print('[C] step: $label -> alive=$alive');
    if (!alive) fail('DEVICE REBOOTED at step: $label');
  }

  // 1 create block
  final idx = await keyed.createBlock(BlockType.undefined, 'CRASH');
  var b = (await keyed.readBlocks())!.first;

  await step('append dict0', () async => b = (await keyed.appendDict(b), b).$2);
  await step('append dict1', () async => b = (await keyed.appendDict(b), b).$2);
  await step('append dict2', () async => b = (await keyed.appendDict(b), b).$2);
  final b2 = (await keyed.readBlocks())!.first;
  await step('write k1', () async {
    await keyed.writeKeyValue(
        b2, 0, 1, BlockMeta(flagsAndType: DataType.number.value, key: 1),
        numberToBytes(1.5));
  });
  await step('write k2', () async {
    await keyed.writeKeyValue(
        b2, 0, 2, BlockMeta(flagsAndType: DataType.integer.value, key: 2),
        [7, 0, 0, 0]);
  });
  await step('write k5 d1', () async {
    await keyed.writeKeyValue(
        b2, 1, 5, BlockMeta(flagsAndType: DataType.bool_.value, key: 5), [1]);
  });
  await step('write k9 d2', () async {
    await keyed.writeKeyValue(
        b2, 2, 9, BlockMeta(flagsAndType: DataType.string.value, key: 9),
        'hi'.codeUnits);
  });

  // open all dicts (page block-open)
  final b3 = (await keyed.readBlocks())!.first;
  await step('open block (readDict x3)', () async {
    for (var d = 0; d < b3.dictCount; d++) {
      await keyed.readDict(b3, d);
    }
  });

  // add a few more entries to stress SetKey
  await step('write k3 d0', () async {
    await keyed.writeKeyValue(
        b3, 0, 3, BlockMeta(flagsAndType: DataType.number.value, key: 3),
        numberToBytes(2.25));
  });
  await step('write k6 d1', () async {
    await keyed.writeKeyValue(
        b3, 1, 6, BlockMeta(flagsAndType: DataType.bool_.value, key: 6), [0]);
  });
  await step('write k7 d0', () async {
    await keyed.writeKeyValue(
        b3, 0, 7, BlockMeta(flagsAndType: DataType.vector.value, key: 7),
        numberToBytes(1.0) + numberToBytes(2.0) + numberToBytes(3.0));
  });

  // deletes
  await step('delete k3 d0', () async {
    await keyed.delete(block: b3.index, dict: 0, key: 3);
  });
  await step('delete dict1', () async {
    await keyed.delete(block: b3.index, dict: 1);
  });
  await step('reload dict0', () async {
    await keyed.readDict(b3, 0);
  });

  // fill the deleted dict1 slot (index 1): must start EMPTY, not expose the
  // deleted key5's stale data.
  await step('fill deleted dict1 (empty)', () async {
    final ok = await keyed.appendDict(b3, index: 1);
    final d = await keyed.readDict(b3, 1);
    // ignore: avoid_print
    print('[C] fill dict1: ok=$ok keys=${d == null ? "NULL" : d.keys} '
        'type=${d == null ? "?" : dataTypeOf(d)}');
  });
  await step('write into filled dict1', () async {
    await keyed.writeKeyValue(
        b3, 1, 8, BlockMeta(flagsAndType: DataType.integer.value, key: 8),
        [5, 0, 0, 0]);
    final d = await keyed.readDict(b3, 1);
    // ignore: avoid_print
    print('[C] dict1 after write: keys=${d == null ? "NULL" : d.keys}');
  });

  // refresh x5 (page auto refresh)
  await step('refresh x5', () async {
    for (var i = 0; i < 5; i++) {
      await keyed.readBlocks();
    }
  });

  // create block at gap index (padding)
  await step('create at index 8 (pad)', () async {
    await keyed.createBlock(BlockType.undefined, 'PAD8', index: 8);
  });
  final b4 = (await keyed.readBlocks())!;
  // ignore: avoid_print
  print('[C] blocks after: ${b4.map((x) => '${x.index}:${x.name}').toList()}');
  await step('open padded gap blocks', () async {
    for (final x in b4) {
      if (x.dictCount > 0) {
        for (var d = 0; d < x.dictCount; d++) {
          await keyed.readDict(x, d);
        }
      }
    }
  });

  // cleanup
  await step('purge', () async {
    for (final x in b4) {
      await keyed.delete(block: x.index);
    }
    await keyed.save();
  });
  // ignore: avoid_print
  print('[C] done');
}

String dataTypeOf(KeyedDict d) =>
    d.meta.typeValue == DataType.none.value
        ? 'None'
        : d.meta.typeValue == DataType.undefined.value
            ? 'Undefined'
            : 'type${d.meta.typeValue}';