@Tags(['hil'])
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/subscription_client.dart';
import 'package:tamuapp/core/types.dart';
import 'hil_helpers.dart';

DeviceEntry? findTamu(DeviceDatabase db) {
  for (final d in db.all) {
    if (d.type == DeviceType.tamuV20A) return d;
  }
  return db.byId(1);
}

void main() {
  final skipReason =
      Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  late DeviceEntry tamu;

  setUpAll(() async {
    if (skipReason is String) return;
    await connectHil();
    final db = DeviceDatabase.instance;
    await db.refreshNetwork();
    await Future<void>.delayed(const Duration(seconds: 2));
    await db.refreshNetwork();
    tamu = findTamu(db) ?? (throw StateError('Tamu not found'));
  });
  tearDownAll(disconnectHil);

  test('the file table never holds two files with the same name', skip: skipReason,
      () async {
    final storage = StorageClient(deviceId: tamu.id);
    final subs = SubscriptionClient(deviceId: tamu.id);

    // Create is idempotently refused while the file exists.
    await storage.deleteFile('BKDUP');
    expect(await storage.createFile('BKDUP', 16), isTrue);
    expect(await storage.createFile('BKDUP', 16), isFalse,
        reason: 'creating an existing name must fail');
    await storage.deleteFile('BKDUP');

    // Create/delete and rename cycles must not leave superseded records behind.
    for (var i = 0; i < 3; i++) {
      await storage.createFile('BKDUP', 16);
      await storage.deleteFile('BKDUP');
    }
    await storage.deleteFile('RENAMED');
    await storage.createFile('TMPNAME', 16);
    expect(await storage.renameFile('TMPNAME', 'RENAMED'), isTrue);
    await storage.deleteFile('RENAMED');

    // Each requester-table save writes SUBREQ through a temp+rename.
    for (var i = 0; i < 3; i++) {
      final entry = RequesterSubscription(
        index: 0,
        providerAddr: tamu.id, // self, so the provider CID 1 completes quickly
        trid: 0,
        targetReg: makeBlockInfo(systemBlockTypeValue, 0, 6, 0),
        sourceReg: makeBlockInfo(systemBlockTypeValue, 0, 6, 0),
        trigger: TriggerType.periodic,
        periodMs: 500,
        minTimeMs: 100,
      );
      await subs.setRequesterSubscription(0, entry: entry);
      await subs.setRequesterSubscription(0); // delete -> another save
    }
    await Future<void>.delayed(const Duration(milliseconds: 300));

    final table = await storage.readFileTable() ?? const <FileRecord>[];
    final counts = <String, int>{};
    for (final record in table) {
      final name = normalizeFileName(record.name);
      counts[name] = (counts[name] ?? 0) + 1;
    }
    // ignore: avoid_print
    print('[STORAGE] files: $counts');

    for (final entry in counts.entries) {
      expect(entry.value, 1, reason: 'duplicate file "${entry.key}"');
    }
    expect(counts.containsKey('SUBREQ'), isTrue,
        reason: 'the requester table file must exist');
    expect(counts.containsKey('DYNMEM'), isFalse,
        reason: 'the obsolete DYNMEM file must be gone');
  }, timeout: const Timeout(Duration(seconds: 90)));

  test('dynamic persistence uses the DT_/DV_ files in the documented format',
      skip: skipReason, () async {
    final reg = RegisterClient(deviceId: tamu.id);
    final storage = StorageClient(deviceId: tamu.id);
    await reg.deleteDynamic(block: 0);
    await reg.createDynamicBlock(BlockType.dynamic, 'DYNCHK', index: 0);
    final blk = DynBlock(
        index: 0,
        meta: BlockMeta(flagsAndType: BlockType.dynamic.value, size: 1),
        name: 'DYNCHK');
    await reg.writeDynamicEntry(
        blk,
        0,
        0,
        BlockMeta(flagsAndType: DataType.number.value | FieldFlags.persistent, size: 4),
        numberToBytes(1.0));
    expect(await reg.saveDynamic(), isTrue);

    final names = (await storage.readFileTable() ?? [])
        .map((r) => normalizeFileName(r.name))
        .toSet();
    // ignore: avoid_print
    print('[STORAGE] after dynamic save: $names');
    expect(names.any((n) => n.startsWith('DT_')), isTrue);
    expect(names.any((n) => n.startsWith('DV_')), isTrue);

    // The DT_ table format matches the app decoder:
    // u8 name_len, name, u16 type, u16 entry_count, then 6 B per entry.
    final dtName = names.firstWhere((n) => n.startsWith('DT_'));
    final bytes = await storage.readFile(dtName, size: 64);
    expect(bytes, isNotNull);
    final nameLen = bytes![0];
    expect(String.fromCharCodes(bytes.sublist(1, 1 + nameLen)), 'DYNCHK');
    final type = bytes[1 + nameLen] | (bytes[1 + nameLen + 1] << 8);
    expect(type, BlockType.dynamic.value);
    final entryCount = bytes[1 + nameLen + 2] | (bytes[1 + nameLen + 3] << 8);
    expect(entryCount, 1);
    final entryFieldKey = bytes[1 + nameLen + 4] | (bytes[1 + nameLen + 5] << 8);
    final entryFlagsAndType =
        bytes[1 + nameLen + 6] | (bytes[1 + nameLen + 7] << 8);
    final entrySize = bytes[1 + nameLen + 8];
    expect(entryFieldKey, 0); // field 0 / key 0
    expect(entryFlagsAndType & 0x3FF, DataType.number.value);
    expect(entryFlagsAndType & FieldFlags.persistent, isNot(0));
    expect(entrySize, 4);

    await reg.deleteDynamic(block: 0);
    await reg.saveDynamic();
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('the Vysi layout is preloaded as LAY_1; old names are gone', skip: skipReason,
      () async {
    final storage = StorageClient(deviceId: tamu.id);
    final table = await storage.readFileTable() ?? const <FileRecord>[];
    final names = table.map((r) => normalizeFileName(r.name)).toSet();
    // ignore: avoid_print
    print('[STORAGE] files: $names');
    expect(names, contains('LAY_1'));
    expect(names, isNot(contains('VYSIV1')));
    expect(names, isNot(contains('LAY5X5')));

    final record = table.firstWhere((r) => normalizeFileName(r.name) == 'LAY_1');
    expect(record.size, 222, reason: '2-byte header + 11*10 u16 indexes');
    final bytes = await storage.readFile('LAY_1', size: 222);
    expect(bytes, isNotNull);
    expect(bytes!.length, 222);
    expect(bytes[0], 11, reason: 'u8 width');
    expect(bytes[1], 10, reason: 'u8 height');
    for (var i = 0; i < 110; i++) {
      final v = bytes[2 + i * 2] | (bytes[2 + i * 2 + 1] << 8);
      expect(v == 0xFFFF || v < 86, isTrue, reason: 'bad LED index at $i: $v');
    }
  }, timeout: const Timeout(Duration(seconds: 60)));
}
