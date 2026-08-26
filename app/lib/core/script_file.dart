/// Script storage file format (Docs/Services/Script.md "File blocks").
///
/// Header (32 B): name char[16], input/output/variable/constant counts (u8 each),
/// input meta/value lengths (u16 each), constant values / instruction lengths (u32 each).
/// Then: input meta (dict BlockMeta + per-input key BlockMeta), input values (aligned
/// defaults), output names (count x 16), variable names (count x 16), constant meta
/// (count x 4), constant values (aligned), instructions (4-byte symbols).
library;

import 'dart:typed_data';

import 'script_asm.dart';
import 'types.dart';

/// Interaction style for a script input - how the app presents it (button, switch,
/// picker, slider, plain text). "automatic" derives a default from the data type.
enum InputStyle {
  automatic(0),
  button(1),
  switch_(2),
  picker(3),
  slider(4),
  text(5);

  final int value;
  const InputStyle(this.value);

  String get label => switch (this) {
        InputStyle.automatic => 'Automatic',
        InputStyle.button => 'Button',
        InputStyle.switch_ => 'Switch',
        InputStyle.picker => 'Picker',
        InputStyle.slider => 'Slider',
        InputStyle.text => 'Text field',
      };

  static InputStyle fromValue(int value) {
    for (final s in InputStyle.values) {
      if (s.value == value) return s;
    }
    return InputStyle.automatic;
  }

  /// The style suggested for a data type (used when "automatic").
  static InputStyle forType(DataType type) => switch (type) {
        DataType.bool_ => InputStyle.switch_,
        DataType.enum_ => InputStyle.picker,
        DataType.string => InputStyle.text,
        DataType.number || DataType.integer => InputStyle.slider,
        DataType.colour => InputStyle.picker,
        _ => InputStyle.text,
      };

  /// Interaction styles the given expected type may use (Automatic is always
  /// allowed and resolves to [forType]). Used to restrict the style picker.
  static List<InputStyle> allowedFor(DataType type) {
    switch (type) {
      case DataType.bool_:
        return [InputStyle.automatic, InputStyle.switch_, InputStyle.button, InputStyle.text];
      case DataType.enum_:
        return [InputStyle.automatic, InputStyle.picker, InputStyle.button];
      case DataType.number:
      case DataType.integer:
        return [InputStyle.automatic, InputStyle.slider, InputStyle.text];
      case DataType.string:
        return [InputStyle.automatic, InputStyle.text];
      case DataType.colour:
        return [InputStyle.automatic, InputStyle.picker];
      default:
        return [InputStyle.automatic, InputStyle.text];
    }
  }
}

/// One script input: a keyed-dictionary entry (key id, expected type, default value
/// and interaction style).
class ScriptInput {
  int key;
  int flagsAndType;
  List<int> defaultValue;
  InputStyle style;

  ScriptInput({
    required this.key,
    required this.flagsAndType,
    required this.defaultValue,
    this.style = InputStyle.automatic,
  });

  DataType get dataType => DataType.fromValue(flagsAndType & 0x03FF);
  int get valueLen => defaultValue.length;
}

/// One constant (read-only variable stored in the file).
class ScriptConstant {
  int flagsAndType;
  List<int> value;

  ScriptConstant({required this.flagsAndType, required this.value});

  DataType get dataType => DataType.fromValue(flagsAndType & 0x03FF);
}

/// A parsed (or to-be-serialised) script file.
class ScriptFileData {
  String name;
  List<ScriptInput> inputs;
  List<String> outputNames;
  List<String> variableNames;
  List<ScriptConstant> constants;
  List<ScriptSymbol> instructions;

  ScriptFileData({
    this.name = '',
    this.inputs = const [],
    this.outputNames = const [],
    this.variableNames = const [],
    this.constants = const [],
    this.instructions = const [],
  });

  int get inputCount => inputs.length;
  int get outputCount => outputNames.length;
  int get variableCount => variableNames.length;
  int get constantCount => constants.length;

  /// Serialises the file to bytes (little-endian, bounded to uint32 sizes).
  List<int> toBytes() {
    final nameBytes = _pad16(name);
    final inputMeta = _buildInputMeta();
    final inputValues = <int>[];
    for (final input in inputs) {
      inputValues.addAll(input.defaultValue);
      _pad4(inputValues);
    }
    final outputNames = _namesSection(this.outputNames);
    final variableNames = _namesSection(this.variableNames);
    final constantMeta = <int>[];
    final constantValues = <int>[];
    for (final c in constants) {
      constantMeta.addAll(_metaToBytes(c.flagsAndType, c.value.length));
      constantValues.addAll(c.value);
      _pad4(constantValues);
    }
    final instr = <int>[];
    for (final s in instructions) {
      instr.addAll(s.toBytes());
    }

    final out = <int>[];
    out.addAll(nameBytes);
    out.add(inputCount & 0xFF);
    out.add(outputCount & 0xFF);
    out.add(variableCount & 0xFF);
    out.add(constantCount & 0xFF);
    out.addAll(_u16(inputMeta.length));
    out.addAll(_u16(inputValues.length));
    out.addAll(_u32(constantValues.length));
    out.addAll(_u32(instr.length));
    out.addAll(inputMeta);
    out.addAll(inputValues);
    out.addAll(outputNames);
    out.addAll(variableNames);
    out.addAll(constantMeta);
    out.addAll(constantValues);
    out.addAll(instr);
    return out;
  }

  List<int> _buildInputMeta() {
    final meta = <int>[];
    // Dict BlockMeta: type undefined, Key invalid, Size = number of keys.
    meta.addAll(_metaToBytes(DataType.undefined.value, inputCount));
    for (final input in inputs) {
      // Per-key BlockMeta: type + flags, Key = the input's dict key, Size = value length,
      // followed by the interaction-style byte.
      meta.addAll(Uint8List(4)
        ..[0] = input.flagsAndType & 0xFF
        ..[1] = (input.flagsAndType >> 8) & 0xFF
        ..[2] = input.key & 0xFF
        ..[3] = input.valueLen & 0xFF);
      meta.add(input.style.value & 0xFF);
    }
    return meta;
  }

  /// Parses a raw file buffer. Returns null when the structure is invalid.
  static ScriptFileData? parse(List<int> bytes) {
    if (bytes.length < 32) return null;
    var cursor = 0;
    String nameOf(List<int> b) {
      final text = String.fromCharCodes(b.takeWhile((x) => x != 0));
      return text.trimRight();
    }

    final name = nameOf(bytes.sublist(0, 16));
    cursor += 16;
    final inputCount = bytes[cursor++];
    final outputCount = bytes[cursor++];
    final variableCount = bytes[cursor++];
    final constantCount = bytes[cursor++];
    final inputMetaLen = _le16(bytes, cursor);
    cursor += 2;
    final inputValueLen = _le16(bytes, cursor);
    cursor += 2;
    final constantValuesLen = _le32(bytes, cursor);
    cursor += 4;
    final instructionLen = _le32(bytes, cursor);
    cursor += 4;

    if (bytes.length <
        cursor + inputMetaLen + inputValueLen + outputCount * 16 +
            variableCount * 16 + constantCount * 4 + constantValuesLen + instructionLen) {
      return null;
    }

    final inputs = <ScriptInput>[];
    if (inputCount > 0) {
      // Input meta: dict BlockMeta (4) + per-input [key BlockMeta (4) + style byte (1)].
      // Legacy files omit the style byte (4 bytes per input) - style defaults to automatic.
      final legacy = inputMetaLen == 4 + inputCount * 4;
      final newFormat = inputMetaLen == 4 + inputCount * 5;
      if (!legacy && !newFormat) return null;
      final metaStart = cursor;
      cursor += 4; // skip dict meta
      var valueCursor = metaStart + inputMetaLen;
      for (var i = 0; i < inputCount; i++) {
        final meta = _metaFromBytes(bytes, cursor);
        cursor += 4;
        final style = newFormat ? InputStyle.fromValue(bytes[cursor++]) : InputStyle.automatic;
        final valueLen = meta.size;
        final vStart = valueCursor;
        valueCursor += (valueLen + 3) & ~3;
        if (valueCursor > metaStart + inputMetaLen + inputValueLen) return null;
        inputs.add(ScriptInput(
          key: meta.key,
          flagsAndType: meta.flagsAndType,
          defaultValue: bytes.sublist(vStart, vStart + valueLen),
          style: style,
        ));
      }
      cursor = metaStart + inputMetaLen + inputValueLen;
    } else {
      cursor += inputMetaLen + inputValueLen;
    }

    List<String> readNames(int count) {
      final names = <String>[];
      for (var i = 0; i < count; i++) {
        names.add(nameOf(bytes.sublist(cursor, cursor + 16)));
        cursor += 16;
      }
      return names;
    }

    final outputNames = readNames(outputCount);
    final variableNames = readNames(variableCount);

    final constants = <ScriptConstant>[];
    for (var i = 0; i < constantCount; i++) {
      final meta = _metaFromBytes(bytes, cursor);
      cursor += 4;
      final valueLen = meta.size;
      final vStart = cursor;
      cursor += (valueLen + 3) & ~3;
      if (cursor > bytes.length) return null;
      constants.add(ScriptConstant(
        flagsAndType: meta.flagsAndType,
        value: bytes.sublist(vStart, vStart + valueLen),
      ));
    }

    final instr = <ScriptSymbol>[];
    var iCursor = cursor;
    final instrEnd = iCursor + instructionLen;
    while (iCursor + 4 <= instrEnd) {
      instr.add(ScriptSymbol.fromBytes(bytes, iCursor));
      iCursor += 4;
    }

    return ScriptFileData(
      name: name,
      inputs: inputs,
      outputNames: outputNames,
      variableNames: variableNames,
      constants: constants,
      instructions: instr,
    );
  }

  List<int> _namesSection(List<String> names) {
    final out = <int>[];
    for (final n in names) {
      out.addAll(_pad16(n));
    }
    return out;
  }
}

List<int> _pad16(String s) {
  final bytes = List<int>.filled(16, 0);
  final raw = s.codeUnits;
  for (var i = 0; i < 16 && i < raw.length; i++) {
    bytes[i] = raw[i] & 0xFF;
  }
  return bytes;
}

void _pad4(List<int> bytes) {
  while (bytes.length % 4 != 0) {
    bytes.add(0);
  }
}

List<int> _metaToBytes(int flagsAndType, int size) => Uint8List(4)
  ..[0] = flagsAndType & 0xFF
  ..[1] = (flagsAndType >> 8) & 0xFF
  ..[2] = 0xFF // Key: invalid for plain fields
  ..[3] = size & 0xFF;

BlockMeta _metaFromBytes(List<int> bytes, int offset) => BlockMeta.fromBytes(bytes, offset);

int _le16(List<int> bytes, int offset) =>
    bytes[offset] | (bytes[offset + 1] << 8);

int _le32(List<int> bytes, int offset) =>
    bytes[offset] |
    (bytes[offset + 1] << 8) |
    (bytes[offset + 2] << 16) |
    (bytes[offset + 3] << 24);

List<int> _u16(int v) => [v & 0xFF, (v >> 8) & 0xFF];

List<int> _u32(int v) => [
      v & 0xFF,
      (v >> 8) & 0xFF,
      (v >> 16) & 0xFF,
      (v >> 24) & 0xFF,
    ];