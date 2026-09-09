@Tags(['hil'])
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/keyedmem.dart';
import 'package:tamuapp/core/dynmem.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/value_editor.dart' show dataTypeLabel;

import 'hil_helpers.dart';

/// Live probe of the Keyed/Dynamic memory clients against the Tamu.
Future<void> runTests() async {
  // ---- keyed ----
  final keyed = KeyedMemoryClient(deviceId: 1);
  // purge leftovers
  final existing = await keyed.readBlocks();
  // ignore: avoid_print
  print('[K] existing blocks: '
      '${existing?.map((b) => '${b.index}:${b.name}(${b.blockType.label}, ${b.dictCount} dicts)').toList()}');
  for (final b in existing ?? []) {
    await keyed.delete(block: b.index);
  }
  await keyed.save();
  final idx = await keyed.createBlock(BlockType.undefined, 'PROBE');
  // ignore: avoid_print
  print('[K] created block index=$idx');
  final blocks = await keyed.readBlocks();
  final block = blocks!.first;
  // ignore: avoid_print
  print('[K] block readback: index=${block.index} name=${block.name} '
      'dictCount=${block.dictCount}');
  final dictOk = await keyed.appendDict(block);
  // ignore: avoid_print
  print('[K] appendDict=$dictOk');
  final fresh = (await keyed.readBlocks())!.first;
  // ignore: avoid_print
  print('[K] after append: dictCount=${fresh.dictCount}');
  final dict = await keyed.readDict(fresh, 0);
  // ignore: avoid_print
  print('[K] readDict: ${dict == null ? "NULL" : "keys=${dict.keys} meta=${dict.meta.flagsAndType}"}');
  final w = await keyed.writeKeyValue(
      fresh, 0, 7, BlockMeta(flagsAndType: DataType.number.value, size: 4), numberToBytes(1.5));
  // ignore: avoid_print
  print('[K] writeKeyValue=${w != null && w.isNotEmpty ? numberFromBytes(w) : "NULL"}');
  final entry = await keyed.readEntry(fresh, 0, 7);
  // ignore: avoid_print
  print('[K] readEntry: ${entry == null ? "NULL" : numberFromBytes(entry.value)}');
  // second dictionary
  final dictOk2 = await keyed.appendDict(fresh);
  final fresh2 = (await keyed.readBlocks())!.first;
  // ignore: avoid_print
  print('[K] second appendDict=$dictOk2 dictCount=${fresh2.dictCount}');
  final d0 = await keyed.readDict(fresh2, 0);
  final d1 = await keyed.readDict(fresh2, 1);
  // ignore: avoid_print
  print('[K] dict0=${d0 == null ? "NULL" : d0.keys} dict1=${d1 == null ? "NULL" : d1.keys}');
  await keyed.writeKeyValue(
      fresh2, 1, 3, BlockMeta(flagsAndType: DataType.bool_.value, key: 3), [1]);
  final e2 = await keyed.readEntry(fresh2, 1, 3);
  final d1b = await keyed.readDict(fresh2, 1);
  // ignore: avoid_print
  print('[K] entry(d1,k3)=${e2 == null ? "NULL" : e2.value} '
      'dict1 after=${d1b == null ? "NULL" : d1b.keys}');

  // ---- dictionary type set + append-with-type ----
  final typeSet = await keyed.writeDictMeta(fresh2, 1, DataType.integer);
  final d1Typed = await keyed.readDict(fresh2, 1);
  // ignore: avoid_print
  print('[K] dict1 type set=$typeSet readback=${d1Typed?.meta.dataType}');
  expect(typeSet, isTrue, reason: 'dict type write failed');
  expect(d1Typed?.meta.dataType, DataType.integer,
      reason: 'dict type did not stick');
  final dictWithType = await keyed.appendDict(fresh2, type: DataType.colour);
  final fresh3 = (await keyed.readBlocks())!.first;
  final d3 = await keyed.readDict(fresh3, fresh3.dictCount - 1);
  // ignore: avoid_print
  print('[K] append-with-type=$dictWithType type=${d3?.meta.dataType}');
  expect(d3?.meta.dataType, DataType.colour,
      reason: 'append-with-type did not stick');

  // ---- backup value read (CID 4) ----
  await keyed.save();
  final bkEntry = await keyed.readBackupEntry(fresh2, 1, 3);
  // ignore: avoid_print
  print('[K] backup entry(d1,k3)=${bkEntry == null ? "NULL" : bkEntry.value} '
      'type=${bkEntry?.meta.dataType}');
  expect(bkEntry, isNotNull, reason: 'keyed backup read returned null');
  expect(bkEntry?.meta.dataType, DataType.bool_,
      reason: 'keyed backup entry type mismatch');

  // ---- dynmem type change ----
  final dyn = DynamicMemoryClient(deviceId: 1);
  final dExisting = await dyn.readBlocks();
  for (final b in dExisting ?? []) {
    await dyn.delete(block: b.index);
  }
  await dyn.save();
  final didx = await dyn.createBlock(BlockType.undefined, 'DPROBE');
  // ignore: avoid_print
  print('[D] created block index=$didx');
  // ---- backup value read (CID 4) ----
  final dBlocks = await dyn.readBlocks();
  final dBk = dBlocks?.firstOrNull;
  if (dBk != null) {
    await dyn.writeField(
        dBk,
        DynField(index: 0,
            meta: BlockMeta(flagsAndType: DataType.number.value),
            value: numberToBytes(2.5)),
        numberToBytes(2.5));
    await dyn.save();
    final bkField = await dyn.readBackupField(dBk, 0);
    // ignore: avoid_print
    print('[D] backup field0=${bkField == null ? "NULL" : numberFromBytes(bkField.value)}');
    expect(bkField, isNotNull, reason: 'dyn backup read returned null');
    if (bkField != null) {
      expect(numberFromBytes(bkField.value), closeTo(2.5, 0.01),
          reason: 'dyn backup value mismatch');
    }
  }
  // ---- multi-block delete semantics (dyn) ----
  await dyn.save();
  final iA = await dyn.createBlock(BlockType.undefined, 'AAA');
  final iB = await dyn.createBlock(BlockType.undefined, 'BBB');
  final iC = await dyn.createBlock(BlockType.undefined, 'CCC');
  // ignore: avoid_print
  print('[M] created A=$iA B=$iB C=$iC');
  await dyn.delete(block: iB!);
  final afterDelB = await dyn.readBlocks();
  // ignore: avoid_print
  print('[M] after deleting B: '
      '${afterDelB?.map((b) => b.name).toList()}');
  final aStill = afterDelB?.where((b) => b.name == 'AAA').firstOrNull;
  if (aStill != null) {
    final e = await dyn.appendEntry(
        aStill, BlockMeta(flagsAndType: DataType.number.value), numberToBytes(5));
    // ignore: avoid_print
    print('[M] append into surviving A=${e != null ? numberFromBytes(e) : "FAIL"}');
  }
  // cleanup A and C so later runs start clean (keep DPROBE for the [D] part)
  for (final b in afterDelB ?? []) {
    if (b.name != 'DPROBE') {
      await dyn.delete(block: b.index);
    }
  }
  await dyn.save();


  final dblocks = (await dyn.readBlocks())!;
  final dblock = dblocks.first;
  final appended = await dyn.appendEntry(dblock,
      BlockMeta(flagsAndType: DataType.number.value), numberToBytes(9));
  // ignore: avoid_print
  print('[D] appendEntry=${appended != null ? numberFromBytes(appended) : "NULL"}');
  await dyn.readField(dblock, 0);
  // now change the entry TYPE to bool
  final changed = await dyn.writeField(
      dblock,
      dblock.fields[0]!,
      [1],
      newType: DataType.bool_);
  // ignore: avoid_print
  print('[D] type change to bool: ${changed == null ? "NULL" : "value bytes $changed"}');
  await dyn.readField(dblock, 0);
  // ignore: avoid_print
  print('[D] field meta after change: '
      '${DataType.fromValue(dblock.fields[0]!.meta.typeValue)}');
  // refresh meta so appendEntry targets the real next index
  final dblockFresh = await dyn.readBlockMeta(dblock.index);
  final dblock2 = dblockFresh ?? dblock;
  // single-entry delete: add a second entry, delete it, first must survive
  final appended2 = await dyn.appendEntry(dblock2,
      BlockMeta(flagsAndType: DataType.bool_.value), [1]);
  // ignore: avoid_print
  print('[D] second entry appended=${appended2 != null}');
  final delOne = await dyn.delete(block: dblock2.index, field: 1);
  final afterDel = await dyn.readBlocks();
  final dBlockAfter = afterDel!.where((b) => b.name == 'DPROBE').first;
  // ignore: avoid_print
  print('[D] delete one entry ok=$delOne '
      'block still present=true fieldCount=${dBlockAfter.fieldCount}');
  // stable-index entry delete: field 1 was deleted -> field 0 must still be
  // AT INDEX 0 (never shifted), and field 1 reads back as a None placeholder.
  final kept = await dyn.readField(dblock2, 0);
  final noneField = await dyn.readField(dblock2, 1);
  // ignore: avoid_print
  print('[D] field0 after field1-delete: '
      'type=${kept == null ? "NULL" : DataType.fromValue(kept.meta.typeValue)} '
      'value=${kept == null ? "?" : formatProbe(DataType.fromValue(kept.meta.typeValue), kept.value)} '
      'indexStable=${kept != null && kept.index == 0}');
  // ignore: avoid_print
  print('[D] field1 reads back as: '
      '${noneField == null ? "NULL" : DataType.fromValue(noneField.meta.typeValue)}');

  // ---- keyed key delete (in-place None, others survive) ----
  await keyed.readBlockMeta(idx!);
  final kprobe = (await keyed.readBlocks())!.first;
  await keyed.readDict(kprobe, 0);
  final delKeyOk = await keyed.writeKeyValue(
      kprobe, 0, 7, BlockMeta(flagsAndType: DataType.none.value, key: 7), []);
  final d0c = await keyed.readDict(kprobe, 0);
  // ignore: avoid_print
  print('[K] keyed key7 delete: ok=${delKeyOk != null} '
      'keys after=${d0c == null ? "NULL" : d0c.keys}');
  final k7gone = await keyed.readEntry(kprobe, 0, 7);
  final k3still = await keyed.readEntry(kprobe, 1, 3);
  // ignore: avoid_print
  print('[K] key7 invisible=${k7gone == null} '
      'dict1 key3 still readable=${k3still != null} '
      'value=${k3still == null ? "?" : k3still.value}');
  // dictionary type change still lands after a None-marked entry
  final dictType = await keyed.writeKeyValue(
      kprobe, 0, 9, BlockMeta(flagsAndType: DataType.integer.value, key: 9),
      [42, 0, 0, 0]);
  final d0d = await keyed.readDict(kprobe, 0);
  // ignore: avoid_print
  print('[K] add key9 after delete: ok=${dictType != null} '
      'keys=${d0d == null ? "NULL" : d0d.keys}');

  // ---- keyed delete via CID 1 (key level) ----
  final delKeyCid1 = await keyed.delete(block: kprobe.index, dict: 1, key: 3);
  final d1c = await keyed.readDict(kprobe, 1);
  // ignore: avoid_print
  print('[K] CID1 key3 delete: ok=$delKeyCid1 keys after='
      '${d1c == null ? "NULL" : d1c.keys}');

  // ---- keyed delete via CID 1 (dictionary level) ----
  final delDictOk = await keyed.delete(block: kprobe.index, dict: 0);
  final d0e = await keyed.readDict(kprobe, 0);
  // ignore: avoid_print
  print('[K] CID1 dict0 delete: ok=$delDictOk '
      'dict0 after=${d0e == null ? "NULL" : d0e.keys} '
      'metaType=${d0e == null ? "?" : dataTypeLabel(d0e.meta.dataType)}');

  // ---- create-at-index: repurpose the None tombstone of dict0's block? No -
  //     create a fresh block pinned to the tombstoned PROBE index... instead
  //     verify appending a new block after deletion lands after the tombstone
  //     and that an explicit index fills a deleted block slot.
  await keyed.save();
  final kb2 = await keyed.createBlock(BlockType.undefined, 'KPIN2');
  final deletePinned = await keyed.delete(block: kb2!);
  final kb3 = await keyed.createBlock(BlockType.undefined, 'KPIN3', index: kb2);
  // ignore: avoid_print
  print('[K] create-at-index: deleted=$deletePinned recreated at $kb3 '
      '(same slot=${kb3 == kb2})');
  final kblocks = (await keyed.readBlocks())!;
  // ignore: avoid_print
  print('[K] blocks after pin test: '
      '${kblocks.map((b) => '${b.index}:${b.name}').toList()}');

  // ---- index padding: create blocks at gaps (None pads the holes) ----
  await keyed.save();
  final kpad = await keyed.createBlock(BlockType.undefined, 'KPAD', index: 7);
  final ksum = await keyed.readBlocks();
  // ignore: avoid_print
  print('[K] create KPAD at index 7 -> index=$kpad '
      'total=${ksum == null ? "?" : ksum.length} '
      'visible=${ksum == null ? "?" : ksum.map((b) => b.name).toList()}');
  // block meta of a padded None slot answers with type None
  final padMeta = await keyed.readBlockMeta(5);
  // ignore: avoid_print
  print('[K] padded slot 5 meta type='
      '${padMeta == null ? "NULL" : padMeta.blockType.label}');

  // ---- dynamic: same padding for blocks and entries ----
  await dyn.save();
  final dpad = await dyn.createBlock(BlockType.undefined, 'DPAD', index: 6);
  final dsum = await dyn.readBlocks();
  // ignore: avoid_print
  print('[D] create DPAD at index 6 -> index=$dpad '
      'total=${dsum == null ? "?" : dsum.length} '
      'visible=${dsum == null ? "?" : dsum.map((b) => b.name).toList()}');
  final dpadBlock = (await dyn.readBlocks())!
      .where((b) => b.name == 'DPAD').firstOrNull;
  if (dpadBlock != null) {
    // entry at index 3 pads fields 0..2 with None
    final e3 = await dyn.appendEntry(
        dpadBlock,
        BlockMeta(flagsAndType: DataType.number.value, size: 4),
        numberToBytes(3.5),
        index: 3);
    final fresh = await dyn.readBlockMeta(dpadBlock.index);
    // ignore: avoid_print
    print('[D] entry at index 3: ok=${e3 != null} '
        'fieldCount=${fresh?.fieldCount}');
    final f0 = await dyn.readField(fresh!, 0);
    final f3 = await dyn.readField(fresh, 3);
    // ignore: avoid_print
    print('[D] pad field0 type=${f0 == null ? "NULL" : dataTypeLabel(f0.meta.dataType)} '
        'field3=${f3 == null ? "NULL" : formatProbe(f3.meta.dataType, f3.value)}');
  }
  // cleanup pads
  await dyn.save();
  for (final b in (await dyn.readBlocks()) ?? <DynBlock>[]) {
    await dyn.delete(block: b.index);
  }
  await dyn.save();
}

String formatProbe(DataType type, List<int> bytes) {
  if (type == DataType.bool_) return bytes.isNotEmpty && bytes[0] != 0 ? 'true' : 'false';
  if (type == DataType.number && bytes.length >= 4) {
    final v = numberFromBytes(bytes);
    return v.toStringAsFixed(2);
  }
  return 'bytes=$bytes';
}

void main() async {
  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;
  final isDAS = Platform.environment['TAMU_HIL'] == '/dev/ttyACM1';
  final skipReasonDAS = isDAS ? 'DAS does not have dynamic/keyed memory services' : (skipReason is String ? skipReason : false);

  setUpAll(() async => await connectHil());
  tearDownAll(disconnectHil);

  test('memory probe - keyed & dynamic', () async {
    await runTests();
  }, timeout: const Timeout(Duration(minutes: 5)), skip: skipReasonDAS);
}