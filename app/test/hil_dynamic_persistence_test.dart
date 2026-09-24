@Tags(['hil'])
library;
import 'dart:io';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/types.dart';
import 'hil_helpers.dart';

/// Verifies per-block dynamic persistence (Docs/Services/Register.md: DT_XXX / DV_XXX
/// files): save writes a block's table + persistent space, delete+save cleans the files
/// without shifting positions, and a device reset restores persistent entries from
/// flash (volatile entries come back zeroed).
void main() async {
  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;
  setUpAll(() async {
    if (skipReason is String) return;
    await connectHil();
  });
  tearDownAll(disconnectHil);

  test('per-block DT/DV save + cleanup', skip: skipReason, () async {
    final c = RegisterClient(deviceId: 1);
    final st = StorageClient(deviceId: 1);
    Future<Set<String>> names() async {
      final tbl = await st.readFileTable();
      if (tbl == null) return <String>{};
      return tbl.map((f) => normalizeFileName(f.name)).toSet();
    }

    // Clean slate.
    final count = await c.getInstanceCount(BlockType.dynamic.value) ?? 0;
    for (var i = 0; i < count; i++) {
      await c.deleteDynamic(block: i);
    }
    await c.saveDynamic();

    // RENDER at 0: persistent (0,0)=42, volatile (0,1)=7.
    await c.createDynamicBlock(BlockType.dynamic, 'RENDER', index: 0);
    final b = DynBlock(index: 0, meta: BlockMeta(flagsAndType: BlockType.dynamic.value, size: 1), name: 'RENDER');
    await c.writeDynamicEntry(b, 0, 0,
        BlockMeta(flagsAndType: FieldFlags.persistent | DataType.number.value, key: 0), numberToBytes(42.0));
    await c.writeDynamicEntry(b, 0, 1,
        BlockMeta(flagsAndType: DataType.number.value, key: 1), numberToBytes(7.0));
    await c.saveDynamic();

    var files = await names();
    if (!files.contains('DT_00') || !files.contains('DV_00')) {
      fail('DT_00/DV_00 missing after save: ${files.where((f) => f.startsWith('DT_') || f.startsWith('DV_')).join(',')}');
    }

    // Backup readback (CID 0x15) sees the persistent value.
    final backup = await c.readDynamicBackupField(b, 0);
    if (backup == null || (numberFromBytes(backup.value) - 42.0).abs() > 0.01) {
      fail('persistent backup readback wrong: ${backup?.value ?? []}');
    }

    // Delete + save removes the files; recreate at the same index works.
    await c.deleteDynamic(block: 0);
    await c.saveDynamic();
    files = await names();
    if (files.contains('DT_00') || files.contains('DV_00')) {
      fail('tombstoned files not cleaned on save');
    }
    final idx = await c.createDynamicBlock(BlockType.dynamic, 'RENDER', index: 0);
    if (idx != 0) fail('recreate not at index 0');
    await c.deleteDynamic(block: 0);
    await c.saveDynamic();
  });

  test('a write whose BlockMeta.Size exceeds the payload is clamped', skip: skipReason, () async {
    final c = RegisterClient(deviceId: 1);

    // Clean slate.
    final count = await c.getInstanceCount(BlockType.dynamic.value) ?? 0;
    for (var i = 0; i < count; i++) {
      await c.deleteDynamic(block: i);
    }
    await c.saveDynamic();
    await c.createDynamicBlock(BlockType.dynamic, 'CLAMP', index: 0);
    final b = DynBlock(index: 0, meta: BlockMeta(flagsAndType: BlockType.dynamic.value, size: 1), name: 'CLAMP');

    // Declare size 16 but send only 4 value bytes: the device must not read past the frame.
    final payload = <int>[
      ...blockInfoBytes(BlockType.dynamic.value, 0, 0, 0),
      ...BlockMeta(flagsAndType: DataType.number.value, key: 0, size: 16).toBytes(),
      ...numberToBytes(1.5),
    ];
    final reply = await c.request(2, payload: payload);
    print('[P] clamp write reply=${reply?.length}');
    if (reply == null) fail('oversized write got no reply');

    final e = await c.readDynamicField(b, 0);
    print('[P] clamp entry size=${e?.meta.size} value=${e?.value}');
    if (e == null || e.meta.size != 4 || e.value.length != 4) {
      fail('the oversized write was not clamped to the payload (size=${e?.meta.size})');
    }

    await c.deleteDynamic(block: 0);
    await c.saveDynamic();
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('boot persistence restores persistent entries', skip: skipReason, () async {
    final port = Platform.environment['TAMU_HIL']!;
    final c = RegisterClient(deviceId: 1);
    final count = await c.getInstanceCount(BlockType.dynamic.value) ?? 0;
    for (var i = 0; i < count; i++) {
      await c.deleteDynamic(block: i);
    }
    await c.saveDynamic();

    await c.createDynamicBlock(BlockType.dynamic, 'RENDER', index: 0);
    final b = DynBlock(index: 0, meta: BlockMeta(flagsAndType: BlockType.dynamic.value, size: 1), name: 'RENDER');
    await c.writeDynamicEntry(b, 0, 0,
        BlockMeta(flagsAndType: FieldFlags.persistent | DataType.number.value, key: 0), numberToBytes(42.0));
    await c.writeDynamicEntry(b, 0, 1,
        BlockMeta(flagsAndType: DataType.number.value, key: 1), numberToBytes(7.0));
    final saved = await c.saveDynamic();
    await Future<void>.delayed(const Duration(milliseconds: 500));
    final st = StorageClient(deviceId: 1);
    final files = await st.readFileTable();
    final names = files == null ? <String>[] :
        files.map((f) => normalizeFileName(f.name)).toList();
    print('[P] save=$saved DT_00=${names.contains('DT_00')} DV_00=${names.contains('DV_00')}');
    if (!names.contains('DT_00') || !names.contains('DV_00')) fail('files missing after save');

    // Hard reset: release the link, reset via esptool, re-connect.
    await ConnectionManager.instance.disconnect();
    await Process.run(
        '/home/akyirr/.platformio/penv/bin/python',
        ['/home/akyirr/.platformio/packages/tool-esptoolpy/esptool.py', '--port', port, 'run']);
    await Future<void>.delayed(const Duration(seconds: 5));
    final err = await connectHil();
    if (err != null) fail('reconnect failed: $err');

    final m = await c.readDynamicBlockMeta(0);
    if (m == null || m.name.trimRight() != 'RENDER') fail('block 0 not restored after reset');
    final b2 = DynBlock(index: 0, meta: m.meta, name: m.name);
    final v0 = await c.readDynamicField(b2, 0, 0);
    final v1 = await c.readDynamicField(b2, 0, 1);
    if (v0 == null || (numberFromBytes(v0.value) - 42.0).abs() > 0.01) fail('persistent 42 not restored');
    if (v1 == null || v1.value.length != 4 || !v1.value.every((x) => x == 0)) {
      fail('volatile entry should be zeroed after reset (not persisted)');
    }

    await c.deleteDynamic(block: 0);
    await c.saveDynamic();
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('a malformed DT table is rejected without overflowing', skip: skipReason, () async {
    final port = Platform.environment['TAMU_HIL']!;
    final c = RegisterClient(deviceId: 1);
    final st = StorageClient(deviceId: 1);

    // Clean slate, then persist one block so DT_00 / DV_00 exist.
    final count = await c.getInstanceCount(BlockType.dynamic.value) ?? 0;
    for (var i = 0; i < count; i++) {
      await c.deleteDynamic(block: i);
    }
    await c.saveDynamic();
    await c.createDynamicBlock(BlockType.dynamic, 'RENDER', index: 0);
    final b = DynBlock(index: 0, meta: BlockMeta(flagsAndType: BlockType.dynamic.value, size: 1), name: 'RENDER');
    await c.writeDynamicEntry(b, 0, 0,
        BlockMeta(flagsAndType: FieldFlags.persistent | DataType.number.value, key: 0), numberToBytes(42.0));
    await c.saveDynamic();
    await Future<void>.delayed(const Duration(milliseconds: 500));

    // Corrupt the table with a name length far beyond the block's 24-byte name buffer while
    // keeping the rest valid (one persistent entry of 4 bytes matches DV_00), so the only
    // thing that can reject it is the name-length bound. Before the fix this memcpy'd past
    // the loader's stack descriptor (and its NUL terminator) at boot.
    const fat = 0x0800 | 0x06; // Persistent | Number
    final corrupt = <int>[
      200, ...List<int>.filled(200, 0x41), // name length + name
      BlockType.dynamic.value & 0xFF, (BlockType.dynamic.value >> 8) & 0xFF, // type
      1, 0, // entry_count
      0, 0, // fieldKey (field 0, key 0)
      fat & 0xFF, (fat >> 8) & 0xFF, // flagsAndType
      4, 0, // size, pad
    ];
    if (!await st.writeFile('DT_00', corrupt)) fail('could not write the corrupt DT_00');

    await ConnectionManager.instance.disconnect();
    await Process.run(
        '/home/akyirr/.platformio/penv/bin/python',
        ['/home/akyirr/.platformio/packages/tool-esptoolpy/esptool.py', '--port', port, 'run']);
    await Future<void>.delayed(const Duration(seconds: 5));
    final err = await connectHil();
    if (err != null) fail('reconnect failed after the malformed table: $err');

    // The core must survive and reject the malformed table outright (no block 0).
    final m = await c.readDynamicBlockMeta(0);
    print('[P] malformed DT: block0 name=${m?.name}');
    if (m != null) fail('malformed DT was accepted: block 0 name=${m.name}');

    // Clean up the corrupt files (block 0 does not exist, so delete the files directly).
    await st.deleteFile('DT_00');
    await st.deleteFile('DV_00');
  }, timeout: const Timeout(Duration(minutes: 2)));
}