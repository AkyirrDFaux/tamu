/// The app's registry of script error codes (firmware `Core/Services/ScriptDefs.h`).
///
/// Docs/Services/Script.md "Error Model": an error is a 32-bit code whose meaning is defined by
/// the instruction that produced it - there is no class layer - and "the human-readable names are
/// the app's, nothing goes on the device and there is no mirroring contract". The generic
/// `SCRIPT_ERR_*` values are what the engine carries today, so they are the names the app shows.
///
/// A code the registry does not know still renders as text, never as a bare integer: a surface
/// that prints `2` where a fault happened tells the reader nothing.
///
/// `test/firmware_contract_test.dart` compares this table with the firmware header, so the two
/// cannot drift apart unnoticed.
library;

/// The script error codes and their names.
class ScriptErrorCode {
  /// No error. Every code space starts at 1, so 0 is universally "no error".
  static const none = 0;
  static const unknownOp = 1;
  static const type = 2;
  static const operand = 3;
  static const bounds = 4;
  static const stack = 5;
  static const register = 6;
  static const timeout = 7;
  static const notImpl = 8;

  /// Code -> name, in the firmware's numbering (0..8).
  static const Map<int, String> names = {
    none: 'SCRIPT_ERR_NONE',
    unknownOp: 'SCRIPT_ERR_UNKNOWN_OP',
    type: 'SCRIPT_ERR_TYPE',
    operand: 'SCRIPT_ERR_OPERAND',
    bounds: 'SCRIPT_ERR_BOUNDS',
    stack: 'SCRIPT_ERR_STACK',
    register: 'SCRIPT_ERR_REGISTER',
    timeout: 'SCRIPT_ERR_TIMEOUT',
    notImpl: 'SCRIPT_ERR_NOT_IMPL',
  };

  /// The name of [code]. A code outside the registry is still named as text
  /// (`Unnamed error 12`) so nothing prints the number alone.
  static String name(int code) => names[code] ?? 'Unnamed error $code';
}
