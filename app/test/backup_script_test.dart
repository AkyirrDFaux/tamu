import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup_script.dart';
import 'package:tamuapp/core/hex_bytes.dart';
import 'package:tamuapp/core/script_draft.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/types.dart';

ScriptDraft _draft() {
  final draft = ScriptDraft(
    functionName: 'TestFn',
    properties: ScriptProperties.loadOnBoot | ScriptProperties.runOnLoad,
  );
  draft.inputs.add(
    ScriptDraftValue(
      name: 'In',
      type: DataType.number,
      value: numberToBytes(1.5),
      spec: const ScriptInputSpec(
        uiType: ScriptUiType.slider,
        min: 0,
        max: 10,
        step: 0.5,
      ),
    ),
  );
  draft.outputs.add(
    ScriptDraftValue(name: 'Out', type: DataType.number, size: 4),
  );
  draft.variables.add(
    ScriptDraftValue(name: 'Var', type: DataType.integer, size: 4),
  );
  draft.constants.add(
    ScriptDraftValue(
      name: 'K',
      // A value no Q8.8 predefine reproduces (Q16.16 raw 13107 is not a multiple of 256), so the
      // constant stays a constant and this fixture keeps exercising the semantic value round-trip
      // (`script_literals_test.dart` covers the literals that DO become predefines).
      type: DataType.number,
      value: numberToBytes(0.1999969482421875),
    ),
  );
  draft.lines.add(
    ScriptLine(
      destinations: [ScriptSymbol.output(0)],
      instruction: ScriptSymbol.instruction(catMath, 0), // Set
      operands: [
        ScriptSymbol.input(0),
        ScriptSymbol.predefine(preMathOp, 0), // +
        ScriptSymbol.constant(0),
      ],
    ),
  );
  draft.lines.add(
    ScriptLine(
      destinations: [ScriptSymbol.variable(0)],
      instruction: ScriptSymbol.instruction(catLogic, 12), // Select
      operands: [
        ScriptSymbol.variable(0),
        ScriptSymbol.constant(0),
        ScriptSymbol.predefine(preBool, 0),
      ],
    ),
  );
  draft.lines.add(
    ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6)),
  ); // Halt
  return draft;
}

/// A draft's image with the slot declared in the Properties word, as a real `SCR_` file is.
List<int> _imageFor(ScriptDraft draft, int slot) {
  final image = draft.toImage();
  final props = scriptPropertiesWithSlot(draft.properties, slot);
  image[0] = props & 0xFF;
  image[1] = (props >> 8) & 0xFF;
  return image;
}

/// A script whose one constant is a raw hex value: `Undefined` is the hex-carrying type
/// (`encodeSemantic` writes `{'hex': ...}` for it), so a digit left over is visible in the bytes.
BackupScript _hexConstantScript(String hex) => BackupScript(
  slot: 3,
  functionName: 'Beacon',
  properties: const [],
  inputs: const [],
  outputs: const [],
  variables: const [],
  constants: [
    BackupScriptValue(
      name: 'Payload',
      type: DataType.undefined.word,
      size: 2,
      value: {'hex': hex},
    ),
  ],
  lines: const [],
);

void main() {
  test('script serialises semantically and re-serialises identically', () {
    final draft = _draft();
    final backup = BackupScript.fromDraft(3, draft);

    expect(backup.slot, 3);
    expect(backup.functionName, 'TestFn');
    expect(backup.properties, ['Load on boot', 'Run on load']);
    expect(backup.inputs.single.type, 'Number');
    expect(backup.inputs.single.ui!.style, 'Slider');
    expect(backup.constants.single.value, 0.1999969482421875);

    final restored = BackupScript.fromJson(backup.toJson()).toDraft();
    expect(restored.functionName, 'TestFn');
    // The rebuilt Properties word carries the flags and the declared slot (bits 8-15).
    expect(restored.properties & 0xFF, draft.properties);
    expect(scriptPropertiesSlot(restored.properties), 3);
    expect(restored.inputs.single.spec.uiType, ScriptUiType.slider);
    expect(restored.inputs.single.spec.max, 10);
    expect(restored.inputs.single.value, numberToBytes(1.5));
    expect(restored.constants.single.value, numberToBytes(0.1999969482421875));
    expect(restored.lines.length, 3);
    // The rebuilt SCR_XX image is byte-identical to the original.
    expect(restored.toImage(), _imageFor(draft, 3));
  });

  test('script can be decoded from a raw SCR_XX image', () {
    final image = _imageFor(_draft(), 5);
    final backup = BackupScript.fromImage(5, image);
    expect(backup.slot, 5);
    expect(backup.functionName, 'TestFn');
    expect(backup.lines.first.instruction.category, 'Math');
    expect(backup.lines[1].operands.last.kind, 'predefine');
    expect(backup.lines[1].operands.last.subtype, 'Bool');
    expect(backup.toDraft().toImage(), image);
  });

  test('original firmware image is unchanged by a semantic round-trip', () {
    final image = _draft().toImage();
    // Decode via the file parser, go semantic, and re-encode through the draft.
    final parsed = ScriptFileData.parse(image);
    final backup = BackupScript.fromDraft(0, ScriptDraft.fromFile(parsed));
    expect(backup.toDraft().toImage(), image);
  });

  // The script half of Akyirr's hex ruling (2026-11-12): the archive import stays LENIENT - a
  // script that restores today keeps restoring - and every dropped digit is a warning line naming
  // the slot and the value it came from, instead of vanishing with the nibble.
  test('a script constant read short keeps its bytes and says which value', () {
    final backup = _hexConstantScript('12345');
    final warnings = <HexTruncation>[];

    final draft = backup.toDraft(warnings: warnings);

    // LENIENT: `12345` still makes `0x12 0x34`, which is what this script has always written.
    expect(draft.constants.single.value, [0x12, 0x34]);
    expect(
      draft.toImage(),
      backup.toDraft().toImage(),
      reason: 'a warning is not a change to the bytes',
    );

    // COUNTED: one line, naming the script slot and the constant it was read from.
    expect(warnings, hasLength(1));
    final warning = warnings.single;
    expect(warning.where, 'Beacon (SCR_003, slot 3) / constant "Payload"');
    expect(warning.text, '12345');
    expect(warning.offset, 4, reason: 'the `5` at index 4 has no pair');
    expect(warning.byteCount, 2);
    expect(
      warning.line,
      'Beacon (SCR_003, slot 3) / constant "Payload" - "12345": odd hex '
      'digit count; the digit at character 4 has no pair, so the value is '
      '2 bytes',
    );
  });

  test('a well-formed script constant produces no warning', () {
    final warnings = <HexTruncation>[];

    final draft = _hexConstantScript('1234').toDraft(warnings: warnings);

    expect(draft.constants.single.value, [0x12, 0x34]);
    expect(warnings, isEmpty);
  });
}
