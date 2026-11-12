/// The app's ONE hex parser (Akyirr's ruling on cleanup-pass-2 finding 4, 2026-11-12:
/// *"definitely approved"* - "one hex parser, strict in the editors, lenient with a counted
/// warning on import").
///
/// There used to be two, and they disagreed about the same input: the archive's `unhexBytes`
/// dropped the odd nibble of `"abc"` and returned `0xAB` without a word, while the value editor's
/// own copy returned null. An import could therefore write a byte pattern nobody had typed, and a
/// value editor could keep the old bytes after a typo without saying so.
///
/// The strictness is now an ARGUMENT of one function ([parseHexBytes]) rather than a choice of
/// copy, so the two callers cannot drift apart again:
///
///  * `strict: true` - the editors. Empty or odd-length input is a failure the caller shows
///    ([HexParse.error]), and no bytes are produced at all. A value editor never writes a
///    truncated pattern.
///  * `strict: false` - archive import. The same input still yields bytes, so a restore does not
///    start refusing archives it imports today, and every dropped digit is a [HexTruncation] the
///    caller can surface: the text, the offset and the resulting byte count.
///
/// Separators are not data in either mode: `0A:FF-01`, `0A FF 01`, `[0A,FF,01]` and `0x0A0xFF01`
/// all read as three bytes. A `0x`/`0X` marker is dropped WHOLE, so `0x0A` is `0x0A` and not the
/// `0x00` a plain "strip every non-hex character" walk would make of it (the `0` would otherwise
/// stand as a digit of its own).
library;

/// One hex string the LENIENT parse could not read as a whole number of bytes.
///
/// The three facts are the ones a reader needs to tell "the archive had a typo" apart from "the app
/// wrote something else": the [text] as it arrived, the [offset] of the digit that had no pair, and
/// the [byteCount] the parse produced. [where] is the row the caller read it from, when it knows
/// one - the parser does not.
class HexTruncation {
  const HexTruncation({
    required this.text,
    required this.offset,
    required this.byteCount,
    this.where,
  });

  /// The hex string, as it arrived (separators and case kept).
  final String text;

  /// The index IN [text] of the hex digit that had no pair and was dropped.
  final int offset;

  /// How many bytes the parse produced from [text].
  final int byteCount;

  /// What the caller was reading, in its own words (a register row, an SNDB record). Null when the
  /// parse was not made for a row.
  final String? where;

  /// The same truncation, attributed to [where].
  HexTruncation at(String where) => HexTruncation(
    text: text,
    offset: offset,
    byteCount: byteCount,
    where: where,
  );

  /// One line, ready to print: what was read, where the odd digit was, what came out.
  String get line {
    final head = where == null ? '' : '$where - ';
    return '$head"$text": odd hex digit count; the digit at character '
        '$offset has no pair, so the value is $byteCount '
        'byte${byteCount == 1 ? '' : 's'}';
  }

  @override
  String toString() => 'HexTruncation($line)';
}

/// What [parseHexBytes] made of one string.
///
/// [bytes] is null only when a STRICT parse failed, and then [error] says why in words a page can
/// print as it stands. A lenient parse never fails: it always produces [bytes], and reports what it
/// had to drop in [warnings]. Exactly one of the two is ever populated.
class HexParse {
  const HexParse._({this.bytes, this.error, this.warnings = const []});

  /// The bytes the string encodes, or null when a strict parse failed.
  final List<int>? bytes;

  /// Why a strict parse failed, in the words a field shows under itself.
  final String? error;

  /// Every dropped digit of a lenient parse, in the order they were found.
  final List<HexTruncation> warnings;

  /// True when [bytes] is usable.
  bool get ok => bytes != null;

  @override
  String toString() => ok
      ? 'HexParse(${bytes!.length} B, ${warnings.length} warning(s))'
      : 'HexParse(failed: $error)';
}

/// Parses [text] as hex bytes, the app's one hex parser.
///
/// [strict] is not a default on purpose: a caller states which side of the ruling it is on, the
/// editors `strict: true` and the archive import `strict: false`.
///
/// A strict parse of an empty or odd-length string fails with an [HexParse.error] and produces no
/// bytes. A lenient parse of an empty string produces no bytes and no warning (there is no digit to
/// lose), and of an odd-length one produces every whole byte plus one [HexTruncation].
HexParse parseHexBytes(String text, {required bool strict}) {
  final digits = <int>[];
  // The index in `text` of each kept digit, so a truncation offset points at the character the
  // reader has to look at rather than at a position in a cleaned-up copy.
  final at = <int>[];
  for (var i = 0; i < text.length; i++) {
    // A `0x`/`0X` marker opens a token: it is dropped whole, and only where a byte starts - inside
    // a run of digits `x` is just another separator.
    if (digits.length.isEven && _isMarker(text, i)) {
      i++;
      continue;
    }
    final value = _nibble(text.codeUnitAt(i));
    if (value == null) continue; // a separator, not data
    digits.add(value);
    at.add(i);
  }
  if (digits.isEmpty) {
    return strict
        ? const HexParse._(error: 'No hex digits')
        : const HexParse._(bytes: <int>[]);
  }
  final whole = digits.length - (digits.length.isOdd ? 1 : 0);
  final bytes = <int>[
    for (var i = 0; i < whole; i += 2) (digits[i] << 4) | digits[i + 1],
  ];
  if (digits.length.isEven) return HexParse._(bytes: bytes);
  if (strict) {
    return const HexParse._(
      error: 'An odd number of hex digits: every byte needs two',
    );
  }
  return HexParse._(
    bytes: bytes,
    warnings: [
      HexTruncation(text: text, offset: at.last, byteCount: bytes.length),
    ],
  );
}

/// The value of one hex digit character, or null when [c] is not one.
int? _nibble(int c) {
  if (c >= 0x30 && c <= 0x39) return c - 0x30; // 0-9
  if (c >= 0x41 && c <= 0x46) return c - 0x41 + 10; // A-F
  if (c >= 0x61 && c <= 0x66) return c - 0x61 + 10; // a-f
  return null;
}

/// True when a `0x`/`0X` marker starts at [i]: a zero followed by an `x`, both of which are then
/// dropped instead of the zero counting as a nibble.
bool _isMarker(String text, int i) {
  if (i + 1 >= text.length) return false;
  if (text.codeUnitAt(i) != 0x30) return false; // '0'
  final next = text.codeUnitAt(i + 1);
  return next == 0x78 || next == 0x58; // 'x' | 'X'
}
