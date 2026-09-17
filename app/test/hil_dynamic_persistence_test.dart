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
  setUpAll(() async => await connectHil());
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
    for (var i = 0; i < count; i++) await c.deleteDynamic(block: i);
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

  test('boot persistence restores persistent entries', skip: skipReason, () async {
    final port = Platform.environment['TAMU_HIL']!;
    final c = RegisterClient(deviceId: 1);
    final count = await c.getInstanceCount(BlockType.dynamic.value) ?? 0;
    for (var i = 0; i < count; i++) await c.deleteDynamic(block: i);
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
}