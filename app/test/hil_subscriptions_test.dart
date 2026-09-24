@Tags(['hil'])
library;

import 'dart:io';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/script_client.dart';
import 'package:tamuapp/core/script_draft.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/subscription_client.dart';
import 'package:tamuapp/core/types.dart';
import 'hil_helpers.dart';

Future<void> discoverDevices() async {
  final db = DeviceDatabase.instance;
  await db.refreshRuntime(0);
  await Future<void>.delayed(const Duration(seconds: 2));
  await db.refreshRuntime(0);
  await Future<void>.delayed(const Duration(seconds: 1));
}

void main() async {
  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  late DeviceEntry tamu;
  late DeviceEntry? das;

  setUpAll(() async {
    if (skipReason is String) return; // HIL not requested: leave the tests skipped
    await connectHil();
    await discoverDevices();
    tamu = findTamu(DeviceDatabase.instance) ??
        (throw StateError('Tamu not found'));
    das = findDas(DeviceDatabase.instance);
    // Deterministic start: clear the Tamu requester table (which cancels the DAS
    // providers) and any dynamic blocks used as subscription targets.
    final client = SubscriptionClient(deviceId: tamu.id);
    var subs = await client.getRequesterSubscriptions();
    while (subs.isNotEmpty) {
      await client.setRequesterSubscription(subs.first.index);
      subs = await client.getRequesterSubscriptions();
    }
    final reg = RegisterClient(deviceId: tamu.id);
    for (final b in await reg.readDynamicBlocks() ?? <DynBlock>[]) {
      await reg.deleteDynamic(block: b.index);
    }
    await reg.saveDynamic();
    await Future<void>.delayed(const Duration(milliseconds: 300));
  });
  tearDownAll(disconnectHil);

  /// Creates a dynamic block at [index] with a single entry (field 0, key 0) and returns
  /// it, so the subscription target is a writable register.
  Future<DynBlock> makeTarget(RegisterClient reg, int index, DataType type, int size, {List<int>? value}) async {
    await reg.deleteDynamic(block: index);
    await reg.createDynamicBlock(BlockType.dynamic, 'SUBTGT', index: index);
    final b = DynBlock(index: index, meta: BlockMeta(flagsAndType: BlockType.dynamic.value, size: 1), name: 'SUBTGT');
    await reg.writeDynamicEntry(b, 0, 0, BlockMeta(flagsAndType: type.value, key: 0),
        value ?? List<int>.filled(size, 0));
    return b;
  }

  test('subscriptions: new entry formats persist (28/32 B)', skip: skipReason, () async {
    final client = SubscriptionClient(deviceId: tamu.id);
    final entry = RequesterSubscription(
      index: 0,
      providerAddr: 1,
      trid: 0xFA00,
      targetReg: makeBlockInfo(0, 0, 3, 0),
      sourceReg: makeBlockInfo(0, 0, 3, 0),
      trigger: TriggerType.deltaPeriodic,
      periodMs: 250,
      minTimeMs: 50,
      deadzone: 0.5,
    );
    expect(await client.setRequesterSubscription(0, entry: entry), isTrue);
    final subs = await client.getRequesterSubscriptions();
    expect(subs, isNotEmpty);
    final got = subs.firstWhere((s) => s.index == 0);
    expect(got.trigger, TriggerType.deltaPeriodic);
    expect(got.deadzone, closeTo(0.5, 1e-3));
    await client.setRequesterSubscription(0);
  }, timeout: const Timeout(Duration(seconds: 20)));

  test('subscriptions: DAS Measured Value flows to a Tamu target', skip: skipReason, () async {
    if (das == null) {
      print('DAS not found; skipping');
      return;
    }
    final reg = RegisterClient(deviceId: tamu.id);
    final target = await makeTarget(reg, 0, DataType.number, 4);

    // DAS Meas1 (ResistiveMeasure inst 0) Measured Value = field 3.
    final sourceReg = makeBlockInfo(BlockType.resistiveMeasure.value, 0, 3, 0);
    final targetReg = makeBlockInfo(BlockType.dynamic.value, 0, 0, 0);

    // Read the provider value directly so we can compare after the subscription.
    final dasReg = RegisterClient(deviceId: das!.id);
    final direct = await dasReg.readBlockField(BlockType.resistiveMeasure.value, 0, 3, 0);
    final expected = direct == null ? 0.0 : numberFromBytes(direct.value);
    print('[SUB] DAS Measured Value = $expected');

    final client = SubscriptionClient(deviceId: tamu.id);
    final entry = RequesterSubscription(
      index: 0,
      providerAddr: das!.id,
      trid: 0xFA00,
      targetReg: targetReg,
      sourceReg: sourceReg,
      trigger: TriggerType.periodic,
      periodMs: 200,
      minTimeMs: 50,
    );
    expect(await client.setRequesterSubscription(0, entry: entry), isTrue);

    // The provider table on the DAS should now hold an entry with our deadzone field.
    final provClient = SubscriptionClient(deviceId: das!.id);
    await Future<void>.delayed(const Duration(milliseconds: 300));
    final provs = await provClient.getProviderSubscriptions();
    print('[SUB] DAS provider entries: ${provs.length}');
    expect(provs, isNotEmpty);

    await Future<void>.delayed(const Duration(milliseconds: 800));
    final applied = await reg.readDynamicField(target, 0, 0);
    final value = applied == null ? 0.0 : numberFromBytes(applied.value);
    print('[SUB] Tamu target value = $value');
    // The applied value should track the provider (non-zero for a connected sensor).
    expect(value, isNot(0.0));

    await client.setRequesterSubscription(0);
  }, timeout: const Timeout(Duration(seconds: 30)));

  test('subscriptions: delta self-loopback on Tamu AccGyr vector', skip: skipReason, () async {
    final reg = RegisterClient(deviceId: tamu.id);
    final target = await makeTarget(reg, 1, DataType.vector, 12);

    final sourceReg = makeBlockInfo(BlockType.accGyr.value, 0, 5, 0); // Acceleration (Vector3)
    final targetReg = makeBlockInfo(BlockType.dynamic.value, 1, 0, 0);

    final client = SubscriptionClient(deviceId: tamu.id);
    final entry = RequesterSubscription(
      index: 0,
      providerAddr: tamu.id, // self-loopback
      trid: 0xFA00,
      targetReg: targetReg,
      sourceReg: sourceReg,
      trigger: TriggerType.deltaPeriodic,
      periodMs: 500,
      minTimeMs: 50,
      deadzone: 0.01,
    );
    expect(await client.setRequesterSubscription(0, entry: entry), isTrue);
    await Future<void>.delayed(const Duration(milliseconds: 800));

    final applied = await reg.readDynamicField(target, 0, 0);
    final nonZero = applied != null && applied.value.any((b) => b != 0);
    print('[SUB] delta target bytes = ${applied?.value}');
    expect(nonZero, isTrue, reason: 'acceleration vector should have flowed to the target');
    await client.setRequesterSubscription(0);
  }, timeout: const Timeout(Duration(seconds: 20)));

  test('subscriptions: script I/O as a source', skip: skipReason, () async {
    final reg = RegisterClient(deviceId: tamu.id);
    final scriptClient = ScriptClient(deviceId: tamu.id);
    final storage = StorageClient(deviceId: tamu.id);

    // SCR_09: Out0 = 42; halt.
    if (await scriptClient.readState(9) != null) await scriptClient.unload(9);
    await storage.deleteFile('SCR_09');
    final draft = ScriptDraft(functionName: 'SubSrc')
      ..outputs.add(ScriptDraftValue(name: 'Out', type: DataType.number, size: 4))
      ..constants.add(ScriptDraftValue(name: 'K', type: DataType.number, size: 4, value: numberToBytes(42)))
      ..lines.add(ScriptLine(
          destinations: [ScriptSymbol.output(0)],
          instruction: ScriptSymbol.instruction(catMath, 0),
          operands: [ScriptSymbol.constant(0)]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));
    expect(await storage.writeFile('SCR_09', draft.toImage()), isTrue);
    expect(await scriptClient.load(9), 9);
    // Run once so the output holds 42.
    await scriptClient.setState(9, ScriptState.running);
    await Future<void>.delayed(const Duration(milliseconds: 300));

    final target = await makeTarget(reg, 2, DataType.number, 4);
    final sourceReg = makeBlockInfo(0x3FE, 9, 2, 0); // script output 0
    final targetReg = makeBlockInfo(BlockType.dynamic.value, 2, 0, 0);

    final client = SubscriptionClient(deviceId: tamu.id);
    final entry = RequesterSubscription(
      index: 0,
      providerAddr: tamu.id,
      trid: 0xFA00,
      targetReg: targetReg,
      sourceReg: sourceReg,
      trigger: TriggerType.periodic,
      periodMs: 200,
      minTimeMs: 50,
    );
    expect(await client.setRequesterSubscription(0, entry: entry), isTrue);
    await Future<void>.delayed(const Duration(milliseconds: 700));

    final applied = await reg.readDynamicField(target, 0, 0);
    print('[SUB] script-sourced target = ${applied == null ? null : numberFromBytes(applied.value)}');
    expect(applied, isNotNull);
    expect(numberFromBytes(applied!.value), closeTo(42.0, 0.01));

    await client.setRequesterSubscription(0);
    await scriptClient.unload(9);
    await storage.deleteFile('SCR_09');
  }, timeout: const Timeout(Duration(seconds: 30)));
}
