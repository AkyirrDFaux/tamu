/// Small literal constants become predefine operands (Docs/Services/Script.md symbol kind 6).
///
/// What these tests pin, in the order the card asks for it:
///
/// * the gate - a value becomes a predefine ONLY when the predefine reproduces it bit-exactly,
///   because a running script's arithmetic must not change (`0.1999969482421875` is the real
///   archive value the Q8.8 round trip does not survive);
/// * a BlockInfo (a register reference) and a container stay constants;
/// * the constant table is rewritten together with the program, so the survivors keep their
///   meaning after the renumbering, and a local function body is rewritten too;
/// * the editor sees a literal as a literal after a load and after a save, and a second save
///   changes nothing.
library;

import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup_script.dart';
import 'package:tamuapp/core/script_draft.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_functions.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/script_literals.dart';
import 'package:tamuapp/core/types.dart';

/// The subtype/value of a converted literal, or null when it stays a constant.
(int, int)? _predefine(DataType type, List<int> bytes) {
  final s = scriptLiteralPredefine(type, Uint8List.fromList(bytes)).symbol;
  return s == null ? null : (s.subtype, s.value);
}

/// A Number constant holding [value] (Q16.16 bytes).
List<int> _num(double value) => numberToBytes(value);

/// An integer-typed constant (`Index`/`Uint32`/`Enum`) holding [value] (plain 4-byte value).
List<int> _int(int value) => uint32ToBytes(value);

String _reason(DataType type, List<int> bytes) =>
    scriptLiteralPredefine(type, Uint8List.fromList(bytes)).reason;

ScriptLine _set(List<ScriptSymbol> operands) => ScriptLine(
  destinations: [ScriptSymbol.variable(0)],
  instruction: ScriptSymbol.instruction(catMath, 0), // Set
  operands: operands,
);

ScriptSymbol _c(int index) => ScriptSymbol.constant(index);

void main() {
  group('the exactness gate', () {
    test('an integral Number becomes an Index literal', () {
      expect(_predefine(DataType.number, _num(100.0)), (preIndex, 100));
      expect(_predefine(DataType.number, _num(6.5)), (
        preNumber,
        1664,
      )); // 6.5 * 256
      expect(_predefine(DataType.number, _num(-5.5)), (
        preNumber,
        64128,
      )); // -1408
      expect(_predefine(DataType.number, _num(0.0)), (preIndex, 0));
    });

    test('a fraction only converts when Q8.8 reproduces it exactly', () {
      // The real value in the akyirr/bambi archives that must stay a constant: Q16.16 raw 13107
      // is not a multiple of 256, so a Q8.8 literal would round it.
      expect(_predefine(DataType.number, _num(0.1999969482421875)), isNull);
      expect(
        _reason(DataType.number, _num(0.1999969482421875)),
        contains('does not survive the Q8.8 round trip'),
      );
      // 0.5 and 1/256 are exact; 1/65536 is not.
      expect(_predefine(DataType.number, _num(0.5)), (preNumber, 128));
      expect(_predefine(DataType.number, _num(1 / 256)), (preNumber, 1));
      expect(_predefine(DataType.number, _num(1 / 65536)), isNull);
      // Out of the Q8.8 range (-128..127.99) even though it is a multiple of 1/256.
      expect(_predefine(DataType.number, _num(130.5)), isNull);
    });

    test('an integer above the 16-bit Index literal stays a constant', () {
      // The device sign-extends bit 15 of an Index literal, so 0x8000 and up would arrive
      // negative - the literal cannot carry them.
      expect(_predefine(DataType.number, _num(40000.0)), isNull);
      expect(_predefine(DataType.integer, _int(40000)), isNull);
      expect(_predefine(DataType.number, _num(32767.0)), (preIndex, 32767));
      expect(_predefine(DataType.number, _num(32768.0)), isNull);
    });

    test('integer, enum, Bool and Char constants carry over', () {
      expect(_predefine(DataType.integer, _int(200)), (preIndex, 200));
      expect(_predefine(DataType.uint32, _int(5)), (preIndex, 5));
      expect(_predefine(DataType.enum_, _int(8)), (preIndex, 8));
      // A negative integer has no Index form; its Q8.8 Number form is exact.
      expect(_predefine(DataType.integer, _int(-3)), (preNumber, 64768));
      expect(
        scriptLiteralPredefine(
          DataType.bool_,
          Uint8List.fromList([1]),
        ).symbol?.subtype,
        preBool,
      );
      expect(
        scriptLiteralPredefine(
          DataType.bool_,
          Uint8List.fromList([1]),
        ).symbol?.value,
        1,
      );
      expect(
        scriptLiteralPredefine(
          DataType.char,
          Uint8List.fromList([65]),
        ).symbol?.subtype,
        preChar,
      );
      // A Bool byte the device would read as "true" is not the value 2.
      expect(_reason(DataType.bool_, [2]), contains('neither false nor true'));
    });

    test('a BlockInfo, a container and a byte string stay constants', () {
      final blockInfo = Uint8List.fromList([0x00, 0x01, 0x02, 0xFC]);
      expect(
        _reason(DataType.blockInfo, blockInfo),
        contains('register reference (BlockInfo)'),
      );
      expect(_reason(DataType.vector, List<int>.filled(8, 0)), isNotEmpty);
      expect(_reason(DataType.colour, List<int>.filled(4, 0)), isNotEmpty);
      expect(_reason(DataType.text, [65, 0]), isNotEmpty);
      expect(_reason(DataType.id, [3]), isNotEmpty);
    });
  });

  group('the constant table and the program move together', () {
    /// Four constants: a BlockInfo, a literal used twice, a literal no Q8.8 carries, and an
    /// unreferenced literal.
    ScriptDraft draft() {
      final d = ScriptDraft();
      d.variables.add(ScriptDraftValue(name: 'V', type: DataType.number));
      d.constants.addAll([
        ScriptDraftValue(
          name: 'REG',
          type: DataType.blockInfo,
          value: Uint8List.fromList([0x00, 0x01, 0x02, 0xFC]),
        ),
        ScriptDraftValue(
          name: 'ONE',
          type: DataType.number,
          value: numberToBytes(1.0),
        ),
        ScriptDraftValue(
          name: 'EPS',
          type: DataType.number,
          value: numberToBytes(0.1999969482421875),
        ),
        ScriptDraftValue(
          name: 'UNUSED',
          type: DataType.number,
          value: numberToBytes(2.0),
        ),
      ]);
      // The unreferenced literal is referenced by nothing; the local body reads ONE and EPS, so
      // a body rewrite is covered too.
      d.lines.add(_set([_c(1), ScriptSymbol.predefine(preMathOp, 0), _c(2)]));
      d.functions.add(
        ScriptFunction(
          name: 'Body',
          body: [
            _set([ScriptSymbol.variable(0), _c(1), _c(2)]),
          ],
        ),
      );
      return d;
    }

    test('converted literals drop out and the survivors keep their order', () {
      final d = draft();
      final result = convertScriptLiterals(
        constants: d.constants,
        lines: d.lines,
        functions: d.functions,
      );
      expect(
        [for (final c in result.constants) c.name],
        ['REG', 'EPS', 'UNUSED'],
      );
      expect([for (final u in result.converted) u.name], ['ONE']);
      // ONE is referenced twice (once from the program, once from the body).
      expect(result.converted.single.operands, 2);
      expect(
        result.uses.firstWhere((u) => u.name == 'UNUSED').reason,
        contains('no operand references it'),
      );
    });

    test(
      'every reference follows the renumbering, in the program and in a body',
      () {
        final d = draft();
        final result = convertScriptLiterals(
          constants: d.constants,
          lines: d.lines,
          functions: d.functions,
        );
        // EPS was constant 2 and is now 1; ONE became an Index literal in place.
        final program = result.lines.single.operands;
        expect(
          [for (final o in program) (o.type, o.subtype, o.value)],
          [
            (symPredefine, preIndex, 1),
            (symPredefine, preMathOp, 0),
            (symConstant, 0, 1),
          ],
        );
        final body = result.functions.single.body.single.operands;
        expect(
          [for (final o in body) (o.type, o.subtype, o.value)],
          [
            (symVariable, 0, 0),
            (symPredefine, preIndex, 1),
            (symConstant, 0, 1),
          ],
        );
        // UNUSED stays in the table behind EPS, so a later reference would be index 2.
        expect(
          [for (final c in result.constants) c.name],
          ['REG', 'EPS', 'UNUSED'],
        );
      },
    );

    test(
      'the instruction stream keeps its length, so flow targets do not move',
      () {
        final d = draft();
        final before = encodeScriptLines(d.assemble().lines);
        final result = convertScriptLiterals(
          constants: d.constants,
          lines: d.lines,
          functions: d.functions,
        );
        final after = encodeScriptLines(
          assembleScriptProgram(
            mainLines: result.lines,
            functions: result.functions,
          ).lines,
        );
        expect(after.length, before.length);
      },
    );

    test('a Flow Jump constant is a line target, not a literal', () {
      final d = ScriptDraft();
      d.constants.add(
        ScriptDraftValue(
          name: 'TARGET',
          type: DataType.integer,
          value: uint32ToBytes(3),
        ),
      );
      d.lines.add(
        ScriptLine(
          instruction: ScriptSymbol.instruction(catFlow, 3), // Jump
          operands: [_c(0)],
        ),
      );
      final result = convertScriptLiterals(
        constants: d.constants,
        lines: d.lines,
      );
      expect(result.constants.single.name, 'TARGET');
      expect(result.lines.single.operands.single.type, symConstant);
      expect(result.uses.single.reason, contains('names it'));
    });

    test(
      'a constant in a destination stays, and still follows the renumbering',
      () {
        // A malformed line the device would refuse (a destination must be writable): it is not
        // turned into a predefine, and the destination symbol still points at its constant.
        final d = ScriptDraft();
        d.constants.addAll([
          ScriptDraftValue(
            name: 'LIT',
            type: DataType.number,
            value: numberToBytes(1.0),
          ),
          ScriptDraftValue(
            name: 'BAD',
            type: DataType.number,
            value: numberToBytes(2.0),
          ),
        ]);
        d.lines.add(
          ScriptLine(
            destinations: [_c(1)],
            instruction: ScriptSymbol.instruction(catMath, 0),
            operands: [_c(0)],
          ),
        );
        final result = convertScriptLiterals(
          constants: d.constants,
          lines: d.lines,
        );
        expect([for (final c in result.constants) c.name], ['BAD']);
        final destination = result.lines.single.destinations.single;
        expect((destination.type, destination.value), (symConstant, 0));
        expect(result.lines.single.operands.single.subtype, preIndex);
      },
    );

    test('a second pass changes nothing', () {
      final d = draft();
      final once = convertScriptLiterals(
        constants: d.constants,
        lines: d.lines,
        functions: d.functions,
      );
      final twice = convertScriptLiterals(
        constants: once.constants,
        lines: once.lines,
        functions: once.functions,
      );
      expect(twice.converted, isEmpty);
      expect(
        [for (final c in twice.constants) c.name],
        ['REG', 'EPS', 'UNUSED'],
      );
    });
  });

  group('the image the encoder writes', () {
    /// A script whose only literal constant is referenced, plus one that must stay.
    ScriptDraft draft() {
      final d = ScriptDraft();
      d.variables.add(ScriptDraftValue(name: 'V', type: DataType.number));
      d.constants.addAll([
        ScriptDraftValue(
          name: 'HALF',
          type: DataType.number,
          value: numberToBytes(0.5),
        ),
        ScriptDraftValue(
          name: 'EPS',
          type: DataType.number,
          value: numberToBytes(0.1999969482421875),
        ),
      ]);
      d.lines.add(_set([_c(0), ScriptSymbol.predefine(preMathOp, 0), _c(1)]));
      return d;
    }

    /// The operand symbols of the first line of an image.
    List<ScriptSymbol> operandsOf(List<int> image) => decodeScriptLines(
      ScriptFileData.parse(image).instructions,
    ).single.operands;

    test(
      'a referenced literal is emitted as a predefine, not a constant slot',
      () {
        final d = draft();
        final image = d.toImage();
        final parsed = ScriptFileData.parse(image);
        expect(parsed.constants.length, 1); // HALF is gone, EPS stays
        expect(parsed.constantNames, ['EPS']);
        final operands = operandsOf(image);
        expect(
          (operands[0].type, operands[0].subtype, operands[0].value),
          (
            symPredefine,
            preNumber,
            128, // 0.5 in Q8.8
          ),
        );
        expect((operands[2].type, operands[2].value), (symConstant, 0));
      },
    );

    test('a load shows the literal, and load -> save -> load is stable', () {
      final first = draft().toImage();
      final loaded = ScriptDraft.fromFile(ScriptFileData.parse(first));
      expect([for (final c in loaded.constants) c.name], ['EPS']);
      expect(
        (
          loaded.lines.single.operands[0].type,
          loaded.lines.single.operands[0].subtype,
        ),
        (symPredefine, preNumber),
      );
      // Saving what was loaded reproduces the same image, and loading it again is a no-op.
      final second = loaded.toImage();
      expect(second, first);
      final again = ScriptDraft.fromFile(ScriptFileData.parse(second));
      expect(again.toImage(), first);
      expect([for (final c in again.constants) c.name], ['EPS']);
    });

    test('an archive semantic script normalises on the way to its draft', () {
      // An old archive: both values still stored as constants.
      final backup = BackupScript(
        slot: 1,
        functionName: 'Old',
        properties: const [],
        inputs: const [],
        outputs: const [],
        variables: const [
          BackupScriptValue(name: 'V', type: 'Number', size: 4),
        ],
        constants: const [
          BackupScriptValue(name: 'HALF', type: 'Number', size: 4, value: 0.5),
          BackupScriptValue(
            name: 'EPS',
            type: 'Number',
            size: 4,
            value: 0.1999969482421875,
          ),
        ],
        lines: [
          const BackupLine(
            destinations: [BackupSymbol(kind: 'variable', value: 0)],
            instruction: BackupSymbol(
              kind: 'instruction',
              category: 'Math',
              value: 0,
            ),
            operands: [
              BackupSymbol(kind: 'constant', value: 0),
              BackupSymbol(kind: 'predefine', subtype: 'Math op', value: 0),
              BackupSymbol(kind: 'constant', value: 1),
            ],
          ),
        ],
      );

      final restored = backup.toDraft();
      expect([for (final c in restored.constants) c.name], ['EPS']);
      // Re-serialising the draft carries the reduced table too - the archive is what the setup
      // re-save writes, not just the image.
      final resaved = BackupScript.fromDraft(1, restored);
      expect([for (final c in resaved.constants) c.name], ['EPS']);
      expect(resaved.lines.single.operands.first.kind, 'predefine');
      expect(resaved.lines.single.operands.first.subtype, 'Number');
      expect(resaved.lines.single.operands.first.value, 128);
      // And the image it rebuilds decodes back to the same thing.
      final image = restored.toImage();
      expect(ScriptFileData.parse(image).constants.length, 1);
    });
  });
}
