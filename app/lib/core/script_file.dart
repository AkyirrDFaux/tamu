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

import 'script_instructions.dart';
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

/// File header: a 16-bit Properties word, then five uint8 counts (Input, Output, Variable,
/// Constant, Function), one padding byte, and the four uint32 lengths - 24 bytes total
/// (Docs/Services/Script.md "File Blocks").
const int scriptHeaderSize = 24;

/// Size of one function-reservation record: the file a linked global function lives in, its
/// memory-space offset and its instruction-space offset (Docs "File Blocks").
const int scriptFunctionReservationSize = 16;

/// Version byte of the UI-info blob. Kept at 2: the firmware reads the function name from a v2
/// blob (`UiBlob[0] == 2`), so the function block is a *trailing append* the firmware ignores,
/// not a version bump. The v2 sections are self-delimiting, so any bytes after them are the
/// function block.
const int scriptUiInfoVersion = 2;

/// The file id stored in a slot-map entry for an empty slot (Docs CID 0: "0xFFFF for an empty
/// slot").
const int scriptSlotEmpty = 0xFFFF;

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
  DataType.bool_ => const [
    ScriptUiType.auto,
    ScriptUiType.toggle,
    ScriptUiType.button,
  ],
  DataType.number || DataType.integer || DataType.uint32 => const [
    ScriptUiType.auto,
    ScriptUiType.number,
    ScriptUiType.slider,
  ],
  DataType.enum_ ||
  DataType.devType => const [ScriptUiType.auto, ScriptUiType.dropdown],
  DataType.colour => const [ScriptUiType.auto, ScriptUiType.swatch],
  _ => const [ScriptUiType.auto],
};

/// Min/max/step limits only apply to the numeric input styles.
bool uiStyleSupportsLimits(int uiType) =>
    uiType == ScriptUiType.number || uiType == ScriptUiType.slider;

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

  ScriptInputSpec copyWith({
    int? uiType,
    double? min,
    double? max,
    double? step,
    List<String>? options,
  }) => ScriptInputSpec(
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
  // Load-related states, appended so the existing values keep their numbers (Docs/Services/
  // Script.md "State"): the slot's space exists but it cannot run yet, or the load was refused
  // and the slot keeps its reservation until an explicit unload.
  static const reservedNotReady = 6;
  static const loadFailed = 7;

  static String label(int state) => switch (state) {
    stopped => 'Stopped',
    running => 'Running',
    paused => 'Paused',
    waiting => 'Waiting',
    finished => 'Finished',
    error => 'Error',
    reservedNotReady => 'Reserved (not ready)',
    loadFailed => 'Load failed',
    _ => 'Unknown ($state)',
  };
}

/// Properties bits (Docs/Services/Script.md "Properties").
class ScriptProperties {
  static const loadOnBoot = 1 << 0;
  static const runOnLoad = 1 << 1;

  /// The file is a function library, not a script.
  static const functionLibrary = 1 << 2;

  /// The uint8 slot a load-on-boot script loads into (bits 8-15); a boot load is
  /// deterministic rather than the identity slot or the first free one.
  static const slotShift = 8;
  static const slotMask = 0xFF << slotShift;
}

/// The slot index stored in a Properties word (bits 8-15).
int scriptPropertiesSlot(int properties) =>
    (properties & ScriptProperties.slotMask) >> ScriptProperties.slotShift;

/// [properties] with its slot field (bits 8-15) set to [slot].
int scriptPropertiesWithSlot(int properties, int slot) =>
    (properties & ~ScriptProperties.slotMask) |
    ((slot & 0xFF) << ScriptProperties.slotShift);

/// The kind of a script function (Docs/Services/Script.md "Functions and Macros"). Local and
/// macro are laid out by the app into the instruction stream; global is linked at load.
enum ScriptFunctionKind {
  local,
  macro,
  global;

  static const int _localCode = 0;
  static const int _macroCode = 1;
  static const int _globalCode = 2;

  int get code => switch (this) {
    local => _localCode,
    macro => _macroCode,
    global => _globalCode,
  };

  static ScriptFunctionKind fromCode(int code) => switch (code) {
    _macroCode => macro,
    _globalCode => global,
    _ => local,
  };

  String get label => switch (this) {
    local => 'Local',
    macro => 'Macro',
    global => 'Global',
  };
}

/// One declared function (Docs/Services/Script.md "Functions and Macros").
///
/// A local/macro function owns its [body]; a global function carries the library [globalFileId]
/// and the two offsets a function reservation records (its body lives in the library file).
class ScriptFunction {
  String name;
  ScriptFunctionKind kind;
  List<ScriptLine> body;
  int globalFileId;
  int globalMemOffset;
  int globalInstrOffset;

  ScriptFunction({
    this.name = '',
    this.kind = ScriptFunctionKind.local,
    List<ScriptLine>? body,
    this.globalFileId = 0,
    this.globalMemOffset = 0,
    this.globalInstrOffset = 0,
  }) : body = body ?? [];

  bool get isGlobal => kind == ScriptFunctionKind.global;

  /// True when the app lays the body out (local or macro).
  bool get hasBody => !isGlobal;

  ScriptFunction copy() => ScriptFunction(
    name: name,
    kind: kind,
    body: [for (final l in body) l.clone()],
    globalFileId: globalFileId,
    globalMemOffset: globalMemOffset,
    globalInstrOffset: globalInstrOffset,
  );
}

/// A 16-byte function-reservation record for a referenced global function.
///
/// Script.md does not pin the layout; the firmware (`Core/Services/ScriptDefs.h`
/// `ParseFuncReservation`, the parallel Stage 3 change) does, and this matches it exactly:
/// `FileId u16 | MemOffset u16 | InstrOffset u16 | 10 reserved (zero)`, little-endian.
List<int> encodeFunctionReservation({
  required int fileId,
  required int memOffset,
  required int instrOffset,
}) {
  final out = List<int>.filled(scriptFunctionReservationSize, 0);
  out[0] = fileId & 0xFF;
  out[1] = (fileId >> 8) & 0xFF;
  out[2] = memOffset & 0xFF;
  out[3] = (memOffset >> 8) & 0xFF;
  out[4] = instrOffset & 0xFF;
  out[5] = (instrOffset >> 8) & 0xFF;
  // bytes 6..16 are reserved zero.
  return out;
}

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
  DataType.text,
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
  DataType.text => 16,
  DataType.filename => 8,
  DataType.sn => 14,
  DataType.blockInfo => 4,
  _ => 4,
};

// Codec helpers come from types.dart: uint32ToBytes/uint32FromBytes and
// numberToBytes/numberFromBytes (Q16.16).

/// Packs `values` (one byte list per entry) into the 4-byte-strided blob the firmware
/// expects, padding/clamping each entry to its declared ValueInfo size.
Uint8List packScriptValues(List<ValueInfo> infos, List<List<int>> values) {
  var total = 0;
  for (final info in infos) {
    total += align4(info.size);
  }
  final out = Uint8List(total);
  var offset = 0;
  for (var i = 0; i < infos.length; i++) {
    final info = infos[i];
    final value = i < values.length ? values[i] : const <int>[];
    final n = value.length < info.size ? value.length : info.size;
    out.setRange(offset, offset + n, value);
    offset += align4(info.size);
  }
  return out;
}

/// Extracts one entry's bytes from a strided blob.
Uint8List unpackScriptValue(List<ValueInfo> infos, List<int> blob, int index) {
  var offset = 0;
  for (var i = 0; i < infos.length; i++) {
    if (i == index) {
      final size = infos[i].size;
      final start = offset;
      final end = offset + size;
      if (end > blob.length) return Uint8List(0);
      return Uint8List.fromList(blob.sublist(start, end));
    }
    offset += align4(infos[i].size);
  }
  return Uint8List(0);
}

/// Builds a `SCR_XXX` file image.
class ScriptFileBuilder {
  final int properties;
  final List<ValueInfo> inputs;
  final List<ValueInfo> outputs;
  final List<ValueInfo> variables;
  final List<ValueInfo> constants;
  final List<List<int>> inputDefaults;
  final List<List<int>> constantValues;
  final List<int> instructions;
  final String functionName;
  final List<String> inputNames;
  final List<String> outputNames;
  final List<String> variableNames;
  final List<String> constantNames;
  final List<ScriptInputSpec> inputSpecs;

  /// Function reservations (one 16-byte record per linked global function). The count is the
  /// header's Function count.
  final List<List<int>> functionReservations;

  /// Declared functions, serialised into the app-only UI blob (local/macro bodies with their
  /// `Call` targets + global-function parameters). Empty leaves the blob at its v2 shape.
  final List<ScriptFunction> functions;

  /// The line count of the main program at the head of the assembled instruction stream. Read
  /// back to split main + bodies when [functions] is non-empty.
  final int functionMainLineCount;

  /// Main-program `Call` line index -> function index (the UI blob preserves which function a
  /// main-program Call targets, since assembly rewrites the wire operand to a bare line index).
  final Map<int, int> mainCallTargets;

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
    this.functionReservations = const [],
    this.functions = const [],
    this.functionMainLineCount = 0,
    this.mainCallTargets = const {},
  });

  /// Packs the function reservations, one record per entry padded/clamped to 16 bytes.
  Uint8List _functionBlob() {
    final out = Uint8List(
      functionReservations.length * scriptFunctionReservationSize,
    );
    var offset = 0;
    for (final record in functionReservations) {
      final n = record.length < scriptFunctionReservationSize
          ? record.length
          : scriptFunctionReservationSize;
      out.setRange(offset, offset + n, record);
      offset += scriptFunctionReservationSize;
    }
    return out;
  }

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
      final spec = i < inputSpecs.length
          ? inputSpecs[i]
          : const ScriptInputSpec();
      out.add(spec.uiType & 0xFF);
      out.addAll([0, 0, 0]);
      out.addAll(numberToBytes(spec.min));
      out.addAll(numberToBytes(spec.max));
      out.addAll(numberToBytes(spec.step));
    }
    // v2: named enum choices, one label list per input (count 0 when unused).
    for (var i = 0; i < inputs.length; i++) {
      final spec = i < inputSpecs.length
          ? inputSpecs[i]
          : const ScriptInputSpec();
      putNames(spec.options, spec.options.length);
    }

    // The function block (a trailing append after the v2 sections; the firmware ignores it).
    // Omitted entirely when there are no functions, so an ordinary script's blob is unchanged.
    if (functions.isNotEmpty) {
      void putU16(int v) => out.addAll([v & 0xFF, (v >> 8) & 0xFF]);
      void putU32(int v) => out.addAll(uint32ToBytes(v));
      out.add(functions.length & 0xFF);
      putU16(functionMainLineCount);
      putU16(mainCallTargets.length);
      final targetKeys = mainCallTargets.keys.toList()..sort();
      for (final key in targetKeys) {
        putU16(key);
        putU16(mainCallTargets[key]!);
      }
      for (final f in functions) {
        putString(f.name);
        out.add(f.kind.code & 0xFF);
        if (f.isGlobal) {
          putU32(f.globalFileId);
          putU32(f.globalMemOffset);
          putU32(f.globalInstrOffset);
        } else {
          putU16(f.body.length);
          for (final line in f.body) {
            final target = line.functionTarget;
            out.add(target == null || target > 0xFE ? 0xFF : target);
            final bytes = encodeScriptLines([line]);
            putU16(bytes.length);
            out.addAll(bytes);
          }
        }
      }
    }
    return Uint8List.fromList(out);
  }

  Uint8List build() {
    final constBlob = packScriptValues(constants, constantValues);
    final defBlob = packScriptValues(inputs, inputDefaults);
    final funcBlob = _functionBlob();
    final ui = _uiInfo();

    final metaLen =
        (inputs.length + outputs.length + variables.length + constants.length) *
        4;
    final total =
        scriptHeaderSize +
        metaLen +
        constBlob.length +
        defBlob.length +
        funcBlob.length +
        instructions.length +
        ui.length;
    final out = Uint8List(total);

    // 16-bit Properties, then Input/Output/Variable/Constant/Function counts, one padding byte.
    out[0] = properties & 0xFF;
    out[1] = (properties >> 8) & 0xFF;
    out[2] = inputs.length;
    out[3] = outputs.length;
    out[4] = variables.length;
    out[5] = constants.length;
    out[6] = functionReservations.length;
    out[7] = 0; // reserved: pads the counts to 4-byte alignment
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
    out.setRange(offset, offset + funcBlob.length, funcBlob);
    offset += funcBlob.length;
    out.setRange(offset, offset + instructions.length, instructions);
    offset += instructions.length;
    out.setRange(offset, offset + ui.length, ui);
    return out;
  }
}

/// A parsed `SCR_XXX` image.
class ScriptFileData {
  final int properties;
  final List<ValueInfo> inputs;
  final List<ValueInfo> outputs;
  final List<ValueInfo> variables;
  final List<ValueInfo> constants;
  final Uint8List constantValues;
  final Uint8List inputDefaults;

  /// Function reservations: one 16-byte record per linked global function.
  final Uint8List functionReservations;

  /// Declared functions decoded from the UI blob (empty for a v2 blob).
  final List<ScriptFunction> functions;

  /// The line count of the main program at the head of [instructions] (0 when no function
  /// block is present).
  final int functionMainLineCount;

  /// Main-program `Call` line index -> function index.
  final Map<int, int> mainCallTargets;

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
    required this.functionReservations,
    required this.instructions,
    required this.functionName,
    required this.inputNames,
    required this.outputNames,
    required this.variableNames,
    required this.constantNames,
    required this.inputSpecs,
    this.functions = const [],
    this.functionMainLineCount = 0,
    this.mainCallTargets = const {},
  });

  /// Number of declared functions (the header's Function count).
  int get functionCount =>
      functionReservations.length ~/ scriptFunctionReservationSize;

  /// Value bytes of one constant.
  Uint8List constantValue(int index) =>
      unpackScriptValue(constants, constantValues, index);

  /// Default bytes of one input.
  Uint8List inputDefault(int index) =>
      unpackScriptValue(inputs, inputDefaults, index);

  String nameOf(List<String> names, int index, String prefix) =>
      index < names.length && names[index].isNotEmpty
      ? names[index]
      : '$prefix $index';

  /// Parses an image. Throws [FormatException] on a truncated/inconsistent file.
  static ScriptFileData parse(List<int> bytes) {
    if (bytes.length < scriptHeaderSize) {
      throw const FormatException('script file shorter than the header');
    }
    // 16-bit Properties, then Input/Output/Variable/Constant/Function counts, one padding byte.
    final properties = bytes[0] | (bytes[1] << 8);
    final inCount = bytes[2];
    final outCount = bytes[3];
    final varCount = bytes[4];
    final constCount = bytes[5];
    final funcCount = bytes[6];
    // bytes[7] is the reserved padding byte.
    final constLen = uint32FromBytes(bytes, 8);
    final defLen = uint32FromBytes(bytes, 12);
    final instrLen = uint32FromBytes(bytes, 16);
    final uiLen = uint32FromBytes(bytes, 20);

    final metaLen = (inCount + outCount + varCount + constCount) * 4;
    final funcLen = funcCount * scriptFunctionReservationSize;
    final need =
        scriptHeaderSize +
        metaLen +
        constLen +
        defLen +
        funcLen +
        instrLen +
        uiLen;
    if (need > bytes.length) {
      throw const FormatException('script file lengths exceed the file size');
    }

    var offset = scriptHeaderSize;
    List<ValueInfo> readMetas(int count) {
      final list = <ValueInfo>[];
      for (var i = 0; i < count; i++) {
        list.add(ValueInfo.fromBytes(bytes, offset));
        offset += 4;
      }
      return list;
    }

    final inputs = readMetas(inCount);
    final outputs = readMetas(outCount);
    final variables = readMetas(varCount);
    final constants = readMetas(constCount);

    final constantValues = Uint8List.fromList(
      bytes.sublist(offset, offset + constLen),
    );
    offset += constLen;
    final inputDefaults = Uint8List.fromList(
      bytes.sublist(offset, offset + defLen),
    );
    offset += defLen;
    final functionReservations = Uint8List.fromList(
      bytes.sublist(offset, offset + funcLen),
    );
    offset += funcLen;
    final instructions = Uint8List.fromList(
      bytes.sublist(offset, offset + instrLen),
    );
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
      functionReservations: functionReservations,
      instructions: instructions,
      functionName: info.functionName,
      inputNames: info.inputNames,
      outputNames: info.outputNames,
      variableNames: info.variableNames,
      constantNames: info.constantNames,
      inputSpecs: info.inputSpecs,
      functions: info.functions,
      functionMainLineCount: info.functionMainLineCount,
      mainCallTargets: info.mainCallTargets,
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

  int u16() {
    if (pos + 2 > ui.length) return 0;
    final v = u16FromBytes(ui, pos);
    pos += 2;
    return v;
  }

  int u32() {
    if (pos + 4 > ui.length) return 0;
    final v = uint32FromBytes(ui, pos);
    pos += 4;
    return v;
  }
}

/// Parses the whole UI-info blob in a single walk: function name, the four name lists, the
/// per-input specifications and (v3) the function block.
({
  String functionName,
  List<String> inputNames,
  List<String> outputNames,
  List<String> variableNames,
  List<String> constantNames,
  List<ScriptInputSpec> inputSpecs,
  List<ScriptFunction> functions,
  int functionMainLineCount,
  Map<int, int> mainCallTargets,
})
_parseUiInfo(List<int> ui, int inputCount) {
  if (ui.isEmpty) {
    return (
      functionName: '',
      inputNames: const <String>[],
      outputNames: const <String>[],
      variableNames: const <String>[],
      constantNames: const <String>[],
      inputSpecs: const <ScriptInputSpec>[],
      functions: const <ScriptFunction>[],
      functionMainLineCount: 0,
      mainCallTargets: const <int, int>{},
    );
  }
  final version = ui[0];
  if (version != scriptUiInfoVersion) {
    // Unknown/older blob (UI info v1 is no longer supported): the first byte was the
    // function-name length in the pre-versioned layout, so recover the name if we can.
    final n = ui[0];
    final name = (1 + n <= ui.length)
        ? String.fromCharCodes(ui.sublist(1, 1 + n))
        : '';
    return (
      functionName: name,
      inputNames: const <String>[],
      outputNames: const <String>[],
      variableNames: const <String>[],
      constantNames: const <String>[],
      inputSpecs: const <ScriptInputSpec>[],
      functions: const <ScriptFunction>[],
      functionMainLineCount: 0,
      mainCallTargets: const <int, int>{},
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
    final min = numberFromBytes(ui, c.pos);
    c.pos += 4;
    final max = numberFromBytes(ui, c.pos);
    c.pos += 4;
    final step = numberFromBytes(ui, c.pos);
    c.pos += 4;
    specs.add(ScriptInputSpec(uiType: uiType, min: min, max: max, step: step));
  }

  // enum option labels (UI info v2+).
  for (var i = 0; i < specs.length && c.pos < ui.length; i++) {
    final n = c.u8();
    final options = <String>[];
    for (var k = 0; k < n && c.pos < ui.length; k++) {
      options.add(c.string());
    }
    if (options.isNotEmpty) specs[i] = specs[i].copyWith(options: options);
  }

  // Function block (a trailing append after the v2 sections; the firmware ignores it).
  final functions = <ScriptFunction>[];
  var mainLineCount = 0;
  final mainCallTargets = <int, int>{};
  if (c.pos < ui.length) {
    final functionCount = c.u8();
    if (functionCount > 0 && c.pos < ui.length) {
      mainLineCount = c.u16();
      final targetCount = c.u16();
      for (var i = 0; i < targetCount && c.pos + 4 <= ui.length; i++) {
        mainCallTargets[c.u16()] = c.u16();
      }
      for (var i = 0; i < functionCount && c.pos < ui.length; i++) {
        final name = c.string();
        final kind = ScriptFunctionKind.fromCode(c.u8());
        if (kind == ScriptFunctionKind.global) {
          final fileId = c.u32();
          final memOffset = c.u32();
          final instrOffset = c.u32();
          functions.add(
            ScriptFunction(
              name: name,
              kind: kind,
              globalFileId: fileId,
              globalMemOffset: memOffset,
              globalInstrOffset: instrOffset,
            ),
          );
        } else {
          final lineCount = c.u16();
          final body = <ScriptLine>[];
          for (var l = 0; l < lineCount && c.pos < ui.length; l++) {
            final target = c.u8();
            final len = c.u16();
            if (c.pos + len > ui.length) break;
            final decoded = decodeScriptLines(ui.sublist(c.pos, c.pos + len));
            c.pos += len;
            if (decoded.isNotEmpty) {
              final line = decoded.first;
              line.functionTarget = target == 0xFF ? null : target;
              body.add(line);
            }
          }
          functions.add(ScriptFunction(name: name, kind: kind, body: body));
        }
      }
    }
  }

  return (
    functionName: functionName,
    inputNames: inputNames,
    outputNames: outputNames,
    variableNames: variableNames,
    constantNames: constantNames,
    inputSpecs: specs,
    functions: functions,
    functionMainLineCount: mainLineCount,
    mainCallTargets: mainCallTargets,
  );
}
