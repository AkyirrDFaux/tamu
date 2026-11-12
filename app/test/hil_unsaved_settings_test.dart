// Card t_7250e6b2: the app owns the "unsaved" state of a `Persistent` write (E71).
//
// A write to `Enabled` or `Display name` lands in the device's RAM; only Save All (CID 5) writes
// the device's `.SV` backup, and the device's read-back cannot tell the two apart. The panel
// therefore keeps the change itself and says so (`core/unsaved_settings.dart`,
// `ui/unsaved_settings_panel.dart`).
//
// This file drives the REAL panel over the rig, in the pairs the card asks for. Each case leaves
// the device in a state the operator then probes with a reset, so the cases are selected one at a
// time (`--plain-name`) and a reset runs between them - see the card's handoff for the recipe:
//
//   TAMU_HIL=/dev/ttyACM0 LD_LIBRARY_PATH=build/linux/x64/debug/bundle/lib \
//     flutter test --no-pub --tags hil test/hil_unsaved_settings_test.dart --plain-name unsaved
//
// Every case preps only the RAM value it needs to differ from (the panel skips a write of the value
// the device already holds), so a case is repeatable without a reset between two runs of it.
@Timeout(Duration(minutes: 10))
@Tags(['hil'])
library;

import 'package:flutter/foundation.dart'
    show debugDefaultTargetPlatformOverride;
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/ble_security.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/core/unsaved_settings.dart';
import 'package:tamuapp/ui/ble_security_page.dart';
import 'package:tamuapp/ui/notice_host.dart';
import 'package:tamuapp/ui/theme.dart';

import 'hil/hil_env.dart';

void main() {
  final env = HilEnv();
  setUpAll(() async {
    await env.connectAndScan();
    // The HIL environment pins the host's target platform for the app's own stack; a widget test
    // verifies Flutter's debug variables are back at their defaults when it ends.
    debugDefaultTargetPlatformOverride = null;
  });
  tearDownAll(env.finish);

  /// Real wall-clock time for the panel's requests: `pumpAndSettle` drives frames, not the bus,
  /// and a request's own timers are the test's fake ones. Each round lets the event loop run and
  /// then draws what landed.
  Future<void> letTheBusRun(WidgetTester tester, {int rounds = 12}) async {
    for (var i = 0; i < rounds; i++) {
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 300)),
      );
      await tester.pump(const Duration(milliseconds: 50));
    }
  }

  /// [letTheBusRun], stopping as soon as [finder] is on screen (or the rounds run out).
  Future<void> pumpUntil(WidgetTester tester, Finder finder) async {
    for (var i = 0; i < 60; i++) {
      if (finder.evaluate().isNotEmpty) return;
      await letTheBusRun(tester, rounds: 1);
    }
  }

  /// Writes one field straight through the client, as the SETUP of a case: the panel skips a write
  /// whose value the device already holds, so the value it must change has to be known first. This
  /// is not the path under test - it is device prep, like a reset.
  Future<void> prep(WidgetTester tester, int field, List<int> bytes) async {
    final client = RegisterClient(deviceId: coreId);
    await tester.runAsync(() async {
      final read = await client.readBlockField(
        bleSettingsBlockTypeValue,
        0,
        field,
        0,
      );
      if (read != null && sameFieldValue(bytes, read.value)) return;
      await client.writeBlockField(
        bleSettingsBlockTypeValue,
        0,
        field,
        0,
        ValueInfo(
          type: read?.meta.type ?? DataType.bool_.value,
          size: bytes.length,
          flags: read?.meta.flags ?? ValueFlags.persistent,
        ),
        bytes,
      );
    });
    await tester.pump();
  }

  /// Mounts the panel against the live link and waits for its six field reads to land, then
  /// returns the store it records into.
  Future<UnsavedSettings> mountPanel(WidgetTester tester) async {
    // A tall window so the radio card is reachable without a scroll gesture.
    tester.view.physicalSize = const Size(900, 1600);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final store = UnsavedSettings.forTest();
    final core = DeviceDatabase.instance.byId(coreId)?.id ?? coreId;
    // `isConnected` is left to the app's own manager: the rig link is what the panel must talk to.
    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        builder: noticeHostBuilder,
        home: BleSecurityPage(deviceId: core, unsaved: store),
      ),
    );
    await pumpUntil(tester, find.text('Radio'));
    addTearDown(() async {
      await tester.pumpWidget(const SizedBox());
    });
    return store;
  }

  /// Sets the advertised name through the panel's own dialog.
  Future<void> setDisplayName(WidgetTester tester, String name) async {
    await tester.tap(find.byTooltip('Set the display name'));
    await tester.pumpAndSettle();
    await tester.enterText(find.byType(TextField), name);
    await tester.tap(find.widgetWithText(FilledButton, 'Set name'));
    await letTheBusRun(tester);
  }

  Future<void> tapSaveAll(WidgetTester tester) async {
    await tester.tap(find.widgetWithText(FilledButton, 'Save all'));
    await letTheBusRun(tester);
  }

  List<int> padded(String text) => [
    ...text.codeUnits,
    ...List<int>.filled(bleDisplayNameWidth - text.codeUnits.length, 0),
  ];

  testWidgets('unsaved: the panel holds the name and says so', (tester) async {
    if (!env.hilRequested) {
      markTestSkipped('TAMU_HIL not set');
      return;
    }
    await prep(tester, bleFieldDisplayName, padded(''));
    final store = await mountPanel(tester);
    expect(find.text('Radio'), findsOneWidget, reason: 'the block was read');

    await setDisplayName(tester, 'Tamu Desk');

    expect(store.describe(coreId), 'Display name');
    expect(find.text('Not saved: Display name.'), findsOneWidget);
    expect(
      find.textContaining('the device holds it in RAM only'),
      findsOneWidget,
    );
    // No Save All went out: the write is RAM-only, which is what a reset undoes.
    expect(store.isDirty(coreId), isTrue);
    // ignore: avoid_print
    print(
      'RIG unsaved: display name written, store says "${store.describe(coreId)}", '
      'strip on screen',
    );
  });

  testWidgets('saved: Save all reads the name back and clears the state', (
    tester,
  ) async {
    if (!env.hilRequested) {
      markTestSkipped('TAMU_HIL not set');
      return;
    }
    await prep(tester, bleFieldDisplayName, padded(''));
    final store = await mountPanel(tester);

    await setDisplayName(tester, 'Tamu Desk');
    await tapSaveAll(tester);

    expect(store.isDirty(coreId), isFalse);
    expect(find.textContaining('Not saved:'), findsNothing);
    // ignore: avoid_print
    print(
      'RIG saved: Save all cleared the store (dirty=${store.isDirty(coreId)}), '
      'strip gone',
    );
  });

  testWidgets(
    'unsaved: the radio is switched off in RAM only, and the panel warns',
    (tester) async {
      if (!env.hilRequested) {
        markTestSkipped('TAMU_HIL not set');
        return;
      }
      await prep(tester, bleFieldEnabled, const [1]);
      final store = await mountPanel(tester);

      await tester.tap(find.text('Enabled'));
      await letTheBusRun(tester);

      expect(store.describe(coreId), 'Enabled');
      expect(find.text('Not saved: Enabled.'), findsOneWidget);
      // ignore: avoid_print
      print(
        'RIG unsaved: Enabled written off, store says "${store.describe(coreId)}", '
        'strip on screen',
      );
    },
  );

  testWidgets('saved: Save all keeps the radio off', (tester) async {
    if (!env.hilRequested) {
      markTestSkipped('TAMU_HIL not set');
      return;
    }
    await prep(tester, bleFieldEnabled, const [1]);
    final store = await mountPanel(tester);

    await tester.tap(find.text('Enabled'));
    await letTheBusRun(tester);
    await tapSaveAll(tester);

    expect(store.isDirty(coreId), isFalse);
    // ignore: avoid_print
    print('RIG saved: Enabled=false saved (dirty=${store.isDirty(coreId)})');
  });

  testWidgets('restore: the radio on and the name empty, both saved', (
    tester,
  ) async {
    if (!env.hilRequested) {
      markTestSkipped('TAMU_HIL not set');
      return;
    }
    await prep(tester, bleFieldEnabled, const [0]);
    final store = await mountPanel(tester);

    await tester.tap(find.text('Enabled'));
    await letTheBusRun(tester);
    await prep(tester, bleFieldDisplayName, padded('X'));
    await setDisplayName(tester, '');
    await tapSaveAll(tester);

    expect(store.isDirty(coreId), isFalse);
    // ignore: avoid_print
    print('RIG restore: Enabled on and Display name empty, both saved');
  });
}
