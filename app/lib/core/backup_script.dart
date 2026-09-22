/// Semantic script representation for backups (Docs/App/Backup.md: the storage
/// format describes "all the functionalities", including Scripts, in words).
///
/// A `SCR_XX` file is decoded into a [BackupScript] (function name, properties,
/// inputs/outputs/variables/constants with type words and semantic values, and the
/// instruction lines) so a firmware change to the binary packing does not invalidate
/// a backup. It re-serialises through the normal [ScriptDraft] codec.
library;

import 'dart:typed_data';

import 'backup_value.dart';
import 'script_draft.dart';
import 'script_file.dart';
import 'script_instructions.dart';
import 'types.dart';

class BackupInputSpec {
  final String style; // ScriptUiType label
  final double min;
  final double max;
  final double step;

  const BackupInputSpec({this.style = 'Auto', this.min = 0, this.max = 0, this.step = 0});

  Map<String, dynamic> toJson() => {'style': style, 'min': min, 'max': max, 'step': step};

  static BackupInputSpec fromJson(Map<String, dynamic> json) => BackupInputSpec(
        style: json['style'] as String? ?? 'Auto',
        min: (json['min'] as num?)?.toDouble() ?? 0,
        max: (json['max'] as num?)?.toDouble() ?? 0,
        step: (json['step'] as num?)?.toDouble() ?? 0,
      );
}

class BackupScriptValue {
  final String name;
  final String type; // data type word
  final int size;
  final Object? value; // input default / constant value (semantic; null when unused)
  final BackupInputSpec? ui; // inputs only

  const BackupScriptValue({
    required this.name,
    required this.type,
    required this.size,
    this.value,
    this.ui,
  });

  Map<String, dynamic> toJson() => {
        'name': name,
        'type': type,
        'size': size,
        if (value != null) 'value': value,
        if (ui != null) 'ui': ui!.toJson(),
      };

  static BackupScriptValue fromJson(Map<String, dynamic> json) => BackupScriptValue(
        name: json['name'] as String? ?? '',
        type: json['type'] as String? ?? 'Number',
        size: (json['size'] as num?)?.toInt() ?? 4,
        value: json['value'],
        ui: json['ui'] == null
            ? null
            : BackupInputSpec.fromJson(json['ui'] as Map<String, dynamic>),
      );
}

/// One program symbol, tagged with a word kind.
class BackupSymbol {
  final String kind;
  final int value;
  final String? subtype; // predefine subtype name
  final String? category; // instruction category name

  const BackupSymbol({
    required this.kind,
    this.value = 0,
    this.subtype,
    this.category,
  });

  Map<String, dynamic> toJson() => {
        'kind': kind,
        if (subtype != null) 'subtype': subtype,
        if (category != null) 'category': category,
        'value': value,
      };

  static BackupSymbol fromJson(Map<String, dynamic> json) => BackupSymbol(
        kind: json['kind'] as String? ?? 'undefined',
        value: (json['value'] as num?)?.toInt() ?? 0,
        subtype: json['subtype'] as String?,
        category: json['category'] as String?,
      );

  static BackupSymbol of(ScriptSymbol s) {
    switch (s.type) {
      case symInput:
        return BackupSymbol(kind: 'input', value: s.value);
      case symOutput:
        return BackupSymbol(kind: 'output', value: s.value);
      case symVariable:
        return BackupSymbol(kind: 'variable', value: s.value);
      case symConstant:
        return BackupSymbol(kind: 'constant', value: s.value);
      case symEndline:
        return const BackupSymbol(kind: 'endline');
      case symPredefine:
        return BackupSymbol(
            kind: 'predefine',
            subtype: ScriptSymbol.predefineName(s.subtype),
            value: s.value);
      case symInstruction:
        return BackupSymbol(
            kind: 'instruction',
            category: ScriptInstructionDef.categoryName(s.subtype),
            value: s.value);
      default:
        return BackupSymbol(kind: 'undefined', value: s.value);
    }
  }

  ScriptSymbol toSymbol() {
    switch (kind) {
      case 'input':
        return ScriptSymbol.input(value);
      case 'output':
        return ScriptSymbol.output(value);
      case 'variable':
        return ScriptSymbol.variable(value);
      case 'constant':
        return ScriptSymbol.constant(value);
      case 'endline':
        return ScriptSymbol.endline;
      case 'predefine':
        return ScriptSymbol.predefine(_predefineSubtype(subtype), value);
      case 'instruction':
        return ScriptSymbol.instruction(_category(category), value);
      default:
        return ScriptSymbol.instruction(catService, 4); // Nop
    }
  }
}

class BackupLine {
  final List<BackupSymbol> destinations;
  final BackupSymbol instruction;
  final List<BackupSymbol> operands;

  const BackupLine({
    required this.destinations,
    required this.instruction,
    required this.operands,
  });

  Map<String, dynamic> toJson() => {
        'destinations': [for (final d in destinations) d.toJson()],
        'instruction': instruction.toJson(),
        'operands': [for (final o in operands) o.toJson()],
      };

  static BackupLine fromJson(Map<String, dynamic> json) => BackupLine(
        destinations: (json['destinations'] as List? ?? [])
            .map((e) => BackupSymbol.fromJson(e as Map<String, dynamic>))
            .toList(),
        instruction: BackupSymbol.fromJson(
            (json['instruction'] as Map<String, dynamic>?) ?? const {'kind': 'instruction'}),
        operands: (json['operands'] as List? ?? [])
            .map((e) => BackupSymbol.fromJson(e as Map<String, dynamic>))
            .toList(),
      );
}

int _predefineSubtype(String? name) {
  for (final (value, label) in scriptPredefineSubtypes) {
    if (label == name) return value;
  }
  return preBool;
}

int _category(String? name) {
  for (var c = 0; c <= catCompose; c++) {
    if (ScriptInstructionDef.categoryName(c) == name) return c;
  }
  return catService;
}

const List<String> _propertyWords = ['Load on boot', 'Run on load'];

class BackupScript {
  final int slot; // SCR slot (0..63), or -1 when unknown
  final String functionName;
  final List<String> properties;
  final List<BackupScriptValue> inputs;
  final List<BackupScriptValue> outputs;
  final List<BackupScriptValue> variables;
  final List<BackupScriptValue> constants;
  final List<BackupLine> lines;

  const BackupScript({
    required this.slot,
    required this.functionName,
    required this.properties,
    required this.inputs,
    required this.outputs,
    required this.variables,
    required this.constants,
    required this.lines,
  });

  Map<String, dynamic> toJson() => {
        'slot': slot,
        'function': functionName,
        'properties': properties,
        'inputs': [for (final v in inputs) v.toJson()],
        'outputs': [for (final v in outputs) v.toJson()],
        'variables': [for (final v in variables) v.toJson()],
        'constants': [for (final v in constants) v.toJson()],
        'lines': [for (final l in lines) l.toJson()],
      };

  static BackupScript fromJson(Map<String, dynamic> json) => BackupScript(
        slot: (json['slot'] as num?)?.toInt() ?? -1,
        functionName: json['function'] as String? ?? '',
        properties: (json['properties'] as List?)?.cast<String>() ?? const [],
        inputs: _values(json['inputs']),
        outputs: _values(json['outputs']),
        variables: _values(json['variables']),
        constants: _values(json['constants']),
        lines: (json['lines'] as List? ?? [])
            .map((e) => BackupLine.fromJson(e as Map<String, dynamic>))
            .toList(),
      );

  static List<BackupScriptValue> _values(Object? raw) => (raw as List? ?? [])
      .map((e) => BackupScriptValue.fromJson(e as Map<String, dynamic>))
      .toList();

  /// Decodes a raw `SCR_XX` image into the semantic form.
  static BackupScript fromImage(int slot, List<int> image) {
    final file = ScriptFileData.parse(image);
    final draft = ScriptDraft.fromFile(file);
    return fromDraft(slot, draft);
  }

  static BackupScript fromDraft(int slot, ScriptDraft draft) {
    BackupScriptValue value(ScriptDraftValue v, {required bool isInput}) =>
        BackupScriptValue(
          name: v.name,
          type: dataTypeWord(v.type),
          size: v.size,
          value: v.value.isEmpty ? null : encodeSemantic(v.type, v.value),
          ui: isInput
              ? BackupInputSpec(
                  style: ScriptUiType.label(v.spec.uiType),
                  min: v.spec.min,
                  max: v.spec.max,
                  step: v.spec.step)
              : null,
        );
    return BackupScript(
      slot: slot,
      functionName: draft.functionName,
      properties: [
        for (var i = 0; i < _propertyWords.length; i++)
          if (draft.properties & (1 << i) != 0) _propertyWords[i],
      ],
      inputs: [for (final v in draft.inputs) value(v, isInput: true)],
      outputs: [for (final v in draft.outputs) value(v, isInput: false)],
      variables: [for (final v in draft.variables) value(v, isInput: false)],
      constants: [for (final v in draft.constants) value(v, isInput: false)],
      lines: [
        for (final l in draft.lines)
          BackupLine(
            destinations: [for (final d in l.destinations) BackupSymbol.of(d)],
            instruction: BackupSymbol.of(l.instruction),
            operands: [for (final o in l.operands) BackupSymbol.of(o)],
          )
      ],
    );
  }

  /// Rebuilds the editable draft (and thus the `SCR_XX` image via `toImage`).
  ScriptDraft toDraft() {
    ScriptDraftValue value(BackupScriptValue v) {
      final type = dataTypeFromWord(v.type) ?? DataType.number;
      final bytes = decodeSemantic(type, v.value, size: v.size);
      return ScriptDraftValue(
        name: v.name,
        type: type,
        size: v.size,
        value: bytes == null ? Uint8List(0) : Uint8List.fromList(bytes),
        spec: v.ui == null
            ? const ScriptInputSpec()
            : ScriptInputSpec(
                uiType: _uiStyle(v.ui!.style),
                min: v.ui!.min,
                max: v.ui!.max,
                step: v.ui!.step),
      );
    }

    var props = 0;
    for (var i = 0; i < _propertyWords.length; i++) {
      if (properties.contains(_propertyWords[i])) props |= 1 << i;
    }
    return ScriptDraft(
      functionName: functionName,
      properties: props,
      inputs: [for (final v in inputs) value(v)],
      outputs: [for (final v in outputs) value(v)],
      variables: [for (final v in variables) value(v)],
      constants: [for (final v in constants) value(v)],
      lines: [
        for (final l in lines)
          ScriptLine(
            destinations: [for (final d in l.destinations) d.toSymbol()],
            instruction: l.instruction.toSymbol(),
            operands: [for (final o in l.operands) o.toSymbol()],
          )
      ],
    );
  }
}

int _uiStyle(String style) {
  for (final v in [
    ScriptUiType.auto,
    ScriptUiType.number,
    ScriptUiType.slider,
    ScriptUiType.toggle,
    ScriptUiType.button,
    ScriptUiType.dropdown,
    ScriptUiType.swatch,
  ]) {
    if (ScriptUiType.label(v) == style) return v;
  }
  return ScriptUiType.auto;
}
