/// Reports what a setup's scripts cost, per script, from the setup JSON alone.
///
/// Card t_11b675f9 (the encoder emits small literals as predefines instead of constants) needs a
/// measurement it can be re-run against: per script the constant count, the script image size,
/// its constant-space RAM, and - the part the card actually gates - the EXECUTED meaning of every
/// line, so a before/after pair can be compared for identical instructions, destinations and
/// operand values rather than for identical bytes.
///
/// The script image is built through the app's own codec (`BackupScript.toDraft().toImage()`),
/// and every symbol is then resolved the way the device resolves it (`firmware/src/Core/Services/
/// ScriptVm.h`, `Resolve` + `ToScalar`): predefines are materialised from their subtype, a
/// constant from its ValueInfo and value bytes, and the result is reduced to what an instruction
/// consumes - a numeric scalar's Q16.16 word and its truncated integer, or a raw
/// `(dtype, size, bytes)` for a non-numeric value. A constant and the predefine that carries the
/// same value therefore print the same token, which is what makes the before/after diff prove
/// "same effect" instead of "same file".
///
/// ```
/// dart run tool/script_literals_report.dart DEVICE.json [...]         # measure one or more
/// dart run tool/script_literals_report.dart --diff BEFORE.txt AFTER.txt # compare two dumps
/// ```
///
/// A dump line ends at the image's last symbol; `== device` lines aggregate the device's scripts.
/// Write each run to its own file and diff them:
///
/// ```
/// dart run tool/script_literals_report.dart ak.json > before.txt
/// dart run tool/script_literals_report.dart ak.json > after.txt
/// dart run tool/script_literals_report.dart --diff before.txt after.txt
/// ```
///
/// Filenames are placeholders: the tool measures whatever per-device JSONs it is given.
///
/// Exit 0 always for a measurement; the `--diff` mode exits 1 when a line performs differently.
library;

import 'dart:convert';
import 'dart:io';
import 'package:tamuapp/core/backup_script.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/types.dart';

// The data types the device treats as numbers (`IsNumericDtype`, ScriptVm.h). A Vector, Matrix,
// Colour or Text is NOT one: it is copied raw, so its bytes and size are its meaning.
const Set<int> _numericTypes = {
  0x03, // Id
  0x04, // Bool
  0x05, // Index
  0x06, // Number
  0x0C, // Enum
  0x0E, // Uint32
  0x0F, // DevType
};

void main(List<String> args) {
  final rest = [...args];
  if (rest.isNotEmpty && rest.first == '--diff') {
    if (rest.length != 3) {
      stderr.writeln(
        'usage: script_literals_report.dart --diff <before> <after>',
      );
      exit(2);
    }
    exit(_diff(rest[1], rest[2]));
  }
  // `--symbols` prints each operand as the SYMBOL it is (a constant index, a predefine subtype)
  // instead of the value it resolves to - the form that shows a conversion happened, where the
  // default dump deliberately hides it because the value is the same either way.
  final symbols = rest.isNotEmpty && rest.first == '--symbols';
  if (symbols) rest.removeAt(0);
  if (rest.isEmpty) {
    stderr.writeln(
      'usage: script_literals_report.dart [--symbols] <device.json> [...]\n'
      '       script_literals_report.dart --diff <before> <after>',
    );
    exit(2);
  }
  for (final path in rest) {
    _measure(path, symbols: symbols);
  }
}

/// One resolved value as an instruction consumes it.
String _token(int dtype, int size, List<int> bytes) {
  if (!_numericTypes.contains(dtype)) {
    return 'R$dtype:${bytes.length}:${_hex(bytes)}';
  }
  final raw = _decodeScalarInt(dtype, size, bytes);
  // The two facets every instruction consumes: the Q16.16 word (a Number's raw value; an
  // integer type scaled by 16, which is what `ToScalar` builds) and the truncated integer.
  final numRaw = dtype == 0x06 ? raw : raw << 16;
  final intVal = dtype == 0x06 ? raw >> 16 : raw;
  return 'N$numRaw/$intVal';
}

int _decodeScalarInt(int dtype, int size, List<int> bytes) {
  int load(bool sign) {
    final n = size < bytes.length ? size : bytes.length;
    var acc = 0;
    for (var i = 0; i < n && i < 4; i++) {
      acc |= bytes[i] << (8 * i);
    }
    if (!sign) return acc;
    if (n == 4) {
      // A full-width signed value: expand bit 31 (Dart ints are 64-bit, so it is manual).
      return (acc & 0x80000000) != 0 ? acc - 0x100000000 : acc;
    }
    if (n > 0 && (bytes[n - 1] & 0x80) != 0) acc |= ~((1 << (8 * n)) - 1);
    return acc;
  }

  switch (dtype) {
    case 0x06: // Number: the Q16.16 word itself
      return load(true);
    case 0x05: // Index: signed
      return load(true);
    case 0x04: // Bool: any nonzero byte is true
      return bytes.isNotEmpty && bytes[0] != 0 ? 1 : 0;
    default: // Uint32, Enum, DevType, Id: unsigned
      return load(false);
  }
}

String _hex(List<int> bytes) =>
    bytes.map((b) => b.toRadixString(16).padLeft(2, '0')).join();

/// Resolves one symbol from a parsed image, exactly like the device's `Resolve`.
String _resolve(ScriptFileData f, ScriptSymbol s) {
  switch (s.type) {
    case symInput:
      if (s.value >= f.inputs.length) return 'I?${s.value}';
      final info = f.inputs[s.value];
      return 'I${s.value}[${_token(info.type, info.size, f.inputDefault(s.value))}]';
    case symOutput:
      if (s.value >= f.outputs.length) return 'O?${s.value}';
      final info = f.outputs[s.value];
      return 'O${s.value}[${info.type}:${info.size}]';
    case symVariable:
      if (s.value >= f.variables.length) return 'V?${s.value}';
      final info = f.variables[s.value];
      return 'V${s.value}[${info.type}:${info.size}]';
    case symConstant:
      if (s.value >= f.constants.length) return 'C?${s.value}';
      final info = f.constants[s.value];
      return 'C[${_token(info.type, info.size, f.constantValue(s.value))}]';
    case symPredefine:
      // `Resolve`, predefines: materialised into a 4-byte scratch, then reduced by _token.
      switch (s.subtype) {
        case preIndex:
          return 'C[${_token(0x05, 2, [s.value & 0xFF, (s.value >> 8) & 0xFF])}]';
        case preChar:
          return 'C[${_token(0x0E, 1, [s.value & 0xFF])}]';
        case preBool:
          return 'C[${_token(0x04, 1, [s.value != 0 ? 1 : 0])}]';
        case preState:
        case preMathOp:
          return 'C[${_token(0x0C, 1, [s.value & 0xFF])}]';
        case preType:
          return 'C[${_token(0x0C, 2, [s.value & 0xFF, (s.value >> 8) & 0xFF])}]';
        case preNumber:
          final sign = s.value >= 0x8000 ? s.value - 0x10000 : s.value;
          final raw = (sign << 8) & 0xFFFFFFFF;
          return 'C[${_token(0x06, 4, uint32ToBytes(raw))}]';
        default:
          return 'P?${s.subtype}:${s.value}';
      }
    case symInstruction:
      return 'X${ScriptInstructionDef.categoryName(s.subtype)}/${s.value}';
    case symEndline:
      return 'E';
    default:
      return '?${s.type}:${s.value}';
  }
}

/// One symbol as the operand position's shape, without resolving it: a constant keeps its index,
/// a predefine its subtype and value. Used by `--symbols` to show a conversion that the resolved
/// dump hides on purpose.
String _symbolToken(ScriptSymbol s) => switch (s.type) {
  symInput => 'I${s.value}',
  symOutput => 'O${s.value}',
  symVariable => 'V${s.value}',
  symConstant => 'C${s.value}',
  symPredefine => 'P${s.subtype}:${s.value}',
  symInstruction => 'X${s.subtype}/${s.value}',
  symEndline => 'E',
  _ => '?${s.type}:${s.value}',
};

String _line(
  ScriptFileData f,
  ScriptLine line,
  int index, {
  bool symbols = false,
}) {
  String one(ScriptSymbol s) => symbols ? _symbolToken(s) : _resolve(f, s);
  final d = line.destinations.map(one).join(',');
  final o = line.operands.map(one).join(',');
  return '  [$index] ${ScriptInstructionDef.categoryName(line.instruction.subtype)}'
      '/${line.instruction.value} d[$d] o[$o]';
}

/// A stable 32-bit fingerprint of the image (FNV-1a): enough to say "these bytes are the same",
/// with no dependency the app does not already carry.
String _sha(List<int> bytes) {
  var hash = 0x811c9dc5;
  for (final b in bytes) {
    hash = ((hash ^ b) * 0x01000193) & 0xFFFFFFFF;
  }
  return hash.toRadixString(16).padLeft(8, '0');
}

/// The RAM a script's constant space occupies: one ValueInfo per constant (4 bytes) plus the
/// value bytes, each entry aligned to 4 (`Docs/Services/Script.md` "File Blocks": the tables sit
/// in the value's own run-time order).
int _constSpace(ScriptFileData f) {
  var total = 0;
  for (final c in f.constants) {
    total += 4 + align4(c.size);
  }
  return total;
}

/// ASCII-safe digest of a single script's JSON block, to report the archive's own byte delta.
int _jsonBytes(Object? value) => utf8.encode(jsonEncode(value)).length;

void _measure(String path, {bool symbols = false}) {
  final Object? raw;
  try {
    raw = jsonDecode(File(path).readAsStringSync());
  } on FileSystemException catch (e) {
    stderr.writeln('FAIL: $path: ${e.message}');
    exit(2);
  } on FormatException catch (e) {
    stderr.writeln('FAIL: $path: ${e.message}');
    exit(2);
  }
  final doc = raw as Map<String, dynamic>;
  final scripts = (doc['scripts'] as List? ?? []).cast<Map<String, dynamic>>();
  final name = (doc['name'] as String?) ?? path;
  stdout.writeln('== device $name ($path)');
  var totalConsts = 0;
  var totalImage = 0;
  var totalConstSpace = 0;
  var totalJson = 0;
  var totalResaved = 0;
  for (final entry in scripts) {
    final script = BackupScript.fromJson(entry);
    final draft = script.toDraft();
    final image = draft.toImage();
    final f = ScriptFileData.parse(image);
    final lines = decodeScriptLines(f.instructions);
    totalConsts += f.constants.length;
    totalImage += image.length;
    totalConstSpace += _constSpace(f);
    totalJson += _jsonBytes(entry);
    // What the semantic form weighs once the app re-saves it: the archive's own byte delta is
    // this figure, because a setup stores a script semantically and nothing else.
    totalResaved += _jsonBytes(
      BackupScript.fromDraft(script.slot, draft).toJson(),
    );
    stdout.writeln(
      '-- slot ${script.slot} "${script.functionName}" consts=${f.constants.length} '
      'inputs=${f.inputs.length} outputs=${f.outputs.length} '
      'vars=${f.variables.length} lines=${lines.length} image=${image.length} '
      'constspace=${_constSpace(f)} sha256=${_sha(image)}',
    );
    for (var i = 0; i < lines.length; i++) {
      stdout.writeln(_line(f, lines[i], i, symbols: symbols));
    }
  }
  stdout.writeln(
    '== device $name: scripts=${scripts.length} consts=$totalConsts '
    'image_bytes=$totalImage constspace_bytes=$totalConstSpace '
    'scripts_json_bytes=$totalJson resaved_scripts_json_bytes=$totalResaved',
  );
}

/// Compares two dumps line by line, ignoring the byte/size figures (that is the point) but
/// keeping the per-line resolved tokens, which are the executed meaning.
int _diff(String beforePath, String afterPath) {
  List<String> read(String p) => File(p)
      .readAsLinesSync()
      .where((l) => !l.startsWith('-- slot') && !l.startsWith('== device'))
      .toList();
  final a = read(beforePath);
  final b = read(afterPath);
  var diffs = 0;
  for (var i = 0; i < a.length || i < b.length; i++) {
    final x = i < a.length ? a[i] : '<none>';
    final y = i < b.length ? b[i] : '<none>';
    if (x != y) {
      if (diffs < 40) {
        stdout.writeln('[$i]\n  before $x\n  after  $y');
      }
      diffs++;
    }
  }
  stdout.writeln(
    'RESULT: ${diffs == 0 ? "IDENTICAL" : "$diffs differing line(s)"} '
    '(${a.length} vs ${b.length} program lines)',
  );
  return diffs == 0 ? 0 : 1;
}
