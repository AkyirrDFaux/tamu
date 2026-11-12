// The IMPORT half of Akyirr's hex ruling (2026-11-12: "definitely approved" - one hex parser,
// strict in the editors, lenient with a counted warning on import).
//
// What the archive side must keep doing, and what it now does:
//
//   the value the archive carries      before                          after
//   {'hex': 'abc'} (odd digits)        [0xAB], silently               [0xAB] AND one
//                                                                     HexTruncation line on
//                                                                     the plan and the report
//   {'hex': '0A:FF-01'}                [0x0A, 0xFF, 0x01], silently   the same, no warning
//
// The ruling is explicit that a restore must NOT start refusing archives it imports today, so
// every case here asserts the bytes landed as well as the warning: a line without bytes would be
// the silent-refusal regression, and bytes without a line is the defect the ruling ends.

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/hex_bytes.dart';
import 'package:tamuapp/core/types.dart';

/// A Tamu archive carrying [entries] in one PWM block.
BackupDevice _archive(List<BackupEntry> entries) => BackupDevice(
  type: 'Tamu v2.0A',
  typeId: 1,
  id: idFromString('0.1')!,
  name: 'Tamu',
  blocks: [
    BackupBlock(
      type: 'PWM output',
      typeIndex: BlockType.pwm.value,
      instance: 0,
      name: 'Fan',
      isDynamic: false,
      entries: entries,
    ),
  ],
);

/// The same device live, with one `Undefined` row holding [value].
LiveDevice _live({List<int> value = const [0x00]}) => LiveDevice(
  id: idFromString('0.1')!,
  name: 'Tamu',
  capabilities:
      Capability.scripts | Capability.subscriptionRequest | Capability.core,
  blocks: [
    LiveBlock(
      type: BlockType.pwm.value,
      instance: 0,
      name: 'Fan',
      isDynamic: false,
      entries: [
        LiveEntry(
          field: 0,
          key: 0,
          fieldName: 'Payload',
          keyName: 'Key 0',
          meta: ValueInfo(type: DataType.undefined.value, size: 1),
          value: value,
        ),
      ],
    ),
  ],
);

/// An archive row of the untyped shape whose semantic value is a raw hex string (`encodeSemantic`
/// writes `{'hex': ...}` for `Undefined`), which is the one import path with no length requirement
/// of its own - so a digit left over is visible in the bytes it produces.
BackupEntry _hexRow(String hex, {int size = 1}) => BackupEntry(
  field: 'Payload',
  fieldIndex: 0,
  key: 'Key 0',
  keyIndex: 0,
  type: DataType.undefined.word,
  flags: const ['Persistent'],
  value: {'hex': hex},
  size: size,
);

RestorePlan _plan(String hex, {List<int> liveValue = const [0x00]}) =>
    planFromSnapshot(
      devices: [
        _archive([_hexRow(hex)]),
      ],
      live: [_live(value: liveValue)],
      inventories: const {},
    );

void main() {
  test('an archive hex value with a digit left over is written, and counted', () {
    final plan = _plan('abc');
    final item = plan.items.single;

    // LENIENT: the row still resolves, so an archive that restored before still restores.
    expect(item.issue, isNull);
    expect(item.ready, isTrue);
    expect(item.resolvedBytes, [
      0xAB,
    ], reason: 'the odd digit is dropped, as before');

    // COUNTED: the plan says which row read short, what the text was, where the digit was, and how
    // many bytes came out of it - instead of losing the nibble without a word.
    expect(plan.hexWarnings, hasLength(1));
    final warning = plan.hexWarnings.single;
    expect(warning.where, restoreItemSubject(item));
    expect(
      warning.where,
      'PWM output "Fan" #0 / Payload [Key 0] (field 0 key 0)',
    );
    expect(warning.text, 'abc');
    expect(warning.offset, 2);
    expect(warning.byteCount, 1);
    expect(
      warning.line,
      'PWM output "Fan" #0 / Payload [Key 0] (field 0 key 0) - "abc": odd '
      'hex digit count; the digit at character 2 has no pair, so the value '
      'is 1 byte',
    );
  });

  test('a well-formed hex value produces no warning', () {
    final plan = _plan('AB');

    expect(plan.items.single.resolvedBytes, [0xAB]);
    expect(plan.hexWarnings, isEmpty);
  });

  test('separators are read as bytes, with nothing to warn about', () {
    // Three bytes from separated text, against a row that declares three.
    final plan = planFromSnapshot(
      devices: [
        _archive([_hexRow('0A:FF-01', size: 3)]),
      ],
      live: [
        LiveDevice(
          id: idFromString('0.1')!,
          name: 'Tamu',
          capabilities: Capability.core,
          blocks: [
            LiveBlock(
              type: BlockType.pwm.value,
              instance: 0,
              name: 'Fan',
              isDynamic: false,
              entries: [
                LiveEntry(
                  field: 0,
                  key: 0,
                  fieldName: 'Payload',
                  keyName: 'Key 0',
                  meta: ValueInfo(type: DataType.undefined.value, size: 3),
                  value: const [0x00, 0x00, 0x00],
                ),
              ],
            ),
          ],
        ),
      ],
      inventories: const {},
    );

    expect(plan.items.single.resolvedBytes, [0x0A, 0xFF, 0x01]);
    expect(plan.hexWarnings, isEmpty);
  });

  test('re-resolving a plan does not duplicate the warning', () {
    final plan = _plan('abc');

    plan.resolve();
    plan.resolve();

    expect(plan.hexWarnings, hasLength(1));
  });

  test('the report counts, lists and copies what was read short', () {
    final warning = parseHexBytes('abc', strict: false).warnings.single;

    final report = RestoreReport(
      at: DateTime.utc(2026, 11, 12, 9, 30),
      written: 3,
      reasons: const [],
      hexWarnings: [warning.at('PWM output "Fan" #0 / Payload')],
    );

    // A count on the first line, after `failed` - never folded into it.
    expect(
      report.counts,
      '3 written · 0 skipped (structural) · 0 failed · '
      '1 read short (a hex digit had no pair)',
    );
    expect(report.failed, 0);
    expect(report.hexWarningLines, hasLength(1));
    expect(
      report.hexWarningLines.single,
      'PWM output "Fan" #0 / Payload - "abc": odd hex digit count; the digit '
      'at character 2 has no pair, so the value is 1 byte',
    );
    // The pasted text carries the same line under its own heading.
    expect(report.text, contains('Read short: an odd hex digit count (1)'));
    expect(report.text, contains('"abc"'));
    // And so does the structured half a card can quote after the fact.
    expect((report.metadata['hexWarnings'] as List).single, {
      'where': 'PWM output "Fan" #0 / Payload',
      'text': 'abc',
      'offset': 2,
      'bytes': 1,
    });
  });

  test('a report with nothing read short says nothing about it', () {
    final report = RestoreReport(
      at: DateTime.utc(2026, 11, 12, 9, 30),
      written: 3,
      reasons: const [],
    );

    expect(report.counts, '3 written · 0 skipped (structural) · 0 failed');
    expect(report.hexWarningLines, isEmpty);
    expect(report.text, isNot(contains('Read short')));
    expect(report.metadata['hexWarnings'], isEmpty);
  });

  test('HexTruncation is what the report holds, so the facts survive', () {
    // The parse is the only source of these three facts: a report line can never be written by
    // hand from a count.
    final truncation = parseHexBytes('12:345', strict: false).warnings.single;

    expect(truncation.text, '12:345');
    expect(truncation.offset, 5, reason: 'the `5` at index 5 has no pair');
    expect(truncation.byteCount, 2);
  });
}
