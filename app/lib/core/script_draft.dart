/// Editable in-memory model of a script (Docs/App/Service views/Script.md). The editor
/// loads a `SCR_XXX` file into a [ScriptDraft], edits it, and serialises it back with
/// [ScriptDraft.toImage] - working for stored scripts as well as loaded ones.
library;

import 'dart:typed_data';

import 'script_file.dart';
import 'script_functions.dart';
import 'script_instructions.dart';
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
    return ScriptDraft(
      functionName: f.functionName,
      properties: f.properties,
      inputs: inputs,
      outputs: outputs,
      variables: variables,
      constants: constants,
      functionReservations: [
        for (
          var o = 0;
          o + scriptFunctionReservationSize <= f.functionReservations.length;
          o += scriptFunctionReservationSize
        )
          f.functionReservations.sublist(o, o + scriptFunctionReservationSize),
      ],
      functions: [for (final fn in f.functions) fn.copy()],
      lines: mainLines,
    );
  }

  /// Assembles the program with its functions: the instruction stream (main + laid-out
  /// bodies), the function reservations for referenced global functions, and any assembly
  /// errors (a cycle or too-deep nesting).
  ScriptAssembly assemble() =>
      assembleScriptProgram(mainLines: lines, functions: functions);

  /// Main-program `Call` line index -> function index, for the file's UI blob.
  Map<int, int> get _mainCallTargets => {
    for (var i = 0; i < lines.length; i++)
      if (lines[i].isCall && lines[i].functionTarget != null)
        i: lines[i].functionTarget!,
  };

  Uint8List toImage() {
    final assembly = assemble();
    // Assembly must be clean before upload - the editor refuses a cyclic/too-deep graph first.
    // If it is not (a direct toImage caller skipped validation), fall back to the unassembled
    // program rather than writing an empty instruction stream.
    final hasFunctions = functions.isNotEmpty;
    final assembled = hasFunctions && assembly.ok;
    return ScriptFileBuilder(
      properties: properties,
      inputs: [for (final v in inputs) v.info],
      outputs: [for (final v in outputs) v.info],
      variables: [for (final v in variables) v.info],
      constants: [for (final v in constants) v.info],
      inputDefaults: [for (final v in inputs) v.value.toList()],
      constantValues: [for (final v in constants) v.value.toList()],
      instructions: encodeScriptLines(assembled ? assembly.lines : lines),
      functionName: functionName,
      inputNames: [for (final v in inputs) v.name],
      outputNames: [for (final v in outputs) v.name],
      variableNames: [for (final v in variables) v.name],
      constantNames: [for (final v in constants) v.name],
      inputSpecs: [for (final v in inputs) v.spec],
      functionReservations: assembled
          ? assembly.reservations
          : functionReservations,
      functions: functions,
      functionMainLineCount: assembled ? assembly.mainLineCount : 0,
      mainCallTargets: assembled ? _mainCallTargets : const {},
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
