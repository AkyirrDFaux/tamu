@Tags(['hil'])
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/subscription_client.dart';
import 'package:tamuapp/core/types.dart';
import 'hil_helpers.dart';

void main() {
  final skipReason = hilSetup();

  late DeviceEntry tamu;

  setUpAll(() async {
    if (skipReason != null) return;
    final db = DeviceDatabase.instance;
    await db.refreshNetwork();
    await Future<void>.delayed(const Duration(seconds: 2));
    await db.refreshNetwork();
    tamu = findTamu(db) ?? (throw StateError('Tamu not found'));
  });

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
      if (i < 2) await subs.setRequesterSubscription(0); // delete -> another save
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
    expect(counts.containsKey('.SUBREQ'), isTrue,
        reason: 'the requester table file must exist');
    expect(counts.containsKey('DYNMEM'), isFalse,
        reason: 'the obsolete DYNMEM file must be gone');
  }, timeout: const Timeout(Duration(seconds: 90)));

  test('dynamic persistence uses the .DT_/.DV_ files in the documented format',
      skip: skipReason, () async {
    final reg = RegisterClient(deviceId: tamu.id);
    final storage = StorageClient(deviceId: tamu.id);
    await reg.deleteDynamic(block: 0);
    await reg.createDynamicBlock('DYNCHK', index: 0);
    final blk = DynBlock(
        index: 0,
        meta: ValueInfo(type: BlockType.dynamic.value, size: 1),
        name: 'DYNCHK');
    await reg.writeDynamicEntry(
        blk,
        0,
        0,
        ValueInfo(type: DataType.number.value, flags: ValueFlags.persistent, size: 4),
        numberToBytes(1.0));
    expect(await reg.saveAll(), isTrue);

    final names = (await storage.readFileTable() ?? [])
        .map((r) => normalizeFileName(r.name))
        .toSet();
    // ignore: avoid_print
    print('[STORAGE] after Save All: $names');
    expect(names.any((n) => n.startsWith('.DT_')), isTrue);
    expect(names.any((n) => n.startsWith('.DV_')), isTrue);

    // The .DT_ table format matches the app decoder: Name (16 chars, NUL-padded),
    // u16 entry_count, u16 reserved, then 8 B per entry (Field&Key, MemoryOffset,
    // ValueInfo). The block's bank type is derived from the file's global index, not stored.
    final dtName = names.firstWhere((n) => n.startsWith('.DT_'));
    final bytes = await storage.readFile(dtName, size: 64);
    expect(bytes, isNotNull);
    expect(String.fromCharCodes(bytes!.sublist(0, 6)), 'DYNCHK');
    final entryCount = bytes[16] | (bytes[17] << 8);
    expect(entryCount, 1);
    final entryFieldKey = bytes[20] | (bytes[21] << 8);
    final entryOffset = bytes[22] | (bytes[23] << 8);
    final entryType = bytes[24] | (bytes[25] << 8);
    final entrySize = bytes[26];
    final entryFlags = bytes[27];
    expect(entryFieldKey, 0); // field 0 / key 0
    expect(entryOffset, 0);
    expect(entryType & 0x3FF, DataType.number.value);
    expect(entryFlags & ValueFlags.persistent, isNot(0));
    expect(entrySize, 4);

    await reg.deleteDynamic(block: 0);
    await reg.saveAll();
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
    expect(record.size, 223, reason: '3-byte header (limit, w, h) + 11*10 u16 indexes');
    final bytes = await storage.readFile('LAY_1', size: 223);
    expect(bytes, isNotNull);
    expect(bytes!.length, 223);
    expect(bytes[0], 178, reason: 'u8 brightness limit (0-255 as a percentage: 70%)');
    expect(bytes[1], 11, reason: 'u8 width');
    expect(bytes[2], 10, reason: 'u8 height');
    for (var i = 0; i < 110; i++) {
      final v = bytes[3 + i * 2] | (bytes[3 + i * 2 + 1] << 8);
      expect(v == 0xFFFF || v < 86, isTrue, reason: 'bad LED index at $i: $v');
    }
  }, timeout: const Timeout(Duration(seconds: 60)));
}
