import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/script_draft.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_functions.dart';
import 'package:tamuapp/core/script_instructions.dart';

ScriptSymbol _set() => ScriptSymbol.instruction(catMath, 0);

ScriptLine _halt() =>
    ScriptLine(instruction: ScriptSymbol.instruction(catFlow, 6));

/// A Flow Call to function [index] with the placeholder operand the editor writes.
ScriptLine _call(int index) => ScriptLine(
  instruction: ScriptSymbol.instruction(catFlow, 4),
  operands: [ScriptSymbol.predefine(preIndex, index)],
  functionTarget: index,
);

void main() {
  group('assembly: local functions and macros', () {
    test('a local body is laid out once and the Call points at its line', () {
      final body = [_halt()];
      final functions = [ScriptFunction(name: 'F', body: body)];
      final main = [
        _call(0),
        _call(0), // a second use site reuses the same body
        _halt(),
      ];

      final asm = assembleScriptProgram(mainLines: main, functions: functions);
      expect(asm.errors, isEmpty);
      expect(asm.mainLineCount, 3);
      // main (3 lines) + one body (1 line).
      expect(asm.lines.length, 4);
      // Both Call sites resolve to the body's absolute line index (3).
      expect(asm.lines[0].operands.single.value, 3);
      expect(asm.lines[1].operands.single.value, 3);
      expect(asm.reservations, isEmpty);
    });

    test('a macro is laid out the same way as a local function', () {
      final functions = [
        ScriptFunction(
          name: 'M',
          kind: ScriptFunctionKind.macro,
          body: [_halt()],
        ),
      ];
      final asm = assembleScriptProgram(
        mainLines: [_call(0)],
        functions: functions,
      );
      expect(asm.errors, isEmpty);
      expect(asm.lines[0].operands.single.value, 1); // main is 1 line
      expect(asm.reservations, isEmpty);
    });

    test('only used bodies are laid out (one body per script)', () {
      final functions = [
        ScriptFunction(name: 'Used', body: [_halt()]),
        ScriptFunction(name: 'Unused', body: [_halt(), _halt()]),
      ];
      final asm = assembleScriptProgram(
        mainLines: [_call(0)],
        functions: functions,
      );
      expect(asm.errors, isEmpty);
      expect(asm.lines.length, 1 + 1); // main + the used body only
    });

    test('nested calls resolve inside a laid-out body', () {
      final functions = [
        ScriptFunction(name: 'Outer', body: [_call(1)]), // calls Inner
        ScriptFunction(name: 'Inner', body: [_halt()]),
      ];
      final asm = assembleScriptProgram(
        mainLines: [_call(0)],
        functions: functions,
      );
      expect(asm.errors, isEmpty);
      // main[0] -> Outer start (1, combined absolute); Outer body line -> Inner's offset from
      // Outer's start (1), which the firmware adds to the body's base at load.
      expect(asm.lines[0].operands.single.value, 1);
      expect(asm.lines[1].operands.single.value, 1);
      expect(asm.lines.length, 3);
    });
  });

  group('assembly: global functions', () {
    test('a referenced global function gets a 16-byte reservation', () {
      final functions = [
        ScriptFunction(
          name: 'G',
          kind: ScriptFunctionKind.global,
          globalFileId: 0x12,
          globalMemOffset: 0x40,
          globalInstrOffset: 0x80,
        ),
      ];
      final asm = assembleScriptProgram(
        mainLines: [_call(0)],
        functions: functions,
      );
      expect(asm.errors, isEmpty);
      expect(asm.reservations, hasLength(1));
      expect(asm.reservations.single, hasLength(scriptFunctionReservationSize));
      // FileId u16 | MemOffset u16 | InstrOffset u16 | 10 reserved zero.
      final r = asm.reservations.single;
      expect(r[0] | (r[1] << 8), 0x12);
      expect(r[2] | (r[3] << 8), 0x40);
      expect(r[4] | (r[5] << 8), 0x80);
      expect(r.sublist(6), everyElement(0));
      // A global body is never laid out (linked at load).
      expect(asm.lines.length, 1);
      // The Call operand marks a global call and references reservation 0.
      expect(scriptIsGlobalCall(asm.lines[0].operands.single.value), isTrue);
      expect(scriptGlobalCallIndex(asm.lines[0].operands.single.value), 0);
    });

    test(
      'two referenced globals get one reservation each, in declaration order',
      () {
        final functions = [
          ScriptFunction(
            name: 'A',
            kind: ScriptFunctionKind.global,
            globalFileId: 1,
          ),
          ScriptFunction(
            name: 'B',
            kind: ScriptFunctionKind.global,
            globalFileId: 2,
          ),
        ];
        final asm = assembleScriptProgram(
          mainLines: [_call(1), _call(0)],
          functions: functions,
        );
        expect(asm.reservations, hasLength(2));
        // B -> reservation 1, A -> reservation 0; both flagged as global calls (bit 15).
        expect(asm.lines[0].operands.single.value, scriptCallGlobalBit | 1);
        expect(asm.lines[1].operands.single.value, scriptCallGlobalBit | 0);
      },
    );
  });

  group('assembly: refusals', () {
    test('a cyclic call graph is refused', () {
      final functions = [
        ScriptFunction(name: 'A', body: [_call(1)]),
        ScriptFunction(name: 'B', body: [_call(0)]),
      ];
      final asm = assembleScriptProgram(
        mainLines: [_call(0)],
        functions: functions,
      );
      expect(asm.ok, isFalse);
      expect(asm.errors.any((e) => e.contains('Cyclic')), isTrue);
    });

    test('self-recursion is refused', () {
      final functions = [
        ScriptFunction(name: 'A', body: [_call(0)]),
      ];
      final asm = assembleScriptProgram(
        mainLines: [_call(0)],
        functions: functions,
      );
      expect(asm.ok, isFalse);
      expect(asm.errors.any((e) => e.contains('Cyclic')), isTrue);
    });

    test('nesting deeper than the call-depth cap is refused', () {
      // 9 chained functions: main -> F0 -> ... -> F8 = 9 frames (cap is 8).
      final functions = <ScriptFunction>[
        for (var i = 0; i < 9; i++)
          ScriptFunction(
            name: 'F$i',
            body: [if (i < 8) _call(i + 1) else _halt()],
          ),
      ];
      final asm = assembleScriptProgram(
        mainLines: [_call(0)],
        functions: functions,
      );
      expect(asm.ok, isFalse);
      expect(asm.errors.any((e) => e.contains('nesting')), isTrue);
    });

    test('a chain exactly at the cap assembles', () {
      // 8 chained functions: main -> F0 -> ... -> F7 = 8 frames.
      final functions = <ScriptFunction>[
        for (var i = 0; i < 8; i++)
          ScriptFunction(
            name: 'F$i',
            body: [if (i < 7) _call(i + 1) else _halt()],
          ),
      ];
      final asm = assembleScriptProgram(
        mainLines: [_call(0)],
        functions: functions,
      );
      expect(asm.ok, isTrue, reason: asm.errors.toString());
    });

    test('a Call to an unknown function is refused', () {
      final asm = assembleScriptProgram(mainLines: [_call(5)], functions: []);
      expect(asm.ok, isFalse);
      expect(asm.errors.any((e) => e.contains('unknown function')), isTrue);
    });
  });

  group('file round-trip with functions', () {
    test('local body + Call target survive build -> parse', () {
      final draft = ScriptDraft(functionName: 'Host')
        ..lines.add(_call(0))
        ..lines.add(_halt())
        ..functions.add(
          ScriptFunction(
            name: 'Blink',
            body: [
              ScriptLine(
                destinations: [ScriptSymbol.output(0)],
                instruction: _set(),
                operands: [ScriptSymbol.predefine(preNumber, 128)],
              ),
            ],
          ),
        );

      final parsed = ScriptFileData.parse(draft.toImage());
      expect(parsed.functions, hasLength(1));
      expect(parsed.functions.single.name, 'Blink');
      expect(parsed.functions.single.kind, ScriptFunctionKind.local);
      expect(parsed.functions.single.body, hasLength(1));
      expect(parsed.functionMainLineCount, 2); // main = 2 lines

      final back = ScriptDraft.fromFile(parsed);
      expect(back.functions, hasLength(1));
      expect(back.functions.single.name, 'Blink');
      // The main program's Call target is restored.
      expect(back.lines, hasLength(2));
      expect(back.lines.first.isCall, isTrue);
      expect(back.lines.first.functionTarget, 0);
    });

    test('global function parameters survive build -> parse', () {
      final draft = ScriptDraft()
        ..lines.add(_call(0))
        ..functions.add(
          ScriptFunction(
            name: 'Lib',
            kind: ScriptFunctionKind.global,
            globalFileId: 0x2A,
            globalMemOffset: 0x24,
            globalInstrOffset: 0x60,
          ),
        );

      final parsed = ScriptFileData.parse(draft.toImage());
      expect(parsed.functionCount, 1); // one reservation
      final g = parsed.functions.single;
      expect(g.kind, ScriptFunctionKind.global);
      expect(g.globalFileId, 0x2A);
      expect(g.globalMemOffset, 0x24);
      expect(g.globalInstrOffset, 0x60);

      final back = ScriptDraft.fromFile(parsed);
      expect(back.functions.single.globalFileId, 0x2A);
    });

    test('a script without functions keeps the v2 blob shape', () {
      final draft = ScriptDraft(functionName: 'Plain')..lines.add(_halt());
      final parsed = ScriptFileData.parse(draft.toImage());
      expect(parsed.functions, isEmpty);
      expect(parsed.functionMainLineCount, 0);
      expect(parsed.functionName, 'Plain');
      expect(parsed.functionReservations, isEmpty);
    });
  });

  test('the reservation layout is isolated and matches the firmware', () {
    final r = encodeFunctionReservation(
      fileId: 0x1234,
      memOffset: 7,
      instrOffset: 9,
    );
    expect(r, hasLength(scriptFunctionReservationSize));
    // FileId u16 | MemOffset u16 | InstrOffset u16 | 10 reserved zero.
    expect(r[0] | (r[1] << 8), 0x1234);
    expect(r[2] | (r[3] << 8), 7);
    expect(r[4] | (r[5] << 8), 9);
    expect(r.sublist(6), everyElement(0));
  });

  test('the global-call operand sets bit 15 for the reservation index', () {
    expect(scriptGlobalCallOperandValue(0), 0x8000);
    expect(scriptGlobalCallOperandValue(5), 0x8005);
    expect(scriptIsGlobalCall(0x8005), isTrue);
    expect(scriptIsGlobalCall(300), isFalse);
    expect(scriptGlobalCallIndex(0x8005), 5);
  });
}
