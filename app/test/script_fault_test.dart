import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/script_client.dart';
import 'package:tamuapp/core/script_draft.dart';
import 'package:tamuapp/core/script_errors.dart';
import 'package:tamuapp/core/script_fault.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/types.dart';

/// A script fault report has to read as a sentence: the owner's report of `error 2 at cat 4 op 1`
/// is `SCRIPT_ERR_TYPE` on a `Service/Register read`, and nothing in the record says so.
void main() {
  /// The register of a loaded script's input 0: `Script[slot].f1.k0 (Input)`.
  int scriptInput(int slot) => makeBlockInfo(
    scriptTypeForIndex(slot),
    scriptInstanceForIndex(slot),
    ScriptField.input,
    0,
  );

  /// A program whose line [at] is a `Register read` of the constant [constantIndex].
  List<ScriptLine> programWithRegRead(int at, {int constantIndex = 0}) => [
    for (var i = 0; i < at; i++)
      ScriptLine(instruction: ScriptSymbol.instruction(catService, 4)),
    ScriptLine(
      destinations: [ScriptSymbol.variable(0)],
      instruction: ScriptSymbol.instruction(catService, 1),
      operands: [ScriptSymbol.constant(constantIndex)],
    ),
  ];

  /// The encoding the VM writes into a report record for [category]/[opcode].
  int encoding(int category, int opcode) => 1 + ((category << 8) | opcode);

  group('error-code registry', () {
    test('every SCRIPT_ERR_* name of ScriptDefs.h is known', () {
      expect(ScriptErrorCode.names, {
        0: 'SCRIPT_ERR_NONE',
        1: 'SCRIPT_ERR_UNKNOWN_OP',
        2: 'SCRIPT_ERR_TYPE',
        3: 'SCRIPT_ERR_OPERAND',
        4: 'SCRIPT_ERR_BOUNDS',
        5: 'SCRIPT_ERR_STACK',
        6: 'SCRIPT_ERR_REGISTER',
        7: 'SCRIPT_ERR_TIMEOUT',
        8: 'SCRIPT_ERR_NOT_IMPL',
      });
      expect(ScriptErrorCode.name(2), 'SCRIPT_ERR_TYPE');
    });

    test('a code outside the registry is still text, never a bare integer', () {
      expect(ScriptErrorCode.name(12), 'Unnamed error 12');
    });
  });

  group('decoding a fault', () {
    test(
      'a TYPE fault on a Register read names line, instruction, register, error',
      () {
        final fault = ScriptFault.fromReport(
          ScriptReport(
            state: ScriptState.error,
            depth: 0,
            line: 4,
            instruction: encoding(catService, 1),
            error: ScriptErrorCode.type,
          ),
          program: programWithRegRead(4),
          constantOf: (index) => index == 0 ? scriptInput(2) : null,
        );
        // The text the Scripts page and the editor show, quoted whole.
        expect(
          fault.text,
          'Script fault at line 4 (Register read): '
          'SCRIPT_ERR_TYPE on Script[2].f1.k0 (Input)',
        );
        expect(fault.line, 4);
        expect(fault.instructionName, 'Register read');
        expect(fault.errorName, 'SCRIPT_ERR_TYPE');
        expect(fault.engineLevel, isFalse);
      },
    );

    test('an engine-level fault says so instead of naming a line', () {
      final fault = ScriptFault.fromReport(
        const ScriptReport(
          state: ScriptState.error,
          depth: 0,
          line: 7,
          instruction: 0,
          error: ScriptErrorCode.type,
        ),
      );
      expect(fault.engineLevel, isTrue);
      expect(
        fault.text,
        'Script fault (engine-level): SCRIPT_ERR_TYPE - no instruction behind it',
      );
      expect(fault.text, isNot(contains('line 7')));
    });

    test('without the program the register is omitted, not guessed', () {
      final fault = ScriptFault.fromReport(
        ScriptReport(
          state: ScriptState.error,
          depth: 0,
          line: 4,
          instruction: encoding(catService, 1),
          error: ScriptErrorCode.type,
        ),
      );
      expect(fault.register, isNull);
      expect(
        fault.text,
        'Script fault at line 4 (Register read): SCRIPT_ERR_TYPE',
      );
    });

    test('a line whose instruction differs does not lend its register', () {
      // A file edited (or a program assembled with function bodies) since it was loaded can put
      // another instruction at the recorded index: naming its register would be a guess.
      final program = programWithRegRead(4);
      program[4] = ScriptLine(
        instruction: ScriptSymbol.instruction(catService, 4),
      );
      final fault = ScriptFault.fromReport(
        ScriptReport(
          state: ScriptState.error,
          depth: 0,
          line: 4,
          instruction: encoding(catService, 1),
          error: ScriptErrorCode.type,
        ),
        program: program,
        constantOf: (index) => scriptInput(2),
      );
      expect(fault.register, isNull);
      expect(fault.text, endsWith('SCRIPT_ERR_TYPE'));
    });

    test('a reserved opcode reads as its category and opcode', () {
      // Service opcode 3 is the retired state instruction: no app entry, still nameable.
      final fault = ScriptFault.fromReport(
        ScriptReport(
          state: ScriptState.error,
          depth: 0,
          line: 1,
          instruction: encoding(catService, 3),
          error: ScriptErrorCode.bounds,
        ),
      );
      expect(fault.instructionName, isNull);
      expect(
        fault.text,
        'Script fault at line 1 (Service op 3): SCRIPT_ERR_BOUNDS',
      );
    });

    test('a healthy report is not a fault', () {
      const healthy = ScriptReport(
        state: ScriptState.running,
        depth: 0,
        line: 3,
      );
      expect(draftFault(healthy, ScriptDraft()), isNull);
      expect(
        fileFault(healthy, ScriptFileData.parse(_file([], const []))),
        isNull,
      );
      expect(draftFault(null, ScriptDraft()), isNull);
    });
  });

  group('the register label', () {
    test('a banked script slot is named by its global index', () {
      // Slot 70 lives in the second bank (type 0x3F5, instance 6): the per-bank instance alone
      // would name four different slots.
      final bi = makeBlockInfo(
        scriptTypeForIndex(70),
        scriptInstanceForIndex(70),
        ScriptField.output,
        3,
      );
      expect(scriptRegisterLabel(bi), 'Script[70].f2.k3 (Output)');
    });

    test('a static block carries its field name from the registry', () {
      final bi = makeBlockInfo(
        BlockType.pwm.value,
        0,
        1, // Duty
        0,
      );
      expect(scriptRegisterLabel(bi), 'PWM[0].f1.k0 (Duty)');
    });

    test('an unknown field is named by number alone', () {
      final bi = makeBlockInfo(BlockType.led.value, 1, 9, 0);
      expect(scriptRegisterLabel(bi), 'LED[1].f9.k0');
    });
  });

  group('decoding against the program the app holds', () {
    test('a draft resolves the register from its own BlockInfo constant', () {
      final draft = ScriptDraft(
        constants: [
          ScriptDraftValue(
            name: 'input',
            type: DataType.blockInfo,
            value: blockInfoBytes(
              scriptTypeForIndex(2),
              scriptInstanceForIndex(2),
              ScriptField.input,
              0,
            ),
          ),
        ],
        lines: programWithRegRead(4),
      );
      final fault = draftFault(
        ScriptReport(
          state: ScriptState.error,
          depth: 0,
          line: 4,
          instruction: encoding(catService, 1),
          error: ScriptErrorCode.type,
        ),
        draft,
      );
      expect(
        fault!.text,
        'Script fault at line 4 (Register read): '
        'SCRIPT_ERR_TYPE on Script[2].f1.k0 (Input)',
      );
    });

    test(
      'a stored file resolves the register from its own BlockInfo constant',
      () {
        final image = _file(programWithRegRead(4), [
          blockInfoBytes(
            scriptTypeForIndex(5),
            scriptInstanceForIndex(5),
            ScriptField.output,
            2,
          ),
        ]);
        final fault = fileFault(
          ScriptReport(
            state: ScriptState.error,
            depth: 0,
            line: 4,
            instruction: encoding(catService, 1),
            error: ScriptErrorCode.type,
          ),
          ScriptFileData.parse(image),
        );
        expect(
          fault!.text,
          'Script fault at line 4 (Register read): '
          'SCRIPT_ERR_TYPE on Script[5].f2.k2 (Output)',
        );
      },
    );

    test('a constant that is not a BlockInfo names no register', () {
      final draft = ScriptDraft(
        constants: [
          ScriptDraftValue(
            type: DataType.number,
            value: Uint8List.fromList([0, 0, 0, 0]),
          ),
        ],
        lines: programWithRegRead(1),
      );
      final fault = draftFault(
        ScriptReport(
          state: ScriptState.error,
          depth: 0,
          line: 1,
          instruction: encoding(catService, 1),
          error: ScriptErrorCode.type,
        ),
        draft,
      );
      expect(fault!.register, isNull);
    });
  });
}

/// A minimal `SCR_XXX` image: [lines] as instructions, [constants] as BlockInfo constants.
Uint8List _file(List<ScriptLine> lines, List<List<int>> constants) =>
    ScriptFileBuilder(
      constants: [
        for (var i = 0; i < constants.length; i++)
          ValueInfo(type: DataType.blockInfo.value, size: 4),
      ],
      constantValues: constants,
      instructions: encodeScriptLines(lines),
    ).build();
