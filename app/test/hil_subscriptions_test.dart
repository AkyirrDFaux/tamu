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
    await reg.saveAll();
    await Future<void>.delayed(const Duration(milliseconds: 300));
  });
  tearDownAll(disconnectHil);

  /// Creates a dynamic block at [index] with a single entry (field 0, key 0) and returns
  /// it, so the subscription target is a writable register.
  Future<DynBlock> makeTarget(RegisterClient reg, int index, DataType type, int size, {List<int>? value}) async {
    await reg.deleteDynamic(block: index);
    await reg.createDynamicBlock('SUBTGT', index: index);
    final b = DynBlock(index: index, meta: ValueInfo(type: dynamicTypeForIndex(index), size: 1), name: 'SUBTGT');
    await reg.writeDynamicEntry(b, 0, 0, ValueInfo(type: type.value, key: 0),
        value ?? List<int>.filled(size, 0));
    return b;
  }

  /// The BlockInfo of a dynamic block addressed by its global index (the bank type is
  /// derived from the index, not the old single `BlockType.dynamic` marker).
  int dynReg(int global, int field, [int key = 0]) =>
      makeBlockInfo(dynamicTypeForIndex(global), dynamicInstanceForIndex(global), field, key);

  /// The BlockInfo of a loaded script addressed by its global slot.
  int scriptReg(int global, int field, [int key = 0]) =>
      makeBlockInfo(scriptTypeForIndex(global), scriptInstanceForIndex(global), field, key);

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
    final targetReg = dynReg(0, 0, 0);

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
    final targetReg = dynReg(1, 0, 0);

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

  test('subscriptions: delta scalar honours the deadzone', skip: skipReason, () async {
    final reg = RegisterClient(deviceId: tamu.id);
    final scripts = ScriptClient(deviceId: tamu.id);
    final storage = StorageClient(deviceId: tamu.id);

    // SCR_008: one Number input (a scalar we can drive) defaulting to 100.
    if (await scripts.readState(8) != null) await scripts.unload(8);
    await storage.deleteFile('SCR_008');
    final draft = ScriptDraft(functionName: 'SubScalar')
      ..inputs.add(ScriptDraftValue(name: 'Level', type: DataType.number, value: numberToBytes(100)))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6))); // Halt
    expect(await storage.writeFile('SCR_008', draft.toImage()), isTrue);
    expect(await scripts.load(8, 8), isTrue);

    // Sentinel -1 in the target so "arrived" is distinguishable from "unchanged".
    final target = await makeTarget(reg, 3, DataType.number, 4, value: numberToBytes(-1));
    final targetReg = dynReg(3, 0, 0);
    final sourceReg = scriptReg(8, 1, 0); // script input 0

    final client = SubscriptionClient(deviceId: tamu.id);
    // A long period so only the change branch can send during the test.
    final entry = RequesterSubscription(
      index: 0,
      providerAddr: tamu.id,
      trid: 0xFA00,
      targetReg: targetReg,
      sourceReg: sourceReg,
      trigger: TriggerType.deltaPeriodic,
      periodMs: 60000,
      minTimeMs: 200,
      deadzone: 5.0,
    );
    expect(await client.setRequesterSubscription(0, entry: entry), isTrue);

    Future<double> targetValue() async {
      final e = await reg.readDynamicField(target, 0, 0);
      return e == null ? double.nan : numberFromBytes(e.value);
    }

    // The first evaluation always sends (there is no previous value yet).
    var got = -1.0;
    for (var i = 0; i < 12 && got != 100.0; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 250));
      got = await targetValue();
    }
    // ignore: avoid_print
    print('[SUB] scalar delta initial target = $got');
    expect(got, closeTo(100.0, 0.01));

    final input = await scripts.readEntry(8, ScriptField.input, 0);
    expect(input, isNotNull);
    Future<void> setLevel(double v) async {
      expect(
          await scripts.writeEntry(8, ScriptField.input, 0,
              ValueInfo(type: input!.meta.type, flags: input.meta.flags, key: 0, size: 4), numberToBytes(v)),
          isTrue);
    }

    // A change below the deadzone must NOT be sent.
    await setLevel(101);
    await Future<void>.delayed(const Duration(milliseconds: 1500));
    got = await targetValue();
    // ignore: avoid_print
    print('[SUB] after +1 (deadzone 5) target = $got');
    expect(got, closeTo(100.0, 0.01), reason: 'a sub-deadzone change must not be sent');

    // A change at/above the deadzone must be sent.
    await setLevel(110);
    var big = 100.0;
    for (var i = 0; i < 12 && big != 110.0; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 250));
      big = await targetValue();
    }
    // ignore: avoid_print
    print('[SUB] after +10 target = $big');
    expect(big, closeTo(110.0, 0.01));

    await client.setRequesterSubscription(0);
    await scripts.unload(8);
    await storage.deleteFile('SCR_008');
  }, timeout: const Timeout(Duration(seconds: 30)));

  test('subscriptions: scalar delta provider keeps its last value', skip: skipReason, () async {
    final dasAddr = das!.id;
    final reg = RegisterClient(deviceId: tamu.id);
    final target = await makeTarget(reg, 4, DataType.number, 4, value: numberToBytes(-1));
    // A *writable* source on the DAS: its ResistiveMeasure Filter Coefficient. The LDR
    // (instance 1 Measured Value) was used before, but it drifts with the room light - and
    // because a delta push is fire-and-forget, one lost packet leaves the provider's
    // last-sent hash ahead of the value that reached the target. Driving the source makes the
    // check deterministic while still exercising the DAS's provider table.
    final sourceReg = makeBlockInfo(BlockType.resistiveMeasure.value, 0, 2, 0); // FilterCoeff
    final targetReg = dynReg(4, 0, 0);
    final dasReg = RegisterClient(deviceId: dasAddr);
    final before = await dasReg.readBlockField(BlockType.resistiveMeasure.value, 0, 2, 0);
    expect(before, isNotNull, reason: 'DAS FilterCoeff readable');
    final srcMeta =
        ValueInfo(type: before!.meta.type, flags: before.meta.flags, key: 0, size: before.value.length);

    final client = SubscriptionClient(deviceId: tamu.id);
    final entry = RequesterSubscription(
      index: 0,
      providerAddr: dasAddr,
      trid: 0xFC00,
      targetReg: targetReg,
      sourceReg: sourceReg,
      trigger: TriggerType.deltaPeriodic,
      periodMs: 60000, // long: only the change branch fires
      minTimeMs: 200,
      deadzone: 0.05,
    );
    expect(await client.setRequesterSubscription(0, entry: entry), isTrue);

    // The provider push is fire-and-forget; wait for the DAS to hold the entry before
    // checking its state.
    final provClient = SubscriptionClient(deviceId: dasAddr);
    bool registered(List<ProviderSubscription> provs) =>
        provs.any((p) => p.periodMs == 60000 && p.sourceReg == sourceReg);
    var ready = false;
    for (var attempt = 0; attempt < 10 && !ready; attempt++) {
      await Future<void>.delayed(const Duration(milliseconds: 400));
      ready = registered(await provClient.getProviderSubscriptions());
    }
    expect(ready, isTrue, reason: 'the DAS never received the provider entry');

    // Drive the source to a known value, retrying the nudge if a push went missing.
    var landed = false;
    var sent = 0.0;
    for (var attempt = 0; attempt < 6 && !landed; attempt++) {
      sent = 0.2 + 0.1 * attempt;
      await dasReg.writeBlockField(
          BlockType.resistiveMeasure.value, 0, 2, 0, srcMeta, numberToBytes(sent));
      for (var i = 0; i < 8 && !landed; i++) {
        await Future<void>.delayed(const Duration(milliseconds: 200));
        final t = await reg.readDynamicField(target, 0, 0);
        landed = t != null && (numberFromBytes(t.value) - sent).abs() < 0.005;
      }
    }
    expect(landed, isTrue, reason: 'the pushed value never reached the target');

    // The provider's Hash/Hashlike is the last SENT scalar, so it must equal the raw value that
    // reached the target - not an FNV hash written by a stray confirmation (which used to
    // overwrite it after every update, breaking the scalar deadzone).
    final mine = (await provClient.getProviderSubscriptions())
        .where((p) => p.periodMs == 60000 && p.sourceReg == sourceReg)
        .toList();
    final raw = (sent * 65536).round() & 0xFFFFFFFF;
    final hash = mine.isEmpty ? 0 : mine.first.hash;
    // ignore: avoid_print
    print('[SUB] scalar provider state: sent=$sent hash=0x${hash.toRadixString(16)} '
        'expected=0x${raw.toRadixString(16)}');
    expect(hash, raw, reason: 'provider Hash must hold the last sent scalar, not a hash');

    // Restore the DAS coefficient, then clean up.
    await dasReg.writeBlockField(
        BlockType.resistiveMeasure.value, 0, 2, 0, srcMeta, before.value);
    await client.setRequesterSubscription(0);
    await reg.deleteDynamic(block: 4);
  }, timeout: const Timeout(Duration(seconds: 30)));

  test('subscriptions: delta vector honours the deadzone', skip: skipReason, () async {
    final reg = RegisterClient(deviceId: tamu.id);
    final target = await makeTarget(reg, 5, DataType.vector, 12);
    final sourceReg = makeBlockInfo(BlockType.accGyr.value, 0, 5, 0); // Acceleration (Vector3)
    final targetReg = dynReg(5, 0, 0);
    final client = SubscriptionClient(deviceId: tamu.id);

    Future<void> subscribe(double deadzone) async {
      final entry = RequesterSubscription(
        index: 0,
        providerAddr: tamu.id, // self-loopback
        trid: 0xFA01,
        targetReg: targetReg,
        sourceReg: sourceReg,
        trigger: TriggerType.deltaPeriodic,
        periodMs: 60000, // long: only the change branch can fire
        minTimeMs: 200,
        deadzone: deadzone,
      );
      expect(await client.setRequesterSubscription(0, entry: entry), isTrue);
    }

    Future<List<int>> value() async =>
        (await reg.readDynamicField(target, 0, 0))?.value ?? const [];
    bool same(List<int> a, List<int> b) =>
        a.length == b.length && List.generate(a.length, (i) => a[i] == b[i]).every((x) => x);

    // A deadzone comfortably above the source's noise but below its ~10-unit initial state
    // (the reference starts at zero), so exactly one value arrives and then it stays quiet.
    // It is also > 1.0, which is what the old saturated Q16.16 comparison could not express:
    // back then any change over 1.0 was sent regardless of this number.
    await subscribe(5.0);
    var first = <int>[];
    for (var i = 0; i < 12 && !first.any((b) => b != 0); i++) {
      await Future<void>.delayed(const Duration(milliseconds: 250));
      first = await value();
    }
    // ignore: avoid_print
    print('[SUB] vector delta first = $first');
    expect(first.any((b) => b != 0), isTrue, reason: 'the first value should have arrived');
    var stable = true;
    for (var i = 0; i < 10; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 250));
      if (!same(await value(), first)) {
        stable = false;
        break;
      }
    }
    expect(stable, isTrue, reason: 'a sub-deadzone change must not be sent');

    // Tiny deadzone: the movement now gets through.
    await subscribe(0.001);
    var moved = false;
    for (var i = 0; i < 12 && !moved; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 250));
      moved = !same(await value(), first);
    }
    // ignore: avoid_print
    print('[SUB] vector delta moved=$moved');
    expect(moved, isTrue, reason: 'a change above the deadzone must be sent');

    await client.setRequesterSubscription(0);
    await reg.deleteDynamic(block: 5);
  }, timeout: const Timeout(Duration(seconds: 30)));

  test('subscriptions: script I/O as a source', skip: skipReason, () async {
    final reg = RegisterClient(deviceId: tamu.id);
    final scriptClient = ScriptClient(deviceId: tamu.id);
    final storage = StorageClient(deviceId: tamu.id);

    // SCR_009: Out0 = 42; halt.
    if (await scriptClient.readState(9) != null) await scriptClient.unload(9);
    await storage.deleteFile('SCR_009');
    final draft = ScriptDraft(functionName: 'SubSrc')
      ..outputs.add(ScriptDraftValue(name: 'Out', type: DataType.number, size: 4))
      ..constants.add(ScriptDraftValue(name: 'K', type: DataType.number, size: 4, value: numberToBytes(42)))
      ..lines.add(ScriptLine(
          destinations: [ScriptSymbol.output(0)],
          instruction: ScriptSymbol.instruction(catMath, 0),
          operands: [ScriptSymbol.constant(0)]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));
    expect(await storage.writeFile('SCR_009', draft.toImage()), isTrue);
    expect(await scriptClient.load(9, 9), isTrue);
    // Run once so the output holds 42.
    await scriptClient.setState(9, ScriptState.running);
    await Future<void>.delayed(const Duration(milliseconds: 300));

    final target = await makeTarget(reg, 2, DataType.number, 4);
    final targetReg = dynReg(2, 0, 0);
    final sourceReg = scriptReg(9, 2, 0); // script output 0

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
    await storage.deleteFile('SCR_009');
  }, timeout: const Timeout(Duration(seconds: 30)));

  test('subscriptions: a vector deadzone above 1.0 holds (Q16.16 cap fix)',
      skip: skipReason, () async {
    // The vector delta path used to square the change and the deadzone in full Q16.16, where
    // both overflow 32 bits - so both sides saturated at the same ceiling and every deadzone
    // behaved like 1.0: a change of 3 units was sent even with a deadzone of 100. The source
    // here is a *writable* dynamic vector, so the test is deterministic (the accelerometer
    // version depends on not touching the board).
    final reg = RegisterClient(deviceId: tamu.id);
    final source = await makeTarget(reg, 6, DataType.vector, 12);
    final target = await makeTarget(reg, 7, DataType.vector, 12);
    final client = SubscriptionClient(deviceId: tamu.id);

    Future<void> put(List<double> v) async {
      final bytes = <int>[];
      for (final x in v) {
        bytes.addAll(numberToBytes(x));
      }
      final f = await reg.readDynamicField(source, 0);
      expect(f, isNotNull);
      await reg.writeDynamicField(source, f!, bytes);
    }

    bool same(List<int> a, List<int> b) =>
        a.length == b.length && List.generate(a.length, (i) => a[i] == b[i]).every((x) => x);

    expect(
        await client.setRequesterSubscription(
            0,
            entry: RequesterSubscription(
              index: 0,
              providerAddr: tamu.id, // self-loopback: the core is the provider
              trid: 0xFB01,
              targetReg: dynReg(7, 0, 0),
              sourceReg: dynReg(6, 0, 0),
              trigger: TriggerType.deltaPeriodic,
              periodMs: 60000, // long: only the change branch can fire
              minTimeMs: 200,
              deadzone: 100.0,
            )),
        isTrue);

    Future<List<int>> tval() async =>
        (await reg.readDynamicField(target, 0, 0))?.value ?? const [];

    // A change well past the deadzone sends (the reference starts at zero).
    await put([200.0, 200.0, 200.0]);
    var first = <int>[];
    for (var i = 0; i < 12 && !first.any((b) => b != 0); i++) {
      await Future<void>.delayed(const Duration(milliseconds: 250));
      first = await tval();
    }
    expect(first.any((b) => b != 0), isTrue, reason: 'a change past the deadzone must arrive');

    // ~3 units per axis: far under the 100 deadzone, but over 1.0 - the old saturated
    // comparison sent this, which is the bug.
    await put([203.0, 203.0, 203.0]);
    var stable = true;
    for (var i = 0; i < 8; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 250));
      if (!same(await tval(), first)) {
        stable = false;
        break;
      }
    }
    expect(stable, isTrue, reason: 'a change under the 100 deadzone must not be sent');

    // And past it again still sends, so the gate is not simply stuck shut.
    await put([500.0, 500.0, 500.0]);
    var moved = false;
    for (var i = 0; i < 12 && !moved; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 250));
      moved = !same(await tval(), first);
    }
    expect(moved, isTrue, reason: 'a change past the deadzone must be sent');

    await client.setRequesterSubscription(0);
    await reg.deleteDynamic(block: 6);
    await reg.deleteDynamic(block: 7);
  }, timeout: const Timeout(Duration(seconds: 60)));
}
