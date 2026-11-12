// The app's ONE hex parser (Akyirr's ruling on cleanup-pass-2 finding 4, 2026-11-12: "definitely
// approved" - one hex parser, strict in the editors, lenient with a counted warning on import).
//
// Before it there were two copies that disagreed about the same input, and this file is the
// contract that keeps them from coming back:
//
//   input      strict (the editors)        lenient (archive import)
//   "abc"      null + "an odd number..."   [0xAB] + 1 warning (text, offset 2, 1 byte)
//   ""         null + "No hex digits"      [] and no warning (there is no digit to lose)
//   "0A:FF-01" [0x0A, 0xFF, 0x01]          [0x0A, 0xFF, 0x01]   (never a failure in either)

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/hex_bytes.dart';

void main() {
  group('strict: the editors', () {
    test('an odd digit count is an error, never a truncated value', () {
      final parsed = parseHexBytes('abc', strict: true);

      expect(parsed.ok, isFalse);
      expect(
        parsed.bytes,
        isNull,
        reason: 'the old lenient copy gave 0xAB here',
      );
      expect(parsed.error, 'An odd number of hex digits: every byte needs two');
      expect(parsed.warnings, isEmpty);
    });

    test('empty is an error', () {
      final parsed = parseHexBytes('', strict: true);

      expect(parsed.ok, isFalse);
      expect(parsed.bytes, isNull);
      expect(parsed.error, 'No hex digits');
    });

    test('text with no hex digit at all is the same error', () {
      expect(parseHexBytes('G G', strict: true).error, 'No hex digits');
    });

    test('separators are not data: they read as bytes', () {
      expect(parseHexBytes('0A:FF-01', strict: true).bytes, [0x0A, 0xFF, 0x01]);
      expect(parseHexBytes('0a ff 01', strict: true).bytes, [0x0A, 0xFF, 0x01]);
      expect(parseHexBytes('[0A,FF,01]', strict: true).bytes, [
        0x0A,
        0xFF,
        0x01,
      ]);
      expect(parseHexBytes('AbCdEf', strict: true).bytes, [0xAB, 0xCD, 0xEF]);
      expect(parseHexBytes('abc', strict: true).warnings, isEmpty);
    });

    test('a 0x prefix is a marker, not a leading zero', () {
      // The old copies stripped every non-hex character, so `0x0A` became `00A` -> either a
      // refusal or a phantom `0x00`. Dropping the marker whole is the only reading that keeps the
      // value the user typed.
      expect(parseHexBytes('0x0A', strict: true).bytes, [0x0A]);
      expect(parseHexBytes('0X0A0XFF', strict: true).bytes, [0x0A, 0xFF]);
      expect(parseHexBytes('0x', strict: true).error, 'No hex digits');
    });
  });

  group('lenient: archive import', () {
    test('an odd digit count still yields bytes, and counts the loss', () {
      final parsed = parseHexBytes('abc', strict: false);

      expect(parsed.bytes, [
        0xAB,
      ], reason: 'the import must not start refusing');
      expect(parsed.error, isNull);
      expect(parsed.warnings, hasLength(1));
      final warning = parsed.warnings.single;
      expect(warning.text, 'abc');
      expect(warning.offset, 2, reason: 'the dropped digit is the last one');
      expect(warning.byteCount, 1);
      expect(
        warning.line,
        '"abc": odd hex digit count; the digit at character 2 has no pair, '
        'so the value is 1 byte',
      );
    });

    test('the offset points into the text as it arrived', () {
      final warning = parseHexBytes('ab:cd:e', strict: false).warnings.single;

      expect(warning.offset, 6, reason: 'the `e` at index 6 has no pair');
      expect(warning.byteCount, 2);
    });

    test('empty yields no bytes and no warning', () {
      final parsed = parseHexBytes('', strict: false);

      expect(parsed.bytes, isEmpty);
      expect(parsed.error, isNull);
      expect(parsed.warnings, isEmpty);
    });

    test('separators read as bytes, with nothing to warn about', () {
      for (final text in ['0A:FF-01', '0a ff 01', '0x0A0xFF01']) {
        final parsed = parseHexBytes(text, strict: false);
        expect(parsed.bytes, [0x0A, 0xFF, 0x01], reason: text);
        expect(parsed.warnings, isEmpty, reason: text);
      }
    });

    test('every dropped digit is its own warning', () {
      // Two rows, one bad each: the caller collects a line per row and can count them.
      final warnings = [
        parseHexBytes('abc', strict: false).warnings.single,
        parseHexBytes('12345', strict: false).warnings.single,
      ];

      expect(warnings.map((w) => w.byteCount), [1, 2]);
      expect(warnings.map((w) => w.text), ['abc', '12345']);
    });
  });

  test('one truncation is attributed to the row that read it', () {
    final bare = parseHexBytes('abc', strict: false).warnings.single;

    expect(bare.line.startsWith('"'), isTrue);
    final attributed = bare.at('PWM output "Fan" #0 / Serial');
    expect(
      attributed.line,
      'PWM output "Fan" #0 / Serial - "abc": odd hex digit count; the digit '
      'at character 2 has no pair, so the value is 1 byte',
    );
    expect(attributed.text, bare.text);
    expect(attributed.offset, bare.offset);
    expect(attributed.byteCount, bare.byteCount);
  });
}
