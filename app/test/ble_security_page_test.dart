import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/ble_security.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/core/unsaved_settings.dart';
import 'package:tamuapp/ui/ble_security_page.dart';
import 'package:tamuapp/ui/notice_host.dart';
import 'package:tamuapp/ui/theme.dart';
import 'support/block_reads.dart';

/// The Bluetooth panel, served by a fake device so every state is reachable off the rig.
///
/// The block is six fields and writable over USB or a secured Bluetooth link, so the cases that
/// matter are: a device without the block (the panel draws NOTHING), a reading shown as the device
/// reported it, the bond list decoded from its one array (rename, delete, and the empty slot that
/// says "pair it instead"), the window as a command, the display name as a live setting, a write
/// over a secured Bluetooth link that lands, and a link the device will not write from answered
/// with the reason rather than a silent no-op.
class _FakeBleDevice extends RegisterClient {
  _FakeBleDevice({
    required super.deviceId,
    required this.present,
    Map<(int, int), ({ValueInfo meta, List<int> value})>? fields,
    this.refuseWrites = false,
  }) : fields = fields ?? {};

  /// Whether the enumeration reports the BLE Settings block.
  final bool present;

  /// Models a device that refuses the write (the firmware answers `FLAG_FAIL`, which the client
  /// reports as a null reply). Refusal is what a link that cannot write the block - one that is not
  /// a secured session - gets; a secured Bluetooth link writes it like USB does.
  final bool refuseWrites;

  /// The block's live values, keyed by `(field, key)`; a write lands here so a re-read shows it.
  Map<(int, int), ({ValueInfo meta, List<int> value})> fields;

  /// Every write the page sent, in order.
  final writes = <({int field, int key, int type, List<int> bytes})>[];

  /// How many times the page asked the device to persist (Save All).
  int saves = 0;

  /// What Save All answers. A device that refuses the command, or never answers it, reports
  /// `false` - and that is not a save (`core/unsaved_settings.dart`).
  bool saveAllAnswers = true;

  /// Fields whose answer to a READ is dropped, the way the bus drops a frame. The read-back after
  /// Save All is the app's only check, so a silent one must not pass for a confirmation.
  final Set<int> silentReads = {};

  /// Fields a write is accepted for but does not change - a write that did not land.
  final Set<int> ignoredWrites = {};

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
  ) async => silentReads.contains(field) ? null : fields[(field, key)];

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
    if (ignoredWrites.contains(field)) return const [1];
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
  Future<bool> saveAll({Duration timeout = const Duration(seconds: 25)}) async {
    saves++;
    return saveAllAnswers;
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  // One field read and a fixed-width text value: the two fixtures every panel test needs
  // (`support/block_reads.dart`), under the names this suite reads them by.
  final read = fieldBytes;
  final text = paddedText;

  List<int> addr(String printed) => [
    for (final part in printed.split(':')) int.parse(part, radix: 16),
  ];

  /// A `Bonds` value in slot order, with the reply's four-byte padding on top (the rig answers
  /// the declared 66 bytes as 68), so the page is exercised against the wire shape it will see.
  List<int> bondsValue(List<(String, String)> entries) => [
    ...encodeBleBonds([
      for (var slot = 0; slot < bleBondSlots; slot++)
        slot < entries.length
            ? BleBond(address: addr(entries[slot].$1), name: entries[slot].$2)
            : bleEmptyBond,
    ]),
    0,
    0,
  ];

  const persistent = ValueFlags.persistent;

  /// The block as a device in its default state reports it: Just Works, one bonded phone with a
  /// label, a window open for 48 s, radio on, no display name.
  Map<(int, int), ({ValueInfo meta, List<int> value})> defaultBlock({
    String passkey = '000000',
    int mode = bleModeJustWorks,
    List<(String, String)> bonds = const [('C4:7F:51:AA:BB:CC', 'Work phone')],
    int windowMs = 48000,
    bool enabled = true,
    String displayName = '',
  }) => {
    (bleFieldEnabled, 0): read(
      DataType.bool_.value,
      [enabled ? 1 : 0],
      flags: persistent | ValueFlags.trigger,
      size: 1,
    ),
    (bleFieldPasskey, 0): read(
      DataType.text.value,
      text(passkey, blePasskeyLength),
      flags: ValueFlags.trigger,
      size: blePasskeyLength,
    ),
    (bleFieldMode, 0): read(
      DataType.enum_.value,
      [mode],
      flags: ValueFlags.readOnly,
      size: 1,
    ),
    (bleFieldDisplayName, 0): read(
      DataType.text.value,
      text(displayName, bleDisplayNameWidth),
      flags: persistent | ValueFlags.trigger,
      size: bleDisplayNameWidth,
    ),
    (bleFieldBonds, 0): read(
      DataType.undefined.value,
      bondsValue(bonds),
      flags: ValueFlags.trigger,
      size: bleBondsLength,
    ),
    (bleFieldWindow, 0): read(
      DataType.uint32.value,
      uint32ToBytes(windowMs),
      flags: ValueFlags.trigger,
      size: 4,
    ),
  };

  Future<_FakeBleDevice> pump(
    WidgetTester tester, {
    bool present = true,
    Map<(int, int), ({ValueInfo meta, List<int> value})>? fields,
    bool overUsb = true,
    bool secured = true,
    bool refuseWrites = false,
    UnsavedSettings? store,
    Listenable? linkChanges,
    bool Function()? isConnected,
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
        // The app's chrome: a page's notice is drawn by the window's strip.
        builder: noticeHostBuilder,
        home: BleSecurityPage(
          deviceId: 1,
          isConnected: isConnected ?? () => true,
          linkChanges: linkChanges,
          // A store of its own per mount: the app's is shared, so one case's write would show
          // up as another case's unsaved change.
          unsaved: store ?? UnsavedSettings.forTest(),
          isUsbLink: () => overUsb,
          isSecuredLink: () => secured,
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

  /// Sets one bond's label through the panel's rename dialog: open it on the bond, type [label] and
  /// confirm. Three cases drive that dialog and then assert a different sentence, so the drive lives
  /// here instead of three times over.
  Future<void> setBondLabel(WidgetTester tester, String label) async {
    await tester.tap(find.byTooltip('Rename this bond'));
    await tester.pumpAndSettle();
    await tester.enterText(find.byType(TextField), label);
    await tester.tap(find.widgetWithText(FilledButton, 'Set label'));
    await tester.pumpAndSettle();
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
    testWidgets('shows the six fields as three cards', (tester) async {
      await pump(tester);

      expect(find.text('Pairing'), findsOneWidget);
      expect(find.text('Bonds'), findsOneWidget);
      expect(find.text('Radio'), findsOneWidget);
      // The retired rows are gone: no Action, no bond count, no keyed bond rows, no last link.
      expect(find.text('Action'), findsNothing);
      expect(find.text('Bond count'), findsNothing);
      expect(find.text('Bond address'), findsNothing);
      expect(find.text('Advertised name'), findsNothing);
      expect(find.text('Advertising as'), findsNothing);
      expect(find.text('Last judged link'), findsNothing);
      expect(find.text('Last peer'), findsNothing);
      expect(find.text('Last air'), findsNothing);
      expect(find.text('Last link'), findsNothing);
    });

    testWidgets('decodes the bond array into address and label rows', (
      tester,
    ) async {
      await pump(tester);

      expect(find.text('C4:7F:51:AA:BB:CC'), findsOneWidget);
      expect(find.text('Work phone'), findsOneWidget);
      expect(find.text('Just Works - no code required'), findsOneWidget);
      expect(find.text('Not set - pairing uses Just Works'), findsOneWidget);
      expect(find.text('Discoverable now'), findsOneWidget);
      // 48 s left, and the row says what an open window means.
      expect(
        find.text('0:48 left - any phone may pair until it closes'),
        findsOneWidget,
      );
    });

    testWidgets('an empty slot says a peer gets there by pairing', (
      tester,
    ) async {
      await pump(tester);

      // Three slots, one held: the other two are drawn, and they say how a peer arrives rather
      // than offering an "add" control the device refuses.
      expect(find.text('Empty slot'), findsNWidgets(2));
      expect(
        find.text('A phone appears here once it has paired.'),
        findsNWidgets(2),
      );
      expect(find.textContaining(bleBondCannotAdd), findsOneWidget);
      expect(find.textContaining('1 of 3 slots hold a bond'), findsOneWidget);
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

    testWidgets('a closed window with bonds promises the reconnect', (
      tester,
    ) async {
      await pump(tester, fields: defaultBlock(windowMs: 0));

      expect(
        find.text(
          'closed - no new pairing until a window is open. A bonded phone connects anyway.',
        ),
        findsOneWidget,
      );
      expect(find.text('Bonded phones only'), findsOneWidget);
      expect(
        find.text(
          'Bonded devices can reconnect at any time. New pairings need a window, which the '
          'panel opens over USB or from a secured Bluetooth link.',
        ),
        findsOneWidget,
      );
    });

    testWidgets('an empty store and a closed window invite a pairing', (
      tester,
    ) async {
      await pump(tester, fields: defaultBlock(bonds: const [], windowMs: 0));

      // E64 ruling C: the device is on the air and admits any phone, so the banner is the
      // discoverable one instead of the old disabled-Bluetooth warning.
      expect(find.text('No bonds: any phone may pair'), findsOneWidget);
      expect(find.textContaining('pair from your phone'), findsOneWidget);
      expect(find.byIcon(Icons.bluetooth_disabled), findsNothing);
      final icon = tester.widget<Icon>(
        find.byIcon(Icons.bluetooth_searching).first,
      );
      expect(icon.color, kSuccess);
      expect(find.text('Empty slot'), findsNWidgets(3));
    });

    testWidgets('prints - for a field the device did not answer', (
      tester,
    ) async {
      // The device skipped the passkey, the mode and the window. Their defaults would read as
      // "no passkey, Just Works, closed" - three claims about a device that never said so.
      final fields = defaultBlock()
        ..remove((bleFieldPasskey, 0))
        ..remove((bleFieldMode, 0))
        ..remove((bleFieldWindow, 0));
      await pump(tester, fields: fields);

      expect(
        find.text('The device did not answer every field'),
        findsOneWidget,
      );
      expect(find.text('Not set - pairing uses Just Works'), findsNothing);
      expect(find.text('Just Works - no code required'), findsNothing);
      expect(find.text('0:48 left'), findsNothing);
      // Passkey, mode and the pairing window each print the dash; the bond list was answered.
      expect(find.text('-'), findsNWidgets(3));
      expect(find.text('C4:7F:51:AA:BB:CC'), findsOneWidget);
    });

    testWidgets('an unanswered bond field makes no claim about the store', (
      tester,
    ) async {
      final fields = defaultBlock()..remove((bleFieldBonds, 0));
      await pump(tester, fields: fields);

      // The banner says it cannot tell what the radio is doing, and the card prints one dash
      // instead of three "Empty slot" rows - which would be a claim the device never made.
      expect(
        find.text('The device did not answer every field'),
        findsOneWidget,
      );
      expect(find.text('Empty slot'), findsNothing);
      expect(find.text('Bond list'), findsOneWidget);
    });

    testWidgets(
      'the display name reads as live, not "next advertising start"',
      (tester) async {
        await pump(tester, fields: defaultBlock(displayName: 'Bench'));

        expect(find.text('Bench - on the air now'), findsOneWidget);
        expect(find.textContaining('next advertising start'), findsNothing);
      },
    );
  });

  group('the bond list', () {
    testWidgets('renaming one entry writes the whole array, nothing else', (
      tester,
    ) async {
      final device = await pump(tester);

      await tester.tap(find.byTooltip('Rename this bond'));
      await tester.pumpAndSettle();
      expect(
        find.textContaining('kept on the device beside its address'),
        findsOneWidget,
      );
      await tester.enterText(find.byType(TextField), 'Bench');
      await tester.tap(find.widgetWithText(FilledButton, 'Set label'));
      await tester.pumpAndSettle();

      final write = device.writes.single;
      expect(write.field, bleFieldBonds);
      expect(write.key, 0);
      expect(write.type, DataType.undefined.value);
      // The device requires the field's declared size: one write describes every slot.
      expect(write.bytes, hasLength(bleBondsLength));
      expect(write.bytes.sublist(0, 6), addr('C4:7F:51:AA:BB:CC'));
      expect(String.fromCharCodes(write.bytes.sublist(6, 12)), 'Bench\x00');
      // An all-zero address after the first entry: the empty slots travel as they were read.
      expect(
        write.bytes.sublist(bleBondEntryLength).every((b) => b == 0),
        isTrue,
      );
      // A trigger, not a persistent field: no Save All after it.
      expect(device.saves, 0);
      expect(
        find.text('Label set to "Bench" for C4:7F:51:AA:BB:CC'),
        findsOneWidget,
      );
    });

    testWidgets(
      'deleting a bond zeroes its address and says who has to pair again',
      (tester) async {
        final device = await pump(
          tester,
          fields: defaultBlock(
            bonds: const [
              ('C4:7F:51:AA:BB:CC', 'Work phone'),
              ('E4:B0:63:C8:20:72', 'Pixel'),
            ],
          ),
        );

        await tester.tap(find.byTooltip('Delete this bond').first);
        await tester.pumpAndSettle();
        // The consequence, before the tap.
        expect(
          find.textContaining(
            'has to be forgotten in its own Bluetooth settings',
          ),
          findsOneWidget,
        );

        await tester.tap(find.widgetWithText(FilledButton, 'Delete bond'));
        await tester.pumpAndSettle();

        final write = device.writes.single;
        expect(write.field, bleFieldBonds);
        expect(write.bytes, hasLength(bleBondsLength));
        // Slot 0 is gone; slot 1 keeps its address, so the rest of the store is untouched.
        expect(
          write.bytes.sublist(0, bleBondEntryLength).every((b) => b == 0),
          isTrue,
        );
        expect(
          write.bytes.sublist(bleBondEntryLength, bleBondEntryLength + 6),
          addr('E4:B0:63:C8:20:72'),
        );
        expect(
          find.text('Bond deleted - that phone has to pair again'),
          findsOneWidget,
        );
      },
    );

    testWidgets('deleting the last bond states that the store is emptied', (
      tester,
    ) async {
      final device = await pump(tester);

      await tester.tap(find.byTooltip('Delete this bond'));
      await tester.pumpAndSettle();
      // The last bond: the device empties its store and owes its automatic window.
      expect(
        find.textContaining(
          'the device opens a five-minute pairing window by itself',
        ),
        findsOneWidget,
      );

      await tester.tap(find.widgetWithText(FilledButton, 'Delete bond'));
      await tester.pumpAndSettle();

      // Every address zero is the forget-all frame.
      expect(device.writes.single.bytes.every((b) => b == 0), isTrue);
      expect(
        find.text(
          'Bond deleted - the store is empty, so the device opens a five-minute pairing window',
        ),
        findsOneWidget,
      );
    });
  });

  group('the pairing window', () {
    testWidgets('a written length opens it, as a Uint32 in milliseconds', (
      tester,
    ) async {
      final device = await pump(tester, fields: defaultBlock(windowMs: 0));

      await tester.tap(find.byTooltip('Set the pairing window'));
      await tester.pumpAndSettle();
      // The rule the device enforces, in the dialog.
      expect(
        find.textContaining('opens the window for the number of milliseconds'),
        findsOneWidget,
      );
      await tester.enterText(find.byType(TextField), '60000');
      await tester.pump();
      await tester.tap(find.widgetWithText(FilledButton, 'Set window'));
      await tester.pumpAndSettle();

      final write = device.writes.single;
      expect(write.field, bleFieldWindow);
      expect(write.key, 0);
      expect(write.type, DataType.uint32.value);
      expect(write.bytes, uint32ToBytes(60000));
      expect(device.saves, 0);
      expect(find.text('Pairing window open for 1:00'), findsOneWidget);
    });

    testWidgets('0 closes an open window from the row', (tester) async {
      final device = await pump(tester);

      await tester.tap(find.byTooltip('Close the pairing window'));
      await tester.pumpAndSettle();

      expect(device.writes.single.field, bleFieldWindow);
      expect(device.writes.single.bytes, uint32ToBytes(0));
      expect(find.text('Pairing window closed'), findsOneWidget);
    });

    testWidgets('the dialog refuses what the device would refuse', (
      tester,
    ) async {
      await pump(tester, fields: defaultBlock(enabled: false, windowMs: 0));

      await tester.tap(find.byTooltip('Set the pairing window'));
      await tester.pumpAndSettle();
      await tester.enterText(find.byType(TextField), '60000');
      await tester.pump();

      // The radio is off, so a non-zero window cannot open - said before the write, not after a
      // refusal.
      expect(
        find.text('the radio is switched off, so no window can open'),
        findsOneWidget,
      );
      final confirm = tester.widget<FilledButton>(
        find.widgetWithText(FilledButton, 'Set window'),
      );
      expect(confirm.onPressed, isNull);
    });
  });

  group('the radio and the name', () {
    testWidgets('a persistent write is unsaved, and the strip names it', (
      tester,
    ) async {
      final store = UnsavedSettings.forTest();
      final device = await pump(tester, store: store);

      await tester.tap(find.text('Enabled'));
      await tester.pumpAndSettle();

      final write = device.writes.single;
      expect(write.field, bleFieldEnabled);
      expect(write.type, DataType.bool_.value);
      expect(write.bytes, [0]);
      // E71: the write is in the device's RAM. Only Save All writes `.SV`, so the panel does not
      // claim the change is set - it says so and holds the state.
      expect(device.saves, 0);
      expect(store.isDirty(1), isTrue);
      expect(store.describe(1), 'Enabled');
      expect(find.text('Not saved: Enabled.'), findsOneWidget);
      expect(
        find.textContaining('the device holds it in RAM only'),
        findsOneWidget,
      );
    });

    testWidgets('the display name is written at the field\'s own width', (
      tester,
    ) async {
      final store = UnsavedSettings.forTest();
      final device = await pump(tester, store: store);

      await tester.tap(find.byTooltip('Set the display name'));
      await tester.pumpAndSettle();
      await tester.enterText(find.byType(TextField), 'Bench');
      await tester.tap(find.widgetWithText(FilledButton, 'Set name'));
      await tester.pumpAndSettle();

      final write = device.writes.single;
      expect(write.field, bleFieldDisplayName);
      expect(write.bytes, text('Bench', bleDisplayNameWidth));
      expect(write.bytes.length, bleDisplayNameWidth);
      // The name is one of the two `Persistent` fields, so the write is not durable yet.
      expect(device.saves, 0);
      expect(store.describe(1), 'Display name');
      expect(find.text('Not saved: Display name.'), findsOneWidget);
    });
  });

  group('saving what the device holds', () {
    /// Writes `Enabled = false` and leaves the panel holding it: the shape every case below
    /// starts from.
    Future<({_FakeBleDevice device, UnsavedSettings store})> switchOff(
      WidgetTester tester, {
      void Function(_FakeBleDevice device)? arrange,
    }) async {
      final store = UnsavedSettings.forTest();
      final device = await pump(tester, store: store);
      arrange?.call(device);
      await tester.tap(find.text('Enabled'));
      await tester.pumpAndSettle();
      return (device: device, store: store);
    }

    testWidgets('a clean Save all re-applies the write and clears the state', (
      tester,
    ) async {
      final run = await switchOff(tester);

      await tester.tap(find.widgetWithText(FilledButton, 'Save all'));
      await tester.pumpAndSettle();

      // The write goes out again (the device may have reset), then Save All, then the read-back
      // that decides the flag.
      expect(run.device.writes, hasLength(2));
      expect(run.device.saves, 1);
      expect(run.store.isDirty(1), isFalse);
      expect(find.text('Not saved: Enabled.'), findsNothing);
      expect(find.text('Saved: Enabled.'), findsOneWidget);
    });

    testWidgets('a refused Save all keeps the change and names the field', (
      tester,
    ) async {
      final run = await switchOff(
        tester,
        arrange: (device) => device.saveAllAnswers = false,
      );

      await tester.tap(find.widgetWithText(FilledButton, 'Save all'));
      await tester.pumpAndSettle();

      expect(run.store.isDirty(1), isTrue);
      expect(
        find.textContaining('Not saved: Enabled - the device did not answer'),
        findsOneWidget,
      );
      expect(find.text('Not saved: Enabled.'), findsOneWidget);
    });

    testWidgets('a read-back that does not answer is not a confirmation', (
      tester,
    ) async {
      final run = await switchOff(
        tester,
        arrange: (device) => device.silentReads.add(bleFieldEnabled),
      );

      await tester.tap(find.widgetWithText(FilledButton, 'Save all'));
      await tester.pumpAndSettle();

      expect(run.store.isDirty(1), isTrue);
      expect(
        find.textContaining('Not confirmed: Enabled - Save all answered'),
        findsOneWidget,
      );
    });

    testWidgets('a value the device still reads as the old one stays unsaved', (
      tester,
    ) async {
      // The write is accepted but does not land: the read-back is what notices.
      final run = await switchOff(
        tester,
        arrange: (device) => device.ignoredWrites.add(bleFieldEnabled),
      );

      await tester.tap(find.widgetWithText(FilledButton, 'Save all'));
      await tester.pumpAndSettle();

      expect(run.store.isDirty(1), isTrue);
      expect(
        find.textContaining('the device reads back a different value'),
        findsOneWidget,
      );
    });

    testWidgets('two fields behind one Save all are both named', (
      tester,
    ) async {
      final store = UnsavedSettings.forTest();
      final device = await pump(tester, store: store);

      await tester.tap(find.text('Enabled'));
      await tester.pumpAndSettle();
      await tester.tap(find.byTooltip('Set the display name'));
      await tester.pumpAndSettle();
      await tester.enterText(find.byType(TextField), 'Bench');
      await tester.tap(find.widgetWithText(FilledButton, 'Set name'));
      await tester.pumpAndSettle();

      expect(store.describe(1), 'Enabled and Display name');
      device.saveAllAnswers = false;
      await tester.tap(find.widgetWithText(FilledButton, 'Save all'));
      await tester.pumpAndSettle();

      expect(
        find.textContaining(
          'Not saved: Enabled and Display name - the device did not answer',
        ),
        findsOneWidget,
      );
    });

    testWidgets('the app saves what it holds when it goes to the background', (
      tester,
    ) async {
      final run = await switchOff(tester);

      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.paused);
      await tester.pumpAndSettle();

      // Nobody can answer a dialog in the background, so the panel saves instead of asking; the
      // state clears only because the save and its read-back landed.
      expect(run.device.saves, 1);
      expect(run.store.isDirty(1), isFalse);
      tester.binding.handleAppLifecycleStateChanged(AppLifecycleState.resumed);
      await tester.pumpAndSettle();
    });
  });

  group('leaving and reconnecting with unsaved writes', () {
    /// Pushes the panel over a first page, so a pop is a real navigation and can be intercepted.
    Future<({_FakeBleDevice device, UnsavedSettings store})> pushPanel(
      WidgetTester tester,
    ) async {
      tester.view.physicalSize = const Size(900, 1600);
      tester.view.devicePixelRatio = 1.0;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final store = UnsavedSettings.forTest();
      final device = _FakeBleDevice(
        deviceId: 1,
        present: true,
        fields: defaultBlock(),
      );
      await tester.pumpWidget(
        MaterialApp(
          theme: buildTheme(),
          builder: noticeHostBuilder,
          home: Builder(
            builder: (context) => Scaffold(
              body: Center(
                child: FilledButton(
                  onPressed: () => Navigator.of(context).push(
                    MaterialPageRoute<void>(
                      builder: (_) => BleSecurityPage(
                        deviceId: 1,
                        isConnected: () => true,
                        unsaved: store,
                        clientFactory: (_) => device,
                      ),
                    ),
                  ),
                  child: const Text('Open the panel'),
                ),
              ),
            ),
          ),
        ),
      );
      await tester.tap(find.text('Open the panel'));
      await tester.pumpAndSettle();
      addTearDown(() async {
        await tester.pumpWidget(const SizedBox());
      });
      return (device: device, store: store);
    }

    testWidgets('leaving asks, and the state survives the visit', (
      tester,
    ) async {
      final run = await pushPanel(tester);
      await tester.tap(find.text('Enabled'));
      await tester.pumpAndSettle();

      // The pop is blocked while the change is unsaved.
      await tester.pageBack();
      await tester.pumpAndSettle();
      expect(find.text('Unsaved changes'), findsOneWidget);
      expect(
        find.textContaining('holds changes that are not saved: Enabled.'),
        findsOneWidget,
      );

      await tester.tap(find.text('Leave without saving'));
      await tester.pumpAndSettle();
      // The page is gone and the device still holds the change in RAM: the state is the device's,
      // not the page's.
      expect(find.text('Open the panel'), findsOneWidget);
      expect(run.store.isDirty(1), isTrue);
    });

    testWidgets('coming back to the panel still reports the change', (
      tester,
    ) async {
      final run = await pushPanel(tester);
      await tester.tap(find.text('Enabled'));
      await tester.pumpAndSettle();
      await tester.pageBack();
      await tester.pumpAndSettle();
      await tester.tap(find.text('Leave without saving'));
      await tester.pumpAndSettle();

      await tester.tap(find.text('Open the panel'));
      await tester.pumpAndSettle();

      expect(find.text('Not saved: Enabled.'), findsOneWidget);
      expect(run.store.isDirty(1), isTrue);
    });

    testWidgets('a link that drops says the save can no longer be sent', (
      tester,
    ) async {
      final link = ChangeNotifier();
      var up = true;
      final store = UnsavedSettings.forTest();
      await pump(
        tester,
        store: store,
        linkChanges: link,
        isConnected: () => up,
      );
      await tester.tap(find.text('Enabled'));
      await tester.pumpAndSettle();

      up = false;
      link.notifyListeners();
      await tester.pumpAndSettle();

      expect(find.text('Not saved - no link'), findsOneWidget);
      expect(
        find.textContaining('The link dropped with unsaved changes: Enabled.'),
        findsOneWidget,
      );
      expect(store.isDirty(1), isTrue);
    });

    testWidgets('a link that comes back offers the writes back', (
      tester,
    ) async {
      final link = ChangeNotifier();
      var up = true;
      final store = UnsavedSettings.forTest();
      final device = await pump(
        tester,
        store: store,
        linkChanges: link,
        isConnected: () => up,
      );
      await tester.tap(find.text('Enabled'));
      await tester.pumpAndSettle();
      up = false;
      link.notifyListeners();
      await tester.pumpAndSettle();

      up = true;
      link.notifyListeners();
      await tester.pumpAndSettle();

      expect(find.text('The link is back'), findsOneWidget);
      expect(find.textContaining('never saved: Enabled'), findsOneWidget);
      await tester.tap(find.widgetWithText(FilledButton, 'Re-apply and save'));
      await tester.pumpAndSettle();

      // Re-applied (the second write is the re-apply), saved, read back, cleared.
      expect(device.writes, hasLength(2));
      expect(device.saves, 1);
      expect(store.isDirty(1), isFalse);
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
        // The way back is not a cable: after Just Works the phone pairs without a code and holds
        // a secured link that may set one again (`Docs/Services/App Interface.md:31`). The retired
        // "Set a new code here over USB" claim would tell the user otherwise.
        expect(
          find.textContaining(
            'over USB, or from the secured Bluetooth link the phone holds once it pairs '
            'again without a code',
          ),
          findsOneWidget,
        );
        expect(find.textContaining('Set a new code here over USB'), findsNothing);

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

  group('over a Bluetooth link', () {
    // The firmware accepts a write from the SECURED session the radio admitted
    // (`Blocks/BleWritePolicy.h` `BleWriteAllowed`), so the panel states that rule: a secured link
    // changes these settings like the cable does, and only a link that is not a secured session is
    // refused - with the way out. The retired copy ('Bluetooth settings can only be changed over a
    // USB link') is gone, not reworded.
    testWidgets('a secured link may change the block, and says so', (
      tester,
    ) async {
      final device = await pump(tester, overUsb: false);

      expect(
        find.textContaining('The link is a secured session'),
        findsOneWidget,
      );
      expect(
        find.textContaining('only be changed over a USB link'),
        findsNothing,
      );

      // The claim in the copy, exercised: the write lands over this link.
      await setBondLabel(tester, 'Bench');

      expect(device.writes.single.field, bleFieldBonds);
      // The reading is still shown: the block is readable over any link.
      expect(find.text('C4:7F:51:AA:BB:CC'), findsOneWidget);
    });

    testWidgets('a link that is not a secured session is told why', (
      tester,
    ) async {
      await pump(tester, overUsb: false, secured: false);

      expect(
        find.textContaining('not a secured session, so the device refuses'),
        findsOneWidget,
      );
      expect(
        find.textContaining('Pair this phone with the device again'),
        findsOneWidget,
      );
    });

    testWidgets('a refused write is explained, not dropped', (tester) async {
      final device = await pump(
        tester,
        overUsb: false,
        secured: false,
        refuseWrites: true,
      );

      await tester.tap(find.byTooltip('Set the passkey'));
      await tester.pumpAndSettle();
      // The dialog says it before the code is typed, not only after the refusal.
      expect(
        find.descendant(
          of: find.byType(AlertDialog),
          matching: find.textContaining('not a secured session'),
        ),
        findsOneWidget,
      );
      await tester.enterText(find.byType(TextField), '123456');
      await tester.pump();
      await tester.tap(find.widgetWithText(FilledButton, 'Set passkey'));
      await tester.pumpAndSettle();

      expect(device.writes, isEmpty);
      expect(
        find.textContaining('The device refused the change'),
        findsOneWidget,
      );
    });

    testWidgets('a refused bond write is explained too', (tester) async {
      await pump(tester, overUsb: false, secured: false, refuseWrites: true);

      await setBondLabel(tester, 'Bench');

      expect(
        find.textContaining('The device refused the change'),
        findsOneWidget,
      );
    });

    testWidgets('a refused bonds write on USB asks for a re-read', (
      tester,
    ) async {
      // The device decides the whole frame: a list it has moved on from is refused outright, and
      // the sentence says so instead of leaving a control that silently did nothing.
      await pump(tester, refuseWrites: true);

      await setBondLabel(tester, 'Bench');

      expect(
        find.textContaining('not the one this panel read'),
        findsOneWidget,
      );
    });
  });

  group('orientation', () {
    // The panel states the structural rule once: it is a ListView, so a short window (landscape
    // on a phone) scrolls instead of clipping, and the cards stay in one column in both
    // orientations. The measurements here are evidence, not the rule.
    testWidgets(
      'a short landscape window scrolls to every card without overflowing',
      (tester) async {
        await pump(tester, size: const Size(1280, 400));

        expect(tester.takeException(), isNull);
        expect(find.text('Discoverable now'), findsOneWidget);
        // The lowest note is reachable by scrolling, not cut off.
        final note = find.textContaining('If this phone stops connecting');
        expect(note, findsNothing);
        await tester.scrollUntilVisible(note, 200);
        expect(note, findsOneWidget);
        expect(tester.takeException(), isNull);
      },
    );

    testWidgets('a narrow portrait window keeps every row on screen', (
      tester,
    ) async {
      await pump(tester, size: const Size(360, 740));

      expect(tester.takeException(), isNull);
      expect(find.text('Discoverable now'), findsOneWidget);
      // A bond row's address and both of its buttons survive a phone-width column.
      await tester.scrollUntilVisible(find.text('C4:7F:51:AA:BB:CC'), 200);
      expect(find.byTooltip('Rename this bond'), findsOneWidget);
      expect(find.byTooltip('Delete this bond'), findsOneWidget);
      await tester.scrollUntilVisible(
        find.textContaining('If this phone stops connecting'),
        200,
      );
      expect(tester.takeException(), isNull);
    });
  });

  group('the closing note', () {
    // E68: the device revokes every bond when the passkey changes, so deleting an entry is not
    // the only way they are cleared and the note must not say it is.
    testWidgets('names the passkey change beside deleting an entry', (
      tester,
    ) async {
      await pump(tester);
      final note = find.textContaining('or by changing the passkey');
      await tester.scrollUntilVisible(note, 200);

      expect(note, findsOneWidget);
      expect(find.textContaining('only be cleared'), findsNothing);
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
