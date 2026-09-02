@Tags(['hil'])
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/keyedmem.dart';
import 'package:tamuapp/core/types.dart';

import 'hil_helpers.dart';

/// Reproduces the keyed memory page refresh + dictionary-open flow.
void main() {
  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  setUpAll(() async {
    await connectHil();
  });

  tearDownAll(disconnectHil);

  test('keyed page refresh probe', () async {
    final keyed = KeyedMemoryClient(deviceId: 1);

    // purge
    final existing = await keyed.readBlocks();
    // ignore: avoid_print
    print('[R] existing: ${existing?.map((b) => '${b.index}:${b.name}').toList()}');
    for (final b in existing ?? []) {
      await keyed.delete(block: b.index);
    }
    await keyed.save();

    // build a populated block: 3 dicts, several entries incl. a deleted key
    final idx = await keyed.createBlock(BlockType.undefined, 'RPROBE');
    // ignore: avoid_print
    print('[R] created block=$idx');
    final b0 = (await keyed.readBlocks())!.first;
    await keyed.appendDict(b0); // dict0
    await keyed.appendDict(b0); // dict1
    await keyed.appendDict(b0); // dict2
    final b1 = (await keyed.readBlocks())!.first;
    await keyed.writeKeyValue(
        b1, 0, 1, BlockMeta(flagsAndType: DataType.number.value, key: 1), numberToBytes(1.5));
    await keyed.writeKeyValue(
        b1, 0, 2, BlockMeta(flagsAndType: DataType.integer.value, key: 2), [7, 0, 0, 0]);
    await keyed.writeKeyValue(
        b1, 1, 5, BlockMeta(flagsAndType: DataType.bool_.value, key: 5), [1]);
    await keyed.writeKeyValue(
        b1, 2, 9, BlockMeta(flagsAndType: DataType.string.value, key: 9), 'hi'.codeUnits);
    await keyed.writeKeyValue(
        b1, 0, 3, BlockMeta(flagsAndType: DataType.number.value, key: 3), numberToBytes(2.25));
    // delete key 3 -> creates a None placeholder inside dict0
    final del = await keyed.delete(block: b1.index, dict: 0, key: 3);
    // ignore: avoid_print
    print('[R] deleted key3: $del');

    // ---- simulate the page refresh: readBlocks() + per-dict load ----
    final blocks = await keyed.readBlocks();
    // ignore: avoid_print
    print('[R] refresh readBlocks: ${blocks == null ? "NULL" : blocks.map((b) => "${b.index}:${b.name} dicts=${b.dictCount}").toList()}');
    if (blocks == null) fail('readBlocks returned null');

    for (final block in blocks) {
      for (var d = 0; d < block.dictCount; d++) {
        final dict = await keyed.readDict(block, d);
        // ignore: avoid_print
        print('[R] block ${block.index} dict $d: '
            'metaType=${dict == null ? "NULL" : dataTypeOf(dict)} '
            'visible=${dict?.keys}');
      }
    }

    // ---- repeat refresh to catch instability ----
    for (var i = 0; i < 3; i++) {
      final again = await keyed.readBlocks();
      if (again == null) {
        // ignore: avoid_print
        print('[R] refresh attempt $i FAILED (null)');
      } else {
        // ignore: avoid_print
        print('[R] refresh attempt $i ok: ${again.length} blocks');
      }
    }

    await keyed.delete(block: b1.index);
    await keyed.save();
    // ignore: avoid_print
    print('[R] done');
  }, timeout: const Timeout(Duration(minutes: 3)), skip: skipReason);
}

String dataTypeOf(KeyedDict d) =>
    d.meta.typeValue == DataType.none.value
        ? 'None'
        : d.meta.typeValue == DataType.undefined.value
            ? 'Undefined'
            : 'type${d.meta.typeValue}';
