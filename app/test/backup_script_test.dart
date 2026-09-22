import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup_script.dart';
import 'package:tamuapp/core/script_draft.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/types.dart';

ScriptDraft _draft() {
  final draft = ScriptDraft(
    functionName: 'TestFn',
    properties: ScriptProperties.loadOnBoot | ScriptProperties.runOnLoad,
  );
  draft.inputs.add(ScriptDraftValue(
    name: 'In',
    type: DataType.number,
    value: numberToBytes(1.5),
    spec: const ScriptInputSpec(uiType: ScriptUiType.slider, min: 0, max: 10, step: 0.5),
  ));
  draft.outputs.add(ScriptDraftValue(name: 'Out', type: DataType.number, size: 4));
  draft.variables
      .add(ScriptDraftValue(name: 'Var', type: DataType.integer, size: 4));
  draft.constants
      .add(ScriptDraftValue(name: 'K', type: DataType.number, value: numberToBytes(42)));
  draft.lines.add(ScriptLine(
    destinations: [ScriptSymbol.output(0)],
    instruction: ScriptSymbol.instruction(catMath, 1), // Add
    operands: [ScriptSymbol.input(0), ScriptSymbol.constant(0)],
  ));
  draft.lines.add(ScriptLine(
    instruction: ScriptSymbol.instruction(catLogic, 6), // Compare =
    operands: [ScriptSymbol.variable(0), ScriptSymbol.predefine(preBool, 1)],
  ));
  draft.lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6))); // Halt
  return draft;
}

void main() {
  test('script serialises semantically and re-serialises identically', () {
    final draft = _draft();
    final backup = BackupScript.fromDraft(3, draft);

    expect(backup.slot, 3);
    expect(backup.functionName, 'TestFn');
    expect(backup.properties, ['Load on boot', 'Run on load']);
    expect(backup.inputs.single.type, 'Number');
    expect(backup.inputs.single.ui!.style, 'Slider');
    expect(backup.constants.single.value, 42.0);

    final restored = BackupScript.fromJson(backup.toJson()).toDraft();
    expect(restored.functionName, 'TestFn');
    expect(restored.properties, draft.properties);
    expect(restored.inputs.single.spec.uiType, ScriptUiType.slider);
    expect(restored.inputs.single.spec.max, 10);
    expect(restored.inputs.single.value, numberToBytes(1.5));
    expect(restored.constants.single.value, numberToBytes(42));
    expect(restored.lines.length, 3);
    // The rebuilt SCR_XX image is byte-identical to the original.
    expect(restored.toImage(), draft.toImage());
  });

  test('script can be decoded from a raw SCR_XX image', () {
    final image = _draft().toImage();
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
}
