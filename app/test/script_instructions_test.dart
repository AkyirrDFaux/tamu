import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup_script.dart';
import 'package:tamuapp/core/script_draft.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/types.dart';
void main() {
  test('instruction encode/decode round-trip', () {
    final lines = [
      ScriptLine(destinations: [ScriptSymbol.output(0)], instruction: ScriptSymbol.instruction(catMath, 1), operands: [
        ScriptSymbol.variable(0),
        ScriptSymbol.input(0),
      ]),
      ScriptLine(instruction: ScriptSymbol.instruction(catTime, 0), operands: [ScriptSymbol.constant(0)]),
    ];
    final bytes = encodeScriptLines(lines);
    final back = decodeScriptLines(bytes);
    expect(back.length, 2);
    expect(back[0].destinations.single.type, symOutput);
    expect(back[0].instruction.subtype, catMath);
    expect(back[0].instruction.value, 1);
    expect(back[0].operands.map((s) => s.type), [symVariable, symInput]);
    expect(back[1].destinations, isEmpty);
    expect(back[1].instruction.subtype, catTime);
  });

  test('validity check reports missing destination and arity', () {
    final lines = [
      ScriptLine(instruction: ScriptSymbol.instruction(catMath, 1)), // Add needs dest + 2 ops
    ];
    final errors = validateScriptLines(lines, const ScriptValidationContext());
    expect(errors.any((e) => e.contains('needs a destination')), isTrue);
    expect(errors.any((e) => e.contains('operands')), isTrue);
  });

  test('validity check accepts a well-formed line', () {
    final lines = [
      ScriptLine(
        destinations: [ScriptSymbol.output(0)],
        instruction: ScriptSymbol.instruction(catMath, 1),
        operands: [ScriptSymbol.variable(0), ScriptSymbol.input(0)],
      ),
    ];
    final ctx = ScriptValidationContext(
      inputTypes: [DataType.number.value],
      outputTypes: [DataType.number.value],
      variableTypes: [DataType.number.value],
    );
    expect(validateScriptLines(lines, ctx), isEmpty);
  });

  test('draft round-trips names, input specs and instructions', () {
    final draft = ScriptDraft(functionName: 'Blink', properties: ScriptProperties.loadOnBoot)
      ..inputs.add(ScriptDraftValue(
          name: 'Speed',
          type: DataType.number,
          size: 4,
          spec: const ScriptInputSpec(uiType: ScriptUiType.slider, min: 0, max: 10, step: 0.5)))
      ..outputs.add(ScriptDraftValue(name: 'Flash', type: DataType.bool_, size: 1))
      ..variables.add(ScriptDraftValue(name: 'T', type: DataType.number, size: 4))
      ..constants.add(ScriptDraftValue(
          name: 'Period', type: DataType.number, size: 4, value: numberToBytes(2.5)))
      ..lines.add(ScriptLine(
        destinations: [ScriptSymbol.output(0)],
        instruction: ScriptSymbol.instruction(catTime, 0),
        operands: [ScriptSymbol.constant(0)],
      ));

    final parsed = ScriptFileData.parse(draft.toImage());
    expect(parsed.functionName, 'Blink');
    expect(parsed.inputNames, ['Speed']);
    expect(parsed.outputNames, ['Flash']);
    expect(parsed.variableNames, ['T']);
    expect(parsed.constantNames, ['Period']);
    expect(parsed.inputSpecs.single.uiType, ScriptUiType.slider);
    expect(parsed.inputSpecs.single.max, closeTo(10, 1e-6));
    expect(decodeScriptLines(parsed.instructions).length, 1);

    final restored = ScriptDraft.fromFile(parsed);
    expect(restored.lines.first.instruction.subtype, catTime);
    expect(restored.constants.single.value.length, 4);
  });

  test('type-driven UI styles and derived sizes', () {
    // Input styles are limited by the data type.
    expect(uiStylesForType(DataType.bool_),
        containsAll([ScriptUiType.toggle, ScriptUiType.button]));
    expect(uiStylesForType(DataType.bool_), isNot(contains(ScriptUiType.slider)));
    expect(uiStylesForType(DataType.number), contains(ScriptUiType.slider));
    expect(uiStylesForType(DataType.string), [ScriptUiType.auto]);

    // Limits only apply to the numeric input styles.
    expect(uiStyleSupportsLimits(ScriptUiType.slider), isTrue);
    expect(uiStyleSupportsLimits(ScriptUiType.toggle), isFalse);

    // Size is derived from the type, or from the value when present.
    expect(defaultSizeForType(DataType.number), 4);
    final v = ScriptDraftValue(type: DataType.number, size: 4);
    expect(v.size, 4);
    v.setValue(Uint8List.fromList([1, 2, 3, 4, 5, 6, 7, 8]));
    expect(v.size, 8);
    v.setType(DataType.bool_);
    expect(v.type, DataType.bool_);
    expect(v.size, 1);
    expect(v.value, isEmpty);
  });

  test('instruction destination/operand limits', () {
    ScriptInstructionDef defFor(int cat, int op) =>
        scriptInstructions.firstWhere((d) => d.category == cat && d.op == op);

    final nop = defFor(catService, 4);
    expect(nop.maxDestinations, 0);
    expect(nop.maxOperands, 0);

    final set = defFor(catMath, 0);
    expect(set.maxDestinations, 1);
    expect(set.minOperands, 1);
    expect(set.maxOperands, 1);
    expect(set.numeric, isTrue);

    final regRead = defFor(catService, 1);
    expect(regRead.constantIndex, 0);
    expect(regRead.maxDestinations, 1);

    // Foreign register ops: address operand first, BlockInfo constant second.
    final regReadForeign = defFor(catService, 5);
    expect(regReadForeign.addressIndex, 0);
    expect(regReadForeign.constantIndex, 1);
    expect(regReadForeign.minOperands, 2);
    final regWriteForeign = defFor(catService, 6);
    expect(regWriteForeign.addressIndex, 0);
    expect(regWriteForeign.constantIndex, 1);
    expect(regWriteForeign.minOperands, 3);
  });

  test('all predefine subtypes and math ops are available', () {
    expect(scriptPredefineSubtypes.map((e) => e.$1).toSet(),
        {preState, preType, preIndex, preChar, preMathOp, preBool, preNumber});
    expect(scriptPredefineMathOps.length, 18);
  });

  test('n-ary math ops accept up to 8 operands', () {
    ScriptInstructionDef defFor(int cat, int op) =>
        scriptInstructions.firstWhere((d) => d.category == cat && d.op == op);
    for (final (cat, op) in [(catMath, 1), (catMath, 3), (catMath, 6), (catLogic, 0)]) {
      expect(defFor(cat, op).maxOperands, 8);
    }
    // A 4-operand Add validates and round-trips.
    final line = ScriptLine(
      destinations: [ScriptSymbol.variable(0)],
      instruction: ScriptSymbol.instruction(catMath, 1),
      operands: [
        ScriptSymbol.variable(1),
        ScriptSymbol.variable(2),
        ScriptSymbol.constant(0),
        ScriptSymbol.predefine(preIndex, 3),
      ],
    );
    final ctx = ScriptValidationContext(
        variableTypes: [DataType.number.value, DataType.number.value, DataType.number.value],
        constantTypes: [DataType.number.value]);
    expect(validateScriptLines([line], ctx), isEmpty);
    expect(decodeScriptLines(encodeScriptLines([line])).single.operands.length, 4);
  });

  test('BlockInfo and Number-literal symbols round-trip through the backup', () {
    final draft = ScriptDraft(functionName: 'Reg')
      ..constants.add(ScriptDraftValue(
          name: 'Target',
          type: DataType.blockInfo,
          value: Uint8List.fromList(uint32ToBytes(makeBlockInfo(BlockType.pwm.value, 0, 1, 0)))))
      ..lines.add(ScriptLine(
        instruction: ScriptSymbol.instruction(catService, 2),
        operands: [
          ScriptSymbol.constant(0),
          ScriptSymbol.predefine(preNumber, 128), // 0.5
        ],
      ));
    final parsed = ScriptFileData.parse(draft.toImage());
    expect(parsed.constants.single.type, DataType.blockInfo);
    final restored = ScriptDraft.fromFile(parsed);
    expect(restored.constants.single.type, DataType.blockInfo);
    expect(uint32FromBytes(restored.constants.single.value),
        makeBlockInfo(BlockType.pwm.value, 0, 1, 0));
    expect(restored.lines.single.operands.last.subtype, preNumber);
    expect(restored.lines.single.operands.last.value, 128);

    // And through the semantic backup codec (block word + instance/field/key).
    final backup = BackupScript.fromDraft(0, draft);
    final json = backup.toJson();
    expect(json['constants'][0]['type'], 'BlockInfo');
    final back = BackupScript.fromJson(json).toDraft();
    expect(back.constants.single.type, DataType.blockInfo);
    expect(uint32FromBytes(back.constants.single.value),
        makeBlockInfo(BlockType.pwm.value, 0, 1, 0));
  });

  test('foreign register ops validate the address operand', () {
    // Register read (foreign): operand 0 = address, operand 1 = BlockInfo constant.
    final bad = [
      ScriptLine(
        destinations: [ScriptSymbol.variable(0)],
        instruction: ScriptSymbol.instruction(catService, 5),
        operands: [ScriptSymbol.variable(1), ScriptSymbol.constant(0)],
      ),
    ];
    final ctx = ScriptValidationContext(
        variableTypes: [DataType.number.value, DataType.colour.value]);
    final errors = validateScriptLines(bad, ctx);
    expect(errors.any((e) => e.contains('device address')), isTrue);

    final good = [
      ScriptLine(
        destinations: [ScriptSymbol.variable(0)],
        instruction: ScriptSymbol.instruction(catService, 5),
        operands: [ScriptSymbol.input(0), ScriptSymbol.constant(0)],
      ),
    ];
    final ctxGood = ScriptValidationContext(
        inputTypes: [DataType.id.value], variableTypes: [DataType.number.value]);
    expect(validateScriptLines(good, ctxGood), isEmpty);
  });
}
