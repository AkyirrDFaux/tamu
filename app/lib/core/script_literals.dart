/// Small literal constants as **predefine** operands (Docs/Services/Script.md symbol kind 6).
///
/// A constant costs a `ValueInfo` plus its value bytes in the script's constant space, which is
/// RAM on the device once the script is loaded (Script.md "File Blocks": the ValueInfo tables and
/// the values are the script's run-time space). The format can carry the same literal inline in
/// the operand position instead: `Bool` for true/false, `Index` for an integer, `Char` for one
/// character, `Number` for a Q8.8 fraction. The device materialises a predefine from its subtype
/// alone (`firmware/src/Core/Services/ScriptVm.h`, `Resolve`), so this module decides - for each
/// scalar constant - whether the predefine reproduces its value BIT-EXACTLY, and refuses the
/// conversion when it does not. A lossy conversion would change the arithmetic a running script
/// performs, so the gate is the point of this file, not a check on top of it.
///
/// What the device builds for each subtype (`Resolve` -> `ToScalar`), and what that means here:
///
/// | Predefine | The VM's value                                                     |
/// | --------- | ------------------------------------------------------------------ |
/// | `Index`   | a 2-byte value **sign-extended** (bit 15 is the sign), used as an integer |
/// | `Char`    | one byte, consumed as `Uint32`                                      |
/// | `Bool`    | one byte, 0 or 1                                                    |
/// | `Number`  | `(int16 value) << 8`, i.e. a Q8.8 literal widened to Q16.16        |
///
/// Hence the two limits this file enforces:
///
/// * an `Index` literal covers 0..32767, not 0..65535: the device sign-extends bit 15, so
///   0x8000 and up would arrive as a negative number;
/// * a `Number` literal covers exactly the multiples of 1/256 in -128..127.99, because the
///   narrowing drops `raw & 0xFF`: a Q16.16 constant such as `0.1999969482421875`
///   (raw 13107) has no Q8.8 form and must stay a constant.
///
/// A BlockInfo constant (a register reference) is never converted - a predefine cannot carry one,
/// and the register instructions require a `BlockInfo` constant by spec.
library;

import 'dart:typed_data';

import 'script_instructions.dart';
import 'types.dart';

/// The decision for one constant: the predefine that carries it ([symbol]), or the reason it has
/// to stay a stored constant ([reason]).
class ScriptLiteralPredefine {
  const ScriptLiteralPredefine.convert(this.symbol) : reason = '';
  const ScriptLiteralPredefine.keep(this.reason) : symbol = null;

  /// The predefine operand that reproduces the constant's value, or null when it stays stored.
  final ScriptSymbol? symbol;

  /// Why the constant stays stored (empty when [symbol] is set).
  final String reason;

  bool get converts => symbol != null;
}

/// The largest integer an `Index` predefine carries: the device sign-extends bit 15, so a
/// literal at 0x8000 or above would arrive negative.
const int scriptIndexLiteralMax = 0x7FFF;

/// The predefine that carries [value] exactly, or the reason it must stay a stored constant.
///
/// Only scalar types are considered. A Vector, Matrix, Colour, Text, Name, Id or BlockInfo is
/// either a container or a byte string whose bytes ARE its meaning, and a predefine carries a
/// single 16-bit value.
ScriptLiteralPredefine scriptLiteralPredefine(DataType type, Uint8List value) {
  switch (type) {
    case DataType.blockInfo:
      return const ScriptLiteralPredefine.keep(
        'a register reference (BlockInfo) cannot be a predefine',
      );
    case DataType.bool_:
      if (value.length != 1) {
        return ScriptLiteralPredefine.keep(
          'a Bool is one byte, not ${value.length}',
        );
      }
      if (value[0] > 1) {
        return ScriptLiteralPredefine.keep(
          'a Bool byte of ${value[0]} is neither false nor true',
        );
      }
      return ScriptLiteralPredefine.convert(
        ScriptSymbol.predefine(preBool, value[0]),
      );
    case DataType.char:
      if (value.length != 1) {
        return ScriptLiteralPredefine.keep(
          'a Char is one byte, not ${value.length}',
        );
      }
      return ScriptLiteralPredefine.convert(
        ScriptSymbol.predefine(preChar, value[0]),
      );
    case DataType.number:
      if (value.length != 4) {
        return ScriptLiteralPredefine.keep(
          'a Number is a 4-byte Q16.16, not ${value.length} bytes',
        );
      }
      return _numberLiteral(int32FromBytes(value));
    case DataType.integer:
      return _integerLiteral(_loadInt(value, signed: true), type);
    case DataType.uint32:
    case DataType.enum_:
      return _integerLiteral(_loadInt(value, signed: false), type);
    default:
      return ScriptLiteralPredefine.keep(
        'a ${type.word} is not a scalar a predefine can carry',
      );
  }
}

/// A Q16.16 word: an integral value becomes an `Index` literal, a fraction a Q8.8 `Number`
/// literal, and anything the 16-bit literal cannot reproduce bit-exactly stays a constant.
ScriptLiteralPredefine _numberLiteral(int raw) {
  final text = (raw / 65536.0).toString();
  if (raw % 65536 == 0) {
    final whole = raw >> 16;
    if (whole >= 0 && whole <= scriptIndexLiteralMax) {
      return ScriptLiteralPredefine.convert(
        ScriptSymbol.predefine(preIndex, whole),
      );
    }
    if (whole < 0) return _q88(raw, text);
    return ScriptLiteralPredefine.keep(
      '$text is above the 16-bit Index literal limit ($scriptIndexLiteralMax)',
    );
  }
  return _q88(raw, text);
}

/// The Q8.8 `Number` literal for a Q16.16 word, or a refusal when the narrowing is lossy.
ScriptLiteralPredefine _q88(int raw, String text) {
  if (raw % 256 != 0) {
    return ScriptLiteralPredefine.keep(
      '$text does not survive the Q8.8 round trip '
      '(Q16.16 raw $raw is not a multiple of 256)',
    );
  }
  final q = raw >> 8;
  if (q < -32768 || q > 32767) {
    return ScriptLiteralPredefine.keep(
      '$text is outside the Q8.8 literal range (-128..127.99)',
    );
  }
  return ScriptLiteralPredefine.convert(
    ScriptSymbol.predefine(preNumber, q & 0xFFFF),
  );
}

/// An integer-typed scalar (`Index`, `Uint32`, `Enum`).
ScriptLiteralPredefine _integerLiteral(int v, DataType type) {
  if (v >= 0 && v <= scriptIndexLiteralMax) {
    return ScriptLiteralPredefine.convert(ScriptSymbol.predefine(preIndex, v));
  }
  // A negative integer has no `Index` literal form (the device sign-extends bit 15), but its
  // Q8.8 `Number` form is exact for -128..-1.
  if (v < 0 && v * 256 >= -32768) {
    return ScriptLiteralPredefine.convert(
      ScriptSymbol.predefine(preNumber, (v * 256) & 0xFFFF),
    );
  }
  return ScriptLiteralPredefine.keep(
    '$v (${type.word}) does not fit a 16-bit predefine literal',
  );
}

/// Reads a little-endian scalar of up to 4 bytes (the widths a ValueInfo can declare), the way
/// the device's `LoadInt` does: signed types sign-extend from their own width.
int _loadInt(Uint8List value, {required bool signed}) {
  final n = value.length > 4 ? 4 : value.length;
  var acc = 0;
  for (var i = 0; i < n; i++) {
    acc |= value[i] << (8 * i);
  }
  if (!signed || n == 0) return acc;
  if (n == 4) return (acc & 0x80000000) != 0 ? acc - 0x100000000 : acc;
  if ((value[n - 1] & 0x80) != 0) acc |= ~((1 << (8 * n)) - 1);
  return acc;
}
