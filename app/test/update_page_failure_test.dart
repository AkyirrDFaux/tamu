/// The Update page names the three device failures apart, and says what to do about each.
///
/// A bootloader never jumps to the app by itself and no packet can reset a node, so every failed
/// run leaves the device waiting for a power cycle. One message for all of them ("check the link
/// and retry") left the owner unable to tell a dead link from a node whose flash does not hold
/// data, and left the node silently in its bootloader.
library;

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader_client.dart';
import 'package:tamuapp/ui/update_page.dart';

import 'support/bootloader_transport.dart';
import 'support/fw_package.dart';

/// Mounts the page over [transport] with four chunks (128 bytes) to write, taps Update, and lets
/// the run finish. Every request answers immediately: the fake has no timers, and the client's
/// pacing is off.
Future<void> _runUpdate(
  WidgetTester tester,
  FakeBootloaderTransport transport,
) async {
  tester.view.physicalSize = const Size(1000, 3000);
  tester.view.devicePixelRatio = 1.0;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);

  transport.identity = const BootloaderIdentity(
    appSize: 0x4000,
    name: 'Tamu v2.0A',
  );
  final fakePackage = PackageForTest(target: 'DAS_v0_1', image: testImage(128));
  await tester.pumpWidget(
    MaterialApp(
      home: UpdatePage(
        isConnected: () => true,
        initialPackage: fakePackage.build(),
        initialPackageName: fakePackage.name,
        clientFactory: () =>
            BootloaderClient(transport: transport, writePacing: Duration.zero),
      ),
    ),
  );
  await tester.pumpAndSettle();

  await tester.tap(find.widgetWithText(FilledButton, 'Update'));
  for (var i = 0; i < 12; i++) {
    await tester.pump(const Duration(milliseconds: 1));
  }
}

void main() {
  testWidgets('a silent node is named and the user is told to power-cycle', (
    tester,
  ) async {
    final t = FakeBootloaderTransport()..alwaysNullRead = true;
    await _runUpdate(tester, t);

    expect(find.textContaining('Update stopped (no answer)'), findsOneWidget);
    expect(find.textContaining('the node stopped answering'), findsOneWidget);
    expect(
      find.textContaining('Power-cycle the node:'),
      findsOneWidget,
      reason: 'a node left in its bootloader needs the power switch',
    );
    expect(find.textContaining('nothing was programmed'), findsNothing);
  });

  testWidgets('a refused write is named apart from a silent link', (
    tester,
  ) async {
    final t = FakeBootloaderTransport()..alwaysReject = true;
    await _runUpdate(tester, t);

    expect(
      find.textContaining('Update stopped (write refused)'),
      findsOneWidget,
    );
    expect(find.textContaining('never confirmed the chunk'), findsOneWidget);
    expect(
      find.textContaining('nothing was programmed'),
      findsOneWidget,
      reason: 'the remedy has to say the node holds no new data',
    );
    expect(find.textContaining('Power-cycle the node:'), findsOneWidget);
  });

  testWidgets(
    'a write that does not match is named, with the first differing offset',
    (tester) async {
      // The device loses every chunk and keeps losing the correction: both measures name the
      // image wrong, one correction round runs, and the run stops with the offset.
      final t = FakeBootloaderTransport()..corruptAll = true;
      await _runUpdate(tester, t);

      expect(
        find.textContaining('Update stopped (image does not read back)'),
        findsOneWidget,
      );
      expect(
        find.textContaining('still does not read back'),
        findsOneWidget,
        reason: 'the offset and the count are named',
      );
      expect(
        find.textContaining('digest 0x'),
        findsOneWidget,
        reason: 'both measures are reported, the digest first',
      );
      expect(
        find.textContaining('Power-cycle the node:'),
        findsOneWidget,
        reason: 'the node is still in its bootloader',
      );
    },
  );
}
