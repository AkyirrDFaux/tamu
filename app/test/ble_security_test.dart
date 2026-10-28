import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/ble_security.dart';
import 'package:tamuapp/core/block_registry.dart';
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

    test('reads the window as an unsigned 32-bit millisecond count', () {
      // 300000 ms is the device's own five-minute recovery window - above 16 bits, which is the
      // value a two-byte read would truncate.
      final snapshot = BleSecuritySnapshot.fromReads(block(windowMs: 300000));
      expect(snapshot.windowMs, 300000);
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
    });

    test('a closed window with bonds is not discoverable, and says why', () {
      final snapshot = BleSecuritySnapshot.fromReads(block(bondCount: 2));
      expect(conditionOf(snapshot), BleSecurityCondition.closedBonded);
      expect(bleSecurityHeadline(snapshot.condition), 'Not discoverable');
      expect(
        bleSecurityDetail(snapshot.condition, snapshot),
        contains('advertises nothing outside a pairing window'),
      );
    });

    test('a closed window with no bonds names the boot window', () {
      final snapshot = BleSecuritySnapshot.fromReads(block());
      expect(conditionOf(snapshot), BleSecurityCondition.closedNoBonds);
      expect(
        bleSecurityDetail(snapshot.condition, snapshot),
        contains('five-minute window by itself'),
      );
    });

    test('a block that describes no radio is named as such', () {
      final snapshot = BleSecuritySnapshot.fromReads(const {});
      expect(conditionOf(snapshot), BleSecurityCondition.noRadio);
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
      expect(guidance.steps.first, contains('Bluetooth settings'));
      expect(guidance.steps.join(' '), contains('Forget'));
    });

    test('a link that comes up and dies is the same stale bond', () {
      final guidance = bleLinkGuidance(droppedSoonAfterConnect: true);
      expect(guidance.symptom, BleLinkSymptom.securityRefused);
      expect(guidance.likelyStaleBond, isTrue);
    });

    test('a timeout is the device not answering, not a bond problem', () {
      final guidance = bleLinkGuidance(
        errorText: 'BLE connect failed: TimeoutException after 0:00:30',
      );
      expect(guidance.symptom, BleLinkSymptom.unreachable);
      expect(guidance.likelyStaleBond, isFalse);
      expect(guidance.headline, 'The device did not answer');
      expect(guidance.steps.join(' '), contains('pairing window'));
    });

    test('an unnamed failure lists both causes instead of guessing', () {
      // Android's generic GATT 133 covers both, so the copy must not pick one.
      final guidance = bleLinkGuidance(
        errorText: 'BLE connect failed: GATT_ERROR (status 133)',
      );
      expect(guidance.symptom, BleLinkSymptom.unknown);
      expect(guidance.likelyStaleBond, isFalse);
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
        classifyBleLinkFailure(droppedSoonAfterConnect: true),
        BleLinkSymptom.droppedDuringSecuring,
      );
    });
  });
}
