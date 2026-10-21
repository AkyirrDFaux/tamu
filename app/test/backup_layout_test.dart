import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/backup_layout.dart';
import 'package:tamuapp/core/backup_value.dart';
import 'package:tamuapp/core/types.dart';

/// The `LAY_1` carried by `akyirr/akyirr_setup.zip` (`1_Tamu_v2.0A.json`, `files[0]`): 223 bytes,
/// header 178/11/10, md5 e16670657e4ad45fc6d238ebf8237517. Embedded rather than read out of the
/// setup repo so the test is hermetic; the header and the size are the facts the layout card pinned.
const String _lay1Hex =
    'b20b0affffffffffff1b001c002f0030004300ffffffffffffffffffff0a001a001d002e'
    '00310042004400ffffffffffff09000b0019001e002d003200410045005100ffff000008'
    '000c0018001f002c0033004000460050005200010007000d00170020002b0034003f0047'
    '004f005300020006000e00160021002a0035003e0048004e005400030005000f00150022'
    '00290036003d0049004d005500ffff0400100014002300280037003c004a004c00ffffff'
    'ffffff110013002400270038003b004b00ffffffffffffffffffff12002500260039003a'
    '00ffffffffffff';

List<int> _lay1() => [
  for (var i = 0; i + 1 < _lay1Hex.length; i += 2)
    int.parse(_lay1Hex.substring(i, i + 2), radix: 16),
];

/// `LayoutVysiv1_0` row 0 from `tamu-setup/Vysi1 Layout Reference.h`: the firmware's compiled-in
/// default in its own 1-based form, where 0 means "no LED" and the wire value is the index minus
/// one.
const List<int> _referenceRow0 = [0, 0, 0, 28, 29, 48, 49, 68, 0, 0, 0];

/// FNV-1a (32-bit): a compact fingerprint of the bytes, so both sides of a round trip can be
/// quoted as one value. The byte-list equality in the same test is the real assertion.
int _fingerprint(List<int> bytes) {
  var hash = 0x811C9DC5;
  for (final byte in bytes) {
    hash = (hash ^ byte) * 0x01000193 & 0xFFFFFFFF;
  }
  return hash;
}

/// The LED display block a layout belongs to, with the `Layout File Name` field the restore writes
/// to make the display load the file (ValueTrigger | ValuePersistent).
BackupBlock _displayBlock(String layoutFile) => BackupBlock(
  type: 'LED Display',
  typeIndex: BlockType.vysiDisplay.value,
  instance: 0,
  name: 'LEDDisplay',
  isDynamic: false,
  entries: [
    BackupEntry(
      field: 'Layout File Name',
      fieldIndex: 3,
      key: 'Key 0',
      keyIndex: 0,
      type: 'Filename',
      flags: const ['Persistent', 'Trigger'],
      value: layoutFile,
      size: 8,
    ),
  ],
);

void main() {
  test('the real LAY_1 decodes into the semantic layout', () {
    final bytes = _lay1();
    expect(bytes, hasLength(223));

    final layout = BackupLayout.fromImage('LAY_1', bytes);
    // Header: brightness limit byte, width, height. The byte is 178; the firmware reads it as a
    // percentage, and 178/255 is 69.8 % -> 70 % for display only.
    expect(layout.brightnessLimit, 178);
    expect(layout.brightnessPercent, 70);
    expect(layout.width, 11);
    expect(layout.height, 10);
    expect(layout.leds, hasLength(110));

    // Row 0 on the wire is FFFF FFFF FFFF 001B 001C 002F 0030 0043 FFFF FFFF FFFF: three unused
    // cells, LEDs 27/28/47/48/67, three unused.
    expect(
      [for (var c = 0; c < 11; c++) layout.ledAt(0, c)],
      [null, null, null, 27, 28, 47, 48, 67, null, null, null],
    );
    // The reference array's documented transform: unused -> 0xFFFF, used -> index - 1.
    expect(
      [for (final v in _referenceRow0) v == 0 ? null : v - 1],
      [for (var c = 0; c < 11; c++) layout.ledAt(0, c)],
    );
    // 24 cells carry no LED; every index that is used names one of the chain's 86 LEDs.
    expect(layout.leds.where((led) => led == null), hasLength(24));
    expect(
      layout.leds.whereType<int>().every((led) => led >= 0 && led < 86),
      isTrue,
    );
  });

  test('re-encoding the semantic layout is byte-identical', () {
    final original = _lay1();
    final layout = BackupLayout.fromImage('LAY_1', original);
    final encoded = layout.toImage();

    expect(encoded, original, reason: 'the rebuilt file must be the file');
    expect(encoded, hasLength(223));
    expect(
      _fingerprint(encoded),
      _fingerprint(original),
      reason:
          'fingerprint ${_fingerprint(encoded).toRadixString(16)} vs '
          '${_fingerprint(original).toRadixString(16)}',
    );

    // The same through the archive JSON: this is the shape a restore actually reads, so the
    // brightness byte has to survive a JSON round trip unchanged (a stored 70 % would not).
    final json = layout.toJson();
    expect(json['brightnessLimit'], 178);
    expect((178 * 100 / 255).round(), 70, reason: '178 is 70 % once rounded');
    expect(
      70 * 255 / 100,
      isNot(178),
      reason: '...so a percentage cannot rebuild the byte',
    );
    final reread = BackupLayout.fromJson(
      jsonDecode(jsonEncode(json)) as Map<String, dynamic>,
    );
    expect(reread.brightnessLimit, 178);
    expect(reread.toImage(), original);
  });

  test('the keys name the format, and only a layout file has this form', () {
    // The keys are the format's own words, not offsets (Docs "Layout File").
    expect(backupFileKind('LAY_1'), 'LED layout');
    expect(backupFileKind('LAY_1   '), 'LED layout');
    expect(hasSemanticForm('LED layout'), isTrue);
    expect(hasSemanticForm('Script'), isTrue);
    expect(hasSemanticForm('Text'), isFalse);
    expect(hasSemanticForm('Binary'), isFalse);
    expect(BackupLayout.fromImage('LAY_1', _lay1()).toJson().keys, [
      'file',
      'brightnessLimit',
      'width',
      'height',
      'leds',
    ]);
  });

  test('a semantic-layout backup plans the file and the LayoutFile field', () async {
    final original = _lay1();
    final device = BackupDevice(
      type: 'Tamu v2.0A',
      typeId: 1,
      id: 1,
      name: 'Tamu',
      blocks: [_displayBlock('LAY_1')],
      layouts: [BackupLayout.fromImage('LAY_1', original)],
    );

    // Through the archive, the way a backup travels: the semantic layout and no blob.
    final parsed = parseBackupZip(buildBackupZip([device])).single;
    expect(parsed.layouts, hasLength(1));
    expect(
      parsed.files,
      isEmpty,
      reason: 'the layout does not also travel as bytes',
    );

    final plan = await buildRestorePlan([parsed]);
    final files = plan.items.where((i) => i.kind == RestoreKind.file).toList();
    expect(files, hasLength(1));
    expect(files.single.file!.name, 'LAY_1');
    expect(
      files.single.file!.bytes,
      original,
      reason: 'the write payload is the file the device held',
    );

    // The display's LayoutFile entry is planned as an ordinary entry item: written after the file
    // (files go first), its trigger loads the layout as it lands.
    final field = plan.items
        .where(
          (i) =>
              i.kind == RestoreKind.entry &&
              i.entry!.field == 'Layout File Name',
        )
        .toList();
    expect(field, hasLength(1));
    expect(field.single.entry!.value, 'LAY_1');
    expect(field.single.entry!.flags, contains('Trigger'));

    // Resolved against a live display, the field's write payload is the 8-byte padded file name -
    // what the trigger needs to load the layout that the file item above has just written. (No
    // rig here: the plan is the write contract; applying it needs the device.)
    final live = LiveDevice(
      id: 1,
      name: 'Tamu',
      blocks: [
        LiveBlock(
          type: BlockType.vysiDisplay.value,
          instance: 0,
          name: 'LEDDisplay',
          isDynamic: false,
          entries: [
            LiveEntry(
              field: 3,
              key: 0,
              fieldName: 'Layout File Name',
              keyName: 'Key 0',
              meta: ValueInfo(type: DataType.filename.value, size: 8),
            ),
          ],
        ),
      ],
    );
    plan.liveDevices
      ..clear()
      ..add(live);
    plan.deviceTargets[parsed.id] = live;
    plan.resolve();
    final resolved = plan.items
        .where(
          (i) =>
              i.kind == RestoreKind.entry &&
              i.entry!.field == 'Layout File Name',
        )
        .single;
    expect(resolved.issue, isNull);
    expect(resolved.targetEntry!.field, 3);
    expect(
      resolved.resolvedBytes,
      [...'LAY_1'.codeUnits, 0, 0, 0],
      reason: 'the LayoutFile field carries the name the display loads',
    );
  });

  test('an archive carrying LAY_1 only as raw bytes still restores it', () async {
    final original = _lay1();
    final device = BackupDevice(
      type: 'Tamu v2.0A',
      typeId: 1,
      id: 1,
      name: 'Tamu',
      blocks: [_displayBlock('LAY_1')],
      // An archive captured before layouts had a semantic form: no `layouts`, the file in `files`.
      files: [
        BackupFile(
          name: 'LAY_1',
          kind: 'LED layout',
          data: base64Encode(original),
        ),
      ],
    );
    expect(device.layouts, isEmpty);
    expect(restoresAsRawFile(device, device.files.single), isTrue);

    final parsed = parseBackupZip(buildBackupZip([device])).single;
    expect(parsed.layouts, isEmpty);
    final plan = await buildRestorePlan([parsed]);
    final files = plan.items.where((i) => i.kind == RestoreKind.file).toList();
    expect(files, hasLength(1));
    expect(files.single.file!.bytes, original);
  });

  test(
    'a layout carried both ways is written once, from the semantic form',
    () async {
      final original = _lay1();
      final raw = BackupFile(
        name: 'LAY_1',
        kind: 'LED layout',
        data: base64Encode(original),
      );
      final device = BackupDevice(
        type: 'Tamu v2.0A',
        typeId: 1,
        id: 1,
        name: 'Tamu',
        blocks: const [],
        layouts: [BackupLayout.fromImage('LAY_1', original)],
        files: [raw],
      );
      expect(
        restoresAsRawFile(device, raw),
        isFalse,
        reason: 'the semantic layout is the authoritative copy',
      );
      final plan = await buildRestorePlan([device]);
      expect(plan.items.where((i) => i.kind == RestoreKind.file), hasLength(1));

      // The same with a padded device-side name: the two name one file.
      expect(
        restoresAsRawFile(
          device,
          BackupFile(name: 'LAY_1   ', kind: 'LED layout', data: ''),
        ),
        isFalse,
      );
    },
  );

  test('a layout the decoder cannot read keeps its bytes as the only copy', () {
    // The older 2-byte header: width, height, then the map - so this format reads the first map
    // byte as the height.
    final twoByteHeader = <int>[
      11,
      10,
      for (var i = 0; i < 110; i++) ...[0xFF, 0xFF],
    ];
    expect(twoByteHeader, hasLength(222));
    expect(
      () => BackupLayout.fromImage('LAY_1', twoByteHeader),
      throwsFormatException,
    );
    // A file whose size does not match its header (truncated, then padded).
    expect(
      () => BackupLayout.fromImage('LAY_1', _lay1().sublist(0, 200)),
      throwsFormatException,
    );
    expect(
      () => BackupLayout.fromImage('LAY_1', [..._lay1(), 0]),
      throwsFormatException,
    );
    // A table past the firmware's limit (`MaxLayoutEntries` = 256 cells).
    expect(
      () => BackupLayout.fromImage('LAY_1', [
        178,
        24,
        24,
        ...List<int>.filled(24 * 24 * 2, 0),
      ]),
      throwsFormatException,
    );
    // Too short to hold a header at all.
    expect(
      () => BackupLayout.fromImage('LAY_1', const [178, 11]),
      throwsFormatException,
    );
    // A corrupt semantic entry cannot rebuild a file either.
    final broken = BackupLayout(
      file: 'LAY_1',
      brightnessLimit: 178,
      width: 11,
      height: 10,
      leds: const [1, 2, 3],
    );
    expect(() => broken.toImage(), throwsFormatException);

    // And the capture keeps the bytes of a file the decoder rejected: with no semantic form they
    // are the only copy, whatever the "include device files" choice says.
    expect(
      dropCapturedFile(
        'LAY_1',
        includeFiles: false,
        archivedSemantically: false,
      ),
      isFalse,
    );
  });
}
