/// Decoding a script fault report into a sentence (Docs/Services/Script.md "Error Model").
///
/// CID 3's report record names a fault as a pair: the offending instruction's own encoding,
/// `1 + ((category << 8) | opcode)` (0 = engine-level: no instruction behind the fault), and the
/// instruction-specific error code. Neither half is readable alone - `error 2 at cat 4 op 1` is
/// `SCRIPT_ERR_TYPE` on a `Service/Register read` - and the record carries no operand, so the
/// register the instruction addressed has to be read back from the program the app already has
/// (the stored `SCR_XXX` file, or the editor's draft).
library;

import 'block_registry.dart' show blockInfoFor;
import 'script_client.dart' show ScriptReport;
import 'script_draft.dart';
import 'script_errors.dart';
import 'script_file.dart' show ScriptField, ScriptFileData;
import 'script_instructions.dart';
import 'types.dart';

/// One decoded fault: where it happened, which instruction, which register, which error.
class ScriptFault {
  /// The instruction counter (line index, 0-based) the report recorded. Meaningless when
  /// [engineLevel]: an engine-level fault is not any instruction's doing.
  final int line;

  /// The raw instruction encoding from the record, kept for diagnostics.
  final int instruction;

  /// The raw error code from the record, kept for diagnostics.
  final int error;

  /// The instruction's category/opcode, decoded from [instruction] (null when engine-level).
  final int? category;
  final int? opcode;

  /// The instruction's own name from the app's instruction table (null when the encoding names
  /// no instruction the app knows, e.g. a reserved opcode).
  final String? instructionName;

  /// The register the instruction addressed (`Script[2].f1.k0 (Input)`), when the program the
  /// app has carries a constant BlockInfo operand for this line. Null otherwise - a foreign
  /// register access addresses a device too, and an absent/stale program cannot be guessed at.
  final String? register;

  const ScriptFault({
    required this.line,
    required this.instruction,
    required this.error,
    this.category,
    this.opcode,
    this.instructionName,
    this.register,
  });

  /// True for an instruction encoding of 0: the fault came from the engine, not an instruction.
  bool get engineLevel => instruction == 0;

  /// The error's name from the app's registry (`SCRIPT_ERR_TYPE`).
  String get errorName => ScriptErrorCode.name(error);

  /// The instruction as a person reads it: its label, or its category and opcode when the app
  /// has no entry for that encoding.
  String get instructionLabel {
    final name = instructionName;
    if (name != null) return name;
    final cat = category;
    final op = opcode;
    if (cat == null || op == null) return 'unknown instruction';
    final catName = ScriptInstructionDef.categoryName(cat);
    return catName == '?'
        ? 'unknown instruction (cat $cat op $op)'
        : '$catName op $op';
  }

  /// The sentence the app shows for this fault.
  ///
  /// An engine-level fault names no line: the record's line is whatever the counter happened to
  /// hold, not a position the fault came from.
  String get text {
    if (engineLevel) {
      return 'Script fault (engine-level): $errorName - no instruction behind it';
    }
    final where = 'Script fault at line $line ($instructionLabel)';
    final reg = register;
    return reg == null ? '$where: $errorName' : '$where: $errorName on $reg';
  }

  /// Decodes a CID 3 report record.
  ///
  /// [program] is the instruction stream the app knows for that slot (assembled, so line indexes
  /// match the running program) and [constantOf] resolves a constant symbol's index to its 32-bit
  /// BlockInfo value, or null when that constant is not a BlockInfo. Both are optional: without
  /// them the fault still decodes its instruction and error, it just cannot name the register.
  factory ScriptFault.fromReport(
    ScriptReport report, {
    List<ScriptLine> program = const [],
    int? Function(int constantIndex)? constantOf,
  }) {
    final decoded = ScriptReport.decodeInstruction(report.instruction);
    final def = decoded == null
        ? null
        : scriptInstructionFor(
            ScriptSymbol.instruction(decoded.$1, decoded.$2),
          );
    return ScriptFault(
      line: report.line,
      instruction: report.instruction,
      error: report.error,
      category: decoded?.$1,
      opcode: decoded?.$2,
      instructionName: def?.label,
      register: _registerAt(report, decoded, def, program, constantOf),
    );
  }

  /// The register label of the line the fault names, or null.
  ///
  /// The line is only trusted when it carries the fault's own instruction: a program assembled
  /// with function bodies, or a file edited since it was loaded, can put a different instruction
  /// at that index - naming *its* register would be a guess dressed as a fact.
  static String? _registerAt(
    ScriptReport report,
    (int, int)? decoded,
    ScriptInstructionDef? def,
    List<ScriptLine> program,
    int? Function(int constantIndex)? constantOf,
  ) {
    if (decoded == null || def == null || constantOf == null) return null;
    if (def.constantIndex < 0) return null;
    if (report.line < 0 || report.line >= program.length) return null;
    final line = program[report.line];
    if (!line.instruction.isInstruction ||
        line.instruction.subtype != decoded.$1 ||
        line.instruction.value != decoded.$2) {
      return null;
    }
    if (line.operands.length <= def.constantIndex) return null;
    final operand = line.operands[def.constantIndex];
    if (operand.type != symConstant) return null;
    final blockInfo = constantOf(operand.value);
    return blockInfo == null ? null : scriptRegisterLabel(blockInfo);
  }
}

/// The fault of [report] decoded against an editable draft.
///
/// Null when the record carries no error: a healthy report is not a fault.
/// The assembled program is used when the draft assembles cleanly (its line indexes are the
/// running program's); otherwise the main program alone, which the line check above keeps honest.
ScriptFault? draftFault(ScriptReport? report, ScriptDraft draft) {
  if (report == null || !report.hasError) return null;
  final assembly = draft.assemble();
  return ScriptFault.fromReport(
    report,
    program: assembly.ok ? assembly.lines : draft.lines,
    constantOf: (index) {
      if (index < 0 || index >= draft.constants.length) return null;
      final constant = draft.constants[index];
      if (constant.type != DataType.blockInfo || constant.value.length < 4) {
        return null;
      }
      return uint32FromBytes(constant.value);
    },
  );
}

/// The fault of [report] decoded against a stored script file.
///
/// Null when the record carries no error: a healthy report is not a fault.
ScriptFault? fileFault(ScriptReport? report, ScriptFileData file) {
  if (report == null || !report.hasError) return null;
  return ScriptFault.fromReport(
    report,
    program: decodeScriptLines(file.instructions),
    constantOf: (index) {
      if (index < 0 || index >= file.constants.length) return null;
      if (file.constants[index].dataType != DataType.blockInfo) return null;
      final bytes = file.constantValue(index);
      return bytes.length < 4 ? null : uint32FromBytes(bytes);
    },
  );
}

/// A register address as `Script[2].f1.k0 (Input)`: block, instance, field, key, and the field's
/// name when the app's registry knows it.
///
/// A banked block (the Scripts range 0x3F4-0x3F7, the Dynamic range 0x3F0-0x3F3) is addressed by
/// its global index, not its per-bank instance: `Script[2]` alone would name four different slots.
String scriptRegisterLabel(int blockInfo) {
  final type = blockInfoType(blockInfo);
  final instance = blockInfoInstance(blockInfo);
  final field = blockInfoField(blockInfo);
  final key = blockInfoKey(blockInfo);
  final block = isScriptType(type) || isDynamicType(type)
      ? '${blockTypeLabel(type)}[${globalIndexFor(type, instance)}]'
      : '${blockTypeLabel(type)}[$instance]';
  final fieldName = scriptRegisterFieldName(type, field);
  return '$block.f$field.k$key${fieldName == null ? '' : ' ($fieldName)'}';
}

/// The app's name for a register field: a script's I/O category, or a static block's field name
/// from the block registry. Null when neither knows it (a dynamic block, or a field past the
/// table).
String? scriptRegisterFieldName(int type, int field) {
  if (isScriptType(type)) {
    return switch (field) {
      ScriptField.header => 'Header',
      ScriptField.input => 'Input',
      ScriptField.output => 'Output',
      _ => null,
    };
  }
  return blockInfoFor(BlockType.fromValue(type))?.field(field)?.name;
}
