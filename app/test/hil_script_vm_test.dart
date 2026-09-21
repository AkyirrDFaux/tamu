@Tags(['hil'])
library;
import 'dart:io';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/script_client.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/script_draft.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/types.dart';
import 'hil_helpers.dart';

/// Verifies the script VM (Docs/Services/Script.md): preloaded instructions, arithmetic,
/// flow (While/EndBlock), time (Delay -> Waiting), and the state machine.
void main() async {
  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;
  setUpAll(() async => await connectHil());
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
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(1)], instruction: ScriptSymbol.instruction(catMath, 1), operands: [ScriptSymbol.variable(0), ScriptSymbol.variable(0)]))
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

  test('while loop + end block', skip: skipReason, () async {
    // A=0; Cond=true; While Cond { Cond = A < 3; A = A + 1 } halt  -> A == 3
    final draft = ScriptDraft(functionName: 'Loop')
      ..variables.add(ScriptDraftValue(name: 'A', type: DataType.number, size: 4))
      ..variables.add(ScriptDraftValue(name: 'Cond', type: DataType.bool_, size: 1))
      ..constants.add(ScriptDraftValue(name: 'Zero', type: DataType.number, size: 4, value: numberToBytes(0)))
      ..constants.add(ScriptDraftValue(name: 'One', type: DataType.number, size: 4, value: numberToBytes(1)))
      ..constants.add(ScriptDraftValue(name: 'Three', type: DataType.number, size: 4, value: numberToBytes(3)))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(0)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [ScriptSymbol.constant(0)]))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(1)], instruction: ScriptSymbol.instruction(catMath, 0), operands: [ScriptSymbol.predefine(preBool, 1)]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 1), operands: [ScriptSymbol.variable(1)]))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(0)], instruction: ScriptSymbol.instruction(catMath, 1), operands: [ScriptSymbol.variable(0), ScriptSymbol.constant(1)]))
      ..lines.add(ScriptLine(destinations: [ScriptSymbol.variable(1)], instruction: ScriptSymbol.instruction(catLogic, 8), operands: [ScriptSymbol.variable(0), ScriptSymbol.constant(2)]))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 2)))
      ..lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)));

    final (c, st, slot) = await loadScript(1, draft);
    await c.setState(slot, ScriptState.running);
    final state = await waitState(c, slot, ScriptState.finished);
    print('[VM] loop state=$state');
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
}
