import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/core/wifi_settings.dart';

/// The WiFi Settings block is a compile-in firmware option, off by default, so the app has to
/// behave for both shapes: a device with no block at all (the normal case - nothing shown), and
/// a block that is present but carries nothing (the panel says "not implemented" instead of
/// drawing an empty network). These cases pin the field map, the seven firmware states, the
/// address formatting and the condition rules.
void main() {
  ({ValueInfo meta, List<int> value}) read(
    int type,
    List<int> value, {
    int flags = 0,
  }) => (
    meta: ValueInfo(type: type, size: value.length, flags: flags),
    value: value,
  );

  List<int> text(String s, {int width = 0}) {
    final bytes = s.codeUnits.toList();
    if (width > bytes.length) {
      bytes.addAll(List<int>.filled(width - bytes.length, 0));
    }
    return bytes;
  }

  /// The 16-byte `IP address` field for an IPv4 address, in the IPv4-mapped form the firmware
  /// reports (`::ffff:a.b.c.d`).
  List<int> ipv4Mapped(String dotted) {
    final parts = dotted.split('.').map(int.parse).toList();
    return [for (var i = 0; i < 10; i++) 0, 0xFF, 0xFF, ...parts];
  }

  Map<int, ({ValueInfo meta, List<int> value})?> fields({
    bool? enabled,
    String? ssid,
    String? password,
    int? state,
    List<int>? ip,
  }) => {
    wifiFieldEnabled: enabled == null
        ? null
        : read(DataType.bool_.value, [
            enabled ? 1 : 0,
          ], flags: ValueFlags.persistent),
    wifiFieldSsid: ssid == null
        ? null
        : read(
            DataType.text.value,
            text(ssid, width: 32),
            flags: ValueFlags.persistent,
          ),
    wifiFieldPassword: password == null
        ? null
        : read(
            DataType.text.value,
            text(password, width: 32),
            flags: ValueFlags.persistent,
          ),
    wifiFieldState: state == null
        ? null
        : read(DataType.enum_.value, [state], flags: ValueFlags.readOnly),
    wifiFieldIp: ip == null
        ? null
        : read(DataType.undefined.value, ip, flags: ValueFlags.readOnly),
  };

  group('the block is optional', () {
    test('a device without the block reports no entry', () {
      expect(
        hasWifiSettingsBlock(const [
          (type: 0x03, maxInstance: 0),
          (type: 0x08, maxInstance: 0),
        ]),
        isFalse,
      );
    });

    test('a device with the block reports type 0x0C', () {
      expect(
        hasWifiSettingsBlock(const [
          (type: 0x03, maxInstance: 0),
          (type: wifiSettingsBlockTypeValue, maxInstance: 0),
        ]),
        isTrue,
      );
      expect(wifiSettingsBlockTypeValue, 0x0C);
    });
  });

  group('reading the fields', () {
    test('every field is what the device reported, NUL padding stripped', () {
      final snapshot = WifiSnapshot.fromFields(
        fields(
          enabled: true,
          ssid: 'TamuNet',
          password: '',
          state: wifiStateConnected,
          ip: ipv4Mapped('192.168.1.42'),
        ),
      );
      expect(snapshot.enabled, isTrue);
      expect(snapshot.ssid, 'TamuNet');
      expect(snapshot.state, wifiStateConnected);
      expect(snapshot.ip, '::ffff:192.168.1.42');
    });

    test('a field the device did not answer keeps the empty default', () {
      final snapshot = WifiSnapshot.fromFields(
        fields(state: wifiStateConnecting),
      );
      expect(snapshot.enabled, isFalse);
      expect(snapshot.ssid, '');
      expect(snapshot.ip, '');
      expect(snapshot.state, wifiStateConnecting);
    });

    test('any non-zero Enabled byte reads as on', () {
      expect(
        WifiSnapshot.fromFields({
          wifiFieldEnabled: read(DataType.bool_.value, const [1]),
        }).enabled,
        isTrue,
      );
      expect(
        WifiSnapshot.fromFields({
          wifiFieldEnabled: read(DataType.bool_.value, const [0]),
        }).enabled,
        isFalse,
      );
    });

    test('the seven states are the firmware\'s own names', () {
      expect(wifiStateLabel(wifiStateDisconnected), 'Disconnected');
      expect(wifiStateLabel(wifiStateConnecting), 'Connecting');
      expect(wifiStateLabel(wifiStateConnected), 'Connected');
      expect(wifiStateLabel(wifiStateNoSsid), 'No SSID');
      expect(wifiStateLabel(wifiStateNoPassword), 'No password');
      expect(wifiStateLabel(wifiStateWrongPassword), 'Wrong password');
      expect(wifiStateLabel(wifiStateError), 'Error');
      expect(wifiStateLabels.length, 7);
      expect(wifiStateLabel(9), 'State 9');
    });
  });

  group('the address field', () {
    test('all zero is no address, not 0.0.0.0', () {
      expect(wifiAddressText(List<int>.filled(16, 0)), '');
      expect(wifiAddressText(const []), '');
    });

    test('the IPv4-mapped form prints the document\'s ::ffff: notation', () {
      expect(wifiAddressText(ipv4Mapped('192.168.0.9')), '::ffff:192.168.0.9');
      expect(wifiAddressText(ipv4Mapped('10.0.0.254')), '::ffff:10.0.0.254');
    });

    test(
      'another 16-byte address prints as IPv6, longest zero run compressed',
      () {
        // 2001:0db8:0000:0000:0000:0000:0000:0001
        final v6 = [
          0x20,
          0x01,
          0x0d,
          0xb8,
          0,
          0,
          0,
          0,
          0,
          0,
          0,
          0,
          0,
          0,
          0,
          0x01,
        ];
        expect(wifiAddressText(v6), '2001:db8::1');
        // A single zero group is not compressed (::-compression needs two).
        final oneZero = [
          0x20,
          0x01,
          0x0d,
          0xb8,
          0,
          0,
          0,
          0x01,
          0,
          0x01,
          0,
          0x02,
          0,
          0x03,
          0x04,
          0x05,
        ];
        expect(wifiAddressText(oneZero), '2001:db8:0:1:1:2:3:405');
      },
    );

    test(
      'a field that is not 16 bytes is printed, never turned into an address',
      () {
        expect(wifiAddressText(const [1, 2]), '0102');
      },
    );
  });

  group('the condition the panel reports', () {
    test(
      'an empty Disconnected block with the radio off is "not implemented"',
      () {
        final snapshot = WifiSnapshot.fromFields(
          fields(
            enabled: false,
            ssid: '',
            password: '',
            state: wifiStateDisconnected,
            ip: List<int>.filled(16, 0),
          ),
        );
        expect(snapshot.isEmpty, isTrue);
        expect(snapshot.condition, WifiCondition.notImplemented);
        expect(
          wifiConditionHeadline(snapshot.condition),
          'Not implemented on this device',
        );
        expect(
          wifiConditionDetail(snapshot.condition, snapshot),
          'The WiFi block reports no network, no password and no address, and its State '
          'is Disconnected.',
        );
      },
    );

    test('a block mid-join is not "not implemented"', () {
      // State is the device's answer: it is trying, so the block is not a stub.
      final snapshot = WifiSnapshot.fromFields(
        fields(state: wifiStateConnecting),
      );
      expect(snapshot.condition, WifiCondition.connecting);
      expect(wifiConditionHeadline(snapshot.condition), 'Connecting');
    });

    test(
      'a saved network with the radio off is disabled, not unimplemented',
      () {
        final snapshot = WifiSnapshot.fromFields(
          fields(enabled: false, ssid: 'TamuNet', state: wifiStateDisconnected),
        );
        expect(snapshot.condition, WifiCondition.disabled);
        expect(wifiConditionHeadline(snapshot.condition), 'WiFi is disabled');
        expect(
          wifiConditionDetail(snapshot.condition, snapshot),
          'The radio is switched off; the saved network is "TamuNet".',
        );
      },
    );

    test('an address alone is enough to be a real network', () {
      final snapshot = WifiSnapshot.fromFields(
        fields(
          enabled: false,
          state: wifiStateDisconnected,
          ip: ipv4Mapped('10.0.0.7'),
        ),
      );
      expect(snapshot.isEmpty, isFalse);
      expect(snapshot.condition, WifiCondition.disabled);
    });

    test('the device\'s own failure states each get their own sentence', () {
      final noSsid = WifiSnapshot.fromFields(
        fields(enabled: true, state: wifiStateNoSsid),
      );
      expect(noSsid.condition, WifiCondition.noNetwork);
      expect(wifiConditionHeadline(noSsid.condition), 'No network configured');

      final noPassword = WifiSnapshot.fromFields(
        fields(enabled: true, ssid: 'TamuNet', state: wifiStateNoPassword),
      );
      expect(noPassword.condition, WifiCondition.noPassword);
      expect(wifiConditionHeadline(noPassword.condition), 'No password set');

      final wrong = WifiSnapshot.fromFields(
        fields(enabled: true, ssid: 'TamuNet', state: wifiStateWrongPassword),
      );
      expect(wrong.condition, WifiCondition.wrongPassword);
      expect(
        wifiConditionDetail(wrong.condition, wrong),
        'The device rejected the password for "TamuNet".',
      );
      expect(wifiConditionTone(wrong.condition), WifiTone.error);

      final error = WifiSnapshot.fromFields(
        fields(enabled: true, ssid: 'TamuNet', state: wifiStateError),
      );
      expect(error.condition, WifiCondition.error);
      expect(wifiConditionTone(error.condition), WifiTone.error);
    });

    test('enabled and idle, and the legacy no-SSID fallback', () {
      expect(
        WifiSnapshot.fromFields(
          fields(enabled: true, ssid: 'TamuNet', state: wifiStateDisconnected),
        ).condition,
        WifiCondition.idle,
      );
      // A build that reports Disconnected while enabled with no SSID still reads honestly.
      expect(
        WifiSnapshot.fromFields(
          fields(enabled: true, state: wifiStateDisconnected),
        ).condition,
        WifiCondition.noNetwork,
      );
    });

    test('connected comes straight from the device state', () {
      final connected = WifiSnapshot.fromFields(
        fields(
          enabled: true,
          ssid: 'TamuNet',
          state: wifiStateConnected,
          ip: ipv4Mapped('192.168.0.9'),
        ),
      );
      expect(connected.condition, WifiCondition.connected);
      expect(wifiConditionHeadline(connected.condition), 'Connected');
      expect(
        wifiConditionDetail(connected.condition, connected),
        'The device holds the address ::ffff:192.168.0.9.',
      );
      expect(wifiConditionTone(connected.condition), WifiTone.success);
    });

    test('a state this build does not know is named, not guessed at', () {
      final unknown = WifiSnapshot.fromFields(fields(state: 9));
      expect(unknown.condition, WifiCondition.unknownState);
      expect(wifiConditionHeadline(unknown.condition), 'Unrecognised state');
      expect(
        wifiConditionDetail(unknown.condition, unknown),
        'The device reports State 9, which this build does not know.',
      );
    });

    test('the password is only shown when the device reads it back', () {
      expect(
        WifiSnapshot.fromFields(fields(password: 'hunter2')).passwordReadsBack,
        isTrue,
      );
      // The block is write-only, so this is the normal answer.
      expect(WifiSnapshot.fromFields(fields()).passwordReadsBack, isFalse);
    });
  });
}
