import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/ble_security.dart';
import 'package:tamuapp/core/block_registry.dart';
import 'package:tamuapp/core/connection.dart' show LinkTrouble;
import 'package:tamuapp/core/types.dart';

/// The `Bonds` value the rig answered on 2026-10-29 (`firmware/.pio/probe/bleset6.py`, image
/// 2026.10.29.3): slot 0 holds this host's controller address with the label the probe wrote, slots
/// 1 and 2 are six zero bytes each. The reply carries two pad bytes beyond the declared size.
const String rigBondsHex =
    '4c233874e6f64465736b2070686f6e65000000000000'
    '00000000000000000000000000000000000000000000'
    '00000000000000000000000000000000000000000000';

List<int> hexBytes(String hex) => [
  for (var i = 0; i < hex.length; i += 2)
    int.parse(hex.substring(i, i + 2), radix: 16),
];

/// The `BLE Settings` block's app half, off-device: the field map, the `Bonds` codec, the reading
/// rules, the texts the panel builds (the window countdown and the passkey state) and the guidance
/// after a failed Bluetooth attempt.
void main() {
  ({ValueInfo meta, List<int> value}) read(
    int type,
    List<int> value, {
    int flags = 0,
    int? size,
  }) => (
    meta: ValueInfo(type: type, size: size ?? value.length, flags: flags),
    value: value,
  );

  List<int> text(String s, int width) {
    final bytes = s.codeUnits.toList();
    if (bytes.length > width) {
      bytes.length = width;
    } else {
      bytes.addAll(List<int>.filled(width - bytes.length, 0));
    }
    return bytes;
  }

  /// The six bytes of a printed address.
  List<int> addr(String printed) => [
    for (final part in printed.split(':')) int.parse(part, radix: 16),
  ];

  /// A whole `Bonds` value: [entries] in slot order, the rest empty.
  List<int> bondsValue(List<(String, String)> entries) => encodeBleBonds([
    for (var slot = 0; slot < bleBondSlots; slot++)
      slot < entries.length
          ? BleBond(address: addr(entries[slot].$1), name: entries[slot].$2)
          : bleEmptyBond,
  ]);

  Map<int, ({ValueInfo meta, List<int> value})?> block({
    String passkey = '000000',
    int mode = bleModeJustWorks,
    List<(String, String)> bonds = const [],
    int windowMs = 0,
    bool enabled = true,
    String displayName = '',
    bool paddedBonds = false,
  }) => {
    bleFieldEnabled: read(DataType.bool_.value, [enabled ? 1 : 0], size: 1),
    bleFieldPasskey: read(
      DataType.text.value,
      text(passkey, blePasskeyLength),
      size: blePasskeyLength,
    ),
    bleFieldMode: read(DataType.enum_.value, [mode], size: 1),
    bleFieldDisplayName: read(
      DataType.text.value,
      text(displayName, bleDisplayNameWidth),
      size: bleDisplayNameWidth,
    ),
    bleFieldBonds: read(
      DataType.undefined.value,
      // A read pads the reply to a four-byte boundary, so 66 arrives as 68.
      [...bondsValue(bonds), if (paddedBonds) 0, if (paddedBonds) 0],
      size: bleBondsLength,
    ),
    bleFieldWindow: read(
      DataType.uint32.value,
      uint32ToBytes(windowMs),
      size: 4,
    ),
  };

  group('the field map', () {
    test('names the firmware table\'s six fields in order', () {
      // Blocks/BleSettings.h: the panel reads by number, so a re-ordered table would silently
      // read another field. The registry must name every field in the same order.
      expect(
        [
          bleFieldEnabled,
          bleFieldPasskey,
          bleFieldMode,
          bleFieldDisplayName,
          bleFieldBonds,
          bleFieldWindow,
        ],
        [for (var i = 0; i < bleFieldCount; i++) i],
      );
      expect(bleFieldCount, 6);
      final info = blockInfoFor(BlockType.bleSettings)!;
      expect(info.typeName, 'BLE Settings');
      expect(info.fields.map((field) => field.name), [
        'Enabled',
        'Passkey',
        'Mode',
        'Display name',
        'Bonds',
        'Window',
      ]);
    });

    test('has no field above Window: the redesign retired the rest', () {
      final info = blockInfoFor(BlockType.bleSettings)!;
      expect(info.field(bleFieldCount), isNull);
      expect(info.field(bleFieldCount + 6), isNull);
    });

    test('the Bonds array is three entries of address + name', () {
      expect(bleBondSlots, 3);
      expect(bleBondAddressLength, 6);
      expect(bleBondNameLength, 16);
      expect(bleBondEntryLength, 22);
      expect(bleBondsLength, 66);
      expect(bleBondEntryLength * bleBondSlots, bleBondsLength);
      expect(BlockType.bleSettings.value, bleSettingsBlockTypeValue);
    });

    test('the block is gated on the device enumeration', () {
      expect(hasBleSettingsBlock(const []), isFalse);
      expect(
        hasBleSettingsBlock(const [(type: 0x0C, maxInstance: 0)]),
        isFalse,
      );
      expect(
        hasBleSettingsBlock(const [
          (type: bleSettingsBlockTypeValue, maxInstance: 0),
        ]),
        isTrue,
      );
    });
  });

  group('the Bonds codec', () {
    test('decodes the rig\'s array: printed-order address, then the name', () {
      // The device publishes the address in the order it is printed, and the reply carried two pad
      // bytes past the declared 66: only the first 66 are entries.
      final bonds = decodeBleBonds([...hexBytes(rigBondsHex), 0, 0]);
      expect(bonds, hasLength(bleBondSlots));
      expect(bonds[0].addressText, '4C:23:38:74:E6:F6');
      expect(bonds[0].address, [0x4C, 0x23, 0x38, 0x74, 0xE6, 0xF6]);
      expect(bonds[0].name, 'Desk phone');
      expect(bonds[0].isEmpty, isFalse);
      // An all-zero address is the device's empty slot, not a peer.
      expect(bonds[1].isEmpty, isTrue);
      expect(bonds[1].addressText, '');
      expect(bonds[2].isEmpty, isTrue);
    });

    test('an all-zero value is an empty store, not a missing field', () {
      final bonds = decodeBleBonds(List<int>.filled(bleBondsLength, 0));
      expect(bonds.every((bond) => bond.isEmpty), isTrue);
      expect(bonds, hasLength(bleBondSlots));
    });

    test('decode then encode reproduces the rig\'s own value byte for byte', () {
      // The decoder and the encoder are the two halves of the same write: a panel that read the
      // array and wrote it straight back must send exactly the bytes the device described.
      final wire = hexBytes(rigBondsHex);
      expect(encodeBleBonds(decodeBleBonds(wire)), wire);
    });

    test('a truncated value reads the slots it lost as empty', () {
      // Only the 22 bytes of slot 0 arrived. Slots 1-2 must not shift the address into a name.
      final bonds = decodeBleBonds(hexBytes(rigBondsHex).sublist(0, 22));
      expect(bonds[0].addressText, '4C:23:38:74:E6:F6');
      expect(bonds[0].name, 'Desk phone');
      expect(bonds[1].isEmpty, isTrue);
      expect(bonds[2].isEmpty, isTrue);
    });

    test('a 16-character name needs no terminator', () {
      // The name half is fixed-width and NUL-padded with no terminator guaranteed: a label that
      // fills the field exactly must survive the round trip byte for byte.
      final label = 'Desk phone no. 2'; // 16 characters
      expect(label.length, bleBondNameLength);
      final bytes = encodeBleBonds([
        BleBond(address: addr('11:22:33:44:55:66'), name: label),
        bleEmptyBond,
        bleEmptyBond,
      ]);
      expect(bytes, hasLength(bleBondsLength));
      expect(decodeBleBonds(bytes)[0].name, label);
    });

    test('a rename keeps every other entry exactly as it was read', () {
      final held = decodeBleBonds(
        bondsValue(const [
          ('4C:23:38:74:E6:F6', 'Desk phone'),
          ('E4:B0:63:C8:20:72', 'Pixel'),
        ]),
      );
      final sent = bleBondsRenamed(held, 0, 'Bench');
      final decoded = decodeBleBonds(sent);
      expect(sent, hasLength(bleBondsLength));
      expect(decoded[0].address, held[0].address);
      expect(decoded[0].name, 'Bench');
      // The neighbouring entry is untouched, byte for byte: the device refuses a frame whose
      // slots do not match its store, so one rename cannot disturb another bond.
      expect(decoded[1].address, held[1].address);
      expect(decoded[1].name, 'Pixel');
      expect(decoded[2].isEmpty, isTrue);
    });

    test('clearing an address deletes that bond and leaves the others', () {
      final held = decodeBleBonds(
        bondsValue(const [
          ('4C:23:38:74:E6:F6', 'Desk phone'),
          ('E4:B0:63:C8:20:72', 'Pixel'),
        ]),
      );
      final sent = bleBondsCleared(held, 0);
      final decoded = decodeBleBonds(sent);
      expect(decoded[0].isEmpty, isTrue);
      expect(decoded[0].name, '');
      // The second bond is untouched: one write must not delete the rest.
      expect(decoded[1].addressText, 'E4:B0:63:C8:20:72');
      expect(decoded[1].name, 'Pixel');
    });

    test('clearing the last bond is the all-zero forget-all array', () {
      final held = decodeBleBonds(
        bondsValue(const [('4C:23:38:74:E6:F6', 'Desk phone')]),
      );
      final sent = bleBondsCleared(held, 0);
      // Every address zero: the firmware reads that as "every bond is gone" and answers with the
      // one forget-all path (plus its automatic five-minute window).
      expect(sent.every((byte) => byte == 0), isTrue);
      expect(sent, hasLength(bleBondsLength));
    });
  });

  group('a reading', () {
    test('carries every field the device answered', () {
      final snapshot = BleSecuritySnapshot.fromReads(
        block(
          passkey: '123456',
          mode: bleModePasskey,
          bonds: const [
            ('4C:23:38:74:E6:F6', 'Desk phone'),
            ('E4:B0:63:C8:20:72', 'Pixel'),
          ],
          windowMs: 48000,
          enabled: true,
          displayName: 'Workbench',
          paddedBonds: true,
        ),
      );

      expect(snapshot.passkey, '123456');
      expect(snapshot.passkeySet, isTrue);
      expect(snapshot.mode, bleModePasskey);
      expect(snapshot.bonds, hasLength(bleBondSlots));
      expect(snapshot.bondCount, 2);
      expect(snapshot.emptyBondSlots, 1);
      expect(snapshot.bonds[0].addressText, '4C:23:38:74:E6:F6');
      expect(snapshot.bonds[0].name, 'Desk phone');
      expect(snapshot.bonds[1].name, 'Pixel');
      expect(snapshot.windowMs, 48000);
      expect(snapshot.windowOpen, isTrue);
      expect(snapshot.enabled, isTrue);
      expect(snapshot.displayName, 'Workbench');
    });

    test('the bond count comes from the array, not a field', () {
      // There is no `Bond count` field any more: the number is how many slots hold an address.
      final two = BleSecuritySnapshot.fromReads(
        block(
          bonds: const [('4C:23:38:74:E6:F6', ''), ('E4:B0:63:C8:20:72', '')],
        ),
      );
      expect(two.bondCount, 2);
      expect(two.emptyBondSlots, 1);
      final none = BleSecuritySnapshot.fromReads(block());
      expect(none.bondCount, 0);
      expect(none.emptyBondSlots, 3);
    });

    test('keeps the empty default for a field the device did not answer', () {
      final snapshot = BleSecuritySnapshot.fromReads(const {});
      // An unanswered read is "unknown", not "no passkey": the passkey stays empty rather than
      // becoming the Just Works sentinel, which the panel would print as a real setting.
      expect(snapshot.passkey, '');
      expect(snapshot.mode, bleModeJustWorks);
      expect(snapshot.bondCount, 0);
      expect(snapshot.windowMs, 0);
      expect(snapshot.enabled, isFalse);
      expect(snapshot.displayName, '');
      // Nothing at all: the state a block reports without driving a radio.
      expect(snapshot.isEmpty, isTrue);
    });

    test('records which fields the device actually answered', () {
      final answered = BleSecuritySnapshot.fromReads({
        bleFieldPasskey: read(
          DataType.text.value,
          text('246810', blePasskeyLength),
          size: blePasskeyLength,
        ),
        bleFieldMode: read(DataType.enum_.value, [bleModePasskey], size: 1),
        // Window, Enabled, Bonds and the display name are not answered.
      });
      expect(answered.answeredField(bleFieldPasskey), isTrue);
      expect(answered.answeredField(bleFieldMode), isTrue);
      expect(answered.answeredField(bleFieldWindow), isFalse);
      expect(answered.answeredField(bleFieldEnabled), isFalse);
      expect(answered.answeredField(bleFieldBonds), isFalse);
      // A field that was skipped is not the same as a field the device reported as empty.
      expect(answered.passkey, '246810');
      expect(answered.answeredField(bleFieldDisplayName), isFalse);
      // A hand-stated snapshot claims every field, so nothing is "unknown" about it.
      const stated = BleSecuritySnapshot(enabled: true);
      expect(stated.answeredField(bleFieldWindow), isTrue);
    });

    test('reads the window as an unsigned 32-bit millisecond count', () {
      // 300000 ms is the device's own five-minute recovery window - above 16 bits, which is the
      // value a two-byte read would truncate.
      final snapshot = BleSecuritySnapshot.fromReads(block(windowMs: 300000));
      expect(snapshot.windowMs, 300000);
    });

    test('the bond list explains itself, or says nothing when unanswered', () {
      // A reading that answered something but not the Bonds field makes no claim about the store
      // (a hand-stated snapshot is the one that claims every field, by design).
      expect(
        bleBondsDetail(
          BleSecuritySnapshot.fromReads({
            bleFieldMode: read(DataType.enum_.value, [
              bleModeJustWorks,
            ], size: 1),
          }),
        ),
        isNull,
      );
      // An empty store is the pairing state, and the line says how a peer gets in.
      final empty = BleSecuritySnapshot.fromReads(block());
      expect(bleBondsDetail(empty), contains('Any phone may pair'));
      expect(bleBondsDetail(empty), contains(bleBondCannotAdd));
      // Two of three slots, and a full list (which evicts the oldest).
      final two = BleSecuritySnapshot.fromReads(
        block(
          bonds: const [('4C:23:38:74:E6:F6', ''), ('E4:B0:63:C8:20:72', '')],
        ),
      );
      expect(bleBondsDetail(two), contains('2 of 3 slots'));
      final full = BleSecuritySnapshot.fromReads(
        block(
          bonds: const [
            ('4C:23:38:74:E6:F6', ''),
            ('E4:B0:63:C8:20:72', ''),
            ('D1:22:33:44:55:66', ''),
          ],
        ),
      );
      expect(bleBondsDetail(full), contains('replaces the oldest'));
    });
  });

  group('a refused Bonds write', () {
    final held = decodeBleBonds(
      bondsValue(const [('4C:23:38:74:E6:F6', 'Desk phone')]),
    );

    test('names the link when the app is not on USB', () {
      final sentence = bleBondsRefusal(
        overUsb: false,
        sent: bleBondsRenamed(held, 0, 'Bench'),
        held: held,
      );
      expect(sentence, contains('only be written over USB'));
    });

    test('a fabricated address says a bond is made by pairing', () {
      // The device refuses the whole frame for an address its store does not hold: a register
      // write cannot create a bond. The sentence is about pairing, never a generic failure.
      final sent = encodeBleBonds([
        held[0],
        BleBond(address: addr('AA:BB:CC:DD:EE:01')),
        bleEmptyBond,
      ]);
      final sentence = bleBondsRefusal(overUsb: true, sent: sent, held: held);
      expect(sentence, contains(bleBondCannotAdd));
      expect(sentence, contains('pair'));
      expect(sentence, isNot(contains('refused the change')));
    });

    test('a list the device has moved on from asks for a re-read', () {
      // Every address matches what the panel read, so the panel invented nothing - the device's
      // store simply changed under it (an eviction, or a bond added elsewhere).
      final sentence = bleBondsRefusal(
        overUsb: true,
        sent: bleBondsRenamed(held, 0, 'Bench'),
        held: held,
      );
      expect(sentence, contains('not the one this panel read'));
      expect(sentence, isNot(contains(bleBondCannotAdd)));
    });

    test('the "cannot add" sentence says pairing, never a generic failure', () {
      expect(bleBondCannotAdd, contains('cannot be added by hand'));
      expect(bleBondCannotAdd, contains('pairing'));
    });
  });

  group('the window', () {
    test('0 closes, and the device takes it with the radio down', () {
      expect(bleWindowProblem(0, enabled: true), isNull);
      expect(bleWindowProblem(0, enabled: false), isNull);
    });

    test('a non-zero length needs the radio on', () {
      expect(bleWindowProblem(5000, enabled: true), isNull);
      expect(
        bleWindowProblem(5000, enabled: false),
        contains('radio is switched off'),
      );
    });

    test('nothing above INT32_MAX ms is accepted', () {
      // The device's countdown compares signed, so a longer deadline would read as already past:
      // it refuses rather than clamping.
      expect(bleWindowProblem(bleWindowMaximumMs, enabled: true), isNull);
      expect(
        bleWindowProblem(bleWindowMaximumMs + 1, enabled: true),
        contains('at most'),
      );
      expect(bleWindowProblem(-1, enabled: true), contains('negative'));
    });
  });

  group('the state the panel reports', () {
    BleSecurityCondition conditionOf(BleSecuritySnapshot snapshot) =>
        snapshot.condition;

    test('a switched-off radio wins over everything else', () {
      final snapshot = BleSecuritySnapshot.fromReads(
        block(
          enabled: false,
          bonds: const [('4C:23:38:74:E6:F6', '')],
          windowMs: 30000,
        ),
      );
      expect(conditionOf(snapshot), BleSecurityCondition.radioOff);
      expect(bleSecurityHeadline(snapshot.condition), 'Bluetooth is off');
    });

    test('an open window means discoverable', () {
      final snapshot = BleSecuritySnapshot.fromReads(block(windowMs: 45000));
      expect(conditionOf(snapshot), BleSecurityCondition.windowOpen);
      expect(bleSecurityHeadline(snapshot.condition), 'Discoverable now');
      // The copy names what pairing means at that moment: nobody is bonded yet.
      expect(
        bleSecurityDetail(snapshot.condition, snapshot),
        contains('waiting for its first phone to pair'),
      );
      expect(
        bleSecurityDetail(snapshot.condition, snapshot),
        endsWith('Any device may pair until the window closes.'),
      );
    });

    test('an open window with bonds says the window is for new pairings', () {
      final snapshot = BleSecuritySnapshot.fromReads(
        block(
          bonds: const [('4C:23:38:74:E6:F6', ''), ('E4:B0:63:C8:20:72', '')],
          windowMs: 45000,
        ),
      );
      expect(conditionOf(snapshot), BleSecurityCondition.windowOpen);
      // E64 ruling C: the window governs who may PAIR, never who may reconnect.
      expect(
        bleSecurityDetail(snapshot.condition, snapshot),
        'Any device may pair until the window closes.',
      );
    });

    test('a closed window with bonds says bonded phones still reconnect', () {
      final snapshot = BleSecuritySnapshot.fromReads(
        block(bonds: const [('4C:23:38:74:E6:F6', '')]),
      );
      expect(conditionOf(snapshot), BleSecurityCondition.closedBonded);
      // The device advertises to everyone and refuses the link of a phone whose bond it does not
      // hold (E66: no accept list), so it is not "not discoverable" - it is closed to that phone.
      expect(bleSecurityHeadline(snapshot.condition), 'Bonded phones only');
      expect(
        bleSecurityDetail(snapshot.condition, snapshot),
        'Bonded devices can reconnect at any time. New pairings need a window, which only USB '
        'can open.',
      );
    });

    test('a closed window with no bonds says any phone may pair', () {
      final snapshot = BleSecuritySnapshot.fromReads(block());
      expect(conditionOf(snapshot), BleSecurityCondition.closedNoBonds);
      // E64 ruling C: an empty store is on the air and admits any phone - it is the state a
      // first pairing is made in, not a device off the air.
      expect(
        bleSecurityHeadline(snapshot.condition),
        'No bonds: any phone may pair',
      );
      expect(
        bleSecurityDetail(snapshot.condition, snapshot),
        contains('any phone may pair'),
      );
      expect(
        bleSecurityDetail(snapshot.condition, snapshot),
        contains('five-minute window by itself'),
      );
      expect(
        bleSecurityDetail(snapshot.condition, snapshot),
        isNot(contains('off the air')),
      );
    });

    test('a block that describes no radio is named as such', () {
      final snapshot = BleSecuritySnapshot.fromReads(const {});
      expect(conditionOf(snapshot), BleSecurityCondition.noRadio);
    });

    test('a partial read says so instead of drawing a state from defaults', () {
      // The device answered the passkey and the mode but not Enabled/Window/Bonds. Those defaults
      // would read as "radio off, no window, no bonds" - a claim about the device.
      final snapshot = BleSecuritySnapshot.fromReads({
        bleFieldPasskey: read(
          DataType.text.value,
          text('246810', blePasskeyLength),
          size: blePasskeyLength,
        ),
        bleFieldMode: read(DataType.enum_.value, [bleModePasskey], size: 1),
      });
      expect(snapshot.missingStateField, isTrue);
      expect(conditionOf(snapshot), BleSecurityCondition.incomplete);
      expect(
        bleSecurityHeadline(snapshot.condition),
        'The device did not answer every field',
      );
      expect(bleSecurityDetail(snapshot.condition, snapshot), contains('"-"'));
    });

    test('a reading that answered the state fields is classified normally', () {
      final snapshot = BleSecuritySnapshot.fromReads(block(windowMs: 30000));
      expect(snapshot.missingStateField, isFalse);
      expect(conditionOf(snapshot), BleSecurityCondition.windowOpen);
    });
  });

  group('the window countdown', () {
    test('reads as m:ss and rounds a partial second up', () {
      expect(bleWindowText(0), 'closed');
      expect(bleWindowText(48000), '0:48 left');
      expect(bleWindowText(60000), '1:00 left');
      expect(bleWindowText(300000), '5:00 left');
      // A remainder must not read as closed: the device is still advertising.
      expect(bleWindowText(1), '0:01 left');
      expect(bleWindowText(999), '0:01 left');
    });
  });

  group('the passkey', () {
    test('the device accepts exactly six ASCII digits', () {
      expect(blePasskeyIsValid('123456'), isTrue);
      expect(blePasskeyIsValid('000000'), isTrue);
      expect(blePasskeyIsValid('12345'), isFalse);
      expect(blePasskeyIsValid('1234567'), isFalse);
      expect(blePasskeyIsValid('12345 '), isFalse);
      expect(blePasskeyIsValid('12a456'), isFalse);
      expect(blePasskeyIsValid(''), isFalse);
    });

    test('000000 is the block\'s "no passkey" value, not a code', () {
      expect(blePasskeyNone, '000000');
      expect(blePasskeyNone.length, blePasskeyLength);
      expect(bleModeLabels[bleModeJustWorks], 'Just Works');
      expect(bleModeLabels[bleModePasskey], 'Passkey');
    });
  });

  group('write restrictions', () {
    test('a USB link may change the block', () {
      expect(bleWriteRestriction(overUsb: true), isNull);
    });

    test('any other link is told before the form is filled', () {
      final restriction = bleWriteRestriction(overUsb: false)!;
      expect(restriction, contains('USB'));
      expect(restriction, contains('change'));
    });

    test(
      'a refusal names the link, or the value the device would not take',
      () {
        expect(
          bleWriteRefusal(overUsb: false),
          contains('only be written over USB'),
        );
        expect(
          bleWriteRefusal(overUsb: true, valueHint: 'six digits'),
          contains('six digits'),
        );
        expect(
          bleWriteRefusal(overUsb: true),
          'The device refused the change.',
        );
      },
    );
  });

  group('guidance after a failed Bluetooth attempt', () {
    test('an authentication/encryption error is named as a stale bond', () {
      // The shape Android and BlueZ produce for a link the device will not encrypt.
      final guidance = bleLinkGuidance(
        errorText:
            'BLE connect failed: GATT_INSUFFICIENT_AUTHENTICATION (status 5)',
      );
      expect(guidance.symptom, BleLinkSymptom.securityRefused);
      expect(guidance.likelyStaleBond, isTrue);
      expect(
        guidance.headline,
        'This phone is no longer paired with the device',
      );
      // The card's test is that this must not read as a broken device.
      expect(guidance.detail, contains('the device is working'));
      expect(guidance.detail, contains('firmware update'));
      // E68: a changed passkey revokes every bond too, so it is named as a third cause and the
      // bond list is not left as the only one.
      expect(guidance.detail, contains('changing the passkey'));
      expect(guidance.steps.first, contains('Bluetooth settings'));
      expect(guidance.steps.join(' '), contains('Forget'));
    });

    test('a link that comes up and dies is the same stale bond', () {
      final guidance = bleLinkGuidance(droppedWhileSecuring: true);
      expect(guidance.symptom, BleLinkSymptom.securityRefused);
      expect(guidance.likelyStaleBond, isTrue);
    });

    test('a drop inside the passkey budget is still a securing drop', () {
      // The firmware gives a passkey pairing 30 s (a person types six digits) and Just Works 5 s.
      // A drop at 8 s is therefore a pairing that did not complete, not a link that went away -
      // and it is the band a passkey change (which revokes the phone's bond) lands in.
      for (final lasted in [0, 4999, 5000, 8000, 29999]) {
        expect(
          LinkTrouble(
            linkName: 'Tamu',
            isBle: true,
            lastedMs: lasted,
          ).droppedDuringSecuring,
          isTrue,
          reason: 'a $lasted ms BLE session died inside the securing budget',
        );
      }
    });

    test(
      'a session that outlives both budgets is a lost link, not a refusal',
      () {
        expect(
          LinkTrouble(
            linkName: 'Tamu',
            isBle: true,
            lastedMs: bleSecureBudgetPasskeyMs,
          ).droppedDuringSecuring,
          isFalse,
        );
        // USB never pairs, so a short USB session is just a dead cable.
        expect(
          LinkTrouble(
            linkName: '/dev/ttyACM0',
            isBle: false,
            lastedMs: 300,
          ).droppedDuringSecuring,
          isFalse,
        );
      },
    );

    test('a timeout points at the device, not at a refused bond', () {
      final guidance = bleLinkGuidance(
        errorText: 'BLE connect failed: TimeoutException after 0:00:30',
      );
      expect(guidance.symptom, BleLinkSymptom.unreachable);
      expect(guidance.likelyStaleBond, isFalse);
      expect(guidance.headline, 'The device did not answer this phone');
      // E66: a stranger's CONNECT_REQ is answered and the link refused once it cannot prove a
      // bond, so that refusal reaches the phone as a failed pairing - which leaves the device
      // being off / out of range as the cause of an attempt that got no answer at all. The
      // answered-then-refused mechanism is named first, and an empty store is never the reason.
      expect(guidance.detail, contains('connect request from any phone'));
      expect(guidance.detail, contains('refuses the link'));
      expect(
        guidance.detail.indexOf('refuses the link'),
        lessThan(guidance.detail.indexOf('out of range')),
      );
      expect(guidance.detail, isNot(contains('off the air')));
      expect(guidance.detail, isNot(contains('link layer')));
      expect(guidance.detail, contains('admits any phone'));
      expect(guidance.steps.join(' '), contains('pairing window'));
    });

    test('an unnamed failure lists both causes instead of guessing', () {
      // Android's generic GATT 133 covers both, so the copy must not pick one.
      final guidance = bleLinkGuidance(
        errorText: 'BLE connect failed: GATT_ERROR (status 133)',
      );
      expect(guidance.symptom, BleLinkSymptom.unknown);
      expect(guidance.likelyStaleBond, isFalse);
      // The second cause is a device that is off or has its radio off, never an empty store
      // (an empty store admits any phone, so it can always be found), and never a link-layer drop
      // (E66 deleted the accept list).
      expect(guidance.detail, isNot(contains('off the air')));
      expect(guidance.detail, isNot(contains('link layer')));
      expect(guidance.detail, contains('admits any phone'));
      expect(guidance.steps.join(' '), contains('pairing window'));
      expect(guidance.steps.join(' '), contains('Forget'));
    });

    test('the classifier is reachable without building the copy', () {
      expect(
        classifyBleLinkFailure(errorText: 'insufficient encryption'),
        BleLinkSymptom.securityRefused,
      );
      expect(classifyBleLinkFailure(), BleLinkSymptom.unknown);
      expect(
        classifyBleLinkFailure(droppedWhileSecuring: true),
        BleLinkSymptom.droppedDuringSecuring,
      );
    });
  });
}
