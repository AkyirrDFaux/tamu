/// Host-side checks for the evaluation-setup builder (app/test/current_setup.dart). The
/// scripts it produces are otherwise only exercised against real hardware, so validate the
/// structure and the constant wiring here.
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/script_draft.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/types.dart';

import 'current_setup.dart';

void main() {
  test('the five setup scripts build and validate', () {
    final drafts = <String, ScriptDraft>{
      'SCR_00': scriptTemperature(),
      'SCR_01': scriptEyeMovement(),
      'SCR_02': scriptLidTimer(),
      'SCR_03': scriptBrightness(),
      'SCR_04': scriptEmoteSelector(),
    };
    for (final entry in drafts.entries) {
      final d = entry.value;
      expect(d.inputs, isNotEmpty, reason: '${entry.key} must have inputs');
      final errors = validateScriptLines(d.lines, d.validationContext);
      expect(errors, isEmpty, reason: '${entry.key}: ${errors.join('; ')}');
      expect(d.toImage(), isNotEmpty, reason: '${entry.key} must serialise');
    }
  });

  test('the eye script inverts the gyro sense on both in-plane axes', () {
    final d = scriptEyeMovement();
    // The script carries two "gN = 0 - gN" lines: the mount's gyro sense is inverted relative
    // to the render space, so the in-plane components are negated before they are mapped.
    final negated = <int>{};
    for (final line in d.lines) {
      if (line.instruction.type != symInstruction || line.instruction.subtype != catMath) continue;
      if (line.instruction.value != 0 || line.operands.length != 3) continue;
      final ops = line.operands;
      final zero = ops[0].type == symPredefine && ops[0].subtype == preNumber && ops[0].value == 0;
      final sub = ops[1].type == symPredefine && ops[1].subtype == preMathOp && ops[1].value == 1;
      if (!zero || !sub || ops[2].type != symVariable) continue;
      for (final dest in line.destinations) {
        if (dest.type == symVariable && dest.value == ops[2].value) negated.add(dest.value);
      }
    }
    expect(negated, containsAll(<int>[1, 2]), reason: 'gx and gy are negated');
  });

  test('script 2 publishes the pupil offsets and the emote script consumes them', () {
    final eye = scriptEyeMovement();
    // Two matrix outputs (offset L/R) and no render-block writes.
    expect(eye.outputs.length, 2, reason: 'offset L/R outputs');
    expect(eye.outputs.every((o) => o.type == DataType.matrix), isTrue);
    final writes = eye.lines.where(
        (l) => l.instruction.subtype == catService && l.instruction.value == 2); // Service write
    expect(writes, isEmpty, reason: 'the eye script writes no register fields');

    final emote = scriptEmoteSelector();
    // The emote input is a custom enum with the documented labels.
    expect(emote.inputs.length, 1);
    expect(emote.inputs.first.type, DataType.enum_);
    expect(emote.inputs.first.spec.uiType, ScriptUiType.dropdown);
    expect(emote.inputs.first.spec.options, emoteNames);
    // It drives the lid and reads script 2's outputs.
    final names = [for (final c in emote.constants) c.name];
    expect(names, containsAll(<String>['OFF_L', 'OFF_R', 'LID_FORCE', 'LID_MAXOPEN']));
  });

  test('script input names and enum labels survive a file round-trip', () {
    final image = scriptEmoteSelector().toImage();
    final parsed = ScriptFileData.parse(image);
    expect(parsed.inputNames, ['Emote'], reason: 'input named from the UI info');
    expect(parsed.inputSpecs.first.uiType, ScriptUiType.dropdown);
    expect(parsed.inputSpecs.first.options, emoteNames);

    // Every setup script must carry its input names (the UI shows these, not "Input N").
    final lid = ScriptFileData.parse(scriptLidTimer().toImage());
    expect(lid.inputNames, ['Blink delay', 'Movement time', 'Force close', 'Max opening']);
  });

  test('the brightness script wires the right constants per mode', () {
    final d = scriptBrightness();
    String name(int i) => i < d.constants.length ? d.constants[i].name : '?$i';

    // Collect every "register[constant] = constant" write (the mode switch), by name.
    final writes = <String>{};
    for (final line in d.lines) {
      if (line.instruction.subtype != catService || line.instruction.value != 2) continue;
      if (line.operands.length != 2) continue;
      final target = line.operands[0], value = line.operands[1];
      if (target.type != symConstant || value.type != symConstant) continue;
      writes.add('${name(target.value)}<${name(value.value)}');
    }

    expect(
        writes,
        containsAll(<String>[
          // Dark mode (22..25).
          'L_IRIS_C1<IRIS_C1_DARK', 'L_IRIS_C2<IRIS_C2_DARK',
          'L_BG_COL<BG_DARK', 'L_PUPIL_COL<PUPIL_DARK',
          'R_IRIS_C1<IRIS_C1_DARK', 'R_IRIS_C2<IRIS_C2_DARK',
          'R_BG_COL<BG_DARK', 'R_PUPIL_COL<PUPIL_DARK',
          // Light mode (18..21).
          'L_IRIS_C1<IRIS_C1_LIGHT', 'L_IRIS_C2<IRIS_C2_LIGHT',
          'L_BG_COL<BG_LIGHT', 'L_PUPIL_COL<PUPIL_LIGHT',
          'R_IRIS_C1<IRIS_C1_LIGHT', 'R_IRIS_C2<IRIS_C2_LIGHT',
          'R_BG_COL<BG_LIGHT', 'R_PUPIL_COL<PUPIL_LIGHT',
        ]));
    // Dark constants really are the dark values (guards a swapped index).
    expect(d.constants[22].name, 'IRIS_C1_DARK');
    expect(d.constants[25].name, 'PUPIL_DARK');
    expect(d.constants[18].name, 'IRIS_C1_LIGHT');
    expect(d.constants[21].name, 'PUPIL_LIGHT');
  });
}
