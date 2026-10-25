/// Editable in-memory model of a script (Docs/App/Service views/Script.md). The editor
/// loads a `SCR_XXX` file into a [ScriptDraft], edits it, and serialises it back with
/// [ScriptDraft.toImage] - working for stored scripts as well as loaded ones.
library;

import 'dart:typed_data';

import 'script_file.dart';
import 'script_functions.dart';
import 'script_instructions.dart';
import 'script_literals.dart';
import 'types.dart';

class ScriptDraftValue {
  String name;
  DataType type;

  /// Constant value / input default (unused for outputs and variables). When non-empty,
  /// the declared size follows the value's byte length.
  Uint8List value;

  /// Declared size for value-less entries (outputs/variables); ignored while [value] is set.
  int _declaredSize;

  /// UI specification (inputs only).
  ScriptInputSpec spec;

  ScriptDraftValue({
    this.name = '',
    this.type = DataType.number,
    int? size,
    Uint8List? value,
    this.spec = const ScriptInputSpec(),
  }) : value = value ?? Uint8List(0),
       _declaredSize = 0 {
    _declaredSize = size ?? defaultSizeForType(type);
  }

  /// Size is derived: from the value bytes when present, otherwise from the type.
  int get size => value.isNotEmpty ? value.length : _declaredSize;

  /// Changes the type, deriving a new default size and clearing any value.
  void setType(DataType next) {
    type = next;
    value = Uint8List(0);
    _declaredSize = defaultSizeForType(next);
    if (!uiStylesForType(next).contains(spec.uiType)) {
      spec = ScriptInputSpec(uiType: ScriptUiType.auto);
    }
  }

  /// Sets the value bytes; the declared size follows the value length.
  void setValue(Uint8List next) {
    value = next;
    if (next.isNotEmpty) _declaredSize = next.length;
  }

  ValueInfo get info => ValueInfo(type: type.value, size: size);
}

/// One constant of a draft and what the small-literal pass decided about it
/// ([convertScriptLiterals]).
class ScriptConstantUse {
  const ScriptConstantUse({
    required this.index,
    required this.name,
    required this.type,
    required this.operands,
    required this.predefine,
    required this.reason,
  });

  /// The constant's index in the draft the pass read.
  final int index;
  final String name;
  final DataType type;

  /// Operand positions (in the program and in every local/macro body) that reference it.
  final int operands;

  /// The predefine that now carries the value in those positions, or null when the constant
  /// stays stored.
  final ScriptSymbol? predefine;

  /// Why the constant stays stored (empty when [predefine] is set).
  final String reason;

  bool get converts => predefine != null;
}

/// A draft's constant table and program after its small literals became predefine operands
/// (Docs/Services/Script.md symbol kind 6; see `script_literals.dart`).
class ScriptLiteralConversion {
  const ScriptLiteralConversion({
    required this.constants,
    required this.lines,
    required this.functions,
    required this.uses,
  });

  /// The constants that stay stored, in their original order (only the converted ones are gone,
  /// so the survivors keep their relative order and meaning).
  final List<ScriptDraftValue> constants;

  /// The program with every referencing operand rewritten - and the remaining constant symbols
  /// renumbered to the survivors' new indices.
  final List<ScriptLine> lines;

  /// The declared functions, with local/macro bodies rewritten the same way.
  final List<ScriptFunction> functions;

  /// One entry per constant of the draft the pass read, in index order.
  final List<ScriptConstantUse> uses;

  List<ScriptConstantUse> get converted => [
    for (final u in uses)
      if (u.converts) u,
  ];
}

/// Replaces every small literal constant an operand references with the predefine that carries
/// it, and drops the constant.
///
/// The value is only ever converted when the predefine reproduces it bit-exactly
/// ([scriptLiteralPredefine]); a `BlockInfo`, a container, a lossy fraction, a constant no
/// operand references, or a constant a Flow `Jump`/`Call` uses as a line target stays stored.
/// Conversion does not change the instruction stream's length: one 4-byte symbol replaces
/// another, so flow targets and function layout are untouched.
///
/// The program and every local/macro body are rewritten together with the table - a dropped
/// constant's indices would otherwise shift the references of the survivors - so a re-save
/// cannot renumber or drop a constant in a way that changes what the script does.
ScriptLiteralConversion convertScriptLiterals({
  required List<ScriptDraftValue> constants,
  required List<ScriptLine> lines,
  List<ScriptFunction> functions = const [],
}) {
  // 1. Which constants a predefine can carry at all.
  final candidate = <int, ScriptSymbol>{};
  final reason = <int, String>{};
  for (var i = 0; i < constants.length; i++) {
    final c = constants[i];
    final decision = scriptLiteralPredefine(c.type, c.value);
    if (decision.converts) {
      candidate[i] = decision.symbol!;
    } else {
      reason[i] = decision.reason;
    }
  }

  // 2. How many operands reference each constant, and which of them are line targets.
  final uses = <int, int>{};
  final targets = <int>{};
  void scan(List<ScriptLine> program) {
    for (final line in program) {
      final def = line.def;
      final isTarget =
          def != null &&
          def.category == catFlow &&
          (def.op == 3 || def.op == 4);
      for (var i = 0; i < line.operands.length; i++) {
        final o = line.operands[i];
        if (o.type != symConstant) continue;
        uses[o.value] = (uses[o.value] ?? 0) + 1;
        if (isTarget && i == 0) targets.add(o.value);
      }
    }
  }

  scan(lines);
  for (final f in functions) {
    if (f.hasBody) scan(f.body);
  }

  // 3. A constant nothing references has no operand position to move into, and a line target is
  //    an address rather than a literal: both stay stored (nothing is silently deleted).
  for (final i in candidate.keys.toList()) {
    if ((uses[i] ?? 0) == 0) {
      reason[i] = 'no operand references it';
      candidate.remove(i);
    } else if (targets.contains(i)) {
      reason[i] = 'a Flow Jump/Call uses it as a line target';
      candidate.remove(i);
    }
  }

  final analyses = <ScriptConstantUse>[
    for (var i = 0; i < constants.length; i++)
      ScriptConstantUse(
        index: i,
        name: constants[i].name,
        type: constants[i].type,
        operands: uses[i] ?? 0,
        predefine: candidate[i],
        reason: reason[i] ?? '',
      ),
  ];
  if (candidate.isEmpty) {
    // Nothing converts: hand back what was read, so a load stays allocation-free.
    return ScriptLiteralConversion(
      constants: constants,
      lines: lines,
      functions: functions,
      uses: analyses,
    );
  }

  // 4. The survivors' new indices, in their original order.
  final newIndex = <int, int>{};
  final kept = <ScriptDraftValue>[];
  for (var i = 0; i < constants.length; i++) {
    if (candidate.containsKey(i)) continue;
    newIndex[i] = kept.length;
    kept.add(constants[i]);
  }

  ScriptSymbol rewrite(ScriptSymbol symbol) {
    if (symbol.type != symConstant) return symbol;
    final literal = candidate[symbol.value];
    if (literal != null) return literal;
    final index = newIndex[symbol.value];
    return index == null ? symbol : ScriptSymbol.constant(index);
  }

  ScriptLine rewriteLine(ScriptLine line) => ScriptLine(
    destinations: List.of(line.destinations),
    instruction: line.instruction,
    operands: [for (final o in line.operands) rewrite(o)],
    functionTarget: line.functionTarget,
  );

  assert(() {
    // The survivors keep their original relative order: only converted entries are removed.
    final survivors = [
      for (var i = 0; i < constants.length; i++)
        if (!candidate.containsKey(i)) constants[i],
    ];
    if (survivors.length != kept.length) return false;
    for (var i = 0; i < survivors.length; i++) {
      if (!identical(survivors[i], kept[i])) return false;
    }
    return true;
  }());
  return ScriptLiteralConversion(
    constants: kept,
    lines: [for (final l in lines) rewriteLine(l)],
    functions: [
      for (final f in functions)
        f.hasBody
            ? ScriptFunction(
                name: f.name,
                kind: f.kind,
                body: [for (final l in f.body) rewriteLine(l)],
                globalFileId: f.globalFileId,
                globalMemOffset: f.globalMemOffset,
                globalInstrOffset: f.globalInstrOffset,
              )
            : f,
    ],
    uses: analyses,
  );
}

class ScriptDraft {
  String functionName;
  int properties;
  final List<ScriptDraftValue> inputs;
  final List<ScriptDraftValue> outputs;
  final List<ScriptDraftValue> variables;
  final List<ScriptDraftValue> constants;

  /// Function reservations carried through unchanged (one 16-byte record per linked global
  /// function; the count is the file header's Function count).
  List<List<int>> functionReservations;

  /// Declared functions (local/macro bodies + global parameters), kept in the file's app-only
  /// UI blob and laid out at assembly (Docs/Services/Script.md "Functions and Macros").
  final List<ScriptFunction> functions;

  List<ScriptLine> lines;

  ScriptDraft({
    this.functionName = '',
    this.properties = 0,
    List<ScriptDraftValue>? inputs,
    List<ScriptDraftValue>? outputs,
    List<ScriptDraftValue>? variables,
    List<ScriptDraftValue>? constants,
    List<List<int>>? functionReservations,
    List<ScriptFunction>? functions,
    List<ScriptLine>? lines,
  }) : inputs = inputs ?? [],
       outputs = outputs ?? [],
       variables = variables ?? [],
       constants = constants ?? [],
       functionReservations = functionReservations ?? [],
       functions = functions ?? [],
       lines = lines ?? [];

  factory ScriptDraft.fromFile(ScriptFileData f) {
    final inputs = <ScriptDraftValue>[];
    for (var i = 0; i < f.inputs.length; i++) {
      inputs.add(
        ScriptDraftValue(
          name: f.nameOf(f.inputNames, i, 'Input'),
          type: f.inputs[i].dataType,
          size: f.inputs[i].size,
          value: f.inputDefault(i),
          spec: i < f.inputSpecs.length
              ? f.inputSpecs[i]
              : const ScriptInputSpec(),
        ),
      );
    }
    final outputs = <ScriptDraftValue>[];
    for (var i = 0; i < f.outputs.length; i++) {
      outputs.add(
        ScriptDraftValue(
          name: f.nameOf(f.outputNames, i, 'Output'),
          type: f.outputs[i].dataType,
          size: f.outputs[i].size,
        ),
      );
    }
    final variables = <ScriptDraftValue>[];
    for (var i = 0; i < f.variables.length; i++) {
      variables.add(
        ScriptDraftValue(
          name: f.nameOf(f.variableNames, i, 'Variable'),
          type: f.variables[i].dataType,
          size: f.variables[i].size,
        ),
      );
    }
    final constants = <ScriptDraftValue>[];
    for (var i = 0; i < f.constants.length; i++) {
      constants.add(
        ScriptDraftValue(
          name: f.nameOf(f.constantNames, i, 'Constant'),
          type: f.constants[i].dataType,
          size: f.constants[i].size,
          value: f.constantValue(i),
        ),
      );
    }
    final allLines = decodeScriptLines(f.instructions);
    // With functions the instruction stream is the assembled program (main then bodies); the
    // bodies are reconstructed from the UI-blob definitions, so only the main program's head is
    // kept as the editable program.
    final mainCount = f.functions.isEmpty
        ? allLines.length
        : (f.functionMainLineCount <= allLines.length
              ? f.functionMainLineCount
              : allLines.length);
    final mainLines = allLines.sublist(0, mainCount);
    for (final e in f.mainCallTargets.entries) {
      if (e.key >= 0 && e.key < mainLines.length) {
        mainLines[e.key].functionTarget = e.value;
      }
    }
    // The small literals this file stores as constants become predefine operands here, so the
    // editor shows a literal as a literal from the first load (and a save writes it back that
    // way). `script_literals.dart` owns the exactness gate.
    final literals = convertScriptLiterals(
      constants: constants,
      lines: mainLines,
      functions: [for (final fn in f.functions) fn.copy()],
    );
    return ScriptDraft(
      functionName: f.functionName,
      properties: f.properties,
      inputs: inputs,
      outputs: outputs,
      variables: variables,
      constants: literals.constants,
      functionReservations: [
        for (
          var o = 0;
          o + scriptFunctionReservationSize <= f.functionReservations.length;
          o += scriptFunctionReservationSize
        )
          f.functionReservations.sublist(o, o + scriptFunctionReservationSize),
      ],
      functions: literals.functions,
      lines: literals.lines,
    );
  }

  /// Assembles the program with its functions: the instruction stream (main + laid-out
  /// bodies), the function reservations for referenced global functions, and any assembly
  /// errors (a cycle or too-deep nesting).
  ScriptAssembly assemble() =>
      assembleScriptProgram(mainLines: lines, functions: functions);

  /// Main-program `Call` line index -> function index, for the file's UI blob.
  Map<int, int> _mainCallTargets(List<ScriptLine> program) => {
    for (var i = 0; i < program.length; i++)
      if (program[i].isCall && program[i].functionTarget != null)
        i: program[i].functionTarget!,
  };

  Uint8List toImage() {
    // The encoder's literal rule (Docs/Services/Script.md symbol kind 6): a small literal is
    // emitted as a predefine in its operand position rather than as a constant slot. Applied
    // here as well as at load, so every image the app writes obeys the rule - including one
    // built from an archive's semantic form, which never passes through `fromFile`.
    final literals = convertScriptLiterals(
      constants: constants,
      lines: lines,
      functions: functions,
    );
    final program = literals.lines;
    final assembly = assembleScriptProgram(
      mainLines: program,
      functions: literals.functions,
    );
    // Assembly must be clean before upload - the editor refuses a cyclic/too-deep graph first.
    // If it is not (a direct toImage caller skipped validation), fall back to the unassembled
    // program rather than writing an empty instruction stream.
    final hasFunctions = literals.functions.isNotEmpty;
    final assembled = hasFunctions && assembly.ok;
    return ScriptFileBuilder(
      properties: properties,
      inputs: [for (final v in inputs) v.info],
      outputs: [for (final v in outputs) v.info],
      variables: [for (final v in variables) v.info],
      constants: [for (final v in literals.constants) v.info],
      inputDefaults: [for (final v in inputs) v.value.toList()],
      constantValues: [for (final v in literals.constants) v.value.toList()],
      instructions: encodeScriptLines(assembled ? assembly.lines : program),
      functionName: functionName,
      inputNames: [for (final v in inputs) v.name],
      outputNames: [for (final v in outputs) v.name],
      variableNames: [for (final v in variables) v.name],
      constantNames: [for (final v in literals.constants) v.name],
      inputSpecs: [for (final v in inputs) v.spec],
      functionReservations: assembled
          ? assembly.reservations
          : functionReservations,
      functions: literals.functions,
      functionMainLineCount: assembled ? assembly.mainLineCount : 0,
      mainCallTargets: assembled ? _mainCallTargets(program) : const {},
    ).build();
  }

  ScriptValidationContext get validationContext => ScriptValidationContext(
    inputTypes: [for (final v in inputs) v.type.value],
    outputTypes: [for (final v in outputs) v.type.value],
    variableTypes: [for (final v in variables) v.type.value],
    constantTypes: [for (final v in constants) v.type.value],
  );

  /// The declared data type a symbol refers to (null for predefines/unknown).
  DataType? valueTypeOf(ScriptSymbol s) => switch (s.type) {
    symInput => s.value < inputs.length ? inputs[s.value].type : null,
    symOutput => s.value < outputs.length ? outputs[s.value].type : null,
    symVariable => s.value < variables.length ? variables[s.value].type : null,
    symConstant => s.value < constants.length ? constants[s.value].type : null,
    _ => null,
  };

  /// The list backing [category].
  List<ScriptDraftValue> listOf(ScriptValueCategory category) =>
      switch (category) {
        ScriptValueCategory.input => inputs,
        ScriptValueCategory.output => outputs,
        ScriptValueCategory.variable => variables,
        ScriptValueCategory.constant => constants,
      };
}

/// The four value lists of a script draft, shared by the editor UI.
enum ScriptValueCategory {
  input,
  output,
  variable,
  constant;

  /// The Register field the category is exposed through (variables/constants are internal).
  int get field => switch (this) {
    input => ScriptField.input,
    output => ScriptField.output,
    variable => ScriptField.variable,
    constant => ScriptField.constant,
  };

  /// The symbol type that references an entry of this category.
  int get symbolType => switch (this) {
    input => symInput,
    output => symOutput,
    variable => symVariable,
    constant => symConstant,
  };

  String get label => switch (this) {
    input => 'Input',
    output => 'Output',
    variable => 'Variable',
    constant => 'Constant',
  };

  String get pluralLabel => switch (this) {
    input => 'Inputs',
    output => 'Outputs',
    variable => 'Variables',
    constant => 'Constants',
  };
}
