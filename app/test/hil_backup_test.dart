@Tags(['hil'])
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/backup_script.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/script_draft.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/types.dart';
import 'hil_helpers.dart';

DeviceEntry? findTamu(DeviceDatabase db) {
  for (final d in db.all) {
    if (d.type == DeviceType.tamuV20A) return d;
  }
  return db.byId(1);
}

Future<void> discoverDevices() async {
  final db = DeviceDatabase.instance;
  await db.refreshRuntime(0);
  await db.refreshNetwork();
  await Future<void>.delayed(const Duration(seconds: 2));
  await db.refreshNetwork();
  await Future<void>.delayed(const Duration(milliseconds: 500));
}

void main() {
  final skipReason =
      Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  late DeviceEntry tamu;

  setUpAll(() async {
    if (skipReason is String) return;
    await connectHil();
    await discoverDevices();
    tamu = findTamu(DeviceDatabase.instance) ??
        (throw StateError('Tamu not found'));
    final reg = RegisterClient(deviceId: tamu.id);
    await reg.deleteDynamic(block: 0);
    await reg.saveDynamic();
    final storage = StorageClient(deviceId: tamu.id);
    await storage.deleteFile('BKTEST');
    await Future<void>.delayed(const Duration(milliseconds: 300));
  });
  tearDownAll(disconnectHil);

  test('backup captures the whole registry semantically and restores entries',
      skip: skipReason, () async {
    final reg = RegisterClient(deviceId: tamu.id);
    await reg.deleteDynamic(block: 0);
    await reg.createDynamicBlock(BlockType.dynamic, 'BKTEST', index: 0);
    final block = DynBlock(
        index: 0,
        meta: BlockMeta(flagsAndType: BlockType.dynamic.value, size: 2),
        name: 'BKTEST');
    await reg.writeDynamicEntry(block, 0, 0,
        BlockMeta(flagsAndType: DataType.number.value, size: 4), numberToBytes(42.0));
    await reg.writeDynamicEntry(block, 1, 0,
        BlockMeta(flagsAndType: DataType.string.value, size: 5), 'hello'.codeUnits);
    await Future<void>.delayed(const Duration(milliseconds: 200));

    final device = await captureDevice(tamu.id, includeFiles: false);
    expect(device, isNotNull);
    // The whole register is captured: System, static blocks and dynamic blocks,
    // including read-only runtime entries.
    expect(device!.blocks.any((b) => b.type == 'System'), isTrue);
    expect(device.blocks.any((b) => b.type == 'PWM output'), isTrue);
    final system = device.blocks.firstWhere((b) => b.type == 'System');
    expect(system.entries.any((e) => e.readOnly), isTrue,
        reason: 'read-only entries are kept in the archive');

    final captured = device.blocks.firstWhere((b) => b.isDynamic && b.name == 'BKTEST');
    final numberEntry = captured.entries.firstWhere((e) => e.fieldIndex == 0);
    final stringEntry = captured.entries.firstWhere((e) => e.fieldIndex == 1);
    expect(numberEntry.type, 'Number');
    expect(numberEntry.value, closeTo(42.0, 1e-6));
    expect(stringEntry.value, 'hello');

    // Mutate, then restore from the semantic archive.
    await reg.writeDynamicEntry(block, 0, 0,
        BlockMeta(flagsAndType: DataType.number.value, size: 4), numberToBytes(99.0));
    await reg.writeDynamicEntry(block, 1, 0,
        BlockMeta(flagsAndType: DataType.string.value, size: 5), 'bye!!'.codeUnits);
    await Future<void>.delayed(const Duration(milliseconds: 200));

    final plan = await buildRestorePlan([device]);
    for (final item in plan.items) {
      item.selected = item.kind == RestoreKind.entry &&
          item.block!.isDynamic &&
          item.block!.name == 'BKTEST';
    }
    final result = await applyRestorePlan(plan);
    print('[BACKUP] restored ${result.written} item(s), ${result.failed} failed');
    expect(result.written, 2);
    expect(result.failed, 0);

    final number = await reg.readDynamicField(block, 0, 0);
    final textBack = await reg.readDynamicField(block, 1, 0);
    expect(numberFromBytes(number!.value), closeTo(42.0, 1e-6));
    expect(String.fromCharCodes(textBack!.value).trimRight(), 'hello');

    await reg.deleteDynamic(block: 0);
    await reg.saveDynamic();
  }, timeout: const Timeout(Duration(seconds: 90)));

  test('backup captures scripts semantically and restores them', skip: skipReason,
      () async {
    final storage = StorageClient(deviceId: tamu.id);
    await storage.deleteFile('SCR_09');
    final draft = ScriptDraft(
        functionName: 'BackupFn', properties: ScriptProperties.loadOnBoot);
    draft.inputs.add(ScriptDraftValue(
        name: 'In', type: DataType.number, value: numberToBytes(1.0)));
    draft.outputs.add(ScriptDraftValue(name: 'Out', type: DataType.number, size: 4));
    draft.lines.add(ScriptLine(
        destinations: [ScriptSymbol.output(0)],
        instruction: ScriptSymbol.instruction(catMath, 0), // Set
        operands: [ScriptSymbol.input(0)]));
    draft.lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));
    expect(await storage.writeFile('SCR_09', draft.toImage()), isTrue);

    final device = await captureDevice(tamu.id, includeFiles: true, maxFileBytes: 8192);
    expect(device, isNotNull);
    final script = device!.scripts.firstWhere((s) => s.slot == 9);
    expect(script.functionName, 'BackupFn');
    expect(script.inputs.length, 1);
    expect(script.lines.length, 2);
    // The raw file is still present in the storage section.
    expect(device.files.any((f) => f.name == 'SCR_09'), isTrue);

    await storage.deleteFile('SCR_09');
    final plan = await buildRestorePlan([device]);
    for (final item in plan.items) {
      item.selected = item.kind == RestoreKind.script && item.script!.slot == 9;
    }
    final result = await applyRestorePlan(plan);
    print('[BACKUP] restored ${result.written} script(s), ${result.failed} failed');
    expect(result.written, 1);

    final back = await storage.readFile('SCR_09');
    expect(back, isNotNull);
    expect(BackupScript.fromImage(9, back!).functionName, 'BackupFn');
    await storage.deleteFile('SCR_09');
  }, timeout: const Timeout(Duration(seconds: 90)));

  test('backup captures and restores a device file', skip: skipReason, () async {
    final storage = StorageClient(deviceId: tamu.id);
    await storage.deleteFile('BKTEST');
    expect(await storage.writeFile('BKTEST', [1, 2, 3, 4, 5]), isTrue);

    final device =
        await captureDevice(tamu.id, includeFiles: true, maxFileBytes: 4096);
    expect(device, isNotNull);
    final file = device!.files.firstWhere((f) => f.name == 'BKTEST');
    expect(file.bytes, [1, 2, 3, 4, 5]);
    expect(file.kind, 'Binary');

    await storage.deleteFile('BKTEST');
    await Future<void>.delayed(const Duration(milliseconds: 200));

    final plan = await buildRestorePlan([device]);
    for (final item in plan.items) {
      item.selected = item.kind == RestoreKind.file && item.file!.name == 'BKTEST';
    }
    final result = await applyRestorePlan(plan);
    print('[BACKUP] restored ${result.written} file(s), ${result.failed} failed');
    expect(result.written, 1);

    final back = await storage.readFile('BKTEST', size: 5);
    expect(back, [1, 2, 3, 4, 5]);
    await storage.deleteFile('BKTEST');
  }, timeout: const Timeout(Duration(seconds: 90)));
}
