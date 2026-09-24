@Tags(['hil'])
library;
import 'dart:io';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/script_client.dart';
import 'package:tamuapp/core/script_draft.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/types.dart';
import 'hil_helpers.dart';

/// Verifies the script walking skeleton (Docs/Services/Script.md): a SCR_XX file can be
/// uploaded, loaded, listed, state-controlled, and its IO/variables/constants read and
/// written through the Register service (block type 0x3FE).
void main() async {
  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;
  setUpAll(() async {
    if (skipReason is String) return;
    await connectHil();
  });
  tearDownAll(disconnectHil);

  test('script load / register exposure / unload', skip: skipReason, () async {
    final c = ScriptClient(deviceId: 1);
    final st = StorageClient(deviceId: 1);

    // Clean slate: unload everything and remove our test file.
    for (final slot in await c.loadedScripts()) {
      await c.unload(slot);
    }
    await st.deleteFile('SCR_00');

    final image = ScriptFileBuilder(
      properties: ScriptProperties.loadOnBoot,
      inputs: const [ScriptValueInfo(type: DataType.number, size: 4)],
      outputs: const [ScriptValueInfo(type: DataType.number, size: 4)],
      variables: const [
        ScriptValueInfo(type: DataType.number, size: 4),
        ScriptValueInfo(type: DataType.number, size: 4),
      ],
      constants: const [ScriptValueInfo(type: DataType.number, size: 4)],
      inputDefaults: [numberToBytes(3.5)],
      constantValues: [numberToBytes(9.0)],
      instructions: const [
        // While (true) { }  -> stays Running (yields each tick via the loop guard).
        0x00, 0x02, 0x01, 0x00, // instruction: Flow/While
        0x06, 0x05, 0x01, 0x00, // operand: predefine Bool true
        0x05, 0x00, 0x00, 0x00, // Endline
        0x00, 0x02, 0x02, 0x00, // instruction: Flow/End block
        0x05, 0x00, 0x00, 0x00, // Endline
      ],
      functionName: 'Blink',
    ).build();

    if (!await st.writeFile('SCR_00', image)) fail('upload SCR_00 failed');

    // Load (CID 1).
    final loadedId = await c.load(0);
    print('[S] loadedId=$loadedId');
    if (loadedId != 0) fail('load returned $loadedId');

    // Listed by both the management command and Register enumerate.
    if (!(await c.loadedScripts()).contains(0)) fail('CID 0 list missing script 0');
    if (!(await c.enumerateInstances()).contains(0)) fail('Register enumerate missing script 0');

    // Block meta (name from the file's UI info + field count) - used by the list page.
    final blockMeta = await c.readBlockMeta(0);
    if (blockMeta == null ||
        blockMeta.name != 'Blink' ||
        blockMeta.meta.size != ScriptField.count) {
      fail('block meta wrong: name=${blockMeta?.name} size=${blockMeta?.meta.size}');
    }

    // Script block must be visible through the Register service (client path the
    // Register page uses): readBlocks() lists it, and its category fields/keys read.
    final rc = RegisterClient(deviceId: 1);
    final blocks = await rc.readBlocks();
    if (blocks == null ||
        !blocks.any((b) => b?.type == BlockType.script.value && b?.inst == 0)) {
      fail('script block not listed by readBlocks()');
    }
    final inKeys = await rc.getBlockKeys(BlockType.script.value, 0, ScriptField.input);
    if (inKeys == null || inKeys.length != 1) fail('script input keys wrong: $inKeys');
    final inEntry = await rc.readBlockField(BlockType.script.value, 0, ScriptField.input, 0);
    if (inEntry == null || inEntry.meta.dataType != DataType.number) {
      fail('script input read via register failed');
    }
    final outEntry = await rc.readBlockField(BlockType.script.value, 0, ScriptField.output, 0);
    if (outEntry == null || !outEntry.meta.readOnly) fail('script output should be RO');

    // State: fresh load is Stopped; set/read round-trips.
    if (await c.readState(0) != ScriptState.stopped) fail('initial state not Stopped');
    if (!await c.setState(0, ScriptState.running)) fail('setState failed');
    if (await c.readState(0) != ScriptState.running) fail('state not Running');

    // File metadata (properties/counts/constants) lives in the SCR_XX file; the Register
    // only exposes the script's I/O.
    final parsed = ScriptFileData.parse((await st.readFile('SCR_00'))!);
    if (parsed.properties != ScriptProperties.loadOnBoot) fail('properties wrong');
    if (parsed.inputs.length != 1) fail('input count wrong');
    if (parsed.variables.length != 2) fail('variable count wrong');
    if ((numberFromBytes(parsed.constantValue(0)) - 9.0).abs() > 0.001) {
      fail('constant value wrong');
    }

    // The Register lists only I/O: input keys exist, variable/constant keys do not.
    final regInKeys = await c.enumerateKeys(0, ScriptField.input);
    if (regInKeys.length != 1) fail('input keys wrong: $regInKeys');
    if ((await c.enumerateKeys(0, ScriptField.variable)).isNotEmpty) {
      fail('variables must not be part of the register');
    }
    if ((await c.enumerateKeys(0, ScriptField.constant)).isNotEmpty) {
      fail('constants must not be part of the register');
    }

    // Input default from the file; output is read-only.
    final input = await c.readEntry(0, ScriptField.input, 0);
    if (input == null || (numberFromBytes(input.value) - 3.5).abs() > 0.001) {
      fail('input default wrong: ${input?.value}');
    }
    final output = await c.readEntry(0, ScriptField.output, 0);
    if (output == null || !output.meta.readOnly) fail('output should be read-only');

    // Variables live in the script RAM: CID 7 write + CID 5 read (sliced by size).
    final internal1 = await c.readInternalState(0);
    if (internal1 == null || internal1.variables.length < 8) fail('variable RAM wrong');
    if (numberFromBytes(internal1.variables, 0) != 0) fail('variable 0 not zeroed');
    if (!await c.writeVariable(0, 0, numberToBytes(1.25))) fail('CID 7 write 0 failed');
    if (!await c.writeVariable(0, 1, numberToBytes(2.5))) fail('CID 7 write 1 failed');
    final internal2 = await c.readInternalState(0);
    if ((numberFromBytes(internal2!.variables, 0) - 1.25).abs() > 0.001) {
      fail('variable 0 readback wrong');
    }
    if ((numberFromBytes(internal2.variables, 4) - 2.5).abs() > 0.001) {
      fail('variable 1 readback wrong');
    }

    // Editor debug: stop, move to a line and read the internal state (IC is a line index).
    await c.setState(0, ScriptState.stopped);
    if (!await c.moveToInstruction(0, 1)) fail('moveToInstruction failed');
    final internal = await c.readInternalState(0);
    if (internal == null || internal.instructionCounter != 1) {
      fail('instruction counter wrong: ${internal?.instructionCounter}');
    }

    // Inputs are writable through Register (the app drives script inputs).
    if (!await c.writeEntry(0, ScriptField.input, 0, input.meta, numberToBytes(7.5))) {
      fail('input write failed');
    }
    final input2 = await c.readEntry(0, ScriptField.input, 0);
    if (input2 == null || (numberFromBytes(input2.value) - 7.5).abs() > 0.001) {
      fail('input readback wrong: ${input2?.value}');
    }

    // Unload clears it from both tables.
    if (!await c.unload(0)) fail('unload failed');
    if ((await c.loadedScripts()).isNotEmpty) fail('script still loaded after unload');
    if ((await c.enumerateInstances()).isNotEmpty) fail('Register enumerate not empty after unload');

    await st.deleteFile('SCR_00');
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('created script is stored but not loaded', skip: skipReason, () async {
    final c = ScriptClient(deviceId: 1);
    final st = StorageClient(deviceId: 1);

    // Make sure slot 0x3F is clear, then write a brand-new minimal script.
    if (await c.readState(0x3F) != null) await c.unload(0x3F);
    await st.deleteFile('SCR_3F');

    final image = ScriptFileBuilder(functionName: 'Empty').build();
    if (!await st.writeFile('SCR_3F', image)) fail('create SCR_3F failed');

    // Writing the file must NOT load it.
    if ((await c.loadedScripts()).contains(0x3F)) {
      fail('new script was loaded automatically');
    }

    final id = await c.load(0x3F);
    if (id != 0x3F) fail('load SCR_3F failed: $id');
    final meta = await c.readBlockMeta(0x3F);
    if (meta == null || meta.name != 'Empty') fail('name wrong: ${meta?.name}');
    if (meta.meta.size != ScriptField.count) fail('field count wrong');

    if (!await c.unload(0x3F)) fail('unload failed');
    await st.deleteFile('SCR_3F');
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('edit a stored (not loaded) script', skip: skipReason, () async {
    final c = ScriptClient(deviceId: 1);
    final st = StorageClient(deviceId: 1);
    if (await c.readState(2) != null) await c.unload(2);
    await st.deleteFile('SCR_02');

    // Build an edited draft through the same model the editor uses.
    final draft = ScriptDraft(functionName: 'Edited', properties: ScriptProperties.loadOnBoot)
      ..inputs.add(ScriptDraftValue(
          name: 'Speed', type: DataType.number, size: 4, value: numberToBytes(1.5)))
      ..outputs.add(ScriptDraftValue(name: 'Result', type: DataType.number, size: 4))
      ..variables.add(ScriptDraftValue(name: 'Tmp', type: DataType.number, size: 4))
      ..constants.add(ScriptDraftValue(
          name: 'Gain', type: DataType.number, size: 4, value: numberToBytes(2.0)))
      ..lines.add(ScriptLine(
          destinations: [ScriptSymbol.variable(0)],
          instruction: ScriptSymbol.instruction(catMath, 0),
          operands: [ScriptSymbol.constant(0)]))
      ..lines.add(ScriptLine(
          destinations: [ScriptSymbol.output(0)],
          instruction: ScriptSymbol.instruction(catMath, 0),
          operands: [ScriptSymbol.input(0)]));

    if (!await st.writeFile('SCR_02', draft.toImage())) fail('edit upload failed');
    if ((await c.loadedScripts()).contains(2)) fail('edited stored script was auto-loaded');

    final id = await c.load(2);
    if (id != 2) fail('load edited script failed');
    final meta = await c.readBlockMeta(2);
    if (meta == null || meta.name != 'Edited') fail('edited name not restored: ${meta?.name}');
    final parsed2 = ScriptFileData.parse((await st.readFile('SCR_02'))!);
    if (parsed2.variables.length != 1) fail('variable count wrong');
    final internal = await c.readInternalState(2);
    if (internal == null || internal.variables.length != 4) {
      fail('edited variable space wrong: ${internal?.variables.length}');
    }
    final input = await c.readEntry(2, ScriptField.input, 0);
    if (input == null || (numberFromBytes(input.value) - 1.5).abs() > 0.001) {
      fail('edited input default wrong');
    }

    if (!await c.unload(2)) fail('unload failed');
    await st.deleteFile('SCR_02');
  }, timeout: const Timeout(Duration(minutes: 2)));

  test('a short write fills the rest of a script input', skip: skipReason, () async {
    final c = ScriptClient(deviceId: 1);
    final st = StorageClient(deviceId: 1);
    if (await c.readState(1) != null) await c.unload(1);
    await st.deleteFile('SCR_01');

    // One 16-byte String input.
    final draft = ScriptDraft(functionName: 'Pad')
      ..inputs.add(ScriptDraftValue(name: 'Name', type: DataType.string, size: 16));
    if (!await st.writeFile('SCR_01', draft.toImage())) fail('upload SCR_01 failed');
    if (await c.load(1) != 1) fail('load SCR_01 failed');

    final entry = await c.readEntry(1, ScriptField.input, 0);
    if (entry == null || entry.meta.size != 16) fail('string input missing/size wrong');

    // A shorter String write declares its real length; the device must fill the rest of the
    // fixed-size input with spaces rather than leaving stale bytes.
    final meta = BlockMeta(flagsAndType: entry.meta.flagsAndType, key: 0, size: 3);
    if (!await c.writeEntry(1, ScriptField.input, 0, meta, 'abc'.codeUnits)) {
      fail('short string write failed');
    }
    final back = await c.readEntry(1, ScriptField.input, 0);
    if (back == null || back.value.length != 16) fail('string input size wrong: ${back?.value.length}');
    final text = String.fromCharCodes(back.value);
    if (text != 'abc${' ' * 13}') fail('short string not space-padded: "$text"');

    if (!await c.unload(1)) fail('unload failed');
    await st.deleteFile('SCR_01');
  }, timeout: const Timeout(Duration(minutes: 2)));
}
