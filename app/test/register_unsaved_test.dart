import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/core/unsaved_settings.dart';
import 'package:tamuapp/core/wifi_settings.dart';
import 'package:tamuapp/ui/register_page.dart';
import 'package:tamuapp/ui/theme.dart';

import 'support/block_reads.dart';
import 'support/single_block_client.dart';

/// The Register view's half of E71: a write to a `Persistent` static field lands in the device's
/// RAM and only Save all writes its `.SV` backup, so the page records the write in the device's
/// store and says so - the strip above the tree names the field, leaving the view asks about it,
/// and only a save the device answers (and the field reads back) clears it.
///
/// The WiFi Settings block stands in for any static block with a `Persistent` text field: it has
/// one (`SSID`), plus a field whose write Save all does not carry (the `Password`, which the device
/// keeps in NVS).
class _FakeStaticDevice extends SingleBlockRegisterClient {
  _FakeStaticDevice({required super.deviceId, Map<int, BlockRead>? fields})
    : fields = fields ?? wifiFields();

  /// The block's live values, by field: a write lands here, so the read-back the save ends on
  /// reads the value the page just sent.
  Map<int, BlockRead> fields;

  /// Every write the page sent, in order.
  final writes = <({int field, List<int> bytes})>[];

  /// How many times the page asked the device to persist (Save All).
  int saves = 0;

  /// What Save All answers: a device that does not answer reports `false`.
  bool saveAllAnswers = true;

  @override
  int get blockType => wifiSettingsBlockTypeValue;

  @override
  String get blockName => 'WiFi';

  @override
  int get fieldCount => wifiFieldCount;

  @override
  Future<BlockRead?> readBlockField(
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
    writes.add((field: field, bytes: value));
    final current = fields[field];
    fields[field] = (
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

/// The block's five fields: `SSID` and `Enabled` are `Persistent`, `Password` is not (its trigger
/// stores it in NVS), `State` and `IP address` are read-only.
Map<int, BlockRead> wifiFields() => {
  wifiFieldEnabled: fieldBytes(DataType.bool_.value, const [
    1,
  ], flags: ValueFlags.persistent),
  wifiFieldSsid: fieldBytes(
    DataType.text.value,
    paddedText('TamuNet', 32),
    flags: ValueFlags.persistent,
    size: 32,
  ),
  wifiFieldPassword: fieldBytes(
    DataType.text.value,
    paddedText('hunter2', 32),
    size: 32,
  ),
  wifiFieldState: fieldBytes(DataType.enum_.value, const [
    2,
  ], flags: ValueFlags.readOnly),
  wifiFieldIp: fieldBytes(
    DataType.undefined.value,
    List<int>.filled(16, 0),
    flags: ValueFlags.readOnly,
    size: 16,
  ),
};

/// One pump of the page's work: the write, the read-back behind it and the rebuild that follows
/// are three awaits, and the page's 0.5 s auto-refresh makes `pumpAndSettle` unusable here.
Future<void> pumpWork(WidgetTester tester) async {
  for (var i = 0; i < 4; i++) {
    await tester.pump(const Duration(milliseconds: 10));
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  /// Mounts the tree on its own, with [store] as the device's record of unsaved writes.
  Future<({_FakeStaticDevice device, UnsavedSettings store})> pump(
    WidgetTester tester, {
    Map<int, BlockRead>? fields,
    UnsavedSettings? store,
    Size size = const Size(900, 1600),
  }) async {
    tester.view.physicalSize = size;
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final device = _FakeStaticDevice(deviceId: 1, fields: fields);
    final record = store ?? UnsavedSettings.forTest();
    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        home: RegisterPage(
          deviceId: 1,
          isConnected: () => true,
          // A store of its own per mount: the app's is shared, so one case's write would read as
          // another case's unsaved change.
          unsaved: record,
          clientFactory: (_) => device,
        ),
      ),
    );
    await tester.pump(); // the initial topology load
    await tester.tap(find.text('WiFi').first); // expand the block
    await pumpWork(tester);
    addTearDown(() async {
      // The page's 0.5 s auto-refresh timer has to be cancelled before the test ends.
      await tester.pumpWidget(const SizedBox());
    });
    return (device: device, store: record);
  }

  /// Opens the row [label]'s value editor, types [value] and accepts it.
  Future<void> writeValue(
    WidgetTester tester,
    String label,
    String value,
  ) async {
    await tester.tap(
      find
          .ancestor(of: find.text(label), matching: find.byType(ListTile))
          .first,
    );
    await pumpWork(tester);
    await tester.enterText(find.byType(TextField), value);
    await tester.pump();
    await tester.tap(find.text('OK'));
    await pumpWork(tester);
  }

  testWidgets(
    'a static-field write is recorded and the strip names the field',
    (tester) async {
      final run = await pump(tester);

      await writeValue(tester, 'SSID', 'Bench');

      expect(run.device.writes.single.field, wifiFieldSsid);
      expect(String.fromCharCodes(run.device.writes.single.bytes), 'Bench');
      // The field's own name, as the row prints it - not "field 1".
      expect(run.store.describe(1), 'SSID');
      expect(find.text('Not saved: SSID.'), findsOneWidget);
    },
  );

  testWidgets('leaving the view asks about it and names the field', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(900, 1600);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final store = UnsavedSettings.forTest();
    final device = _FakeStaticDevice(deviceId: 1);
    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        home: Builder(
          builder: (context) => Scaffold(
            body: Center(
              child: FilledButton(
                onPressed: () => Navigator.of(context).push(
                  MaterialPageRoute<void>(
                    builder: (_) => RegisterPage(
                      deviceId: 1,
                      isConnected: () => true,
                      unsaved: store,
                      clientFactory: (_) => device,
                    ),
                  ),
                ),
                child: const Text('Open the register'),
              ),
            ),
          ),
        ),
      ),
    );
    await tester.tap(find.text('Open the register'));
    await tester.pump(); // start the route transition
    await tester.pump(const Duration(milliseconds: 400)); // and finish it
    await tester.tap(find.text('WiFi').first);
    await pumpWork(tester);
    addTearDown(() async {
      await tester.pumpWidget(const SizedBox());
    });

    await writeValue(tester, 'SSID', 'Bench');
    expect(store.isDirty(1), isTrue);

    // The pop is blocked while the change is unsaved, and the dialog names the field.
    await tester.pageBack();
    await pumpWork(tester);
    expect(find.text('Unsaved changes'), findsOneWidget);
    expect(
      find.textContaining('holds changes that are not saved: SSID.'),
      findsOneWidget,
    );

    await tester.tap(find.text('Leave without saving'));
    await pumpWork(tester);
    // The page is gone and the device still holds the value in RAM only: the record is the
    // device's, not the page's.
    expect(find.text('Open the register'), findsOneWidget);
    expect(store.isDirty(1), isTrue);
  });

  testWidgets('Save all in the strip writes the device and clears the state', (
    tester,
  ) async {
    final run = await pump(tester);
    await writeValue(tester, 'SSID', 'Bench');

    await tester.tap(find.widgetWithText(FilledButton, 'Save all'));
    await pumpWork(tester);

    // The save re-applied the write, sent CID 5 and read the field back: only then is the entry
    // gone.
    expect(run.device.saves, 1);
    expect(run.device.writes.length, 2);
    expect(run.store.isDirty(1), isFalse);
    expect(find.textContaining('Not saved:'), findsNothing);
  });

  testWidgets('a Save all the device refuses keeps the field named', (
    tester,
  ) async {
    final run = await pump(tester);
    await writeValue(tester, 'SSID', 'Bench');

    run.device.saveAllAnswers = false;
    await tester.tap(find.widgetWithText(FilledButton, 'Save all'));
    await pumpWork(tester);

    expect(run.store.isDirty(1), isTrue);
    expect(run.store.describe(1), 'SSID');
    expect(
      find.textContaining(
        'Not saved: SSID - the device did not answer Save all',
      ),
      findsOneWidget,
    );
  });

  testWidgets('a write to a field Save all does not carry is not recorded', (
    tester,
  ) async {
    final run = await pump(tester);

    // The password is stored by the device's own NVS trigger, so Save all never writes it: an
    // entry for it could not be cleared by any save action.
    await writeValue(tester, 'Password', 'secret123');

    expect(run.device.writes.single.field, wifiFieldPassword);
    expect(run.device.saves, 0);
    expect(run.store.isDirty(1), isFalse);
    expect(find.textContaining('Not saved:'), findsNothing);
  });

  // Responsive gate. The structural rule is orientation-free: the strip is a fixed band ABOVE the
  // tree's scroll view (it never scrolls away with the rows it names), and the tree keeps its own
  // scrolling column below it. So a short (landscape) window scrolls the rows under a strip that
  // stays put, and a narrow (portrait) one wraps the strip's text instead of clipping it. The sizes
  // below are evidence for that rule, not the rule itself.
  group('a tree with an unsaved write fits both orientations', () {
    for (final size in const [Size(360, 480), Size(640, 360)]) {
      testWidgets('no overflow and the row is reachable at $size', (
        tester,
      ) async {
        final store = UnsavedSettings.forTest()
          ..mark(
            const PendingSetting(
              deviceId: 1,
              blockName: 'WiFi',
              blockType: wifiSettingsBlockTypeValue,
              inst: 0,
              field: wifiFieldSsid,
              fieldName: 'SSID',
              meta: ValueInfo(
                type: 0x0A,
                size: 5,
                flags: ValueFlags.persistent,
                key: 0xFF,
              ),
              bytes: [66, 101, 110, 99, 104],
            ),
          );
        await pump(tester, store: store, size: size);
        await pumpWork(tester);

        expect(
          tester.takeException(),
          isNull,
          reason: 'the page overflowed at $size',
        );
        expect(find.text('Not saved: SSID.'), findsOneWidget);
        // The row underneath is still reachable: the strip takes its own band, the tree scrolls.
        await tester.dragUntilVisible(
          find.text('IP address'),
          find.byType(Scrollable).last,
          const Offset(0, -60),
        );
        expect(find.text('IP address'), findsOneWidget);
      });
    }
  });
}
