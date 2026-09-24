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
      ScriptLine(instruction: ScriptSymbol.instruction(catMath, 5)), // Modulo needs dest + 2 ops
    ];
    final errors = validateScriptLines(lines, const ScriptValidationContext());
    expect(errors.any((e) => e.contains('needs a destination')), isTrue);
    expect(errors.any((e) => e.contains('operands')), isTrue);
  });

  test('validity check accepts a well-formed expression line', () {
    final lines = [
      ScriptLine(
        destinations: [ScriptSymbol.output(0)],
        instruction: ScriptSymbol.instruction(catMath, 0), // Set
        operands: [
          ScriptSymbol.predefine(preMathOp, mathOpOpenParen),
          ScriptSymbol.variable(0),
          ScriptSymbol.predefine(preMathOp, 0), // +
          ScriptSymbol.input(0),
          ScriptSymbol.predefine(preMathOp, mathOpCloseParen),
          ScriptSymbol.predefine(preMathOp, 2), // *
          ScriptSymbol.constant(0),
        ],
      ),
    ];
    final ctx = ScriptValidationContext(
      inputTypes: [DataType.number.value],
      outputTypes: [DataType.number.value],
      variableTypes: [DataType.number.value],
      constantTypes: [DataType.number.value],
    );
    expect(validateScriptLines(lines, ctx), isEmpty);
  });

  test('validity check rejects malformed expressions', () {
    ScriptLine set(List<ScriptSymbol> ops) => ScriptLine(
          destinations: [ScriptSymbol.output(0)],
          instruction: ScriptSymbol.instruction(catMath, 0),
          operands: ops,
        );
    const ctx = ScriptValidationContext();
    // Unbalanced "(".
    expect(
        validateScriptLines([
          set([
            ScriptSymbol.predefine(preMathOp, mathOpOpenParen),
            ScriptSymbol.variable(0),
            ScriptSymbol.predefine(preMathOp, 0),
            ScriptSymbol.variable(0),
          ])
        ], ctx),
        isNotEmpty);
    // Two values in a row.
    expect(
        validateScriptLines([
          set([ScriptSymbol.variable(0), ScriptSymbol.variable(0)])
        ], ctx),
        isNotEmpty);
    // Ends with an operator.
    expect(
        validateScriptLines([
          set([ScriptSymbol.variable(0), ScriptSymbol.predefine(preMathOp, 0)])
        ], ctx),
        isNotEmpty);
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
    expect(set.maxOperands, 32);
    expect(set.numeric, isTrue);
    expect(set.expression, isTrue);

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
    expect(scriptPredefineMathOps.length, 22); // 18 operators + 2 parens + 4 functions
    expect(scriptPredefineMathOps.map((e) => e.$1), containsAll(expressionOps));
    // Logic + comparison operators are present.
    for (final op in [6, 7, 8, 9, 12, 13, 14, 15, 16, 17, 4]) {
      expect(scriptPredefineMathOps.any((e) => e.$1 == op), isTrue, reason: 'op $op');
    }
    // Functions + the Transform instruction.
    for (final op in [20, 21, 22, 23]) {
      expect(expressionFunctions.containsKey(op), isTrue);
    }
    expect(scriptInstructions.any((d) => d.category == catMath && d.op == mathTransformOp), isTrue);
  });

  test('prefix functions validate (unary and binary)', () {
    ScriptSymbol fn(int o) => ScriptSymbol.predefine(preMathOp, o);
    ScriptLine set(List<ScriptSymbol> ops) => ScriptLine(
        destinations: [ScriptSymbol.output(0)],
        instruction: ScriptSymbol.instruction(catMath, 0),
        operands: ops);
    final ctx = ScriptValidationContext(outputTypes: [DataType.number.value]);
    // Set out = size v
    expect(validateScriptLines([set([fn(22), ScriptSymbol.variable(0)])], ctx), isEmpty);
    // Set out = dot a b
    expect(
        validateScriptLines(
            [set([fn(20), ScriptSymbol.variable(0), ScriptSymbol.variable(1)])], ctx),
        isEmpty);
    // Set out = size (missing operand) -> invalid
    expect(validateScriptLines([set([fn(22)])], ctx), isNotEmpty);
    // Set out = dot a (missing 2nd operand) -> invalid
    expect(validateScriptLines([set([fn(20), ScriptSymbol.variable(0)])], ctx), isNotEmpty);
  });

  test('the standalone logic instructions are gone (expression instead)', () {
    for (final op in [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]) {
      expect(scriptInstructions.any((d) => d.category == catLogic && d.op == op), isFalse,
          reason: 'catLogic $op');
    }
    // Only Select remains in catLogic.
    expect(scriptInstructions.where((d) => d.category == catLogic).single.op, 12);
  });

  test('If / While / Wait until take a boolean expression', () {
    ScriptInstructionDef defFor(int cat, int op) =>
        scriptInstructions.firstWhere((d) => d.category == cat && d.op == op);
    for (final (cat, op) in [(catFlow, 0), (catFlow, 1), (catTime, 1)]) {
      final d = defFor(cat, op);
      expect(d.expression, isTrue);
      expect(d.maxOperands, 32);
    }
    // If a > 5 validates and round-trips.
    final line = ScriptLine(
      instruction: ScriptSymbol.instruction(catFlow, 0),
      operands: [
        ScriptSymbol.variable(0),
        ScriptSymbol.predefine(preMathOp, 16), // >
        ScriptSymbol.predefine(preIndex, 5),
      ],
    );
    final ctx = ScriptValidationContext(variableTypes: [DataType.number.value]);
    expect(validateScriptLines([line], ctx), isEmpty);
  });

  test('Add/Subtract/Multiply/Divide/Negate are no longer instructions', () {
    for (final op in [1, 2, 3, 4, 8]) {
      expect(scriptInstructions.any((d) => d.category == catMath && d.op == op), isFalse);
    }
    // The ops that remain: Set(0), Modulo(5), Minimum(6), Maximum(7), Absolute(9), Limit(10).
    for (final op in [0, 5, 6, 7, 9, 10]) {
      expect(scriptInstructions.any((d) => d.category == catMath && d.op == op), isTrue);
    }
  });

  test('a long expression validates and round-trips', () {
    // Set v0 = v1 + v2 + v3 + v1
    final line = ScriptLine(
      destinations: [ScriptSymbol.variable(0)],
      instruction: ScriptSymbol.instruction(catMath, 0),
      operands: [
        ScriptSymbol.variable(1),
        ScriptSymbol.predefine(preMathOp, 0), // +
        ScriptSymbol.variable(2),
        ScriptSymbol.predefine(preMathOp, 0),
        ScriptSymbol.variable(3),
        ScriptSymbol.predefine(preMathOp, 0),
        ScriptSymbol.variable(1),
      ],
    );
    final ctx = ScriptValidationContext(
        variableTypes: [for (var i = 0; i < 4; i++) DataType.number.value]);
    expect(validateScriptLines([line], ctx), isEmpty);
    expect(decodeScriptLines(encodeScriptLines([line])).single.operands.length, 7);
  });

  test('instructions expose operand/destination role hints', () {
    ScriptInstructionDef defFor(int cat, int op) =>
        scriptInstructions.firstWhere((d) => d.category == cat && d.op == op);
    // Limit: value, min, max
    expect(defFor(catMath, 10).operandRoles, ['value', 'min', 'max']);
    expect(defFor(catMath, 10).operandRole(2), 'max');
    expect(defFor(catMath, 10).operandRole(9), isNull); // no hint beyond the list
    // Transform: rot, offset X/Y, scale X/Y, skew
    expect(defFor(catMath, 11).operandRoles.first, 'rot');
    expect(defFor(catMath, 11).operandRoles.length, 6);
    // Set destination + register operand + Select roles.
    expect(defFor(catMath, 0).destinationRole, 'result');
    expect(defFor(catService, 1).operandRole(0), 'register');
    expect(defFor(catService, 1).destinationRole, 'value');
    expect(defFor(catLogic, 12).operandRoles, ['condition', 'if true', 'if false']);
    // Every role list is short (fits under the chip).
    for (final d in scriptInstructions) {
      for (final r in d.operandRoles) {
        expect(r.length, lessThanOrEqualTo(10), reason: '${d.label} role "$r"');
      }
    }
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
