/// Editable in-memory model of a script (Docs/App/Service views/Script.md). The editor
/// loads a `SCR_XX` file into a [ScriptDraft], edits it, and serialises it back with
/// [ScriptDraft.toImage] - working for stored scripts as well as loaded ones.
library;

import 'dart:typed_data';

import 'script_file.dart';
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
  })  : value = value ?? Uint8List(0),
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

  ScriptValueInfo get info => ScriptValueInfo(type: type, size: size);
}

class ScriptDraft {
  String functionName;
  int properties;
  final List<ScriptDraftValue> inputs;
  final List<ScriptDraftValue> outputs;
  final List<ScriptDraftValue> variables;
  final List<ScriptDraftValue> constants;
  List<ScriptLine> lines;

  ScriptDraft({
    this.functionName = '',
    this.properties = 0,
    List<ScriptDraftValue>? inputs,
    List<ScriptDraftValue>? outputs,
    List<ScriptDraftValue>? variables,
    List<ScriptDraftValue>? constants,
    List<ScriptLine>? lines,
  })  : inputs = inputs ?? [],
        outputs = outputs ?? [],
        variables = variables ?? [],
        constants = constants ?? [],
        lines = lines ?? [];

  factory ScriptDraft.fromFile(ScriptFileData f) {
    final inputs = <ScriptDraftValue>[];
    for (var i = 0; i < f.inputs.length; i++) {
      inputs.add(ScriptDraftValue(
        name: f.nameOf(f.inputNames, i, 'Input'),
        type: f.inputs[i].type,
        size: f.inputs[i].size,
        value: f.inputDefault(i),
        spec: i < f.inputSpecs.length ? f.inputSpecs[i] : const ScriptInputSpec(),
      ));
    }
    final outputs = <ScriptDraftValue>[];
    for (var i = 0; i < f.outputs.length; i++) {
      outputs.add(ScriptDraftValue(
        name: f.nameOf(f.outputNames, i, 'Output'),
        type: f.outputs[i].type,
        size: f.outputs[i].size,
      ));
    }
    final variables = <ScriptDraftValue>[];
    for (var i = 0; i < f.variables.length; i++) {
      variables.add(ScriptDraftValue(
        name: f.nameOf(f.variableNames, i, 'Variable'),
        type: f.variables[i].type,
        size: f.variables[i].size,
      ));
    }
    final constants = <ScriptDraftValue>[];
    for (var i = 0; i < f.constants.length; i++) {
      constants.add(ScriptDraftValue(
        name: f.nameOf(f.constantNames, i, 'Constant'),
        type: f.constants[i].type,
        size: f.constants[i].size,
        value: f.constantValue(i),
      ));
    }
    return ScriptDraft(
      functionName: f.functionName,
      properties: f.properties,
      inputs: inputs,
      outputs: outputs,
      variables: variables,
      constants: constants,
      lines: decodeScriptLines(f.instructions),
    );
  }

  Uint8List toImage() => ScriptFileBuilder(
        properties: properties,
        inputs: [for (final v in inputs) v.info],
        outputs: [for (final v in outputs) v.info],
        variables: [for (final v in variables) v.info],
        constants: [for (final v in constants) v.info],
        inputDefaults: [for (final v in inputs) v.value.toList()],
        constantValues: [for (final v in constants) v.value.toList()],
        instructions: encodeScriptLines(lines),
        functionName: functionName,
        inputNames: [for (final v in inputs) v.name],
        outputNames: [for (final v in outputs) v.name],
        variableNames: [for (final v in variables) v.name],
        constantNames: [for (final v in constants) v.name],
        inputSpecs: [for (final v in inputs) v.spec],
      ).build();

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
}
