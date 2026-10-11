import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup_script.dart';
import 'package:tamuapp/core/script_draft.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/types.dart';

/// A script whose main program calls a local function and a global function, so the backup
/// must carry the Functions block (the local body) and a function reservation (the global).
ScriptDraft _scriptWithFunctions() {
  final draft = ScriptDraft(
    functionName: 'WithFns',
    properties: ScriptProperties.loadOnBoot,
  );
  draft.outputs.add(ScriptDraftValue(name: 'Out', type: DataType.number, size: 4));

  // Main: Call(0) local, Call(1) global, Halt.
  draft.lines.add(ScriptLine(
    instruction: ScriptSymbol.instruction(catFlow, 4), // Call
    operands: [ScriptSymbol.predefine(preIndex, 0)],
    functionTarget: 0,
  ));
  draft.lines.add(ScriptLine(
    instruction: ScriptSymbol.instruction(catFlow, 4), // Call
    operands: [ScriptSymbol.predefine(preIndex, 1)],
    functionTarget: 1,
  ));
  draft.lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6))); // Halt

  // A local function body: Set Out = Out + 1, Halt.
  draft.functions.add(ScriptFunction(
    name: 'F',
    kind: ScriptFunctionKind.local,
    body: [
      ScriptLine(
        destinations: [ScriptSymbol.output(0)],
        instruction: ScriptSymbol.instruction(catMath, 0), // Set
        operands: [
          ScriptSymbol.output(0),
          ScriptSymbol.predefine(preMathOp, 0), // +
          ScriptSymbol.constant(0),
        ],
      ),
      ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)), // Halt
    ],
  ));

  // A referenced global function (linked at load; carries a reservation).
  draft.functions.add(ScriptFunction(
    name: 'G',
    kind: ScriptFunctionKind.global,
    globalFileId: 0x123,
    globalMemOffset: 0x40,
    globalInstrOffset: 0x10,
  ));
  return draft;
}

void main() {
  test('a script with functions round-trips byte-identically through JSON (fromDraft)', () {
    final draft = _scriptWithFunctions();
    final image = draft.toImage();

    // Sanity: the image really carries a Function block and a reservation record.
    final parsed = ScriptFileData.parse(image);
    expect(parsed.functions.length, 2);
    expect(parsed.functionCount, 1);

    final backup = BackupScript.fromDraft(7, draft);
    expect(backup.functions.length, 2);
    expect(backup.functions[1].isGlobal, isTrue);
    expect(backup.functions[1].library, 0x123);

    // A strict JSON round-trip (encode to text, decode back).
    final restored = BackupScript.fromJson(
        jsonDecode(jsonEncode(backup.toJson())) as Map<String, dynamic>);
    expect(restored.functions.length, 2);
    expect(restored.functions[0].body.length, 2);
    expect(restored.toDraft().toImage(), image);
  });

  test('a script read from a raw SCR image round-trips byte-identically (fromImage)', () {
    final image = _scriptWithFunctions().toImage();

    final backup = BackupScript.fromImage(7, image);
    expect(backup.functions.length, 2);
    expect(backup.functionReservations.length, 1);
    // The reservation carries the referenced global function's parameters.
    expect(backup.functionReservations.single.library, 0x123);
    expect(backup.functionReservations.single.memoryOffset, 0x40);
    expect(backup.functionReservations.single.instructionOffset, 0x10);

    final restored = BackupScript.fromJson(
        jsonDecode(jsonEncode(backup.toJson())) as Map<String, dynamic>);
    expect(restored.toDraft().toImage(), image);
  });

  test('a functions-less script with reservations round-trips byte-identically', () {
    // An image with a reservation record but no app-only function block (an older file): the
    // reservations must survive the backup on their own, since assembly cannot regenerate them.
    final bits = ScriptFileBuilder(
      instructions: encodeScriptLines([
        ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)),
      ]),
      functionReservations: [
        encodeFunctionReservation(fileId: 0x1AB, memOffset: 0x20, instrOffset: 0x30),
      ],
    ).build();

    final backup = BackupScript.fromImage(2, bits);
    expect(backup.functions, isEmpty);
    expect(backup.functionReservations.length, 1);

    final restored = BackupScript.fromJson(
        jsonDecode(jsonEncode(backup.toJson())) as Map<String, dynamic>);
    expect(restored.toDraft().toImage(), bits);
  });

  test('an old setup JSON without the new fields parses with no functions', () {
    final legacy = <String, dynamic>{
      'slot': 2,
      'function': 'Legacy',
      'properties': ['Run on load'],
      'inputs': <dynamic>[],
      'outputs': <dynamic>[],
      'variables': <dynamic>[],
      'constants': <dynamic>[],
      'lines': <dynamic>[
        {
          'destinations': <dynamic>[],
          'instruction': {'kind': 'instruction', 'category': 'Flow', 'value': 6},
          'operands': <dynamic>[],
        }
      ],
    };

    final backup = BackupScript.fromJson(legacy);
    expect(backup.functions, isEmpty);
    expect(backup.functionReservations, isEmpty);
    final draft = backup.toDraft();
    expect(draft.functions, isEmpty);
    expect(draft.functionReservations, isEmpty);
    // Round-trips to a valid image (Halt only).
    expect(draft.toImage(), isNotEmpty);
  });
}
