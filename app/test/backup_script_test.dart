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
      type: DataType.number,
      value: numberToBytes(42),
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
    // The rebuilt Properties word carries the flags and the declared slot (bits 8-15).
    expect(restored.properties & 0xFF, draft.properties);
    expect(scriptPropertiesSlot(restored.properties), 3);
    expect(restored.inputs.single.spec.uiType, ScriptUiType.slider);
    expect(restored.inputs.single.spec.max, 10);
    expect(restored.inputs.single.value, numberToBytes(1.5));
    expect(restored.constants.single.value, numberToBytes(42));
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
}
