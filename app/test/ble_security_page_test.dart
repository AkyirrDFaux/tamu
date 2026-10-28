import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/ble_security.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/ble_security_page.dart';
import 'package:tamuapp/ui/theme.dart';

/// The Bluetooth panel, served by a fake device so every state is reachable off the rig.
///
/// The block is USB-only for writes and reports the radio's state through its own fields, so the
/// cases that matter are: a device without the block (the panel draws NOTHING), a reading shown
/// as the device reported it, each of the three commands with the consequence stated first, and a
/// refused write over Bluetooth answered with the reason rather than a silent no-op.
class _FakeBleDevice extends RegisterClient {
  _FakeBleDevice({
    required super.deviceId,
    required this.present,
    Map<(int, int), ({ValueInfo meta, List<int> value})>? fields,
    this.refuseWrites = false,
  }) : fields = fields ?? {};

  /// Whether the enumeration reports the BLE Settings block.
  final bool present;

  /// Models the firmware's rule that every write to this block is refused unless it arrived over
  /// USB: the device answers `FLAG_FAIL`, which the client reports as a null reply.
  final bool refuseWrites;

  /// The block's live values, keyed by `(field, key)`; a write lands here so a re-read shows it.
  Map<(int, int), ({ValueInfo meta, List<int> value})> fields;

  /// Every write the page sent, in order.
  final writes = <({int field, int key, int type, List<int> bytes})>[];

  /// How many times the page asked the device to persist (Save All).
  int saves = 0;

  @override
  Future<List<({int type, int maxInstance})>?> enumerateBlockTypes() async => [
    (type: BlockType.button.value, maxInstance: 0),
    if (present) (type: bleSettingsBlockTypeValue, maxInstance: 0),
  ];

  @override
  Future<({ValueInfo meta, List<int> value})?> readBlockField(
    int blockType,
    int instance,
    int field,
    int key,
  ) async => fields[(field, key)];

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
    writes.add((field: field, key: key, type: meta.type, bytes: value));
    final current = fields[(field, key)];
    fields[(field, key)] = (
      meta: ValueInfo(
        type: meta.type,
        size: current?.meta.size ?? value.length,
        flags: current?.meta.flags ?? meta.flags,
      ),
      value: value,
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

  const persistent = ValueFlags.persistent;

  /// The block as a device in its default state reports it: Just Works, one bonded phone, a
  /// window open for 48 s, radio on, no display name (so the System name is advertised).
  Map<(int, int), ({ValueInfo meta, List<int> value})> defaultBlock({
    String passkey = '000000',
    int mode = bleModeJustWorks,
    int bondCount = 1,
    List<String> bonds = const ['C4:7F:51:AA:BB:CC'],
    int windowMs = 48000,
    bool enabled = true,
    String displayName = '',
    String advertisedName = 'Tamu',
  }) => {
    (bleFieldPasskey, 0): read(
      DataType.text.value,
      text(passkey, blePasskeyLength),
      flags: ValueFlags.trigger,
    ),
    (bleFieldMode, 0): read(DataType.enum_.value, [
      mode,
    ], flags: ValueFlags.readOnly),
    (bleFieldBondCount, 0): read(DataType.integer.value, [
      bondCount,
    ], flags: ValueFlags.readOnly),
    for (var key = 0; key < bleBondSlots; key++)
      (bleFieldBond, key): read(
        DataType.text.value,
        text(key < bonds.length ? bonds[key] : '', 20),
        flags: ValueFlags.readOnly,
        size: 20,
      ),
    (bleFieldWindow, 0): read(
      DataType.uint32.value,
      uint32ToBytes(windowMs),
      flags: ValueFlags.readOnly,
      size: 4,
    ),
    (bleFieldAction, 0): read(DataType.enum_.value, const [
      0,
    ], flags: ValueFlags.trigger),
    (bleFieldEnabled, 0): read(DataType.bool_.value, [
      enabled ? 1 : 0,
    ], flags: persistent | ValueFlags.trigger),
    (bleFieldDisplayName, 0): read(
      DataType.text.value,
      text(displayName, bleDisplayNameWidth),
      flags: persistent | ValueFlags.trigger,
      size: bleDisplayNameWidth,
    ),
    (bleFieldAdvertisedName, 0): read(
      DataType.text.value,
      text(advertisedName, bleDisplayNameWidth),
      flags: ValueFlags.readOnly,
      size: bleDisplayNameWidth,
    ),
  };

  Future<_FakeBleDevice> pump(
    WidgetTester tester, {
    bool present = true,
    Map<(int, int), ({ValueInfo meta, List<int> value})>? fields,
    bool overUsb = true,
    bool refuseWrites = false,
    Size size = const Size(900, 1600),
  }) async {
    // A tall portrait window so every card is reachable without scrolling: the assertions below
    // are about what the panel says, not about hit-testing a scroll gesture.
    tester.view.physicalSize = size;
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final device = _FakeBleDevice(
      deviceId: 1,
      present: present,
      fields: fields ?? defaultBlock(),
      refuseWrites: refuseWrites,
    );
    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        home: BleSecurityPage(
          deviceId: 1,
          isConnected: () => true,
          isUsbLink: () => overUsb,
          clientFactory: (_) => device,
        ),
      ),
    );
    await tester.pumpAndSettle();
    // The page ticks the window countdown while one is open, so unmount it however the test ends.
    addTearDown(() async {
      await tester.pumpWidget(const SizedBox());
    });
    expect(tester.takeException(), isNull, reason: 'the panel threw');
    return device;
  }

  group('a device without the block', () {
    testWidgets('the panel draws nothing at all', (tester) async {
      await pump(tester, present: false);

      // No panel, no "unavailable", no empty card: the block is core-only and this device does
      // not carry it.
      expect(find.text('BLE Settings'), findsNothing);
      expect(find.textContaining('unavailable'), findsNothing);
      expect(find.byType(Card), findsNothing);
      // The page shell is still a page; nothing inside it.
      expect(find.text('Bluetooth'), findsOneWidget);
    });
  });

  group('a reading of the block', () {
    testWidgets('shows the mode, the bonded phone and the window countdown', (
      tester,
    ) async {
      await pump(tester);

      expect(find.text('Just Works - no code required'), findsOneWidget);
      expect(find.text('Not set - pairing uses Just Works'), findsOneWidget);
      expect(find.text('C4:7F:51:AA:BB:CC'), findsOneWidget);
      expect(find.text('0:48 left'), findsOneWidget);
      expect(find.text('Discoverable now'), findsOneWidget);
      // The advertised name is the device's resolved answer, not the empty display name.
      expect(find.text('Tamu'), findsOneWidget);
    });

    testWidgets('a passkey is masked until the user asks to see it', (
      tester,
    ) async {
      await pump(
        tester,
        fields: defaultBlock(passkey: '123456', mode: bleModePasskey),
      );

      expect(find.text('Passkey - a code is required'), findsOneWidget);
      // The code reads back (it is the user's own), but it is not printed at rest.
      expect(find.text('123456'), findsNothing);
      expect(find.text('••••••••'), findsOneWidget);

      await tester.tap(find.byTooltip('Show the code'));
      await tester.pumpAndSettle();
      expect(find.text('123456'), findsOneWidget);
    });

    testWidgets('a closed window says so instead of a countdown', (
      tester,
    ) async {
      await pump(tester, fields: defaultBlock(windowMs: 0));

      expect(find.text('closed'), findsOneWidget);
      expect(find.text('Not discoverable'), findsOneWidget);
      expect(find.textContaining('advertises nothing'), findsOneWidget);
    });
  });

  group('setting the passkey', () {
    testWidgets('states the consequences before the write and sends six digits', (
      tester,
    ) async {
      final device = await pump(tester);

      await tester.tap(find.byTooltip('Set the passkey'));
      await tester.pumpAndSettle();

      // The plain-words confirmation the card asks for.
      expect(
        find.textContaining('will ask for this code the next time it pairs'),
        findsOneWidget,
      );
      expect(
        find.textContaining('returns the device to Just Works'),
        findsOneWidget,
      );
      expect(
        find.textContaining('must set a new one over USB'),
        findsOneWidget,
      );

      // The button stays dead until the field holds exactly six digits.
      FilledButton confirm() => tester.widget<FilledButton>(
        find.widgetWithText(FilledButton, 'Set passkey'),
      );
      expect(confirm().onPressed, isNull);
      await tester.enterText(find.byType(TextField), '12345');
      await tester.pump();
      expect(confirm().onPressed, isNull);
      await tester.enterText(find.byType(TextField), '123456');
      await tester.pump();
      expect(confirm().onPressed, isNotNull);

      await tester.tap(find.widgetWithText(FilledButton, 'Set passkey'));
      await tester.pumpAndSettle();

      expect(device.writes, hasLength(1));
      final write = device.writes.single;
      expect(write.field, bleFieldPasskey);
      expect(write.key, 0);
      expect(write.type, DataType.text.value);
      expect(write.bytes, '123456'.codeUnits);
      // The passkey lives in NVS, not the persistent space: no Save All after it.
      expect(device.saves, 0);
      expect(
        find.text(
          'Passkey set - the phone will ask for this code when it next pairs',
        ),
        findsOneWidget,
      );
    });

    testWidgets(
      'clearing it returns the device to Just Works after a confirm',
      (tester) async {
        final device = await pump(
          tester,
          fields: defaultBlock(passkey: '123456', mode: bleModePasskey),
        );

        await tester.tap(find.byTooltip('Clear the passkey'));
        await tester.pumpAndSettle();
        expect(find.textContaining('returns to Just Works'), findsOneWidget);
        expect(find.textContaining('anyone in range can pair'), findsOneWidget);

        await tester.tap(find.widgetWithText(FilledButton, 'Clear passkey'));
        await tester.pumpAndSettle();

        expect(device.writes.single.field, bleFieldPasskey);
        expect(device.writes.single.bytes, blePasskeyNone.codeUnits);
        expect(
          find.text('Passkey cleared - the device pairs with Just Works again'),
          findsOneWidget,
        );
      },
    );
  });

  group('the two commands', () {
    testWidgets('forgetting bonds states that every phone must pair again', (
      tester,
    ) async {
      final device = await pump(tester);

      await tester.tap(find.text('Forget all bonds'));
      await tester.pumpAndSettle();
      // The consequence, before the tap.
      expect(find.textContaining('must pair again'), findsOneWidget);

      await tester.tap(find.widgetWithText(FilledButton, 'Forget bonds'));
      await tester.pumpAndSettle();

      final write = device.writes.single;
      expect(write.field, bleFieldAction);
      expect(write.type, DataType.enum_.value);
      expect(write.bytes, [bleActionForgetBonds]);
      // A command, not a persisted setting.
      expect(device.saves, 0);
      expect(
        find.text('Bonds forgotten - every phone must pair again'),
        findsOneWidget,
      );
    });

    testWidgets('opening the pairing window is one tap, no confirm', (
      tester,
    ) async {
      final device = await pump(tester, fields: defaultBlock(windowMs: 0));

      await tester.tap(find.text('Open pairing window'));
      await tester.pumpAndSettle();

      final write = device.writes.single;
      expect(write.field, bleFieldAction);
      expect(write.key, 0);
      expect(write.bytes, [bleActionOpenWindow]);
      expect(find.text('Pairing window open'), findsOneWidget);
    });
  });

  group('the radio and the name', () {
    testWidgets('the enable switch writes the Bool and saves it', (
      tester,
    ) async {
      final device = await pump(tester);

      await tester.tap(find.text('Enabled'));
      await tester.pumpAndSettle();

      final write = device.writes.single;
      expect(write.field, bleFieldEnabled);
      expect(write.type, DataType.bool_.value);
      expect(write.bytes, [0]);
      // A persistent field: mirrored into the device's `.SV` only by a Save All.
      expect(device.saves, 1);
      expect(find.text('Radio switched off'), findsOneWidget);
    });

    testWidgets('the display name is written at the field\'s own width', (
      tester,
    ) async {
      final device = await pump(tester);

      await tester.tap(find.byTooltip('Set the display name'));
      await tester.pumpAndSettle();
      await tester.enterText(find.byType(TextField), 'Bench');
      await tester.tap(find.widgetWithText(FilledButton, 'Set name'));
      await tester.pumpAndSettle();

      final write = device.writes.single;
      expect(write.field, bleFieldDisplayName);
      expect(write.bytes, text('Bench', bleDisplayNameWidth));
      expect(write.bytes.length, bleDisplayNameWidth);
      expect(device.saves, 1);
    });
  });

  group('over a Bluetooth link', () {
    testWidgets('the panel says the block is read-only before the form', (
      tester,
    ) async {
      await pump(tester, overUsb: false);

      expect(
        find.textContaining('can only be changed over a USB link'),
        findsOneWidget,
      );
      // The reading is still shown: the block is readable over any link.
      expect(find.text('C4:7F:51:AA:BB:CC'), findsOneWidget);
    });

    testWidgets('a refused write is explained, not dropped', (tester) async {
      final device = await pump(tester, overUsb: false, refuseWrites: true);

      await tester.tap(find.byTooltip('Set the passkey'));
      await tester.pumpAndSettle();
      await tester.enterText(find.byType(TextField), '123456');
      await tester.pump();
      await tester.tap(find.widgetWithText(FilledButton, 'Set passkey'));
      await tester.pumpAndSettle();

      expect(device.writes, isEmpty);
      expect(
        find.textContaining('this block can only be written over USB'),
        findsOneWidget,
      );
    });

    testWidgets('a refused command is explained too', (tester) async {
      await pump(tester, overUsb: false, refuseWrites: true);

      await tester.tap(find.text('Open pairing window'));
      await tester.pumpAndSettle();

      expect(
        find.textContaining('this block can only be written over USB'),
        findsOneWidget,
      );
    });
  });

  group('orientation', () {
    // The panel states the structural rule once: it is a ListView, so a short window (landscape
    // on a phone) scrolls instead of clipping, and the banner stays at the top of the scroll
    // content in both orientations. The measurements here are evidence, not the rule.
    testWidgets(
      'a short landscape window scrolls to every card without overflowing',
      (tester) async {
        await pump(tester, size: const Size(1280, 400));

        expect(tester.takeException(), isNull);
        expect(find.text('Discoverable now'), findsOneWidget);
        // The lowest card is reachable by scrolling, not cut off.
        expect(find.text('Advertising as'), findsNothing);
        await tester.scrollUntilVisible(find.text('Advertising as'), 200);
        expect(find.text('Advertising as'), findsOneWidget);
        expect(tester.takeException(), isNull);
      },
    );

    testWidgets('a narrow portrait window keeps every row on screen', (
      tester,
    ) async {
      await pump(tester, size: const Size(360, 740));

      expect(tester.takeException(), isNull);
      expect(find.text('Discoverable now'), findsOneWidget);
      await tester.scrollUntilVisible(find.text('Advertising as'), 200);
      expect(tester.takeException(), isNull);
    });
  });

  group('the stale-bond guidance', () {
    // The dialog the connection page shows after a failed Bluetooth attempt: the case the card
    // says must not look like a broken device, so the wording and the pointer at the phone's own
    // Bluetooth settings are asserted here rather than only in the core.
    testWidgets('says the device is fine and points at the phone settings', (
      tester,
    ) async {
      const linkError = 'GATT_INSUFFICIENT_AUTHENTICATION (status 5)';
      await tester.pumpWidget(
        MaterialApp(
          theme: buildTheme(),
          home: Scaffold(
            body: Builder(
              builder: (context) => TextButton(
                onPressed: () => showBleLinkGuidance(
                  context,
                  bleLinkGuidance(errorText: linkError),
                  linkError: linkError,
                ),
                child: const Text('open'),
              ),
            ),
          ),
        ),
      );
      await tester.tap(find.text('open'));
      await tester.pumpAndSettle();

      expect(
        find.text('This phone is no longer paired with the device'),
        findsOneWidget,
      );
      expect(find.textContaining('the device is working'), findsOneWidget);
      expect(find.textContaining('Bluetooth settings'), findsOneWidget);
      // The platform's own words are kept for a bug report, at the bottom.
      expect(find.text(linkError), findsOneWidget);

      await tester.tap(find.text('Got it'));
      await tester.pumpAndSettle();
      expect(find.text('Got it'), findsNothing);
    });
  });
}
