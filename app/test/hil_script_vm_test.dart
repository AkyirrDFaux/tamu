@Tags(['hil'])
library;
import 'dart:io';
import 'dart:typed_data';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/script_client.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/script_draft.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/subscription_client.dart';
import 'package:tamuapp/core/types.dart';
import 'hil_helpers.dart';

/// Verifies the script VM (Docs/Services/Script.md): preloaded instructions, arithmetic,
/// flow (While/EndBlock), time (Delay -> Waiting), and the state machine.
void main() async {
  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;
  setUpAll(() async {
    if (skipReason is String) return;
    await connectHil();
    // The evaluation setup's subscriptions + dynamic blocks would overwrite the VM tests'
    // own dynamic memory (they share block indexes); clear them first.
    final subs = SubscriptionClient(deviceId: 1);
    var list = await subs.getRequesterSubscriptions();
    while (list.isNotEmpty) {
      await subs.setRequesterSubscription(list.first.index);
      list = await subs.getRequesterSubscriptions();
    }
    final reg = RegisterClient(deviceId: 1);
    for (final b in await reg.readDynamicBlocks() ?? <DynBlock>[]) {
      await reg.deleteDynamic(block: b.index);
    }
    await reg.saveDynamic();
  });
  tearDownAll(disconnectHil);

  /// Builds a draft and uploads + loads it into [slot]; returns the loaded id.
  Future<(ScriptClient, StorageClient, int)> loadScript(int slot, ScriptDraft draft) async {
    final c = ScriptClient(deviceId: 1);
    final st = StorageClient(deviceId: 1);
    if (await c.readState(slot) != null) await c.unload(slot);
    await st.deleteFile('SCR_${slot.toRadixString(16).toUpperCase().padLeft(2, '0')}');
    final name = 'SCR_${slot.toRadixString(16).toUpperCase().padLeft(2, '0')}';
    if (!await st.writeFile(name, draft.toImage())) fail('upload $name failed');
    final id = await c.load(slot);
    if (id != slot) fail('load $name failed: $id');
    return (c, st, slot);
  }

  Future<int> waitState(ScriptClient c, int slot, int want, {int tries = 40}) async {
    for (var i = 0; i < tries; i++) {
      final s = await c.readState(slot);
      if (s == want) return s!;
      await Future<void>.delayed(const Duration(milliseconds: 50));
    }
    return await c.readState(slot) ?? -1;
  }

  Future<void> cleanup(ScriptClient c, StorageClient st, int slot) async {
    await c.unload(slot);
    await st.deleteFile('SCR_${slot.toRadixString(16).toUpperCase().padLeft(2, '0')}');
  }

  test('arithmetic + halt', skip: skipReason, () async {
    final draft = ScriptDraft(functionName: 'Arith')
      ..outputs.add(ScriptDraftValue(name: 'Out', type: DataType.number, size: 4))
      ..variables.add(ScriptDraftValue(name: 'A', type: DataType.number, size: 4))
      ..variables.add(ScriptDraftValue(name: 'B', type: DataType.number, size: 4))
      ..constants.add(ScriptDraftValue(name: 'Two', type: DataType.number, size: 4, value: numberToBytes(2.0)))
      // A = 2; B = A + A; Out = B; halt
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(0)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [ScriptSymbol.constant(0)]))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(1)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [ScriptSymbol.variable(0), ScriptSymbol.predefine(preMathOp, 0), ScriptSymbol.variable(0)]))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.output(0)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [ScriptSymbol.variable(1)]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));

    final (c, st, slot) = await loadScript(0, draft);
    await c.setState(slot, ScriptState.running);
    final state = await waitState(c, slot, ScriptState.finished);
    print('[VM] arith state=$state');
    expect(state, ScriptState.finished);

    final out = await c.readEntry(slot, ScriptField.output, 0);
    expect(out, isNotNull);
    expect(numberFromBytes(out!.value), closeTo(4.0, 0.001));

    // Variables live in the script RAM (CID 5), not the Register.
    final ram = (await c.readInternalState(slot))!.variables;
    expect(numberFromBytes(ram, 0), closeTo(2.0, 0.001));
    expect(numberFromBytes(ram, 4), closeTo(4.0, 0.001));
    await cleanup(c, st, slot);
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('integer destinations round negatives to nearest (not down)', skip: skipReason, () async {
    // A = 0 - 5; B = 2 - 4; C = -3. Stored into Index (integer) variables, so the
    // expression result is rounded to an integer: the nearest value, not floored.
    final draft = ScriptDraft(functionName: 'RoundNeg')
      ..variables.add(ScriptDraftValue(name: 'A', type: DataType.integer, size: 4))
      ..variables.add(ScriptDraftValue(name: 'B', type: DataType.integer, size: 4))
      ..variables.add(ScriptDraftValue(name: 'C', type: DataType.integer, size: 4))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(0)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [
        ScriptSymbol.predefine(preIndex, 0), ScriptSymbol.predefine(preMathOp, 1), ScriptSymbol.predefine(preIndex, 5),
      ]))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(1)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [
        ScriptSymbol.predefine(preIndex, 2), ScriptSymbol.predefine(preMathOp, 1), ScriptSymbol.predefine(preIndex, 4),
      ]))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(2)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [
        ScriptSymbol.predefine(preMathOp, 1), ScriptSymbol.predefine(preIndex, 3), // unary minus
      ]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));

    final (c, st, slot) = await loadScript(8, draft);
    await c.setState(slot, ScriptState.running);
    expect(await waitState(c, slot, ScriptState.finished), ScriptState.finished);

    final ram = (await c.readInternalState(slot))!.variables;
    final a = int32FromBytes(ram, 0), b = int32FromBytes(ram, 4), cc = int32FromBytes(ram, 8);
    print('[VM] roundneg A=$a B=$b C=$cc');
    expect(a, -5);
    expect(b, -2);
    expect(cc, -3);
    await cleanup(c, st, slot);
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('Get time fills an integer destination and rejects a Number one', skip: skipReason, () async {
    // Index destination: accepted, a positive millisecond count.
    final ok = ScriptDraft(functionName: 'GetTime')
      ..variables.add(ScriptDraftValue(name: 'I0', type: DataType.integer, size: 4))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(0)], instruction: ScriptSymbol.instruction(catTime, 2)))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));
    final (c1, st1, slot1) = await loadScript(8, ok);
    await c1.setState(slot1, ScriptState.running);
    expect(await waitState(c1, slot1, ScriptState.finished), ScriptState.finished);
    final ms = int32FromBytes((await c1.readInternalState(slot1))!.variables, 0);
    print('[VM] gettime ms=$ms');
    expect(ms, greaterThan(0));
    await cleanup(c1, st1, slot1);

    // Number destination: rejected (its Q16.16 integer part overflows past ~32767 ms).
    final bad = ScriptDraft(functionName: 'GetTimeNum')
      ..variables.add(ScriptDraftValue(name: 'N0', type: DataType.number, size: 4))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(0)], instruction: ScriptSymbol.instruction(catTime, 2)))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));
    final (c2, st2, slot2) = await loadScript(8, bad);
    await c2.setState(slot2, ScriptState.running);
    final state = await waitState(c2, slot2, ScriptState.error);
    final err = await c2.readError(slot2);
    print('[VM] gettime-number state=$state err=$err');
    expect(state, ScriptState.error);
    expect(err, 2); // SCRIPT_ERR_TYPE
    await cleanup(c2, st2, slot2);
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('while loop + end block', skip: skipReason, () async {
    // A=0; While A < 3 { A = A + 1 } halt  -> A == 3 (While takes the comparison expression)
    final draft = ScriptDraft(functionName: 'Loop')
      ..variables.add(ScriptDraftValue(name: 'A', type: DataType.number, size: 4))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(0)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [ScriptSymbol.predefine(preIndex, 0)]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 1), operands: [
        ScriptSymbol.variable(0), ScriptSymbol.predefine(preMathOp, 14), ScriptSymbol.predefine(preIndex, 3),
      ]))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(0)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [
        ScriptSymbol.variable(0), ScriptSymbol.predefine(preMathOp, 0), ScriptSymbol.predefine(preIndex, 1),
      ]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 2)))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));

    final (c, st, slot) = await loadScript(1, draft);
    await c.setState(slot, ScriptState.running);
    final state = await waitState(c, slot, ScriptState.finished);
    print('[VM] loop state=$state err=${await c.readError(slot)}');
    expect(state, ScriptState.finished);
    final ram = (await c.readInternalState(slot))!.variables;
    expect(numberFromBytes(ram, 0), closeTo(3.0, 0.001));
    await cleanup(c, st, slot);
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('delay waits then finishes', skip: skipReason, () async {
    final draft = ScriptDraft(functionName: 'Delay')
      ..outputs.add(ScriptDraftValue(name: 'Out', type: DataType.number, size: 4))
      ..constants.add(ScriptDraftValue(name: 'Ms', type: DataType.number, size: 4, value: numberToBytes(150)))
      ..constants.add(ScriptDraftValue(name: 'Five', type: DataType.number, size: 4, value: numberToBytes(5)))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catTime, 0), operands: [ScriptSymbol.constant(0)]))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.output(0)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [ScriptSymbol.constant(1)]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));

    final (c, st, slot) = await loadScript(2, draft);
    await c.setState(slot, ScriptState.running);
    await Future<void>.delayed(const Duration(milliseconds: 40));
    final mid = await c.readState(slot);
    print('[VM] delay mid state=$mid');
    final state = await waitState(c, slot, ScriptState.finished);
    print('[VM] delay final state=$state');
    expect(state, ScriptState.finished);
    final out = await c.readEntry(slot, ScriptField.output, 0);
    expect(numberFromBytes(out!.value), closeTo(5.0, 0.001));
    await cleanup(c, st, slot);
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('local register read + write with ScriptUpdated', skip: skipReason, () async {
    // Prepare a dynamic block 0 with an entry (0,0) for the script to write into.
    final rc = RegisterClient(deviceId: 1);
    await rc.deleteDynamic(block: 0);
    await rc.createDynamicBlock(BlockType.dynamic, 'VM', index: 0);
    final dyn = DynBlock(index: 0, meta: BlockMeta(flagsAndType: BlockType.dynamic.value, size: 1), name: 'VM');
    await rc.writeDynamicEntry(dyn, 0, 0, BlockMeta(flagsAndType: DataType.number.value, key: 0), numberToBytes(1.0));

    final bi = uint32ToBytes(makeBlockInfo(0, 0, 0, 0)); // System Device Type
    final biDyn = uint32ToBytes(makeBlockInfo(0x3FF, 0, 0, 0)); // Dynamic block 0, field 0, key 0

    final draft = ScriptDraft(functionName: 'Regs')
      ..outputs.add(ScriptDraftValue(name: 'Out', type: DataType.number, size: 4))
      ..variables.add(ScriptDraftValue(name: 'V', type: DataType.integer, size: 4))
      ..constants.add(ScriptDraftValue(name: 'SysType', type: DataType.uint32, size: 4, value: bi))
      ..constants.add(ScriptDraftValue(name: 'DynBI', type: DataType.uint32, size: 4, value: biDyn))
      ..constants.add(ScriptDraftValue(name: 'Seven', type: DataType.number, size: 4, value: numberToBytes(7.5)))
      // V = read(SysType); Out = V; write(DynBI, 7.5); halt
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(0)], instruction: ScriptSymbol.instruction(catService, 1), operands: [ScriptSymbol.constant(0)]))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.output(0)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [ScriptSymbol.variable(0)]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catService, 2), operands: [ScriptSymbol.constant(1), ScriptSymbol.constant(2)]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));

    final (c, st, slot) = await loadScript(3, draft);
    await c.setState(slot, ScriptState.running);
    final state = await waitState(c, slot, ScriptState.finished);
    print('[VM] regs state=$state');
    expect(state, ScriptState.finished);

    final out = await c.readEntry(slot, ScriptField.output, 0);
    expect(numberFromBytes(out!.value), closeTo(1.0, 0.001)); // Tamu device type

    final written = await rc.readDynamicField(dyn, 0, 0);
    print('[VM] dyn flags=0x${written!.meta.flags.toRadixString(16)} v=${numberFromBytes(written.value)}');
    expect(numberFromBytes(written.value), closeTo(7.5, 0.001));
    expect(written.meta.flags & FieldFlags.scriptUpdated, FieldFlags.scriptUpdated);

    await cleanup(c, st, slot);
    await rc.deleteDynamic(block: 0);
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('compose + extract vector', skip: skipReason, () async {
    final draft = ScriptDraft(functionName: 'Comp')
      ..outputs.add(ScriptDraftValue(name: 'Out', type: DataType.number, size: 4))
      ..variables.add(ScriptDraftValue(name: 'Vec', type: DataType.vector, size: 8))
      ..constants.add(ScriptDraftValue(name: 'Z', type: DataType.number, size: 4, value: numberToBytes(0)))
      ..constants.add(ScriptDraftValue(name: 'One', type: DataType.number, size: 4, value: numberToBytes(1)))
      ..constants.add(ScriptDraftValue(name: 'A', type: DataType.number, size: 4, value: numberToBytes(1.5)))
      ..constants.add(ScriptDraftValue(name: 'B', type: DataType.number, size: 4, value: numberToBytes(2.5)))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(0)], instruction: ScriptSymbol.instruction(catCompose, 0), operands: [ScriptSymbol.constant(0), ScriptSymbol.constant(2)]))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(0)], instruction: ScriptSymbol.instruction(catCompose, 0), operands: [ScriptSymbol.constant(1), ScriptSymbol.constant(3)]))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.output(0)], instruction: ScriptSymbol.instruction(catCompose, 1), operands: [ScriptSymbol.variable(0), ScriptSymbol.constant(0)]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));

    final (c, st, slot) = await loadScript(4, draft);
    await c.setState(slot, ScriptState.running);
    final state = await waitState(c, slot, ScriptState.finished);
    print('[VM] compose state=$state');
    expect(state, ScriptState.finished);
    final out = await c.readEntry(slot, ScriptField.output, 0);
    expect(numberFromBytes(out!.value), closeTo(1.5, 0.001));
    final ram = (await c.readInternalState(slot))!.variables;
    expect(numberFromBytes(ram, 0), closeTo(1.5, 0.001));
    expect(numberFromBytes(ram, 4), closeTo(2.5, 0.001));
    await cleanup(c, st, slot);
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('type error halts with Error state + code', skip: skipReason, () async {
    final draft = ScriptDraft(functionName: 'Bad')
      ..outputs.add(ScriptDraftValue(name: 'Out', type: DataType.number, size: 4))
      ..variables.add(ScriptDraftValue(name: 'Vec', type: DataType.vector, size: 8))
      ..constants.add(ScriptDraftValue(name: 'One', type: DataType.number, size: 4, value: numberToBytes(1)))
      // Add(Out, Vec, 1): Vector operand is not numeric -> operand error.
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.output(0)], instruction: ScriptSymbol.instruction(catMath, 1), operands: [ScriptSymbol.variable(0), ScriptSymbol.constant(0)]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));

    final (c, st, slot) = await loadScript(5, draft);
    await c.setState(slot, ScriptState.running);
    final state = await waitState(c, slot, ScriptState.error);
    print('[VM] error state=$state');
    expect(state, ScriptState.error);
    final err = await c.readError(slot);
    print('[VM] error code=$err');
    expect(err, 3); // SCRIPT_ERR_OPERAND
    await cleanup(c, st, slot);
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('load-on-boot + run-on-load survive a reset', skip: skipReason, () async {
    final port = Platform.environment['TAMU_HIL']!;
    final draft = ScriptDraft(functionName: 'Boot', properties: ScriptProperties.loadOnBoot | ScriptProperties.runOnLoad)
      ..outputs.add(ScriptDraftValue(name: 'Out', type: DataType.number, size: 4))
      ..constants.add(ScriptDraftValue(name: 'Five', type: DataType.number, size: 4, value: numberToBytes(5)))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.output(0)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [ScriptSymbol.constant(0)]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));

    final (c, st, slot) = await loadScript(6, draft);
    await c.unload(slot); // not run yet: only the file must persist

    await ConnectionManager.instance.disconnect();
    await Process.run(
        '/home/akyirr/.platformio/penv/bin/python',
        ['/home/akyirr/.platformio/packages/tool-esptoolpy/esptool.py', '--port', port, 'run']);
    await Future<void>.delayed(const Duration(seconds: 5));
    final err = await connectHil();
    if (err != null) fail('reconnect failed: $err');

    final loaded = await c.loadedScripts();
    print('[VM] boot loaded=$loaded');
    expect(loaded.contains(slot), isTrue, reason: 'load-on-boot script was not loaded');
    final out = await c.readEntry(slot, ScriptField.output, 0);
    expect(numberFromBytes(out!.value), closeTo(5.0, 0.001));

    await cleanup(c, st, slot);
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('log + nop execute without error', skip: skipReason, () async {
    final draft = ScriptDraft(functionName: 'Log')
      ..constants.add(ScriptDraftValue(name: 'Code', type: DataType.number, size: 4, value: numberToBytes(7)))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catService, 0), operands: [ScriptSymbol.constant(0)])) // Log
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catService, 4))) // Nop
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6))); // Halt

    final (c, st, slot) = await loadScript(7, draft);
    await c.setState(slot, ScriptState.running);
    final state = await waitState(c, slot, ScriptState.finished);
    print('[VM] log state=$state err=${await c.readError(slot)}');
    expect(state, ScriptState.finished);
    expect(await c.readError(slot), 0);
    await cleanup(c, st, slot);
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('stored script without load-on-boot stays unloaded after a reset', skip: skipReason, () async {
    final port = Platform.environment['TAMU_HIL']!;
    final c = ScriptClient(deviceId: 1);
    final st = StorageClient(deviceId: 1);
    if (await c.readState(8) != null) await c.unload(8);
    await st.deleteFile('SCR_08');

    final draft = ScriptDraft(functionName: 'Stored') // properties 0 (no load-on-boot)
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catService, 4)))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));
    if (!await st.writeFile('SCR_08', draft.toImage())) fail('write failed');
    if ((await c.loadedScripts()).contains(8)) fail('should not be loaded before reset');

    await ConnectionManager.instance.disconnect();
    await Process.run(
        '/home/akyirr/.platformio/penv/bin/python',
        ['/home/akyirr/.platformio/packages/tool-esptoolpy/esptool.py', '--port', port, 'run']);
    await Future<void>.delayed(const Duration(seconds: 5));
    final err = await connectHil();
    if (err != null) fail('reconnect failed: $err');

    final loaded = await c.loadedScripts();
    print('[VM] stored-only boot loaded=$loaded');
    expect(loaded.contains(8), isFalse, reason: 'stored script must not load on boot');
    await st.deleteFile('SCR_08');
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('expression: precedence, parens, power and sqrt', skip: skipReason, () async {
    ScriptSymbol lit(int n) => ScriptSymbol.predefine(preIndex, n);
    ScriptSymbol op(int o) => ScriptSymbol.predefine(preMathOp, o);
    final draft = ScriptDraft(functionName: 'Expr')
      ..outputs.add(ScriptDraftValue(name: 'O0', type: DataType.number, size: 4))
      ..outputs.add(ScriptDraftValue(name: 'O1', type: DataType.number, size: 4))
      ..outputs.add(ScriptDraftValue(name: 'O2', type: DataType.number, size: 4))
      ..outputs.add(ScriptDraftValue(name: 'O3', type: DataType.number, size: 4))
      ..outputs.add(ScriptDraftValue(name: 'O4', type: DataType.number, size: 4))
      // O0 = 2 + 3 * 4 = 14
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.output(0)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [lit(2), op(0), lit(3), op(2), lit(4)]))
      // O1 = (2 + 3) * 4 = 20
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.output(1)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [op(mathOpOpenParen), lit(2), op(0), lit(3), op(mathOpCloseParen), op(2), lit(4)]))
      // O2 = 3 ^ 2 = 9
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.output(2)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [lit(3), op(5), lit(2)]))
      // O3 = 9 ^ 0.5 = 3
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.output(3)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [lit(9), op(5), ScriptSymbol.predefine(preNumber, 128)]))
      // O4 = -5 + 2 = -3
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.output(4)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [op(1), lit(5), op(0), lit(2)]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));
    final (c, st, slot) = await loadScript(9, draft);
    await c.setState(slot, ScriptState.running);
    final state = await waitState(c, slot, ScriptState.finished);
    print('[VM] expr state=$state err=${await c.readError(slot)}');
    expect(state, ScriptState.finished);
    Future<double> val(int i) async =>
        numberFromBytes((await c.readEntry(slot, ScriptField.output, i))!.value);
    expect(await val(0), closeTo(14.0, 0.01));
    expect(await val(1), closeTo(20.0, 0.01));
    expect(await val(2), closeTo(9.0, 0.01));
    expect(await val(3), closeTo(3.0, 0.01));
    expect(await val(4), closeTo(-3.0, 0.01));
    await cleanup(c, st, slot);
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('vector math: scale a Vector with one op', skip: skipReason, () async {
    final draft = ScriptDraft(functionName: 'VecMath')
      ..outputs.add(ScriptDraftValue(name: 'Mid', type: DataType.number, size: 4))
      ..variables.add(ScriptDraftValue(name: 'V', type: DataType.vector, size: 12))
      ..constants.add(ScriptDraftValue(
          name: 'V0',
          type: DataType.vector,
          size: 12,
          value: Uint8List.fromList([
            ...numberToBytes(1),
            ...numberToBytes(2),
            ...numberToBytes(3),
          ])))
      // V = V0; V = V * 2; Mid = V[1]; halt  -> V = [2,4,6], Mid = 4
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(0)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [ScriptSymbol.constant(0)]))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(0)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [ScriptSymbol.variable(0), ScriptSymbol.predefine(preMathOp, 2), ScriptSymbol.predefine(preIndex, 2)]))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.output(0)], instruction: ScriptSymbol.instruction(catCompose, 1), operands: [ScriptSymbol.variable(0), ScriptSymbol.predefine(preIndex, 1)]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));
    final (c, st, slot) = await loadScript(10, draft);
    await c.setState(slot, ScriptState.running);
    final state = await waitState(c, slot, ScriptState.finished);
    print('[VM] vec state=$state err=${await c.readError(slot)}');
    expect(state, ScriptState.finished);
    final out = await c.readEntry(slot, ScriptField.output, 0);
    expect(numberFromBytes(out!.value), closeTo(4.0, 0.001));
    final ram = (await c.readInternalState(slot))!.variables;
    expect(numberFromBytes(ram, 0), closeTo(2.0, 0.001));
    expect(numberFromBytes(ram, 4), closeTo(4.0, 0.001));
    expect(numberFromBytes(ram, 8), closeTo(6.0, 0.001));
    await cleanup(c, st, slot);
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('expression: Matrix copy preserves the header', skip: skipReason, () async {
    // M = IDENT (2x3); M[0,2] = 5; Out = M[0,2]
    final draft = ScriptDraft(functionName: 'MatExpr')
      ..outputs.add(ScriptDraftValue(name: 'Out', type: DataType.number, size: 4))
      ..variables.add(ScriptDraftValue(name: 'M', type: DataType.matrix, size: 28))
      ..constants.add(ScriptDraftValue(
          name: 'IDENT',
          type: DataType.matrix,
          size: 28,
          value: Uint8List.fromList([
            2, 0, 3, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0,
          ])))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(0)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [ScriptSymbol.constant(0)]))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(0)], instruction: ScriptSymbol.instruction(catCompose, 0), operands: [ScriptSymbol.predefine(preIndex, 2), ScriptSymbol.predefine(preIndex, 5)]))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.output(0)], instruction: ScriptSymbol.instruction(catCompose, 1), operands: [ScriptSymbol.variable(0), ScriptSymbol.predefine(preIndex, 2)]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));
    final (c, st, slot) = await loadScript(13, draft);
    await c.setState(slot, ScriptState.running);
    final state = await waitState(c, slot, ScriptState.finished);
    print('[VM] matexpr state=$state err=${await c.readError(slot)}');
    expect(state, ScriptState.finished);
    expect(numberFromBytes((await c.readEntry(slot, ScriptField.output, 0))!.value),
        closeTo(5.0, 0.01));
    // The matrix header survived the copy.
    final ram = (await c.readInternalState(slot))!.variables;
    expect(ram[0], 2);
    expect(ram[1], 0);
    expect(ram[2], 3);
    await cleanup(c, st, slot);
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('expression: logic, comparisons and modulo', skip: skipReason, () async {
    ScriptSymbol lit(int n) => ScriptSymbol.predefine(preIndex, n);
    ScriptSymbol op(int o) => ScriptSymbol.predefine(preMathOp, o);
    ScriptLine setOut(int i, List<ScriptSymbol> expr) => ScriptLine(
        destinations: [ScriptSymbol.output(i)],
        instruction: ScriptSymbol.instruction(catMath, 0),
        operands: expr);
    final draft = ScriptDraft(functionName: 'Logic')
      ..outputs.add(ScriptDraftValue(name: 'O0', type: DataType.number, size: 4))
      ..outputs.add(ScriptDraftValue(name: 'O1', type: DataType.number, size: 4))
      ..outputs.add(ScriptDraftValue(name: 'O2', type: DataType.number, size: 4))
      ..outputs.add(ScriptDraftValue(name: 'O3', type: DataType.number, size: 4))
      // O0 = (3 > 2) AND (1 < 2) = 1
      ..lines.add(setOut(0, [
        op(mathOpOpenParen), lit(3), op(16), lit(2), op(mathOpCloseParen),
        op(6),
        op(mathOpOpenParen), lit(1), op(14), lit(2), op(mathOpCloseParen),
      ]))
      // O1 = 5 % 3 = 2
      ..lines.add(setOut(1, [lit(5), op(4), lit(3)]))
      // O2 = NOT (2 == 3) = 1
      ..lines.add(setOut(2, [
        op(9), op(mathOpOpenParen), lit(2), op(12), lit(3), op(mathOpCloseParen),
      ]))
      // O3 = 3 XOR 1 = 0 (both truthy)
      ..lines.add(setOut(3, [lit(3), op(8), lit(1)]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));
    final (c, st, slot) = await loadScript(14, draft);
    await c.setState(slot, ScriptState.running);
    final state = await waitState(c, slot, ScriptState.finished);
    print('[VM] logic state=$state err=${await c.readError(slot)}');
    expect(state, ScriptState.finished);
    Future<double> val(int i) async =>
        numberFromBytes((await c.readEntry(slot, ScriptField.output, i))!.value);
    expect(await val(0), closeTo(1.0, 0.01));
    expect(await val(1), closeTo(2.0, 0.01));
    expect(await val(2), closeTo(1.0, 0.01));
    expect(await val(3), closeTo(0.0, 0.01));
    await cleanup(c, st, slot);
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('flow: While takes a boolean expression', skip: skipReason, () async {
    // i = 0; While i < 5 { i = i + 1 }; Out = i; halt  -> i == 5
    final draft = ScriptDraft(functionName: 'FlowExpr')
      ..outputs.add(ScriptDraftValue(name: 'Out', type: DataType.number, size: 4))
      ..variables.add(ScriptDraftValue(name: 'i', type: DataType.number, size: 4))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(0)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [ScriptSymbol.predefine(preIndex, 0)]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 1), operands: [
        ScriptSymbol.variable(0), ScriptSymbol.predefine(preMathOp, 14), ScriptSymbol.predefine(preIndex, 5),
      ]))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(0)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [
        ScriptSymbol.variable(0), ScriptSymbol.predefine(preMathOp, 0), ScriptSymbol.predefine(preIndex, 1),
      ]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 2)))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.output(0)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [ScriptSymbol.variable(0)]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));
    final (c, st, slot) = await loadScript(15, draft);
    await c.setState(slot, ScriptState.running);
    final state = await waitState(c, slot, ScriptState.finished);
    print('[VM] flowexpr state=$state err=${await c.readError(slot)}');
    expect(state, ScriptState.finished);
    expect(numberFromBytes((await c.readEntry(slot, ScriptField.output, 0))!.value),
        closeTo(5.0, 0.01));
    await cleanup(c, st, slot);
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('expression: dot, cross, size', skip: skipReason, () async {
    ScriptSymbol fn(int o) => ScriptSymbol.predefine(preMathOp, o);
    Uint8List vec(List<double> v) =>
        Uint8List.fromList([for (final x in v) ...numberToBytes(x)]);
    final draft = ScriptDraft(functionName: 'VecFn')
      ..outputs.add(ScriptDraftValue(name: 'Dot', type: DataType.number, size: 4))
      ..outputs.add(ScriptDraftValue(name: 'Size', type: DataType.number, size: 4))
      ..outputs.add(ScriptDraftValue(name: 'CrossZ', type: DataType.number, size: 4))
      ..variables.add(ScriptDraftValue(name: 'V', type: DataType.vector, size: 12))
      ..variables.add(ScriptDraftValue(name: 'W', type: DataType.vector, size: 12))
      ..variables.add(ScriptDraftValue(name: 'C', type: DataType.vector, size: 12))
      ..constants.add(ScriptDraftValue(name: 'V0', type: DataType.vector, size: 12, value: vec([3, 4, 0])))
      ..constants.add(ScriptDraftValue(name: 'W0', type: DataType.vector, size: 12, value: vec([1, 0, 0])))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(0)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [ScriptSymbol.constant(0)]))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(1)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [ScriptSymbol.constant(1)]))
      // Dot = dot V V = 25
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.output(0)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [fn(20), ScriptSymbol.variable(0), ScriptSymbol.variable(0)]))
      // Size = size V = 5
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.output(1)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [fn(22), ScriptSymbol.variable(0)]))
      // C = cross V W = (0, 0, -4)
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(2)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [fn(21), ScriptSymbol.variable(0), ScriptSymbol.variable(1)]))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.output(2)], instruction: ScriptSymbol.instruction(catCompose, 1), operands: [ScriptSymbol.variable(2), ScriptSymbol.predefine(preIndex, 2)]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));
    final (c, st, slot) = await loadScript(16, draft);
    await c.setState(slot, ScriptState.running);
    final state = await waitState(c, slot, ScriptState.finished);
    print('[VM] vecfn state=$state err=${await c.readError(slot)}');
    expect(state, ScriptState.finished);
    Future<double> val(int i) async =>
        numberFromBytes((await c.readEntry(slot, ScriptField.output, i))!.value);
    expect(await val(0), closeTo(25.0, 0.01));
    expect(await val(1), closeTo(5.0, 0.01));
    expect(await val(2), closeTo(-4.0, 0.01));
    await cleanup(c, st, slot);
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('expression: transpose + Transform', skip: skipReason, () async {
    ScriptSymbol fn(int o) => ScriptSymbol.predefine(preMathOp, o);
    ScriptSymbol lit(int n) => ScriptSymbol.predefine(preIndex, n);
    final draft = ScriptDraft(functionName: 'Xform')
      ..outputs.add(ScriptDraftValue(name: 'T01', type: DataType.number, size: 4))
      ..outputs.add(ScriptDraftValue(name: 'TX', type: DataType.number, size: 4))
      ..outputs.add(ScriptDraftValue(name: 'TY', type: DataType.number, size: 4))
      ..variables.add(ScriptDraftValue(name: 'M', type: DataType.matrix, size: 28))
      ..variables.add(ScriptDraftValue(name: 'T', type: DataType.matrix, size: 28))
      ..variables.add(ScriptDraftValue(name: 'X', type: DataType.matrix, size: 28))
      ..constants.add(ScriptDraftValue(name: 'M0', type: DataType.matrix, size: 28, value: Uint8List.fromList([
        2, 0, 3, 0,
        ...numberToBytes(1), ...numberToBytes(2), ...numberToBytes(3),
        ...numberToBytes(4), ...numberToBytes(5), ...numberToBytes(6),
      ])))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(0)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [ScriptSymbol.constant(0)])) // M = M0
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(1)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [fn(23), ScriptSymbol.variable(0)])) // T = transpose M (3x2)
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.output(0)], instruction: ScriptSymbol.instruction(catCompose, 1), operands: [ScriptSymbol.variable(1), lit(1)])) // T[0,1] = M[1,0] = 4
      // X = Transform(rot=0, ox=3, oy=4, sx=1, sy=1)
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(2)], instruction: ScriptSymbol.instruction(catMath, 11), operands: [lit(0), lit(3), lit(4), lit(1), lit(1)]))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.output(1)], instruction: ScriptSymbol.instruction(catCompose, 1), operands: [ScriptSymbol.variable(2), lit(2)])) // X[0,2] = ox
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.output(2)], instruction: ScriptSymbol.instruction(catCompose, 1), operands: [ScriptSymbol.variable(2), lit(5)])) // X[1,2] = oy
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));
    final (c, st, slot) = await loadScript(17, draft);
    await c.setState(slot, ScriptState.running);
    final state = await waitState(c, slot, ScriptState.finished);
    print('[VM] xform state=$state err=${await c.readError(slot)}');
    expect(state, ScriptState.finished);
    Future<double> val(int i) async =>
        numberFromBytes((await c.readEntry(slot, ScriptField.output, i))!.value);
    expect(await val(0), closeTo(4.0, 0.01)); // transposed element
    expect(await val(1), closeTo(3.0, 0.01)); // ox
    expect(await val(2), closeTo(4.0, 0.01)); // oy
    // The transposed matrix header is 3x2.
    final ram = (await c.readInternalState(slot))!.variables;
    // M (28) then T (28): T starts at offset 28.
    expect(ram[28], 3);
    expect(ram[30], 2);
    await cleanup(c, st, slot);
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('BlockInfo operand: write a register target', skip: skipReason, () async {
    final reg = RegisterClient(deviceId: 1);
    const block = 5;
    await reg.deleteDynamic(block: block);
    await reg.createDynamicBlock(BlockType.dynamic, 'VMTGT', index: block);
    final b = DynBlock(
        index: block,
        meta: BlockMeta(flagsAndType: BlockType.dynamic.value, size: 1),
        name: 'VMTGT');
    await reg.writeDynamicEntry(
        b, 0, 0, BlockMeta(flagsAndType: DataType.number.value, key: 0), List<int>.filled(4, 0));

    final draft = ScriptDraft(functionName: 'RegWrite')
      ..constants.add(ScriptDraftValue(
          name: 'TARGET',
          type: DataType.blockInfo,
          value: Uint8List.fromList(
              uint32ToBytes(makeBlockInfo(BlockType.dynamic.value, block, 0, 0)))))
      // write[TARGET] = 7; halt
      ..lines.add(ScriptLine(
          instruction: ScriptSymbol.instruction(catService, 2),
          operands: [ScriptSymbol.constant(0), ScriptSymbol.predefine(preIndex, 7)]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));
    final (c, st, slot) = await loadScript(11, draft);
    await c.setState(slot, ScriptState.running);
    final state = await waitState(c, slot, ScriptState.finished);
    print('[VM] blockinfo state=$state err=${await c.readError(slot)}');
    expect(state, ScriptState.finished);
    final applied = await reg.readDynamicField(b, 0, 0);
    expect(numberFromBytes(applied!.value), closeTo(7.0, 0.001));
    await cleanup(c, st, slot);
    await reg.deleteDynamic(block: block);
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('Limit clamps a value to [min, max]', skip: skipReason, () async {
    // Out = Limit(50, 0, 10) = 10; and Low = Limit(-5, 0, 10) = 0
    final draft = ScriptDraft(functionName: 'Limit')
      ..outputs.add(ScriptDraftValue(name: 'Out', type: DataType.number, size: 4))
      ..outputs.add(ScriptDraftValue(name: 'Low', type: DataType.number, size: 4))
      ..lines.add(ScriptLine(
          destinations: [ScriptSymbol.output(0)],
          instruction: ScriptSymbol.instruction(catMath, 10), // Limit
          operands: [
            ScriptSymbol.predefine(preIndex, 50),
            ScriptSymbol.predefine(preIndex, 0),
            ScriptSymbol.predefine(preIndex, 10),
          ]))
      ..constants.add(ScriptDraftValue(
          name: 'Neg', type: DataType.number, size: 4, value: numberToBytes(-5)))
      ..lines.add(ScriptLine(
          destinations: [ScriptSymbol.output(1)],
          instruction: ScriptSymbol.instruction(catMath, 10),
          operands: [
            ScriptSymbol.constant(0), // -5
            ScriptSymbol.predefine(preIndex, 0),
            ScriptSymbol.predefine(preIndex, 10),
          ]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));
    final (c, st, slot) = await loadScript(12, draft);
    await c.setState(slot, ScriptState.running);
    final state = await waitState(c, slot, ScriptState.finished);
    print('[VM] limit state=$state err=${await c.readError(slot)}');
    expect(state, ScriptState.finished);
    expect(numberFromBytes((await c.readEntry(slot, ScriptField.output, 0))!.value),
        closeTo(10.0, 0.001));
    expect(numberFromBytes((await c.readEntry(slot, ScriptField.output, 1))!.value),
        closeTo(0.0, 0.001));
    await cleanup(c, st, slot);
  }, timeout: const Timeout(Duration(minutes: 2)));
}
