import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/ble_security.dart';
import 'package:tamuapp/core/block_registry.dart';
import 'package:tamuapp/core/connection.dart' show LinkTrouble;
import 'package:tamuapp/core/types.dart';

/// The `BLE Settings` block's app half, off-device: the field map, the reading rules, the two
/// texts the panel builds (the window countdown and the passkey state) and the guidance after a
/// failed Bluetooth attempt.
void main() {
  ({ValueInfo meta, List<int> value}) read(
    int type,
    List<int> value, {
    int flags = 0,
  }) => (
    meta: ValueInfo(type: type, size: value.length, flags: flags),
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

  Map<int, ({ValueInfo meta, List<int> value})?> block({
    String passkey = '000000',
    int mode = bleModeJustWorks,
    int bondCount = 0,
    int windowMs = 0,
    bool enabled = true,
    String displayName = '',
    String advertisedName = '',
  }) => {
    bleFieldPasskey: read(DataType.text.value, text(passkey, blePasskeyLength)),
    bleFieldMode: read(DataType.enum_.value, [mode]),
    bleFieldBondCount: read(DataType.integer.value, [bondCount]),
    bleFieldWindow: read(DataType.uint32.value, uint32ToBytes(windowMs)),
    bleFieldEnabled: read(DataType.bool_.value, [enabled ? 1 : 0]),
    bleFieldDisplayName: read(
      DataType.text.value,
      text(displayName, bleDisplayNameWidth),
    ),
    bleFieldAdvertisedName: read(
      DataType.text.value,
      text(advertisedName, bleDisplayNameWidth),
    ),
  };

  Map<int, ({ValueInfo meta, List<int> value})?> bondReads(
    List<String> addresses,
  ) => {
    for (var key = 0; key < bleBondSlots; key++)
      key: read(
        DataType.text.value,
        text(key < addresses.length ? addresses[key] : '', 20),
      ),
  };

  group('the field map', () {
    test('matches the block table the firmware publishes', () {
      // Guards/BleSettings.h: the panel reads by number, so a re-ordered firmware table would
      // silently read another field. The registry must name every field in the same order.
      expect(
        [
          bleFieldPasskey,
          bleFieldMode,
          bleFieldBondCount,
          bleFieldBond,
          bleFieldWindow,
          bleFieldAction,
          bleFieldEnabled,
          bleFieldDisplayName,
          bleFieldAdvertisedName,
          bleFieldLastPeer,
          bleFieldLastAir,
          bleFieldLastLink,
        ],
        [for (var i = 0; i < bleFieldCount; i++) i],
      );
      final info = blockInfoFor(BlockType.bleSettings)!;
      expect(info.typeName, 'BLE Settings');
      expect(info.fields.length, bleFieldCount);
    });

    test(
      'the Bond field exposes exactly the store capacity the firmware pins',
      () {
        expect(bleBondSlots, 3);
        expect(BlockType.bleSettings.value, bleSettingsBlockTypeValue);
      },
    );

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

  group('a reading', () {
    test('carries every field the device answered', () {
      final snapshot = BleSecuritySnapshot.fromReads(
        block(
          passkey: '123456',
          mode: bleModePasskey,
          bondCount: 2,
          windowMs: 48000,
          enabled: true,
          displayName: 'Workbench',
          advertisedName: 'Workbench',
        ),
        bonds: bondReads(const ['C4:7F:51:AA:BB:CC', 'D0:11:22:33:44:55']),
      );

      expect(snapshot.passkey, '123456');
      expect(snapshot.passkeySet, isTrue);
      expect(snapshot.mode, bleModePasskey);
      expect(snapshot.bondCount, 2);
      expect(snapshot.bonds, ['C4:7F:51:AA:BB:CC', 'D0:11:22:33:44:55']);
      expect(snapshot.windowMs, 48000);
      expect(snapshot.windowOpen, isTrue);
      expect(snapshot.enabled, isTrue);
      expect(snapshot.displayName, 'Workbench');
      expect(snapshot.advertisedName, 'Workbench');
    });

    test('drops the empty bond slots instead of listing blanks', () {
      final snapshot = BleSecuritySnapshot.fromReads(
        block(bondCount: 1),
        // The block answers every slot; the unused ones hold padding only.
        bonds: bondReads(const ['C4:7F:51:AA:BB:CC']),
      );
      expect(snapshot.bonds, ['C4:7F:51:AA:BB:CC']);
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
        ),
        bleFieldMode: read(DataType.enum_.value, [bleModePasskey]),
        // Window, Enabled, Bond count and the names are not answered.
      });
      expect(answered.answeredField(bleFieldPasskey), isTrue);
      expect(answered.answeredField(bleFieldMode), isTrue);
      expect(answered.answeredField(bleFieldWindow), isFalse);
      expect(answered.answeredField(bleFieldEnabled), isFalse);
      expect(answered.answeredField(bleFieldBondCount), isFalse);
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

    test('reads the E66 last-link read-out and the verdict it names', () {
      final snapshot = BleSecuritySnapshot.fromReads({
        bleFieldLastPeer: read(
          DataType.text.value,
          text('C4:7F:51:AA:BB:CC', 20),
        ),
        bleFieldLastAir: read(
          DataType.text.value,
          text('5E:9A:1B:2C:3D:4E', 20),
        ),
        bleFieldLastLink: read(DataType.enum_.value, [bleVerdictAdmitted]),
      });

      expect(snapshot.lastPeer, 'C4:7F:51:AA:BB:CC');
      expect(snapshot.lastAir, '5E:9A:1B:2C:3D:4E');
      expect(snapshot.lastLink, bleVerdictAdmitted);
      // The two differing is the resolved rotating address, which is what E66 exists to show.
      expect(snapshot.lastAddressResolved, isTrue);
      expect(bleVerdictLabel(snapshot.lastLink), 'Admitted');

      // An unanswered verdict is not a device state: it keeps the `None` default and the panel
      // prints `-` for it rather than "No link judged" (a snapshot that answered *something* is
      // the only one that can carry a missing field).
      final skipped = BleSecuritySnapshot.fromReads({
        bleFieldMode: read(DataType.enum_.value, [bleModeJustWorks]),
      });
      expect(skipped.lastLink, bleVerdictNone);
      expect(skipped.answeredField(bleFieldLastLink), isFalse);
      expect(skipped.lastAddressResolved, isFalse);
    });

    test('compares the address part of the last-link pair, never its type suffix', () {
      // The device writes both fields as "AA:BB:CC:DD:EE:FF/N". NimBLE normalises the
      // controller's resolved types 2/3 to 0/1, so the suffix is information only: two readings
      // of one address must not read as "resolved" just because their types differ.
      BleSecuritySnapshot readPair(String peer, String air) =>
          BleSecuritySnapshot.fromReads({
            bleFieldLastPeer: read(DataType.text.value, text(peer, 20)),
            bleFieldLastAir: read(DataType.text.value, text(air, 20)),
            bleFieldLastLink: read(DataType.enum_.value, [bleVerdictAdmitted]),
          });

      // Same address, different type: not resolved.
      expect(
        readPair(
          '4C:23:38:74:E6:F6/0',
          '4C:23:38:74:E6:F6/1',
        ).lastAddressResolved,
        isFalse,
      );

      // The rig's own reading: a peer connecting from its public address is equal addresses, and
      // that is the correct result rather than a failed read-out.
      expect(
        readPair(
          '4C:23:38:74:E6:F6/0',
          '4C:23:38:74:E6:F6/0',
        ).lastAddressResolved,
        isFalse,
      );

      // A different address on the air IS the resolution, whatever the types say.
      expect(
        readPair(
          '4C:23:38:74:E6:F6/0',
          '5E:9A:1B:2C:3D:4E/1',
        ).lastAddressResolved,
        isTrue,
      );

      expect(bleAddressPart('5E:9A:1B:2C:3D:4E/1'), '5E:9A:1B:2C:3D:4E');
      // A reading with no suffix compares as the whole text, so an older firmware still works.
      expect(bleAddressPart('5E:9A:1B:2C:3D:4E'), '5E:9A:1B:2C:3D:4E');
      expect(bleAddressPart(''), '');
    });

    test('the verdict vocabulary is the firmware BleLinkVerdict numbering', () {
      // BleAdmissionPolicy.h: 0 unjudged, 1 pending, 2 admitted, 3-6 the four refusals. The
      // numbers are wire values, so a re-ordered firmware enum must fail here rather than label
      // a refusal as an admission.
      expect(
        bleVerdictLabels.keys.toList()..sort(),
        List.generate(7, (i) => i),
      );
      expect(
        bleVerdictLabel(bleVerdictRefusedNotBonded),
        'Refused: not bonded',
      );
      expect(bleVerdictDetail(bleVerdictNone), contains('booted'));
      expect(
        bleVerdictDetail(bleVerdictRefusedNotBonded),
        contains('stranger'),
      );
      // A value this build does not know is named, never dropped.
      expect(bleVerdictLabel(9), 'Verdict 9');
      expect(bleVerdictDetail(9), contains('does not know'));
    });
  });

  group('the state the panel reports', () {
    BleSecurityCondition conditionOf(BleSecuritySnapshot snapshot) =>
        snapshot.condition;

    test('a switched-off radio wins over everything else', () {
      final snapshot = BleSecuritySnapshot.fromReads(
        block(enabled: false, bondCount: 2, windowMs: 30000),
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
        block(bondCount: 2, windowMs: 45000),
      );
      expect(conditionOf(snapshot), BleSecurityCondition.windowOpen);
      // E64 ruling C: the window governs who may PAIR, never who may reconnect.
      expect(
        bleSecurityDetail(snapshot.condition, snapshot),
        'Any device may pair until the window closes.',
      );
    });

    test('a closed window with bonds says bonded phones still reconnect', () {
      final snapshot = BleSecuritySnapshot.fromReads(block(bondCount: 2));
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
      // The device answered the passkey and the mode but not Enabled/Window/Bond count. Those
      // defaults would read as "radio off, no window, no bonds" - a claim about the device.
      final snapshot = BleSecuritySnapshot.fromReads({
        bleFieldPasskey: read(
          DataType.text.value,
          text('246810', blePasskeyLength),
        ),
        bleFieldMode: read(DataType.enum_.value, [bleModePasskey]),
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
      // E68: a changed passkey revokes every bond too, so it is named as a third cause and
      // "Forget all bonds" is not left as the only one.
      expect(guidance.detail, contains('changing the passkey'));
      expect(guidance.detail, isNot(contains('only')));
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
