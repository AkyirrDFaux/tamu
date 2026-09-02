@Tags(['hil'])
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/dynmem.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/value_editor.dart' show dataTypeLabel;

import 'hil_helpers.dart';

/// Reproduces the dynamic memory page flow: create block, open, add entries
/// (append and at an explicit index), edit, delete, refresh.
void main() {
  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  setUpAll(() async {
    await connectHil();
  });

  tearDownAll(disconnectHil);

  test('dynamic page flow', () async {
    final dyn = DynamicMemoryClient(deviceId: 1);
    for (final b in await dyn.readBlocks() ?? <DynBlock>[]) {
      await dyn.delete(block: b.index);
    }
    await dyn.save();

    final idx = await dyn.createBlock(BlockType.undefined, 'DPROBE');
    // ignore: avoid_print
    print('[F] created block=$idx');

    var b = (await dyn.readBlocks())!.first;
    // ignore: avoid_print
    print('[F] fresh fieldCount=${b.fieldCount}');

    final e1 = await dyn.appendEntry(
        b, BlockMeta(flagsAndType: DataType.number.value, size: 4), numberToBytes(1.0));
    // ignore: avoid_print
    print('[F] append entry -> ${e1 == null ? "FAIL" : "ok"}');
    b = (await dyn.readBlocks())!.first;
    // ignore: avoid_print
    print('[F] after append fieldCount=${b.fieldCount}');

    final bOpen = (await dyn.readBlockMeta(b.index))!;
    final f0 = await dyn.readField(bOpen, 0);
    // ignore: avoid_print
    print('[F] field0 read: type=${f0 == null ? "NULL" : dataTypeLabel(f0.meta.dataType)} '
        'value=${f0 == null ? "?" : numberFromBytes(f0.value)}');
    final edit = await dyn.writeField(
        bOpen, f0!, numberToBytes(2.5));
    // ignore: avoid_print
    print('[F] edit field0 -> ${edit == null ? "FAIL" : numberFromBytes(edit)}');

    final del = await dyn.delete(block: bOpen.index, field: 0);
    b = (await dyn.readBlocks())!.first;
    // ignore: avoid_print
    print('[F] delete field0: ok=$del fieldCount=${b.fieldCount}');
    final f0b = await dyn.readField(b, 0);
    // ignore: avoid_print
    print('[F] field0 after delete: '
        '${f0b == null ? "NULL" : dataTypeLabel(f0b.meta.dataType)}');

    final e2 = await dyn.appendEntry(
        b, BlockMeta(flagsAndType: DataType.number.value, size: 4), numberToBytes(3.0),
        index: 0);
    // ignore: avoid_print
    print('[F] fill None slot 0 -> ${e2 == null ? "FAIL" : "ok"}');
    b = (await dyn.readBlocks())!.first;
    final f0c = await dyn.readField(b, 0);
    // ignore: avoid_print
    print('[F] field0 after fill: '
        '${f0c == null ? "NULL" : dataTypeLabel(f0c.meta.dataType)} = '
        '${f0c == null ? "?" : numberFromBytes(f0c.value)}');

    await dyn.delete(block: b.index);
    await dyn.save();
    // ignore: avoid_print
    print('[F] done');
  }, timeout: const Timeout(Duration(minutes: 3)), skip: skipReason);
}
