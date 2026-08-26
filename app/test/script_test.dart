import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/script_asm.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/types.dart';

void main() {
  group('compileLine', () {
    test('compiles a normal instruction line', () {
      final symbols = compileLine('Var0 ADD In0 Const0')!;
      expect(symbols.length, 4);
      expect(symbols[0].type, ScriptSymbolType.variable);
      expect(symbols[0].value, 0);
      expect(symbols[1].type, ScriptSymbolType.instruction);
      expect(symbols[1].opcode, ScriptOpcode.add);
      expect(symbols[2].type, ScriptSymbolType.input);
      expect(symbols[3].type, ScriptSymbolType.constant);
    });

    test('compiles predefine literals', () {
      final symbols = compileLine('Out0 MUL Var1 #5')!;
      expect(symbols[3].type, ScriptSymbolType.predefine);
      expect(symbols[3].predefineText, '#5');
      expect(symbols[3].subtype, PredefineSubtype.index_.value);

      final b = compileLine('Out0 EQ Var1 True')!;
      expect(b[3].predefineText, 'True');

      final c = compileLine('Out0 EQ Var1 \'a\'')!;
      expect(c[3].predefineText, "'a'");
    });

    test('compiles an IF condition with an embedded comparison', () {
      final symbols = compileLine('Var0 IF Var1 LT #5')!;
      expect(symbols[1].opcode, ScriptOpcode.if_);
      expect(symbols[2].type, ScriptSymbolType.variable);
      expect(symbols[3].opcode, ScriptOpcode.cmpLt);
      expect(symbols[4].predefineText, '#5');
    });

    test('compiles flow terminators', () {
      expect(compileLine('END')!.single.opcode, ScriptOpcode.end);
      expect(compileLine('END_IF')!.single.opcode, ScriptOpcode.endIf);
      expect(compileLine('END_WHILE')!.single.opcode, ScriptOpcode.endWhile);
    });

    test('rejects malformed lines', () {
      expect(compileLine(''), isNull);
      expect(compileLine('ADD In0'), isNull); // op cannot start a line
      expect(compileLine('Var0 ADD In0 Foo'), isNull); // bad operand
      expect(compileLine('Const0 ADD In0'), isNull); // output must be Var/Out
    });
  });

  group('splitLines + decompile', () {
    test('round-trips a program', () {
      final symbols = <ScriptSymbol>[
        ...compileLine('Var0 ADD In0 Const0')!,
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
        ...compileLine('Out0 EQ Var0 #10')!,
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
        ...compileLine('END')!,
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
      ];
      final lines = splitLines(symbols);
      expect(lines.length, 3);
      expect(lines[0].decompile(), 'Var0 ADD In0 Const0');
      expect(lines[1].decompile(), 'Out0 EQ Var0 #10');
      expect(lines[2].decompile(), 'END');
      expect(lines[2].degenerate, isTrue);
    });

    test('handles an empty instruction stream', () {
      expect(splitLines(const []), isEmpty);
    });
  });

  group('validateProgram', () {
    ScriptValidation check(List<ScriptSymbol> symbols, {int inC = 2, int outC = 2, int varC = 2, int constC = 2}) =>
        validateProgram(symbols, inputCount: inC, outputCount: outC,
            variableCount: varC, constantCount: constC);

    test('accepts a valid program', () {
      final symbols = <ScriptSymbol>[
        ...compileLine('Var0 ADD In0 Const0')!,
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
        ...compileLine('Out0 EQ Var0 #10')!,
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
        ...compileLine('END')!,
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
      ];
      final v = check(symbols);
      expect(v.ok, isTrue);
    });

    test('requires END', () {
      final symbols = <ScriptSymbol>[
        ...compileLine('Var0 ADD In0 Const0')!,
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
      ];
      expect(check(symbols).ok, isFalse);
    });

    test('catches unbalanced IF/WHILE', () {
      final symbols = <ScriptSymbol>[
        ...compileLine('Var0 IF In0 True')!,
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
        ...compileLine('END')!,
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
      ];
      final v = check(symbols);
      expect(v.ok, isFalse);
      expect(v.errors.join(), contains('Unclosed'));
    });

    test('catches out-of-range operands', () {
      final symbols = <ScriptSymbol>[
        ...compileLine('Var0 ADD In9 Const0')!,
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
        ...compileLine('END')!,
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
      ];
      expect(check(symbols).ok, isFalse);
    });

    test('rejects chained instructions outside conditions', () {
      // A line with an instruction symbol in its operand position.
      final symbols = <ScriptSymbol>[
        const ScriptSymbol(ScriptSymbolType.variable, 0, 0),
        const ScriptSymbol(ScriptSymbolType.instruction, 0, 0), // ADD
        const ScriptSymbol(ScriptSymbolType.input, 0, 0),
        const ScriptSymbol(ScriptSymbolType.instruction, 1, 0), // SUB
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
        ...compileLine('END')!,
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
      ];
      final v = check(symbols);
      expect(v.ok, isFalse);
    });

    test('allows a condition expression inside IF', () {
      final symbols = <ScriptSymbol>[
        ...compileLine('Var0 IF Var1 ADD In0 Const0')!,
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
        ...compileLine('END_IF')!,
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
        ...compileLine('END')!,
        const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
      ];
      final v = check(symbols);
      expect(v.ok, isTrue);
    });
  });

  group('opcode metadata', () {
    test('every opcode has a category, hint and label', () {
      for (final op in ScriptOpcode.values) {
        expect(op.label, isNotEmpty);
        expect(op.hint, isNotEmpty);
        expect(op.category, isNotNull);
      }
    });

    test('recommended ops are a subset', () {
      expect(ScriptOpcode.values.where((op) => op.recommended).length,
          lessThan(ScriptOpcode.values.length));
      expect(ScriptOpcode.add.recommended, isTrue);
    });

    test('categories cover all opcodes exactly once', () {
      final counts = <OpCategory, int>{};
      for (final op in ScriptOpcode.values) {
        counts[op.category] = (counts[op.category] ?? 0) + 1;
      }
      expect(counts.values.reduce((a, b) => a + b), ScriptOpcode.values.length);
    });
  });

  group('input style', () {
    test('automatic is always allowed and resolves per type', () {
      for (final t in DataType.values) {
        if (t == DataType.none || t == DataType.deleted) continue;
        final allowed = InputStyle.allowedFor(t);
        expect(allowed, contains(InputStyle.automatic));
        expect(allowed, contains(InputStyle.forType(t)));
      }
    });

    test('styles are restricted by the expected type', () {
      expect(InputStyle.allowedFor(DataType.bool_),
          containsAll([InputStyle.switch_, InputStyle.button]));
      expect(InputStyle.allowedFor(DataType.number),
          containsAll([InputStyle.slider, InputStyle.text]));
      expect(InputStyle.allowedFor(DataType.bool_),
          isNot(contains(InputStyle.slider)));
      expect(InputStyle.allowedFor(DataType.string),
          isNot(contains(InputStyle.slider)));
    });
  });

  group('script_file', () {
    test('round-trips a file', () {
      final file = ScriptFileData(
        name: 'TestScript',
        inputs: [
          ScriptInput(
              key: 1,
              flagsAndType: 0x03,
              defaultValue: [0x00, 0x00, 0x01, 0x00],
              style: InputStyle.slider),
          ScriptInput(key: 2, flagsAndType: 0x06, defaultValue: [1]),
        ],
        outputNames: ['out_a', 'out_b'],
        variableNames: ['temp'],
        constants: [
          ScriptConstant(flagsAndType: 0x03, value: [0x00, 0x00, 0x10, 0x00]),
        ],
        instructions: [
          ...compileLine('Var0 ADD In0 Const0')!,
          const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
          ...compileLine('END')!,
          const ScriptSymbol(ScriptSymbolType.endLine, 0, 0),
        ],
      );
      final bytes = file.toBytes();
      final parsed = ScriptFileData.parse(bytes)!;
      expect(parsed.name, 'TestScript');
      expect(parsed.inputCount, 2);
      expect(parsed.inputs[0].key, 1);
      expect(parsed.inputs[0].style, InputStyle.slider);
      expect(parsed.inputs[0].defaultValue, [0x00, 0x00, 0x01, 0x00]);
      expect(parsed.inputs[1].defaultValue, [1]);
      expect(parsed.inputs[1].style, InputStyle.automatic);
      expect(parsed.outputNames, ['out_a', 'out_b']);
      expect(parsed.variableNames, ['temp']);
      expect(parsed.constants.single.value, [0x00, 0x00, 0x10, 0x00]);
      final lines = splitLines(parsed.instructions);
      expect(lines.length, 2);
      expect(lines[0].decompile(), 'Var0 ADD In0 Const0');
    });

    test('parses a legacy input meta (no style bytes)', () {
      // Build a file by hand with input_meta = 4 + count*4 (no style byte).
      final file = ScriptFileData(
        name: 'Legacy',
        inputs: [
          ScriptInput(key: 1, flagsAndType: 0x03, defaultValue: [0, 0, 1, 0]),
        ],
      );
      final bytes = file.toBytes();
      // Shrink input_meta_len to the legacy size and drop the style byte.
      final metaLen = 4 + 1 * 4;
      final trimmed = <int>[...bytes];
      trimmed[20] = metaLen & 0xFF;
      trimmed[21] = (metaLen >> 8) & 0xFF;
      // Remove the style byte that follows input 0's key meta (index 32+4+4).
      trimmed.removeAt(32 + 4 + 4);
      final parsed = ScriptFileData.parse(trimmed)!;
      expect(parsed.inputs.single.key, 1);
      expect(parsed.inputs.single.style, InputStyle.automatic);
      expect(parsed.inputs.single.defaultValue, [0, 0, 1, 0]);
    });

    test('rejects truncated input', () {
      expect(ScriptFileData.parse(const []), isNull);
      final bytes = List<int>.filled(32, 0);
      expect(ScriptFileData.parse(bytes), isNotNull); // empty-but-valid header
    });

    test('rejects a short input meta section', () {
      final file = ScriptFileData(
        name: 'x',
        inputs: [
          ScriptInput(key: 1, flagsAndType: 0x03, defaultValue: [1, 0, 0, 0]),
        ],
      );
      final bytes = file.toBytes();
      // Corrupt input_meta_len to 1 (below 4 + count*4).
      bytes[20] = 1;
      bytes[21] = 0;
      expect(ScriptFileData.parse(bytes), isNull);
    });
  });
}