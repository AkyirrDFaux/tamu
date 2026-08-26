/// Script instruction set (Docs/Services/Script.md): the 4-byte symbol encoding,
/// the assembler/disassembler and the editor-side validity checks.
///
/// Symbol: [Type u8][Subtype u8][Value u16 LE]. A program is a stream of symbols
/// grouped into lines, each terminated by an EndLine symbol. A normal line is
/// [Output][Instruction][Operands...] EndLine; flow terminators use degenerate
/// [EndIf|EndWhile|End] EndLine lines. If/While lines embed a math/logic
/// expression (the "M&L processor") that leaves one Bool on the stack.
library;

import 'dart:typed_data';

// --- Symbol types (mirror Core/Functions/Script.h) ---
enum ScriptSymbolType {
  instruction(0),
  input(1),
  output(2),
  variable(3),
  constant(4),
  endLine(5),
  predefine(6);

  final int value;
  const ScriptSymbolType(this.value);

  static ScriptSymbolType fromValue(int value) {
    for (final t in ScriptSymbolType.values) {
      if (t.value == value) return t;
    }
    return ScriptSymbolType.instruction;
  }
}

// --- Instruction subtypes ---
enum ScriptOpcode {
  add(0),
  sub(1),
  mul(2),
  div(3),
  neg(4),
  and_(5),
  or_(6),
  not_(7),
  cmpEq(8),
  cmpNe(9),
  cmpLt(10),
  cmpLe(11),
  cmpGt(12),
  cmpGe(13),
  composeVec(14),
  composeColour(15),
  extract(16),
  memRead(17),
  memWrite(18),
  if_(19),
  while_(20),
  endIf(21),
  endWhile(22),
  end(23),
  delay(24),
  getTime(25),
  pause(26),
  resume(27),
  terminate(28),
  restart(29),
  infoReport(30),
  errorHalt(31),
  macroCall(32);

  final int value;
  const ScriptOpcode(this.value);

  static ScriptOpcode? fromValue(int value) {
    for (final op in ScriptOpcode.values) {
      if (op.value == value) return op;
    }
    return null;
  }

  /// Textual name used by the editor (assembler/disassembler).
  String get label => switch (this) {
        ScriptOpcode.add => 'ADD',
        ScriptOpcode.sub => 'SUB',
        ScriptOpcode.mul => 'MUL',
        ScriptOpcode.div => 'DIV',
        ScriptOpcode.neg => 'NEG',
        ScriptOpcode.and_ => 'AND',
        ScriptOpcode.or_ => 'OR',
        ScriptOpcode.not_ => 'NOT',
        ScriptOpcode.cmpEq => 'EQ',
        ScriptOpcode.cmpNe => 'NE',
        ScriptOpcode.cmpLt => 'LT',
        ScriptOpcode.cmpLe => 'LE',
        ScriptOpcode.cmpGt => 'GT',
        ScriptOpcode.cmpGe => 'GE',
        ScriptOpcode.composeVec => 'COMPOSE_VEC',
        ScriptOpcode.composeColour => 'COMPOSE_COLOR',
        ScriptOpcode.extract => 'EXTRACT',
        ScriptOpcode.memRead => 'MEM_READ',
        ScriptOpcode.memWrite => 'MEM_WRITE',
        ScriptOpcode.if_ => 'IF',
        ScriptOpcode.while_ => 'WHILE',
        ScriptOpcode.endIf => 'END_IF',
        ScriptOpcode.endWhile => 'END_WHILE',
        ScriptOpcode.end => 'END',
        ScriptOpcode.delay => 'DELAY',
        ScriptOpcode.getTime => 'GET_TIME',
        ScriptOpcode.pause => 'PAUSE',
        ScriptOpcode.resume => 'RESUME',
        ScriptOpcode.terminate => 'TERMINATE',
        ScriptOpcode.restart => 'RESTART',
        ScriptOpcode.infoReport => 'INFO_REPORT',
        ScriptOpcode.errorHalt => 'ERROR_HALT',
        ScriptOpcode.macroCall => 'MACRO_CALL',
      };

  static ScriptOpcode? fromLabel(String label) {
    for (final op in ScriptOpcode.values) {
      if (op.label == label.toUpperCase()) return op;
    }
    return null;
  }

  bool get isMathLogic =>
      value >= ScriptOpcode.add.value && value <= ScriptOpcode.cmpGe.value;
}

/// Instruction category used to group the opcode picker.
enum OpCategory {
  math('Math'),
  logic('Logic'),
  compare('Compare'),
  compose('Compose / Extract'),
  memory('Memory'),
  flow('Flow'),
  time('Time'),
  state('State'),
  macro('Macro');

  final String label;
  const OpCategory(this.label);
}

extension ScriptOpcodeInfo on ScriptOpcode {
  OpCategory get category => switch (this) {
        ScriptOpcode.add ||
        ScriptOpcode.sub ||
        ScriptOpcode.mul ||
        ScriptOpcode.div ||
        ScriptOpcode.neg =>
          OpCategory.math,
        ScriptOpcode.and_ ||
        ScriptOpcode.or_ ||
        ScriptOpcode.not_ =>
          OpCategory.logic,
        ScriptOpcode.cmpEq ||
        ScriptOpcode.cmpNe ||
        ScriptOpcode.cmpLt ||
        ScriptOpcode.cmpLe ||
        ScriptOpcode.cmpGt ||
        ScriptOpcode.cmpGe =>
          OpCategory.compare,
        ScriptOpcode.composeVec ||
        ScriptOpcode.composeColour ||
        ScriptOpcode.extract =>
          OpCategory.compose,
        ScriptOpcode.memRead || ScriptOpcode.memWrite => OpCategory.memory,
        ScriptOpcode.if_ ||
        ScriptOpcode.while_ ||
        ScriptOpcode.endIf ||
        ScriptOpcode.endWhile ||
        ScriptOpcode.end =>
          OpCategory.flow,
        ScriptOpcode.delay || ScriptOpcode.getTime => OpCategory.time,
        ScriptOpcode.pause ||
        ScriptOpcode.resume ||
        ScriptOpcode.terminate ||
        ScriptOpcode.restart ||
        ScriptOpcode.infoReport ||
        ScriptOpcode.errorHalt =>
          OpCategory.state,
        ScriptOpcode.macroCall => OpCategory.macro,
      };

  /// One-line hint shown when picking an instruction.
  String get hint => switch (this) {
        ScriptOpcode.add => 'Pop two Numbers, push their sum.',
        ScriptOpcode.sub => 'Pop two Numbers, push a - b.',
        ScriptOpcode.mul => 'Pop two Numbers, push their product.',
        ScriptOpcode.div => 'Pop two Numbers, push a / b (0 on divide-by-zero).',
        ScriptOpcode.neg => 'Pop one Number, push its negation.',
        ScriptOpcode.and_ => 'Pop two values, push True if both are truthy.',
        ScriptOpcode.or_ => 'Pop two values, push True if either is truthy.',
        ScriptOpcode.not_ => 'Pop one value, push its inverse.',
        ScriptOpcode.cmpEq => 'Pop two Numbers, push a == b.',
        ScriptOpcode.cmpNe => 'Pop two Numbers, push a != b.',
        ScriptOpcode.cmpLt => 'Pop two Numbers, push a < b.',
        ScriptOpcode.cmpLe => 'Pop two Numbers, push a <= b.',
        ScriptOpcode.cmpGt => 'Pop two Numbers, push a > b.',
        ScriptOpcode.cmpGe => 'Pop two Numbers, push a >= b.',
        ScriptOpcode.composeVec =>
          'Pop N Numbers (N = op value) and push a Vector.',
        ScriptOpcode.composeColour => 'Pop 4 Numbers and push a Colour (RGBA).',
        ScriptOpcode.extract => 'Pop a Vector/Colour/Matrix + an index, push one element.',
        ScriptOpcode.memRead => 'Pop an address, push the value read from the service.',
        ScriptOpcode.memWrite =>
          'Pop a value + an address, write it; store a Bool success.',
        ScriptOpcode.if_ => 'Evaluate the condition; skip to END_IF when false.',
        ScriptOpcode.while_ => 'Repeat the body while the condition is true.',
        ScriptOpcode.endIf => 'End of an IF body.',
        ScriptOpcode.endWhile => 'Back to the matching WHILE.',
        ScriptOpcode.end => 'Finish the program (state = Finished).',
        ScriptOpcode.delay => 'Pop a Number (ms); wait that long (state = Waiting).',
        ScriptOpcode.getTime => 'Push the uptime in ms (Uint32).',
        ScriptOpcode.pause => 'Pause the script.',
        ScriptOpcode.resume => 'Resume a paused script.',
        ScriptOpcode.terminate => 'Stop and reset to line 0.',
        ScriptOpcode.restart => 'Restart from line 0.',
        ScriptOpcode.infoReport => 'Report the script position to the log.',
        ScriptOpcode.errorHalt => 'Force the script into the Error state.',
        ScriptOpcode.macroCall => 'Run another script as a subroutine (depth limited).',
      };

  /// Commonly used instructions shown first in the picker (recommended, not limiting).
  bool get recommended => switch (this) {
        ScriptOpcode.add ||
        ScriptOpcode.mul ||
        ScriptOpcode.cmpEq ||
        ScriptOpcode.cmpLt ||
        ScriptOpcode.if_ ||
        ScriptOpcode.while_ ||
        ScriptOpcode.delay ||
        ScriptOpcode.getTime ||
        ScriptOpcode.memRead ||
        ScriptOpcode.memWrite ||
        ScriptOpcode.end =>
          true,
        _ => false,
      };
}

// --- Predefine subtypes ---
enum PredefineSubtype {
  state(0),
  type(1),
  index_(2),
  char_(3),
  mathOp(4),
  bool_(5);

  final int value;
  const PredefineSubtype(this.value);
}

/// One 4-byte program symbol.
class ScriptSymbol {
  final ScriptSymbolType type;
  final int subtype; // opcode value for instructions, predefine subtype otherwise
  final int value;

  const ScriptSymbol(this.type, this.subtype, this.value);

  ScriptOpcode? get opcode =>
      type == ScriptSymbolType.instruction ? ScriptOpcode.fromValue(subtype) : null;

  Uint8List toBytes() => Uint8List(4)
    ..[0] = type.value
    ..[1] = subtype & 0xFF
    ..[2] = value & 0xFF
    ..[3] = (value >> 8) & 0xFF;

  static ScriptSymbol fromBytes(List<int> bytes, int offset) => ScriptSymbol(
        ScriptSymbolType.fromValue(bytes[offset]),
        bytes[offset + 1],
        bytes[offset + 2] | (bytes[offset + 3] << 8),
      );

  /// Human-readable operand text for the editor (e.g. "In3", "Var1", "#42").
  String get operandText => switch (type) {
        ScriptSymbolType.input => 'In$value',
        ScriptSymbolType.output => 'Out$value',
        ScriptSymbolType.variable => 'Var$value',
        ScriptSymbolType.constant => 'Const$value',
        ScriptSymbolType.predefine => predefineText,
        ScriptSymbolType.instruction => opcode?.label ?? 'OP$subtype',
        ScriptSymbolType.endLine => 'EndLine',
      };

  String get predefineText => switch (PredefineSubtype.values
      .where((p) => p.value == subtype)
      .firstOrNull) {
        PredefineSubtype.bool_ => value != 0 ? 'True' : 'False',
        PredefineSubtype.char_ => "'${String.fromCharCode(value & 0xFF)}'",
        PredefineSubtype.index_ => '#$value',
        PredefineSubtype.state => 'State:$value',
        PredefineSubtype.type => 'Type:$value',
        PredefineSubtype.mathOp => 'MathOp:$value',
        null => 'Pre$subtype:$value',
      };
}

/// One parsed instruction line.
class ScriptLine {
  final ScriptSymbol? output; // null for degenerate flow terminator lines
  final ScriptOpcode? op; // primary instruction (null for degenerate)
  final List<ScriptSymbol> input; // operand / condition symbols
  final bool degenerate; // [EndIf|EndWhile|End] EndLine

  const ScriptLine({
    this.output,
    this.op,
    this.input = const [],
    this.degenerate = false,
  });

  /// Decompiles the line to editor text (e.g. `Var3 ADD In0 Const1`).
  String decompile() {
    if (degenerate) return op?.label ?? '?';
    final buf = StringBuffer();
    buf.write(output?.operandText ?? '?');
    buf.write(' ${op?.label ?? '?'}');
    for (final s in input) {
      buf.write(' ${s.operandText}');
    }
    return buf.toString();
  }
}

/// Splits a symbol stream into lines (each terminated by an EndLine symbol).
List<ScriptLine> splitLines(List<ScriptSymbol> symbols) {
  final lines = <ScriptLine>[];
  var i = 0;
  while (i < symbols.length) {
    final lineSymbols = <ScriptSymbol>[];
    while (i < symbols.length && symbols[i].type != ScriptSymbolType.endLine) {
      lineSymbols.add(symbols[i]);
      i++;
    }
    if (i < symbols.length) i++; // consume EndLine
    if (lineSymbols.isEmpty) continue;

    // Degenerate flow terminator: first symbol is an instruction.
    final first = lineSymbols.first;
    if (first.type == ScriptSymbolType.instruction) {
      lines.add(ScriptLine(
        degenerate: true,
        op: ScriptOpcode.fromValue(first.subtype),
      ));
      continue;
    }
    if (lineSymbols.length < 2) {
      lines.add(const ScriptLine());
      continue;
    }
    final op = ScriptOpcode.fromValue(lineSymbols[1].subtype);
    lines.add(ScriptLine(
      output: first,
      op: op,
      input: lineSymbols.sublist(2),
    ));
  }
  return lines;
}

/// Compiles one editor line of text into symbols (without the EndLine terminator).
/// Returns null when the line cannot be parsed.
List<ScriptSymbol>? compileLine(String text) {
  final tokens = text
      .trim()
      .split(RegExp(r'\s+'))
      .where((t) => t.isNotEmpty)
      .toList();
  if (tokens.isEmpty) return null;

  final op = ScriptOpcode.fromLabel(tokens.first);
  if (op != null) {
    // Degenerate flow terminator line: END_IF / END_WHILE / END.
    if (op == ScriptOpcode.endIf ||
        op == ScriptOpcode.endWhile ||
        op == ScriptOpcode.end) {
      if (tokens.length != 1) return null;
      return [ScriptSymbol(ScriptSymbolType.instruction, op.value, 0)];
    }
    return null; // an op cannot start a normal line
  }

  final output = _parseOperand(tokens[0]);
  if (output == null ||
      (output.type != ScriptSymbolType.output &&
          output.type != ScriptSymbolType.variable)) {
    return null;
  }
  if (tokens.length < 2) return null;
  final opcode = ScriptOpcode.fromLabel(tokens[1]);
  if (opcode == null) return null;
  final isFlow = opcode == ScriptOpcode.if_ || opcode == ScriptOpcode.while_;
  final result = <ScriptSymbol>[
    output,
    ScriptSymbol(ScriptSymbolType.instruction, opcode.value, 0),
  ];
  for (var i = 2; i < tokens.length; i++) {
    final token = tokens[i];
    // Inside an IF/WHILE condition the embedded "M&L processor" allows math/logic
    // instruction tokens alongside operand symbols.
    if (isFlow) {
      final condOp = ScriptOpcode.fromLabel(token);
      if (condOp != null && condOp.isMathLogic) {
        result.add(ScriptSymbol(ScriptSymbolType.instruction, condOp.value, 0));
        continue;
      }
    }
    final operand = _parseOperand(token);
    if (operand == null) return null;
    result.add(operand);
  }
  return result;
}

/// Parses an operand token into a symbol (In3 / Out1 / Var2 / Const0 / #42 / 'x' /
/// True / False / State:N / Type:N / MathOp:N). Null when invalid.
ScriptSymbol? _parseOperand(String token) {
  final match = RegExp(r'^(In|Out|Var|Const)(\d+)$').firstMatch(token);
  if (match != null) {
    final type = switch (match.group(1)) {
      'In' => ScriptSymbolType.input,
      'Out' => ScriptSymbolType.output,
      'Var' => ScriptSymbolType.variable,
      _ => ScriptSymbolType.constant,
    };
    return ScriptSymbol(type, 0, int.parse(match.group(2)!));
  }
  if (token.startsWith('#')) {
    final v = int.tryParse(token.substring(1));
    if (v == null || v < 0 || v > 0xFFFF) return null;
    return ScriptSymbol(
        ScriptSymbolType.predefine, PredefineSubtype.index_.value, v);
  }
  if (token == 'True' || token == 'true') {
    return ScriptSymbol(
        ScriptSymbolType.predefine, PredefineSubtype.bool_.value, 1);
  }
  if (token == 'False' || token == 'false') {
    return ScriptSymbol(
        ScriptSymbolType.predefine, PredefineSubtype.bool_.value, 0);
  }
  if (token.length == 3 &&
      token.startsWith("'") &&
      token.endsWith("'") &&
      token.codeUnitAt(1) < 0x100) {
    return ScriptSymbol(
        ScriptSymbolType.predefine, PredefineSubtype.char_.value, token.codeUnitAt(1));
  }
  final named = RegExp(r'^(State|Type|MathOp):(\d+)$').firstMatch(token);
  if (named != null) {
    final subtype = switch (named.group(1)) {
      'State' => PredefineSubtype.state.value,
      'Type' => PredefineSubtype.type.value,
      _ => PredefineSubtype.mathOp.value,
    };
    final v = int.parse(named.group(2)!);
    if (v < 0 || v > 0xFFFF) return null;
    return ScriptSymbol(ScriptSymbolType.predefine, subtype, v);
  }
  return null;
}

/// Validation result for a whole program.
class ScriptValidation {
  final List<String> errors;
  final bool ok;
  ScriptValidation(this.errors) : ok = errors.isEmpty;
}

/// Checks the editor validity rules (Docs/Services/Script.md): end matching,
/// operand ranges, output symbol validity, no chained instructions, an End present.
ScriptValidation validateProgram(List<ScriptSymbol> symbols,
    {required int inputCount,
    required int outputCount,
    required int variableCount,
    required int constantCount}) {
  final errors = <String>[];
  final lines = splitLines(symbols);
  if (lines.isEmpty) {
    errors.add('Program is empty');
    return ScriptValidation(errors);
  }
  var flowDepth = 0;
  var hasEnd = false;
  for (var i = 0; i < lines.length; i++) {
    final line = lines[i];
    if (line.degenerate) {
      if (line.op == ScriptOpcode.endIf || line.op == ScriptOpcode.endWhile) {
        if (flowDepth == 0) {
          errors.add('Line ${i + 1}: ${line.op!.label} without matching block');
        } else {
          flowDepth--;
        }
      } else if (line.op == ScriptOpcode.end) {
        hasEnd = true;
      }
      continue;
    }
    if (line.op == null) {
      errors.add('Line ${i + 1}: cannot parse');
      continue;
    }
    final op = line.op!;
    if (op == ScriptOpcode.endIf ||
        op == ScriptOpcode.endWhile ||
        op == ScriptOpcode.end) {
      errors.add('Line ${i + 1}: ${op.label} must stand alone');
      continue;
    }
    if (line.output == null) {
      errors.add('Line ${i + 1}: missing output');
      continue;
    }
    // Output must be an Output or Variable.
    if (line.output!.type != ScriptSymbolType.output &&
        line.output!.type != ScriptSymbolType.variable) {
      errors.add('Line ${i + 1}: output must be Out or Var');
    }
    _checkRange(errors, i, line.output!, inputCount, outputCount, variableCount,
        constantCount);

    final isFlow = line.op == ScriptOpcode.if_ || line.op == ScriptOpcode.while_;
    if (isFlow) flowDepth++;
    for (final s in line.input) {
      if (s.type == ScriptSymbolType.instruction) {
        if (!isFlow || s.opcode == null || !s.opcode!.isMathLogic) {
          errors.add(
              'Line ${i + 1}: instruction symbols are only allowed inside IF/WHILE conditions');
        }
      } else {
        _checkRange(errors, i, s, inputCount, outputCount, variableCount,
            constantCount);
      }
    }
  }
  if (flowDepth != 0) {
    errors.add('Unclosed IF/WHILE block');
  }
  if (!hasEnd) {
    errors.add('Missing END instruction');
  }
  return ScriptValidation(errors);
}

void _checkRange(List<String> errors, int lineNo, ScriptSymbol s, int inputCount,
    int outputCount, int variableCount, int constantCount) {
  final outOfRange = switch (s.type) {
    ScriptSymbolType.input => s.value >= inputCount,
    ScriptSymbolType.output => s.value >= outputCount,
    ScriptSymbolType.variable => s.value >= variableCount,
    ScriptSymbolType.constant => s.value >= constantCount,
    _ => false,
  };
  if (outOfRange) {
    errors.add(
        'Line ${lineNo + 1}: ${s.operandText} out of range (${_countLabel(s.type, inputCount, outputCount, variableCount, constantCount)})');
  }
}

String _countLabel(ScriptSymbolType t, int inputCount, int outputCount,
    int variableCount, int constantCount) => switch (t) {
      ScriptSymbolType.input => 'inputs: $inputCount',
      ScriptSymbolType.output => 'outputs: $outputCount',
      ScriptSymbolType.variable => 'variables: $variableCount',
      ScriptSymbolType.constant => 'constants: $constantCount',
      _ => '',
    };