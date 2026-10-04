/// Script file codec (Docs/Services/Script.md).
///
/// One `SCR_XXX` file holds a script's properties, its input/output/variable/constant
/// ValueInfo tables, constant values, input defaults, instruction symbols and UI info
/// (function/IO/variable/constant names + per-input UI specification).
///
/// The binary packing of the parts the docs leave open is an internal convention shared
/// with the firmware (`Core/Services/Script.h`).
library;

import 'dart:typed_data';

import 'types.dart';

/// Number of loaded script slots. The Scripts range is four banked block types (0x3F4-0x3F7)
/// of 64 instances each, so at most 256 scripts load at once (one global 0..255 index).
const int maxScripts = 256;

/// Stored script files are named `SCR_XXX` (three hex digits), so a file id is 0..0xFFF.
const int maxScriptFiles = 4096;

/// The stored file name for a script file id (`SCR_XXX`, three hex digits).
String scriptFileName(int fileId) =>
    'SCR_${fileId.toRadixString(16).toUpperCase().padLeft(3, '0')}';

/// Parses a `SCR_XXX` name back to its file id, or null if it is not a script file.
int? scriptFileId(String name) {
  final n = name.replaceAll('\x00', '').trim();
  if (!n.startsWith('SCR_')) return null;
  return int.tryParse(n.substring(4), radix: 16);
}

/// File header size: Properties + counts + the four lengths.
const int scriptHeaderSize = 24;

/// Version byte of the UI-info blob (bumped when the layout changes).
const int scriptUiInfoVersion = 2;

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

/// One ValueInfo entry: Type(16) | Size(8) | Flags(8) on the wire (the same layout as
/// [ValueInfo]; `key` is unused because the script block addresses the key separately).
class ScriptValueInfo {
  final DataType type;
  final int size;
  final int flags;

  const ScriptValueInfo({required this.type, required this.size, this.flags = 0});

  Uint8List toBytes() => Uint8List(4)
    ..[0] = type.value & 0xFF
    ..[1] = (type.value >> 8) & 0xFF
    ..[2] = size & 0xFF
    ..[3] = flags;

  static ScriptValueInfo fromBytes(List<int> bytes, [int offset = 0]) => ScriptValueInfo(
        type: DataType.fromValue(bytes[offset] | (bytes[offset + 1] << 8)),
        size: bytes[offset + 2],
        flags: bytes[offset + 3],
      );
}

/// Per-input UI specification (limits are kept as 16.16 fixed point like Number).
class ScriptInputSpec {
  final int uiType;
  final double min;
  final double max;
  final double step;

  /// Named choices for an integer/enum input shown as a dropdown ("custom enum"). Empty for
  /// every other UI style; the stored value stays the plain integer index.
  final List<String> options;

  const ScriptInputSpec({
    this.uiType = ScriptUiType.auto,
    this.min = 0,
    this.max = 0,
    this.step = 0,
    this.options = const <String>[],
  });

  ScriptInputSpec copyWith(
          {int? uiType, double? min, double? max, double? step, List<String>? options}) =>
      ScriptInputSpec(
        uiType: uiType ?? this.uiType,
        min: min ?? this.min,
        max: max ?? this.max,
        step: step ?? this.step,
        options: options ?? this.options,
      );
}

/// Register field categories of a loaded script (the Scripts range 0x3F4-0x3F7). Only Input and Output
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
  DataType.blockInfo,
];

/// A sensible byte size for a newly declared value of [type] (used as the default in the
/// create-script dialog; the user can override it).
int defaultSizeForType(DataType type) => switch (type) {
      DataType.bool_ => 1,
      DataType.enum_ => 1,
      DataType.id || DataType.devType => 2,
      DataType.number || DataType.integer || DataType.uint32 => 4,
      DataType.colour => 4,
      DataType.vector => 8,
      DataType.matrix => 28, // 2x3 wire value: u16 h, u16 w + 6 Numbers
      DataType.string => 16,
      DataType.filename => 8,
      DataType.sn => 14,
      DataType.blockInfo => 4,
      _ => 4,
    };

// Codec helpers come from types.dart: uint32ToBytes/uint32FromBytes and
// numberToBytes/numberFromBytes (Q16.16).

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

/// Builds a `SCR_XXX` file image.
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
      out.addAll(numberToBytes(spec.min));
      out.addAll(numberToBytes(spec.max));
      out.addAll(numberToBytes(spec.step));
    }
    // v2: named enum choices, one label list per input (count 0 when unused).
    for (var i = 0; i < inputs.length; i++) {
      final spec = i < inputSpecs.length ? inputSpecs[i] : const ScriptInputSpec();
      putNames(spec.options, spec.options.length);
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

    out.setAll(0, uint32ToBytes(properties));
    out[4] = inputs.length;
    out[5] = outputs.length;
    out[6] = variables.length;
    out[7] = constants.length;
    out.setAll(8, uint32ToBytes(constBlob.length));
    out.setAll(12, uint32ToBytes(defBlob.length));
    out.setAll(16, uint32ToBytes(instructions.length));
    out.setAll(20, uint32ToBytes(ui.length));

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

/// A parsed `SCR_XXX` image.
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
    final properties = uint32FromBytes(bytes, 0);
    final inCount = bytes[4];
    final outCount = bytes[5];
    final varCount = bytes[6];
    final constCount = bytes[7];
    final constLen = uint32FromBytes(bytes, 8);
    final defLen = uint32FromBytes(bytes, 12);
    final instrLen = uint32FromBytes(bytes, 16);
    final uiLen = uint32FromBytes(bytes, 20);

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
    final info = _parseUiInfo(ui, inCount);

    return ScriptFileData(
      properties: properties,
      inputs: inputs,
      outputs: outputs,
      variables: variables,
      constants: constants,
      constantValues: constantValues,
      inputDefaults: inputDefaults,
      instructions: instructions,
      functionName: info.functionName,
      inputNames: info.inputNames,
      outputNames: info.outputNames,
      variableNames: info.variableNames,
      constantNames: info.constantNames,
      inputSpecs: info.inputSpecs,
    );
  }
}

/// Cursor over the UI-info blob (length-prefixed strings / bytes).
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

/// Parses the whole UI-info blob in a single walk: function name, the four name lists and
/// the per-input specifications.
({
  String functionName,
  List<String> inputNames,
  List<String> outputNames,
  List<String> variableNames,
  List<String> constantNames,
  List<ScriptInputSpec> inputSpecs,
}) _parseUiInfo(List<int> ui, int inputCount) {
  if (ui.isEmpty) {
    return (
      functionName: '',
      inputNames: const <String>[],
      outputNames: const <String>[],
      variableNames: const <String>[],
      constantNames: const <String>[],
      inputSpecs: const <ScriptInputSpec>[],
    );
  }
  if (ui[0] != scriptUiInfoVersion) {
    // Unknown/older blob (UI info v1 is no longer supported): the first byte was the
    // function-name length in the pre-versioned layout, so recover the name if we can.
    final n = ui[0];
    final name = (1 + n <= ui.length) ? String.fromCharCodes(ui.sublist(1, 1 + n)) : '';
    return (
      functionName: name,
      inputNames: const <String>[],
      outputNames: const <String>[],
      variableNames: const <String>[],
      constantNames: const <String>[],
      inputSpecs: const <ScriptInputSpec>[],
    );
  }

  final c = _UiCursor(ui)..pos = 1;
  final functionName = c.string();
  List<String> names() {
    final n = c.u8();
    return [for (var i = 0; i < n; i++) c.string()];
  }

  final inputNames = names();
  final outputNames = names();
  final variableNames = names();
  final constantNames = names();

  final specs = <ScriptInputSpec>[];
  for (var i = 0; i < inputCount; i++) {
    final uiType = c.u8();
    c.pos += 3; // reserved
    if (c.pos + 12 > ui.length) {
      specs.add(ScriptInputSpec(uiType: uiType));
      continue;
    }
    final min = numberFromBytes(ui, c.pos); c.pos += 4;
    final max = numberFromBytes(ui, c.pos); c.pos += 4;
    final step = numberFromBytes(ui, c.pos); c.pos += 4;
    specs.add(ScriptInputSpec(uiType: uiType, min: min, max: max, step: step));
  }

  // enum option labels (UI info v2).
  if (ui[0] == scriptUiInfoVersion) {
    for (var i = 0; i < specs.length && c.pos < ui.length; i++) {
      final n = c.u8();
      final options = <String>[];
      for (var k = 0; k < n && c.pos < ui.length; k++) {
        options.add(c.string());
      }
      if (options.isNotEmpty) specs[i] = specs[i].copyWith(options: options);
    }
  }
  return (
    functionName: functionName,
    inputNames: inputNames,
    outputNames: outputNames,
    variableNames: variableNames,
    constantNames: constantNames,
    inputSpecs: specs,
  );
}
