/// Hardware-in-the-loop test for the Script service (Docs/Services/Script.md):
/// create -> write a program via the write stream -> read it back -> run it and
/// verify states, outputs and the ScriptUpdated flag set by a MEM_WRITE.
///
/// Run like the other HIL tests (requires the Tamu core on a USB port):
/// ```
/// LIBSERIALPORT_PATH=build/linux/x64/debug/bundle/lib/libserialport.so \
/// TAMU_HIL=/dev/ttyACM0 flutter test test/hil_script_test.dart
/// ```
/// Skipped automatically when TAMU_HIL is not set.
@Tags(['hil'])
library;

import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/dynmem.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/script_asm.dart';
import 'package:tamuapp/core/script_client.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/types.dart';

final Timeout hilTimeout = const Timeout(Duration(seconds: 90));

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  debugDefaultTargetPlatformOverride = TargetPlatform.linux;

  final hilTarget = Platform.environment['TAMU_HIL'];
  final skipReason = hilTarget == null ? 'TAMU_HIL not set' : false;

  Future<void> connectApp() async {
    final mgr = ConnectionManager.instance;
    final err = await mgr.connectTo(DiscoveredLink(
        id: hilTarget!, type: LinkType.usb, name: 'Tamu core'));
    if (err != null) fail('connect failed: $err');
    for (var i = 0; i < 20; i++) {
      if (await DeviceDatabase.instance.pingCore()) return;
      await Future<void>.delayed(const Duration(milliseconds: 250));
    }
    fail('core did not answer ping within settle window');
  }

  Future<void> disconnectApp() async {
    await ConnectionManager.instance.disconnect();
  }

  /// Waits until [predicate] is true (poll every 50 ms, up to ~5 s).
  Future<void> waitFor(Future<bool> Function() predicate, String what) async {
    final deadline = DateTime.now().add(const Duration(seconds: 5));
    while (!await predicate()) {
      if (DateTime.now().isAfter(deadline)) fail('timeout waiting for $what');
      await Future<void>.delayed(const Duration(milliseconds: 50));
    }
  }

  List<int> u32(int v) => [
        v & 0xFF,
        (v >> 8) & 0xFF,
        (v >> 16) & 0xFF,
        (v >> 24) & 0xFF,
      ];

  double numberFrom(List<int> bytes) {
    final raw = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
    return (raw & 0x80000000) != 0 ? (raw - 0x100000000) / 65536.0 : raw / 65536.0;
  }

  test('HIL: fresh script has no file, then a save creates it', () async {
    await connectApp();
    addTearDown(disconnectApp);
    final client = ScriptClient(deviceId: 1);

    final id = await client.createScript();
    expect(id, isNotNull);

    // A freshly created script only reserves the ID - no file exists yet, so the
    // editor's read must come back null (not a corrupt-file error).
    expect(await client.readScriptFile(id!), isNull,
        reason: 'no file before the first save');
    expect(await client.readName(id), isNull);

    // The editor saves a blank script exactly like this (name + END line).
    final file = ScriptFileData(
      name: 'Fresh',
      instructions: [
        ...compileLine('END')!,
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
      ],
    );
    expect(await client.writeScriptFile(id, file.toBytes()), isTrue);
    expect(await client.readName(id), 'Fresh');

    final back = await client.readScriptFile(id);
    expect(back, isNotNull);
    final parsed = ScriptFileData.parse(back!);
    expect(parsed, isNotNull);
    expect(parsed!.name, 'Fresh');
    expect(splitLines(parsed.instructions).single.degenerate, isTrue);

    // The blank script runs to Finished immediately (just an END).
    expect(await client.setState(id, ScriptStateCode.running), isTrue);
    await waitFor(() async => (await client.readState(id)) == ScriptStateCode.finished,
        'script finished');

    await client.deleteScript(id);
  }, timeout: hilTimeout, skip: skipReason);

  test('HIL: script create/write/read/run/states/outputs', () async {
    await connectApp();
    addTearDown(disconnectApp);
    final client = ScriptClient(deviceId: 1);

    // Clean any leftover script ids from earlier runs (ids 1..4).
    for (var id = 1; id <= 4; id++) {
      await client.deleteScript(id);
    }
    // ignore: avoid_print
    print('script count: ${await client.count()}');

    final id = await client.createScript();
    expect(id, isNotNull, reason: 'create script');
    // ignore: avoid_print
    print('created script id: $id');

    final file = ScriptFileData(
      name: 'HILTest',
      inputs: [
        ScriptInput(
            key: 1, flagsAndType: DataType.number.value, defaultValue: u32(5 << 16)),
      ],
      outputNames: const ['out_flag'],
      variableNames: const ['acc'],
      constants: [
        ScriptConstant(
            flagsAndType: DataType.number.value, value: u32(10 << 16)),
      ],
      instructions: [
        ...compileLine('Var0 ADD In0 Const0')!,
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
        ...compileLine('Out0 EQ Var0 #15')!,
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
        ...compileLine('END')!,
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
      ],
    );
    final bytes = file.toBytes();
    expect(await client.writeScriptFile(id!, bytes), isTrue,
        reason: 'write script file');

    final back = await client.readScriptFile(id);
    expect(back, isNotNull);
    final parsed = ScriptFileData.parse(back!);
    expect(parsed, isNotNull, reason: 'parse read-back');
    expect(parsed!.name, 'HILTest');
    expect(parsed.inputs.length, 1);
    expect(parsed.outputNames, ['out_flag']);
    expect(parsed.variableNames, ['acc']);
    expect(splitLines(parsed.instructions).length, 3);

    expect(await client.readName(id), 'HILTest');
    final io = await client.readIoSize(id);
    expect(io, (inputs: 1, outputs: 1));
    final info = await client.getInfo(id);
    expect(info!.variables, 1);
    expect(info.instructions, 3);

    final input0 = await client.readInput(id, 0);
    expect(input0, isNotNull);
    expect(numberFrom(input0!.value), closeTo(5.0, 0.001),
        reason: 'input default value');

    // Run it: Var0 = 5 + 10 = 15, Out0 = (15 == 15) = true, then Finished.
    expect(await client.setState(id, ScriptStateCode.running), isTrue);
    await waitFor(() async => (await client.readState(id)) == ScriptStateCode.finished,
        'script finished');
    final state = await client.readState(id);
    expect(state, ScriptStateCode.finished);

    final out0 = await client.readOutput(id, 0);
    expect(out0, isNotNull, reason: 'output value present');
    expect(out0!.value.single, 1, reason: 'out_flag should be true');

    final current = await client.getCurrentInstruction(id);
    expect(current, 2, reason: 'stopped at the END line (index 2)');

    expect(await client.setState(id, ScriptStateCode.stopped), isTrue);
    expect(await client.readState(id), ScriptStateCode.stopped);

    // Clean up.
    expect(await client.deleteScript(id), isTrue);
  }, timeout: hilTimeout, skip: skipReason);

  test('HIL: script MEM_WRITE sets the ScriptUpdated flag', () async {
    await connectApp();
    addTearDown(disconnectApp);
    final client = ScriptClient(deviceId: 1);
    final dyn = DynamicMemoryClient(deviceId: 1);

    // A dynamic block to be written by the script (field 0 created on demand).
    final dynBlock = await dyn.createBlock(BlockType.undefined, 'SCRMEM');
    expect(dynBlock, isNotNull, reason: 'create dynamic block');

    final id = await client.createScript();
    expect(id, isNotNull);

    // Const0 = address [block][field 0][key invalid][DynamicMemory 0x05].
    final address = [
      dynBlock! & 0xFF,
      0,
      invalidIndex,
      ServiceType.dynamicMemory.value,
    ];
    final file = ScriptFileData(
      name: 'MemWrite',
      variableNames: const ['ok'],
      constants: [
        ScriptConstant(flagsAndType: DataType.integer.value, value: address),
        ScriptConstant(
            flagsAndType: DataType.number.value, value: u32(7 << 16)),
      ],
      instructions: [
        ...compileLine('Var0 MEM_WRITE Const1 Const0')!,
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
        ...compileLine('END')!,
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
      ],
    );
    expect(await client.writeScriptFile(id!, file.toBytes()), isTrue);
    expect(await client.setState(id, ScriptStateCode.running), isTrue);
    await waitFor(() async => (await client.readState(id)) == ScriptStateCode.finished,
        'script finished');

    // The script wrote block 0 field 0 = Number 7.0, flagged ScriptUpdated.
    final field = await dyn.readValue(BlockIndex(block: dynBlock, field: 0));
    expect(field, isNotNull, reason: 'written field readable');
    expect(field!.meta.flags & FieldFlags.scriptUpdated, isNot(0),
        reason: 'script write must set ScriptUpdated');
    expect(numberFrom(field.value), closeTo(7.0, 0.001));

    // Clean up: delete the script and the dynamic block.
    await client.deleteScript(id);
    await dyn.delete(block: dynBlock);
  }, timeout: hilTimeout, skip: skipReason);
}