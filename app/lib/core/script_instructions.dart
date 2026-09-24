/// Script instruction model (Docs/Services/Script.md).
///
/// The program is a stream of 4-byte symbols `Type(u8) | Subtype(u8) | Value(u16)`. A
/// line follows the documented Output-Instruction-Input-End shape: any number of
/// destination symbols, then the instruction symbol, then any number of source symbols,
/// terminated by an Endline symbol. The opcode set is an internal convention shared with
/// the (future) firmware VM.
library;

import 'dart:typed_data';

import 'types.dart';

// Symbol types.
const int symInstruction = 0;
const int symInput = 1;
const int symOutput = 2;
const int symVariable = 3;
const int symConstant = 4;
const int symEndline = 5;
const int symPredefine = 6;

// Predefine subtypes.
const int preState = 0;
const int preType = 1;
const int preIndex = 2;
const int preChar = 3;
const int preMathOp = 4;
const int preBool = 5;
const int preNumber = 6; // 16-bit Q8.8 fixed-point literal

/// Instruction categories (the symbol Subtype for instruction symbols).
const int catMath = 0;
const int catLogic = 1;
const int catFlow = 2;
const int catTime = 3;
const int catService = 4;
const int catCompose = 5;

class ScriptSymbol {
  final int type;
  final int subtype;
  final int value;

  const ScriptSymbol(this.type, this.subtype, this.value);

  static const ScriptSymbol endline = ScriptSymbol(symEndline, 0, 0);

  static ScriptSymbol input(int index) => ScriptSymbol(symInput, 0, index);
  static ScriptSymbol output(int index) => ScriptSymbol(symOutput, 0, index);
  static ScriptSymbol variable(int index) => ScriptSymbol(symVariable, 0, index);
  static ScriptSymbol constant(int index) => ScriptSymbol(symConstant, 0, index);
  static ScriptSymbol predefine(int subtype, int value) =>
      ScriptSymbol(symPredefine, subtype, value);
  static ScriptSymbol instruction(int category, int op) =>
      ScriptSymbol(symInstruction, category, op);

  bool get isInstruction => type == symInstruction;
  bool get isEndline => type == symEndline;

  Uint8List toBytes() => Uint8List(4)
    ..[0] = type & 0xFF
    ..[1] = subtype & 0xFF
    ..[2] = value & 0xFF
    ..[3] = (value >> 8) & 0xFF;

  static ScriptSymbol fromBytes(List<int> bytes, [int offset = 0]) => ScriptSymbol(
        bytes[offset], bytes[offset + 1], bytes[offset + 2] | (bytes[offset + 3] << 8));

  static String predefineName(int subtype) => switch (subtype) {
        preState => 'State',
        preType => 'Type',
        preIndex => 'Index',
        preChar => 'Char',
        preMathOp => 'Math op',
        preBool => 'Bool',
        preNumber => 'Number',
        _ => 'Predefine',
      };
}

/// One instruction definition: opcode, category, label and the operand shape used by the
/// editor's context recommendations and the validity check.
class ScriptInstructionDef {
  final int op;
  final int category;
  final String label;
  final bool destination;
  final int minOperands;
  final int maxOperands;
  final bool numeric;

  /// Operand index that must be a Constant (a 4-byte BlockInfo), or -1.
  final int constantIndex;

  /// Operand index that is a device address (Id), or -1.
  final int addressIndex;

  /// True when the operands form an infix expression (`Set`): values interleaved with
  /// `Math op` predefines and parentheses.
  final bool expression;

  const ScriptInstructionDef({
    required this.op,
    required this.category,
    required this.label,
    this.destination = false,
    this.minOperands = 0,
    this.maxOperands = 0,
    this.numeric = false,
    this.constantIndex = -1,
    this.addressIndex = -1,
    this.expression = false,
  });

  ScriptSymbol symbol() => ScriptSymbol.instruction(category, op);

  /// Maximum number of destination symbols for this instruction (single-destination set).
  int get maxDestinations => destination ? 1 : 0;

  static String categoryName(int category) => switch (category) {
        catMath => 'Math',
        catLogic => 'Logic',
        catFlow => 'Flow',
        catTime => 'Time',
        catService => 'Service',
        catCompose => 'Compose',
        _ => '?',
      };
}

/// Predefine "Math op." values (Docs/Services/Script.md predefine subtypes). Values 0..17
/// are the operator enum; 18/19 are the expression parentheses (inline in a `Set` line).
/// Shifts (10/11) and Modulo (4) are not offered (no shift support; Modulo is an instruction).
const List<(int, String)> scriptPredefineMathOps = [
  (0, 'Add'),
  (1, 'Subtract'),
  (2, 'Multiply'),
  (3, 'Divide'),
  (4, 'Modulo'),
  (5, 'Power'),
  (6, 'AND'),
  (7, 'OR'),
  (8, 'XOR'),
  (9, 'NOT'),
  (12, 'Compare ='),
  (13, 'Compare !='),
  (14, 'Compare <'),
  (15, 'Compare <='),
  (16, 'Compare >'),
  (17, 'Compare >='),
  (18, 'Open parenthesis'),
  (19, 'Close parenthesis'),
];

/// Expression parenthesis operator values (inline in a `Set` operand stream).
const int mathOpOpenParen = 18;
const int mathOpCloseParen = 19;

/// Inline operators the editor offers for an expression (`Set` / `If` / `While` / `Wait until`).
const Set<int> expressionOps = {
  0, 1, 2, 3, 4, 5, // + - * / % ^
  6, 7, 8, 9, // AND OR XOR NOT
  12, 13, 14, 15, 16, 17, // == != < <= > >=
  mathOpOpenParen, mathOpCloseParen,
};

/// Predefine subtypes offered in the picker (name + subtype value).
const List<(int, String)> scriptPredefineSubtypes = [
  (preState, 'State'),
  (preType, 'Type'),
  (preIndex, 'Index'),
  (preChar, 'Char'),
  (preMathOp, 'Math op'),
  (preBool, 'Bool'),
  (preNumber, 'Number'),
];

const List<ScriptInstructionDef> scriptInstructions = [
  // Math: Set evaluates an infix expression (scalar/vector/matrix, element-wise with
  // scalar broadcast). Modulo/Minimum/Maximum/Absolute/Limit remain separate instructions.
  ScriptInstructionDef(op: 0, category: catMath, label: 'Set', destination: true, minOperands: 1, maxOperands: 32, numeric: true, expression: true),
  ScriptInstructionDef(op: 5, category: catMath, label: 'Modulo', destination: true, minOperands: 2, maxOperands: 8, numeric: true),
  ScriptInstructionDef(op: 6, category: catMath, label: 'Minimum', destination: true, minOperands: 2, maxOperands: 8, numeric: true),
  ScriptInstructionDef(op: 7, category: catMath, label: 'Maximum', destination: true, minOperands: 2, maxOperands: 8, numeric: true),
  ScriptInstructionDef(op: 9, category: catMath, label: 'Absolute', destination: true, minOperands: 1, maxOperands: 1, numeric: true),
  ScriptInstructionDef(op: 10, category: catMath, label: 'Limit', destination: true, minOperands: 3, maxOperands: 3, numeric: true),
  // Logic: only Select remains (comparisons/logic moved into the expression).
  ScriptInstructionDef(op: 12, category: catLogic, label: 'Select', destination: true, minOperands: 3, maxOperands: 3),
  // Flow (If/While take a boolean expression)
  ScriptInstructionDef(op: 0, category: catFlow, label: 'If', minOperands: 1, maxOperands: 32, numeric: true, expression: true),
  ScriptInstructionDef(op: 1, category: catFlow, label: 'While', minOperands: 1, maxOperands: 32, numeric: true, expression: true),
  ScriptInstructionDef(op: 2, category: catFlow, label: 'End block', minOperands: 0, maxOperands: 0),
  ScriptInstructionDef(op: 3, category: catFlow, label: 'Jump', minOperands: 1, maxOperands: 1),
  ScriptInstructionDef(op: 4, category: catFlow, label: 'Call', minOperands: 1, maxOperands: 1),
  ScriptInstructionDef(op: 5, category: catFlow, label: 'Return', minOperands: 0, maxOperands: 0),
  ScriptInstructionDef(op: 6, category: catFlow, label: 'Halt', minOperands: 0, maxOperands: 0),
  // Time
  ScriptInstructionDef(op: 0, category: catTime, label: 'Delay', minOperands: 1, maxOperands: 1),
  ScriptInstructionDef(op: 1, category: catTime, label: 'Wait until', minOperands: 1, maxOperands: 32, numeric: true, expression: true),
  ScriptInstructionDef(op: 2, category: catTime, label: 'Get time', destination: true, minOperands: 0, maxOperands: 0),
  // Service
  ScriptInstructionDef(op: 0, category: catService, label: 'Log', minOperands: 1, maxOperands: 4),
  ScriptInstructionDef(op: 1, category: catService, label: 'Register read', destination: true, minOperands: 1, maxOperands: 1, constantIndex: 0),
  ScriptInstructionDef(op: 2, category: catService, label: 'Register write', minOperands: 2, maxOperands: 2, constantIndex: 0),
  ScriptInstructionDef(op: 3, category: catService, label: 'Script state', minOperands: 1, maxOperands: 1),
  ScriptInstructionDef(op: 4, category: catService, label: 'Nop', minOperands: 0, maxOperands: 0),
  ScriptInstructionDef(op: 5, category: catService, label: 'Register read (foreign)', destination: true, minOperands: 2, maxOperands: 2, constantIndex: 1, addressIndex: 0),
  ScriptInstructionDef(op: 6, category: catService, label: 'Register write (foreign)', minOperands: 3, maxOperands: 3, constantIndex: 1, addressIndex: 0),
  // Compose
  ScriptInstructionDef(op: 0, category: catCompose, label: 'Compose', destination: true, minOperands: 2, maxOperands: 4),
  ScriptInstructionDef(op: 1, category: catCompose, label: 'Extract', destination: true, minOperands: 2, maxOperands: 2),
];

ScriptInstructionDef? scriptInstructionFor(ScriptSymbol symbol) {
  if (!symbol.isInstruction) return null;
  for (final def in scriptInstructions) {
    if (def.category == symbol.subtype && def.op == symbol.value) return def;
  }
  return null;
}

/// A single instruction line.
class ScriptLine {
  final List<ScriptSymbol> destinations;
  ScriptSymbol instruction;
  final List<ScriptSymbol> operands;

  ScriptLine({
    List<ScriptSymbol>? destinations,
    required this.instruction,
    List<ScriptSymbol>? operands,
  })  : destinations = destinations ?? [],
        operands = operands ?? [];

  ScriptInstructionDef? get def => scriptInstructionFor(instruction);
}

/// Encodes lines to the symbol stream (each line ends with an Endline symbol).
Uint8List encodeScriptLines(List<ScriptLine> lines) {
  final out = <int>[];
  for (final line in lines) {
    for (final d in line.destinations) {
      out.addAll(d.toBytes());
    }
    out.addAll(line.instruction.toBytes());
    for (final o in line.operands) {
      out.addAll(o.toBytes());
    }
    out.addAll(ScriptSymbol.endline.toBytes());
  }
  return Uint8List.fromList(out);
}

/// Decodes the symbol stream into lines. Unknown trailing bytes are ignored.
List<ScriptLine> decodeScriptLines(List<int> bytes) {
  final lines = <ScriptLine>[];
  final symbols = <ScriptSymbol>[];
  for (var o = 0; o + 4 <= bytes.length; o += 4) {
    symbols.add(ScriptSymbol.fromBytes(bytes, o));
  }

  var i = 0;
  while (i < symbols.length) {
    final destinations = <ScriptSymbol>[];
    ScriptSymbol? instruction;
    final operands = <ScriptSymbol>[];
    while (i < symbols.length && !symbols[i].isInstruction && !symbols[i].isEndline) {
      destinations.add(symbols[i++]);
    }
    if (i < symbols.length && symbols[i].isInstruction) {
      instruction = symbols[i++];
      while (i < symbols.length && !symbols[i].isEndline) {
        operands.add(symbols[i++]);
      }
    }
    if (i < symbols.length && symbols[i].isEndline) i++; // consume Endline
    if (instruction != null) {
      lines.add(ScriptLine(destinations: destinations, instruction: instruction, operands: operands));
    } else if (destinations.isNotEmpty) {
      // Orphan symbols without an instruction: keep them editable as a Nop line.
      lines.add(ScriptLine(
        destinations: destinations,
        instruction: ScriptSymbol.instruction(catService, 4),
        operands: operands,
      ));
    }
  }
  return lines;
}

/// Type information used by the validity check.
class ScriptValidationContext {
  final List<int> inputTypes;
  final List<int> outputTypes;
  final List<int> variableTypes;
  final List<int> constantTypes;

  const ScriptValidationContext({
    this.inputTypes = const [],
    this.outputTypes = const [],
    this.variableTypes = const [],
    this.constantTypes = const [],
  });

  /// The declared data type of a symbol, or null when it is a predefine/unknown.
  int? typeOf(ScriptSymbol s) {
    switch (s.type) {
      case symInput:
        return s.value < inputTypes.length ? inputTypes[s.value] : null;
      case symOutput:
        return s.value < outputTypes.length ? outputTypes[s.value] : null;
      case symVariable:
        return s.value < variableTypes.length ? variableTypes[s.value] : null;
      case symConstant:
        return s.value < constantTypes.length ? constantTypes[s.value] : null;
      default:
        return null;
    }
  }
}

/// Numeric data types (the fixed-point number and the integer-like types).
bool scriptTypeIsNumeric(int type) => const {0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0C, 0x0E, 0x0F}
    .contains(type & 0x03FF);

/// Checks a program for structural/type problems (Docs: "check type compatibility and
/// program validity before sending"). Returns an empty list when valid.
List<String> validateScriptLines(List<ScriptLine> lines, ScriptValidationContext context) {
  final errors = <String>[];
  for (var i = 0; i < lines.length; i++) {
    final line = lines[i];
    final def = line.def;
    final where = 'Line ${i + 1}';
    if (def == null) {
      errors.add('$where: unknown instruction');
      continue;
    }
    if (def.destination && line.destinations.isEmpty) {
      errors.add('$where (${def.label}): needs a destination');
    }
    if (!def.destination && line.destinations.isNotEmpty) {
      errors.add('$where (${def.label}): takes no destination');
    }
    for (final d in line.destinations) {
      if (d.type != symVariable && d.type != symOutput) {
        errors.add('$where: destination must be a variable or output');
      }
    }
    if (line.operands.length < def.minOperands || line.operands.length > def.maxOperands) {
      errors.add('$where (${def.label}): expects ${def.minOperands}..${def.maxOperands} operands '
          '(${line.operands.length} given)');
    }
    if (def.constantIndex >= 0 && line.operands.length > def.constantIndex) {
      final o = line.operands[def.constantIndex];
      final t = context.typeOf(o);
      if (o.type != symConstant) {
        errors.add('$where (${def.label}): operand ${def.constantIndex + 1} must be a constant '
            '(BlockInfo)');
      } else if (t != null && t != DataType.blockInfo.value) {
        errors.add('$where (${def.label}): operand ${def.constantIndex + 1} must be a BlockInfo');
      }
    }
    if (def.addressIndex >= 0 && line.operands.length > def.addressIndex) {
      final a = line.operands[def.addressIndex];
      final t = context.typeOf(a);
      const addrTypes = {0x03 /*Id*/, 0x05 /*Index*/, 0x06 /*Number*/, 0x0E /*Uint32*/};
      if (a.type != symConstant && t != null && !addrTypes.contains(t)) {
        errors.add('$where (${def.label}): operand ${def.addressIndex + 1} must be a device '
            'address (Id)');
      }
    }
    if (def.numeric) {
      for (final o in line.operands) {
        final t = context.typeOf(o);
        if (t != null && !scriptTypeIsNumeric(t)) {
          errors.add('$where (${def.label}): operand is not numeric');
          break;
        }
      }
    }
    if (def.expression) errors.addAll(_validateExpression(line, i));
  }
  return errors;
}

/// Structural check for an infix expression (`Set`): balanced parentheses and a
/// value/operator alternation (a leading or after-operator `-` is unary).
List<String> _validateExpression(ScriptLine line, int lineIndex) {
  final errors = <String>[];
  final where = 'Line ${lineIndex + 1}';
  // Binary operators: Add, Subtract, Multiply, Divide, Modulo, Power, AND, OR, XOR and
  // the comparisons. Subtract (1) and NOT (9) may also be unary (where a value is expected).
  const binaryOps = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 13, 14, 15, 16, 17};
  var depth = 0;
  var expectValue = true;
  for (final o in line.operands) {
    final isOp = o.type == symPredefine && o.subtype == preMathOp;
    if (isOp && (o.value == mathOpOpenParen || o.value == mathOpCloseParen)) {
      if (o.value == mathOpOpenParen) {
        if (!expectValue) {
          errors.add('$where: unexpected "("');
          return errors;
        }
        depth++;
      } else {
        if (expectValue) {
          errors.add('$where: unexpected ")"');
          return errors;
        }
        if (depth == 0) {
          errors.add('$where: unbalanced ")"');
          return errors;
        }
        depth--;
        expectValue = false;
      }
      continue;
    }
    if (isOp) {
      // Unary minus / NOT where a value is expected: still expect a value afterwards.
      if (expectValue && (o.value == 1 || o.value == 9)) continue;
      if (expectValue || !binaryOps.contains(o.value)) {
        errors.add('$where: unexpected operator');
        return errors;
      }
      expectValue = true;
      continue;
    }
    if (!expectValue) {
      errors.add('$where: two values in a row');
      return errors;
    }
    expectValue = false;
  }
  if (depth != 0) errors.add('$where: unbalanced "("');
  if (expectValue && line.operands.isNotEmpty) {
    errors.add('$where: expression ends with an operator');
  }
  return errors;
}
