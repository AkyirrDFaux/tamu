/// The Update page names the device failures apart, and says what to do about each.
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

/// A small node - four one-chunk pages, the DAS geometry scaled down - so a run is a handful of
/// requests and every request answers immediately: the fake has no timers.
FakeBootloaderTransport _device() =>
    FakeBootloaderTransport(appSize: 128, pageSize: 32, name: 'Tamu v2.0A');

/// Mounts the page over [transport] with four chunks (128 bytes) to write, taps Update, and lets
/// the run finish.
Future<void> _runUpdate(
  WidgetTester tester,
  FakeBootloaderTransport transport,
) async {
  tester.view.physicalSize = const Size(1000, 3000);
  tester.view.devicePixelRatio = 1.0;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);

  final fakePackage = PackageForTest(
    target: 'DAS_v0_1',
    image: testImage(transport.appSize),
  );
  await tester.pumpWidget(
    MaterialApp(
      home: UpdatePage(
        isConnected: () => true,
        initialPackage: fakePackage.build(),
        initialPackageName: fakePackage.name,
        clientFactory: () => BootloaderClient(transport: transport),
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
    // The node answers identify and then goes quiet for the read-back: the run stops on the
    // first read it does not answer, and names silence rather than the link.
    final t = _device()
      ..beforeReadBack = (device, round) {
        device.silent = true;
      };
    await _runUpdate(tester, t);

    expect(find.textContaining('Update stopped (node silent)'), findsOneWidget);
    expect(find.textContaining('the node did not answer'), findsOneWidget);
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
    // The node refuses the first chunk it is asked to program (it answers its own failure frame,
    // the core's reason 0x04): the run stops at that write.
    final t = _device()..rejectWriteOnce.add(0);
    await _runUpdate(tester, t);

    expect(
      find.textContaining('Update stopped (node refused)'),
      findsOneWidget,
    );
    expect(find.textContaining('the node refused it'), findsOneWidget);
    expect(
      find.textContaining('A refused erase or write'),
      findsOneWidget,
      reason: 'the remedy has to say the page was not blank when the write arrived',
    );
    expect(find.textContaining('Power-cycle the node:'), findsOneWidget);
  });

  testWidgets(
    'a write that does not match is named, with the first differing offset',
    (tester) async {
      // The read path lies while the flash is right: the node's own CRC agrees and the read-back
      // differs, which no correction round can settle. Five rounds run and the run stops naming
      // both measures and the offsets.
      final t = _device()..readLies = true;
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
        find.textContaining('CRC matches, read-back differs'),
        findsOneWidget,
        reason: 'both measures are reported, and neither wins the disagreement',
      );
      expect(
        find.textContaining('Power-cycle the node:'),
        findsOneWidget,
        reason: 'the node is still in its bootloader',
      );
    },
  );
}
