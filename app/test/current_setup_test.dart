/// Host-side checks for the evaluation-setup builder (app/test/current_setup.dart). The
/// scripts it produces are otherwise only exercised against real hardware, so validate the
/// structure and the constant wiring here.
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/script_draft.dart';
import 'package:tamuapp/core/script_instructions.dart';

import 'current_setup.dart';

void main() {
  test('the four setup scripts build and validate', () {
    final drafts = <String, ScriptDraft>{
      'SCR_00': scriptTemperature(),
      'SCR_01': scriptEyeMovement(),
      'SCR_02': scriptLidTimer(),
      'SCR_03': scriptBrightness(),
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
