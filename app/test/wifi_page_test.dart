import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/core/wifi_settings.dart';
import 'package:tamuapp/ui/formatters.dart';
import 'package:tamuapp/ui/theme.dart';
import 'package:tamuapp/ui/wifi_page.dart';

/// The WiFi panel, served by a fake device so the states are reachable off-rig.
///
/// The block is a compile-in firmware option that is off by default, so the two shapes that
/// matter most are: a device whose enumeration carries no `0x0C` (the panel draws NOTHING - not
/// an empty card, not an error), and a block that is present but empty (the panel says WiFi is
/// not implemented on this device). The rest covers the values read straight from the block, the
/// password's secret treatment, and a write reaching the device followed by a device save.
class _FakeWifiDevice extends RegisterClient {
  _FakeWifiDevice({
    required super.deviceId,
    required this.present,
    Map<int, ({ValueInfo meta, List<int> value})>? fields,
    this.refuseWrites = false,
  }) : fields = fields ?? {};

  /// Whether the enumeration reports the WiFi Settings block.
  final bool present;

  /// Models the firmware's USB-only write rule: every write is answered with a refusal.
  final bool refuseWrites;

  /// The block's live field values; a write lands here so a re-read shows it.
  Map<int, ({ValueInfo meta, List<int> value})> fields;

  /// Every write the page sent, in order.
  final writes = <({int field, List<int> bytes})>[];

  /// How many times the page asked the device to persist (Save All).
  int saves = 0;

  @override
  Future<List<({int type, int maxInstance})>?> enumerateBlockTypes() async => [
    (type: BlockType.button.value, maxInstance: 0),
    if (present) (type: wifiSettingsBlockTypeValue, maxInstance: 0),
  ];

  @override
  Future<({ValueInfo meta, List<int> value})?> readBlockField(
    int blockType,
    int instance,
    int field,
    int key,
  ) async => fields[field];

  @override
  Future<List<int>?> writeBlockField(
    int blockType,
    int instance,
    int field,
    int key,
    ValueInfo meta,
    List<int> value,
  ) async {
    if (refuseWrites) return null;
    writes.add((field: field, bytes: value));
    final current = fields[field];
    fields[field] = (
      meta: ValueInfo(
        type: meta.type,
        size: current?.meta.size ?? value.length,
        flags: current?.meta.flags ?? meta.flags,
      ),
      // The password is write-only on this fake: the device keeps the secret, and answers a
      // read of it with nothing. That is the shape the app must not paper over.
      value: field == wifiFieldPassword
          ? List<int>.filled(current?.meta.size ?? value.length, 0)
          : value,
    );
    return const [1];
  }

  @override
  Future<bool> saveAll() async {
    saves++;
    return true;
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  ({ValueInfo meta, List<int> value}) field(
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

  /// The `IP address` field's bytes for an IPv4 address, in the IPv4-mapped form the firmware
  /// reports (`::ffff:a.b.c.d`).
  List<int> ipv4Mapped(String dotted) => [
    for (var i = 0; i < 10; i++) 0,
    0xFF,
    0xFF,
    ...dotted.split('.').map(int.parse),
  ];

  const persistent = ValueFlags.persistent;
  const readOnly = ValueFlags.readOnly;

  /// The stock answer of a WiFi build that carries the block but no radio: settings off and
  /// empty, state Disconnected, no address.
  Map<int, ({ValueInfo meta, List<int> value})> emptyBlock() => {
    wifiFieldEnabled: field(DataType.bool_.value, const [0], flags: persistent),
    wifiFieldSsid: field(
      DataType.text.value,
      text('', 32),
      flags: persistent,
      size: 32,
    ),
    wifiFieldPassword: field(
      DataType.text.value,
      text('', 32),
      flags: persistent,
      size: 32,
    ),
    wifiFieldState: field(DataType.enum_.value, const [0], flags: readOnly),
    wifiFieldIp: field(
      DataType.undefined.value,
      List<int>.filled(16, 0),
      flags: readOnly,
      size: 16,
    ),
  };

  Future<_FakeWifiDevice> pump(
    WidgetTester tester, {
    required bool present,
    Map<int, ({ValueInfo meta, List<int> value})>? fields,
    bool overUsb = true,
    bool refuseWrites = false,
  }) async {
    final device = _FakeWifiDevice(
      deviceId: 1,
      present: present,
      fields: fields,
      refuseWrites: refuseWrites,
    );
    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        home: WifiPage(
          deviceId: 1,
          isConnected: () => true,
          isUsbLink: () => overUsb,
          clientFactory: (_) => device,
        ),
      ),
    );
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull, reason: 'the panel threw');
    return device;
  }

  group('a device without the block', () {
    testWidgets('the panel draws nothing at all', (tester) async {
      await pump(tester, present: false);

      // No panel, no "unavailable", no empty card: the block is a compile-in option and this
      // device does not carry it.
      expect(find.text('WiFi Settings'), findsNothing);
      expect(find.textContaining('Not implemented'), findsNothing);
      expect(find.textContaining('unavailable'), findsNothing);
      expect(find.byType(Card), findsNothing);
      // The page shell is still a page; nothing inside it.
      expect(find.text('WiFi'), findsOneWidget);
    });
  });

  group('a block that is present but empty', () {
    testWidgets('the panel says WiFi is not implemented on this device', (
      tester,
    ) async {
      await pump(tester, present: true, fields: emptyBlock());

      expect(find.text('Not implemented on this device'), findsOneWidget);
      expect(
        find.text(
          'The WiFi block reports no network, no password and no address, and its State '
          'is Disconnected.',
        ),
        findsOneWidget,
      );
    });

    testWidgets('the fields print what the block holds', (tester) async {
      await pump(tester, present: true, fields: emptyBlock());

      expect(find.text('State'), findsOneWidget);
      expect(
        find.text('Disconnected'),
        findsOneWidget,
      ); // straight from the block
      expect(find.text('IP address'), findsOneWidget);
      expect(find.text('-'), findsOneWidget); // no address
      expect(find.text('not set'), findsOneWidget); // no SSID
      expect(find.text('not read back by the device'), findsOneWidget);
      expect(find.text('The radio is off'), findsOneWidget);
    });
  });

  group('a block that reports a live network', () {
    Map<int, ({ValueInfo meta, List<int> value})> connected() => {
      wifiFieldEnabled: field(DataType.bool_.value, const [
        1,
      ], flags: persistent),
      wifiFieldSsid: field(
        DataType.text.value,
        text('TamuNet', 32),
        flags: persistent,
        size: 32,
      ),
      wifiFieldPassword: field(
        DataType.text.value,
        text('hunter2', 32),
        flags: persistent,
        size: 32,
      ),
      wifiFieldState: field(DataType.enum_.value, const [2], flags: readOnly),
      wifiFieldIp: field(
        DataType.undefined.value,
        ipv4Mapped('192.168.0.9'),
        flags: readOnly,
        size: 16,
      ),
    };

    testWidgets('SSID, State and IP come straight from the block', (
      tester,
    ) async {
      await pump(tester, present: true, fields: connected());

      expect(
        find.text('Connected'),
        findsNWidgets(2),
      ); // banner headline + State row
      expect(
        find.text('The device holds the address ::ffff:192.168.0.9.'),
        findsOneWidget,
      );
      expect(find.text('TamuNet'), findsOneWidget);
      expect(find.text('::ffff:192.168.0.9'), findsOneWidget);
      expect(find.text('The radio is used'), findsOneWidget);
      expect(find.textContaining('Not implemented'), findsNothing);
    });

    testWidgets('the password is masked until it is deliberately revealed', (
      tester,
    ) async {
      await pump(tester, present: true, fields: connected());

      // The device reads the password back, so the app may show it - masked.
      expect(find.text(kSecretMask), findsOneWidget);
      expect(find.text('hunter2'), findsNothing);

      await tester.tap(find.byTooltip('Reveal'));
      await tester.pumpAndSettle();
      expect(find.text('hunter2'), findsOneWidget);

      await tester.tap(find.byTooltip('Hide'));
      await tester.pumpAndSettle();
      expect(find.text('hunter2'), findsNothing);
      expect(find.text(kSecretMask), findsOneWidget);
    });

    testWidgets('a write-only device never shows the password it was given', (
      tester,
    ) async {
      final fields = connected()
        ..[wifiFieldPassword] = field(
          DataType.text.value,
          text('', 32),
          flags: persistent,
          size: 32,
        );
      final device = await pump(tester, present: true, fields: fields);
      expect(find.text('not read back by the device'), findsOneWidget);

      await tester.tap(find.byTooltip('Set the password'));
      await tester.pumpAndSettle();
      await tester.enterText(find.byType(TextField), 'secret123');
      await tester.tap(find.text('OK'));
      await tester.pumpAndSettle();

      // The device was given the password...
      final write = device.writes.single;
      expect(write.field, wifiFieldPassword);
      expect(String.fromCharCodes(write.bytes.take(9)), 'secret123');
      // The password is stored by the device's own trigger (NVS), not by a Save All.
      expect(device.saves, 0);
      // ...and the panel still says it is not read back, with the typed value nowhere on screen.
      expect(find.text('not read back by the device'), findsOneWidget);
      expect(find.text('secret123'), findsNothing);
    });
  });

  group('writing to the block', () {
    testWidgets('enabling WiFi writes the field and saves the device', (
      tester,
    ) async {
      final device = await pump(tester, present: true, fields: emptyBlock());

      await tester.tap(find.byType(Switch));
      await tester.pumpAndSettle();

      expect(device.writes.single.field, wifiFieldEnabled);
      expect(device.writes.single.bytes, [1]);
      expect(device.saves, 1);
      // The panel shows the device's answer after the write, not the switch's own state.
      expect(find.text('The radio is used'), findsOneWidget);
      expect(find.text('No network configured'), findsOneWidget);
    });

    testWidgets('setting the SSID sends it at the width the block declared', (
      tester,
    ) async {
      final device = await pump(tester, present: true, fields: emptyBlock());

      await tester.tap(find.byTooltip('Set the SSID'));
      await tester.pumpAndSettle();
      await tester.enterText(find.byType(TextField), 'TamuNet');
      await tester.tap(find.text('OK'));
      await tester.pumpAndSettle();

      final write = device.writes.single;
      expect(write.field, wifiFieldSsid);
      expect(write.bytes.length, 32, reason: 'the field is 32 bytes wide');
      expect(String.fromCharCodes(write.bytes.sublist(0, 7)), 'TamuNet');
      expect(write.bytes.skip(7).every((b) => b == 0), isTrue);
      expect(device.saves, 1);
      expect(find.text('TamuNet'), findsOneWidget);
    });

    testWidgets('a password write is not followed by a device save', (
      tester,
    ) async {
      final device = await pump(tester, present: true, fields: emptyBlock());

      await tester.tap(find.byTooltip('Set the password'));
      await tester.pumpAndSettle();
      await tester.enterText(find.byType(TextField), 'secret123');
      await tester.tap(find.text('OK'));
      await tester.pumpAndSettle();

      // The password lives in NVS on the device: its trigger already stored it, so a Save All
      // would be a step that does nothing for this field.
      expect(device.writes.single.field, wifiFieldPassword);
      expect(device.saves, 0);
      expect(find.text('Password stored on the device'), findsOneWidget);
    });

    testWidgets('a refused write says why it was refused', (tester) async {
      await pump(
        tester,
        present: true,
        fields: emptyBlock(),
        overUsb: false,
        refuseWrites: true,
      );

      await tester.tap(find.byType(Switch));
      await tester.pumpAndSettle();

      expect(
        find.text(
          'The device refused the write - WiFi settings can only be changed over USB',
        ),
        findsOneWidget,
      );
    });
  });

  group('the write rule is stated up front', () {
    testWidgets('a BLE link gets the USB hint', (tester) async {
      await pump(tester, present: true, fields: emptyBlock(), overUsb: false);
      expect(
        find.textContaining('Changing these settings needs a USB link'),
        findsOneWidget,
      );
    });

    testWidgets('a USB link gets no hint', (tester) async {
      await pump(tester, present: true, fields: emptyBlock(), overUsb: true);
      expect(
        find.textContaining('Changing these settings needs a USB link'),
        findsNothing,
      );
    });
  });

  // Responsive gate. The structural rule is orientation-free: the panel is ONE scrolling column
  // (banner, then the settings card, then the note), and every row is a label+value line that
  // ellipsizes or wraps. So a short (landscape) window scrolls to the last row instead of
  // clipping it, and a narrow (portrait) one wraps the banner text rather than overflowing it.
  // The sizes below are evidence for that rule, not the rule itself.
  group('the panel fits both orientations', () {
    for (final size in const [Size(360, 480), Size(640, 360), Size(800, 360)]) {
      testWidgets('no overflow and the last row is reachable at $size', (
        tester,
      ) async {
        tester.view.physicalSize = size;
        tester.view.devicePixelRatio = 1.0;
        addTearDown(tester.view.resetPhysicalSize);
        addTearDown(tester.view.resetDevicePixelRatio);

        await pump(tester, present: true, fields: emptyBlock());
        expect(
          tester.takeException(),
          isNull,
          reason: 'the panel overflowed at $size',
        );

        await tester.dragUntilVisible(
          find.text('IP address'),
          find.byType(ListView),
          const Offset(0, -80),
        );
        expect(find.text('IP address'), findsOneWidget);

        await tester.pumpWidget(const SizedBox()); // dispose the timer
      });
    }
  });
}
