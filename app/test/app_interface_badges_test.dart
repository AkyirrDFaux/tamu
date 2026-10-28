import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/ui/theme.dart';
import 'package:tamuapp/ui/viewer_kit.dart';

/// The connection badge on the device view: one chip per app interface the device reports live
/// (System field 8 bits). The point of the badge is the bit field itself - a device can be on
/// USB and BLE at once, and both must be visible.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  Future<void> pump(WidgetTester tester, int bits) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        home: Scaffold(body: AppInterfaceBadges(bits)),
      ),
    );
  }

  testWidgets('USB alone reads USB', (tester) async {
    await pump(tester, 1);
    expect(find.text('USB'), findsOneWidget);
    expect(find.text('BLE'), findsNothing);
    expect(find.text('WiFi'), findsNothing);
    expect(find.text('No link'), findsNothing);
  });

  testWidgets('USB and BLE together reads both', (tester) async {
    await pump(tester, 3);
    expect(find.text('USB'), findsOneWidget);
    expect(find.text('BLE'), findsOneWidget);
    expect(find.text('WiFi'), findsNothing);
  });

  testWidgets('all three interfaces', (tester) async {
    await pump(tester, 7);
    expect(find.text('USB'), findsOneWidget);
    expect(find.text('BLE'), findsOneWidget);
    expect(find.text('WiFi'), findsOneWidget);
  });

  testWidgets('no interface reads No link, never nothing at all', (
    tester,
  ) async {
    await pump(tester, 0);
    expect(find.text('No link'), findsOneWidget);
  });

  testWidgets('a bit with no name is shown rather than dropped', (
    tester,
  ) async {
    await pump(tester, 1 | 0x20);
    expect(find.text('USB'), findsOneWidget);
    expect(find.text('unknown 0x20'), findsOneWidget);
  });

  // The badge lives in a title row on the device view, so it must fit a phone held upright: the
  // rule is that it is a WRAP, not a Row - extra chips move to a second line instead of
  // overflowing. 160 px is narrower than the three chips together, which is the evidence.
  testWidgets('the chips wrap instead of overflowing a narrow row', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(160, 320);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    await pump(tester, 7 | 0x20);
    expect(tester.takeException(), isNull, reason: 'the badge overflowed');
    expect(find.byType(Wrap), findsOneWidget);
    expect(find.text('WiFi'), findsOneWidget);
  });
}
