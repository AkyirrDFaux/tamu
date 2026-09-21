/// Script file codec (Docs/Services/Script.md).
///
/// One `SCR_XX` file holds a script's properties, its input/output/variable/constant
/// ValueInfo tables, constant values, input defaults, instruction symbols and UI info
/// (function/IO/variable/constant names + per-input UI specification).
///
/// The binary packing of the parts the docs leave open is an internal convention shared
/// with the firmware (`Core/Services/Script.h`).
library;

import 'dart:typed_data';

import 'types.dart';

/// Number of script slots (SCR_00..SCR_3F).
const int maxScripts = 64;

/// File header size: Properties + counts + the four lengths.
const int scriptHeaderSize = 24;

/// One instruction symbol: Type(u8) | Subtype(u8) | Value(u16).
const int scriptSymbolSize = 4;

/// Version byte of the UI-info blob (bumped when the layout changes).
const int scriptUiInfoVersion = 1;

/// UI widget kinds an input can be rendered as (Docs/App/Service views/Script.md:
/// "UI preview ... sliders, buttons, toggles").
class ScriptUiType {
  static const auto = 0;
  static const number = 1;
  static const slider = 2;
  static const toggle = 3;
  static const button = 4;
  static const dropdown = 5;
  static const swatch = 6;

  static String label(int uiType) => switch (uiType) {
        auto => 'Auto',
        number => 'Number field',
        slider => 'Slider',
        toggle => 'Toggle',
        button => 'Button',
        dropdown => 'Dropdown',
        swatch => 'Swatch',
        _ => 'Auto',
      };
}

/// The input UI styles that are meaningful for a data type (the editor only offers these).
List<int> uiStylesForType(DataType type) => switch (type) {
      DataType.bool_ => const [ScriptUiType.auto, ScriptUiType.toggle, ScriptUiType.button],
      DataType.number ||
      DataType.integer ||
      DataType.idx ||
      DataType.uint32 =>
        const [ScriptUiType.auto, ScriptUiType.number, ScriptUiType.slider],
      DataType.enum_ || DataType.devType =>
        const [ScriptUiType.auto, ScriptUiType.dropdown],
      DataType.colour => const [ScriptUiType.auto, ScriptUiType.swatch],
      _ => const [ScriptUiType.auto],
    };

/// Min/max/step limits only apply to the numeric input styles.
bool uiStyleSupportsLimits(int uiType) =>
    uiType == ScriptUiType.number || uiType == ScriptUiType.slider;

/// One ValueInfo entry: type (low 10 bits) + flags (high 6 bits), key (unused),
/// size (byte length of the value).
class ScriptValueInfo {
  final DataType type;
  final int size;
  final int flags;

  const ScriptValueInfo({required this.type, required this.size, this.flags = 0});

  int get flagsAndType => (flags & FieldFlags.mask) | (type.value & 0x03FF);

  Uint8List toBytes() => Uint8List(4)
    ..[0] = flagsAndType & 0xFF
    ..[1] = (flagsAndType >> 8) & 0xFF
    ..[2] = 0
    ..[3] = size & 0xFF;

  static ScriptValueInfo fromBytes(List<int> bytes, [int offset = 0]) {
    final fat = bytes[offset] | (bytes[offset + 1] << 8);
    return ScriptValueInfo(
      type: DataType.fromValue(fat & 0x03FF),
      size: bytes[offset + 3],
      flags: fat & FieldFlags.mask,
    );
  }
}

/// Per-input UI specification (limits are kept as 16.16 fixed point like Number).
class ScriptInputSpec {
  final int uiType;
  final double min;
  final double max;
  final double step;

  const ScriptInputSpec({
    this.uiType = ScriptUiType.auto,
    this.min = 0,
    this.max = 0,
    this.step = 0,
  });

  ScriptInputSpec copyWith({int? uiType, double? min, double? max, double? step}) =>
      ScriptInputSpec(
        uiType: uiType ?? this.uiType,
        min: min ?? this.min,
        max: max ?? this.max,
        step: step ?? this.step,
      );
}

/// Register field categories of a loaded script (block type 0x3FE). Only Input and Output
/// are exposed through the Register; Variables/Constants are internal (Script CID 5/7) and
/// Header is script metadata.
class ScriptField {
  static const header = 0;
  static const input = 1;
  static const output = 2;
  static const variable = 3; // internal: script RAM (not a register field)
  static const constant = 4; // internal: file data (not a register field)

  /// Number of register fields the block advertises (Header reserved + Input + Output).
  static const count = 3;
}

/// Header keys (field 0).
class ScriptHeaderKey {
  static const state = 0;
  static const instructionCounter = 1;
  static const properties = 2;
  static const inputCount = 3;
  static const outputCount = 4;
  static const variableCount = 5;
  static const constantCount = 6;
  static const fileId = 7;
  static const error = 8;
  static const count = 9;
}

/// Script states (Docs/Services/Script.md "State").
class ScriptState {
  static const stopped = 0;
  static const running = 1;
  static const paused = 2;
  static const waiting = 3;
  static const finished = 4;
  static const error = 5;

  static String label(int state) => switch (state) {
        stopped => 'Stopped',
        running => 'Running',
        paused => 'Paused',
        waiting => 'Waiting',
        finished => 'Finished',
        error => 'Error',
        _ => 'Unknown ($state)',
      };
}

/// Properties bits.
class ScriptProperties {
  static const loadOnBoot = 1 << 0;
  static const runOnLoad = 1 << 1;
}

int _align4(int size) => (size + 3) & ~3;

/// Data types offered for script IO/variable/constant declarations (the dictionary marker
/// types are not valid here).
const List<DataType> scriptValueTypes = [
  DataType.bool_,
  DataType.integer,
  DataType.number,
  DataType.enum_,
  DataType.colour,
  DataType.vector,
  DataType.matrix,
  DataType.string,
  DataType.filename,
  DataType.uint32,
];

/// A sensible byte size for a newly declared value of [type] (used as the default in the
/// create-script dialog; the user can override it).
int defaultSizeForType(DataType type) => switch (type) {
      DataType.bool_ => 1,
      DataType.enum_ => 1,
      DataType.id || DataType.devType => 2,
      DataType.number || DataType.integer || DataType.idx || DataType.uint32 => 4,
      DataType.colour => 4,
      DataType.vector => 8,
      DataType.matrix => 24,
      DataType.string => 16,
      DataType.filename => 8,
      DataType.sn => 14,
      _ => 4,
    };

void _putU32(Uint8List out, int offset, int value) {
  out[offset] = value & 0xFF;
  out[offset + 1] = (value >> 8) & 0xFF;
  out[offset + 2] = (value >> 16) & 0xFF;
  out[offset + 3] = (value >> 24) & 0xFF;
}

int _getU32(List<int> bytes, int offset) =>
    bytes[offset] | (bytes[offset + 1] << 8) | (bytes[offset + 2] << 16) | (bytes[offset + 3] << 24);

int _numberToRaw(double v) => (v * 65536.0).round();
double _rawToNumber(int raw) => (raw & 0x80000000) != 0 ? (raw - 0x100000000) / 65536.0 : raw / 65536.0;

/// Packs `values` (one byte list per entry) into the 4-byte-strided blob the firmware
/// expects, padding/clamping each entry to its declared ValueInfo size.
Uint8List packScriptValues(List<ScriptValueInfo> infos, List<List<int>> values) {
  var total = 0;
  for (final info in infos) {
    total += _align4(info.size);
  }
  final out = Uint8List(total);
  var offset = 0;
  for (var i = 0; i < infos.length; i++) {
    final info = infos[i];
    final value = i < values.length ? values[i] : const <int>[];
    final n = value.length < info.size ? value.length : info.size;
    out.setRange(offset, offset + n, value);
    offset += _align4(info.size);
  }
  return out;
}

/// Extracts one entry's bytes from a strided blob.
Uint8List unpackScriptValue(List<ScriptValueInfo> infos, List<int> blob, int index) {
  var offset = 0;
  for (var i = 0; i < infos.length; i++) {
    if (i == index) {
      final size = infos[i].size;
      final start = offset;
      final end = offset + size;
      if (end > blob.length) return Uint8List(0);
      return Uint8List.fromList(blob.sublist(start, end));
    }
    offset += _align4(infos[i].size);
  }
  return Uint8List(0);
}

/// Builds a `SCR_XX` file image.
class ScriptFileBuilder {
  final int properties;
  final List<ScriptValueInfo> inputs;
  final List<ScriptValueInfo> outputs;
  final List<ScriptValueInfo> variables;
  final List<ScriptValueInfo> constants;
  final List<List<int>> inputDefaults;
  final List<List<int>> constantValues;
  final List<int> instructions;
  final String functionName;
  final List<String> inputNames;
  final List<String> outputNames;
  final List<String> variableNames;
  final List<String> constantNames;
  final List<ScriptInputSpec> inputSpecs;

  const ScriptFileBuilder({
    this.properties = 0,
    this.inputs = const [],
    this.outputs = const [],
    this.variables = const [],
    this.constants = const [],
    this.inputDefaults = const [],
    this.constantValues = const [],
    this.instructions = const [],
    this.functionName = '',
    this.inputNames = const [],
    this.outputNames = const [],
    this.variableNames = const [],
    this.constantNames = const [],
    this.inputSpecs = const [],
  });

  Uint8List _uiInfo() {
    final out = <int>[scriptUiInfoVersion];
    void putString(String s) {
      final bytes = s.codeUnits;
      final n = bytes.length > 255 ? 255 : bytes.length;
      out.add(n);
      out.addAll(bytes.take(n));
    }

    void putNames(List<String> names, int count) {
      out.add(count & 0xFF);
      for (var i = 0; i < count; i++) {
        putString(i < names.length ? names[i] : '');
      }
    }

    putString(functionName);
    putNames(inputNames, inputs.length);
    putNames(outputNames, outputs.length);
    putNames(variableNames, variables.length);
    putNames(constantNames, constants.length);
    for (var i = 0; i < inputs.length; i++) {
      final spec = i < inputSpecs.length ? inputSpecs[i] : const ScriptInputSpec();
      out.add(spec.uiType & 0xFF);
      out.addAll([0, 0, 0]);
      final minB = Uint8List(4); _putU32(minB, 0, _numberToRaw(spec.min)); out.addAll(minB);
      final maxB = Uint8List(4); _putU32(maxB, 0, _numberToRaw(spec.max)); out.addAll(maxB);
      final stepB = Uint8List(4); _putU32(stepB, 0, _numberToRaw(spec.step)); out.addAll(stepB);
    }
    return Uint8List.fromList(out);
  }

  Uint8List build() {
    final constBlob = packScriptValues(constants, constantValues);
    final defBlob = packScriptValues(inputs, inputDefaults);
    final ui = _uiInfo();

    final metaLen = (inputs.length + outputs.length + variables.length + constants.length) * 4;
    final total =
        scriptHeaderSize + metaLen + constBlob.length + defBlob.length + instructions.length + ui.length;
    final out = Uint8List(total);

    _putU32(out, 0, properties);
    out[4] = inputs.length;
    out[5] = outputs.length;
    out[6] = variables.length;
    out[7] = constants.length;
    _putU32(out, 8, constBlob.length);
    _putU32(out, 12, defBlob.length);
    _putU32(out, 16, instructions.length);
    _putU32(out, 20, ui.length);

    var offset = scriptHeaderSize;
    for (final info in [...inputs, ...outputs, ...variables, ...constants]) {
      out.setRange(offset, offset + 4, info.toBytes());
      offset += 4;
    }
    out.setRange(offset, offset + constBlob.length, constBlob);
    offset += constBlob.length;
    out.setRange(offset, offset + defBlob.length, defBlob);
    offset += defBlob.length;
    out.setRange(offset, offset + instructions.length, instructions);
    offset += instructions.length;
    out.setRange(offset, offset + ui.length, ui);
    return out;
  }
}

/// A parsed `SCR_XX` image.
class ScriptFileData {
  final int properties;
  final List<ScriptValueInfo> inputs;
  final List<ScriptValueInfo> outputs;
  final List<ScriptValueInfo> variables;
  final List<ScriptValueInfo> constants;
  final Uint8List constantValues;
  final Uint8List inputDefaults;
  final Uint8List instructions;
  final String functionName;
  final List<String> inputNames;
  final List<String> outputNames;
  final List<String> variableNames;
  final List<String> constantNames;
  final List<ScriptInputSpec> inputSpecs;

  const ScriptFileData({
    required this.properties,
    required this.inputs,
    required this.outputs,
    required this.variables,
    required this.constants,
    required this.constantValues,
    required this.inputDefaults,
    required this.instructions,
    required this.functionName,
    required this.inputNames,
    required this.outputNames,
    required this.variableNames,
    required this.constantNames,
    required this.inputSpecs,
  });

  /// Value bytes of one constant.
  Uint8List constantValue(int index) =>
      unpackScriptValue(constants, constantValues, index);

  /// Default bytes of one input.
  Uint8List inputDefault(int index) =>
      unpackScriptValue(inputs, inputDefaults, index);

  String nameOf(List<String> names, int index, String prefix) =>
      index < names.length && names[index].isNotEmpty ? names[index] : '$prefix $index';

  /// Parses an image. Throws [FormatException] on a truncated/inconsistent file.
  static ScriptFileData parse(List<int> bytes) {
    if (bytes.length < scriptHeaderSize) {
      throw const FormatException('script file shorter than the header');
    }
    final properties = _getU32(bytes, 0);
    final inCount = bytes[4];
    final outCount = bytes[5];
    final varCount = bytes[6];
    final constCount = bytes[7];
    final constLen = _getU32(bytes, 8);
    final defLen = _getU32(bytes, 12);
    final instrLen = _getU32(bytes, 16);
    final uiLen = _getU32(bytes, 20);

    final metaLen = (inCount + outCount + varCount + constCount) * 4;
    final need = scriptHeaderSize + metaLen + constLen + defLen + instrLen + uiLen;
    if (need > bytes.length) {
      throw const FormatException('script file lengths exceed the file size');
    }

    var offset = scriptHeaderSize;
    List<ScriptValueInfo> readMetas(int count) {
      final list = <ScriptValueInfo>[];
      for (var i = 0; i < count; i++) {
        list.add(ScriptValueInfo.fromBytes(bytes, offset));
        offset += 4;
      }
      return list;
    }

    final inputs = readMetas(inCount);
    final outputs = readMetas(outCount);
    final variables = readMetas(varCount);
    final constants = readMetas(constCount);

    final constantValues = Uint8List.fromList(bytes.sublist(offset, offset + constLen));
    offset += constLen;
    final inputDefaults = Uint8List.fromList(bytes.sublist(offset, offset + defLen));
    offset += defLen;
    final instructions = Uint8List.fromList(bytes.sublist(offset, offset + instrLen));
    offset += instrLen;
    final ui = bytes.sublist(offset, offset + uiLen);

    return ScriptFileData(
      properties: properties,
      inputs: inputs,
      outputs: outputs,
      variables: variables,
      constants: constants,
      constantValues: constantValues,
      inputDefaults: inputDefaults,
      instructions: instructions,
      functionName: _parseFunctionName(ui),
      inputNames: _parseNames(ui, 0, inCount),
      outputNames: _parseNames(ui, 1, outCount),
      variableNames: _parseNames(ui, 2, varCount),
      constantNames: _parseNames(ui, 3, constCount),
      inputSpecs: _parseInputSpecs(ui, inCount),
    );
  }
}

/// Cursor over the UI-info name sections. `_parseNames`/`_parseInputSpecs` re-walk the
/// blob from the start so they stay independent of call order.
class _UiCursor {
  final List<int> ui;
  int pos = 0;
  _UiCursor(this.ui);

  String string() {
    if (pos >= ui.length) return '';
    final n = ui[pos++];
    if (pos + n > ui.length) return '';
    final s = String.fromCharCodes(ui.sublist(pos, pos + n));
    pos += n;
    return s;
  }

  int u8() {
    if (pos >= ui.length) return 0;
    return ui[pos++];
  }
}

/// Walks past the function name and the first `section` name lists, returning the
/// `index`-th name of the requested section (0=inputs, 1=outputs, 2=variables, 3=constants).
List<String> _parseNames(List<int> ui, int section, int count) {
  if (ui.isEmpty || ui[0] != scriptUiInfoVersion) return const [];
  final c = _UiCursor(ui)..pos = 1;
  c.string(); // function name
  for (var s = 0; s < 4; s++) {
    final n = c.u8();
    final names = <String>[];
    for (var i = 0; i < n; i++) {
      names.add(c.string());
    }
    if (s == section) return names;
    // Skip remaining sections for the requested one.
  }
  return const [];
}

String _parseFunctionName(List<int> ui) {
  if (ui.isEmpty || ui[0] != scriptUiInfoVersion) {
    // Legacy/unknown blob: first byte was the name length.
    if (ui.isEmpty) return '';
    final n = ui[0];
    if (1 + n <= ui.length) return String.fromCharCodes(ui.sublist(1, 1 + n));
    return '';
  }
  final c = _UiCursor(ui)..pos = 1;
  return c.string();
}

List<ScriptInputSpec> _parseInputSpecs(List<int> ui, int inputCount) {
  if (ui.isEmpty || ui[0] != scriptUiInfoVersion) return const [];
  final c = _UiCursor(ui)..pos = 1;
  c.string(); // function name
  for (var s = 0; s < 4; s++) {
    final n = c.u8();
    for (var i = 0; i < n; i++) {
      c.string();
    }
  }
  final specs = <ScriptInputSpec>[];
  for (var i = 0; i < inputCount; i++) {
    final uiType = c.u8();
    c.pos += 3; // reserved
    if (c.pos + 12 > ui.length) {
      specs.add(ScriptInputSpec(uiType: uiType));
      continue;
    }
    final min = _rawToNumber(_getU32(ui, c.pos)); c.pos += 4;
    final max = _rawToNumber(_getU32(ui, c.pos)); c.pos += 4;
    final step = _rawToNumber(_getU32(ui, c.pos)); c.pos += 4;
    specs.add(ScriptInputSpec(uiType: uiType, min: min, max: max, step: step));
  }
  return specs;
}
