// The UX 1c batch 5 layout gate (Connection / Devices / Device view): every row
// and tile the batch touched is pumped at a portrait phone (360x640) and a
// landscape one (800x360), and each case asserts the structural rule, not only
// "no exception".
//
// The harness is the one the shared-layer responsive gate uses
// (`responsive_layout_test.dart`): `tester.view.physicalSize` plus
// `devicePixelRatio = 1.0`, a fresh mount, one layout frame and one painted one,
// then `tester.takeException()`.
//
// The rules this file pins:
//   * the Devices filter bar is a Wrap: the three controls sit on one line when
//     the width allows it and move to the next line when it does not, so a 360 px
//     window no longer overflows the row that wanted ~420 px for its longest real
//     labels ("Net 255", "Type: Tamu v2.0A");
//   * a device row is one [DeviceTile] with its readout as trailing content, and
//     a long name ellipsizes inside it;
//   * the graph sizes its blocks from the width it is given - three per row in
//     the wide arrangement, two at 360 - and never overflows while staying
//     pannable;
//   * the Device view's app-bar title row keeps the name inside the bar (the
//     name ellipsizes; the rename mark keeps its slot).
//
// The measurements below are the supporting evidence; the decision per element
// is the structural rule above, not the pixel sizes.

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/connection_page.dart';
import 'package:tamuapp/ui/device_view_page.dart';
import 'package:tamuapp/ui/devices_page.dart';
import 'package:tamuapp/ui/theme.dart';
import 'package:tamuapp/ui/widgets.dart';

const Size _portrait = Size(360, 640);
const Size _landscape = Size(800, 360);
const List<Size> _bothOrientations = [_portrait, _landscape];

/// The two sizes every touched element is pumped at, named as the card names them.
String _name(Size size) => '${size.width.toInt()}x${size.height.toInt()}';

/// Sets the logical window size for one test and restores it afterwards.
void _useWindow(WidgetTester tester, Size size) {
  tester.view.physicalSize = size;
  tester.view.devicePixelRatio = 1.0;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
}

/// Pumps [child] as the whole window: one layout frame, then a painted one.
/// `pumpAndSettle` is not usable here - the Connection page's 1 s autorefresh
/// timer never settles.
Future<void> _pump(WidgetTester tester, Size size, Widget child) async {
  _useWindow(tester, size);
  await tester.pumpWidget(MaterialApp(theme: buildTheme(), home: child));
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 100));
}

void _report(String line) => debugPrint('[batch 5 gate] $line');

/// One device for the graph and row cases; ids above 1 are nodes, id 1 is the
/// core (`DeviceEntry.isCore`).
DeviceEntry _device(int id, String name, DeviceType type) => DeviceEntry(id: id)
  ..name = name
  ..type = type;

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('Devices filter bar', () {
    for (final size in _bothOrientations) {
      testWidgets('the three controls fit a ${_name(size)} window', (
        tester,
      ) async {
        await _pump(
          tester,
          size,
          Scaffold(
            body: Align(
              alignment: Alignment.bottomCenter,
              child: DevicesFilterBar(
                // The longest real labels: a net of 255 and the longest device
                // type name.
                netFilter: 255,
                typeFilter: DeviceType.tamuV20A,
                sort: DevicesSort.name,
                onCycleNet: () {},
                onCycleType: () {},
                onSortChanged: (_) {},
              ),
            ),
          ),
        );
        expect(
          tester.takeException(),
          isNull,
          reason: 'the filter bar overflowed at ${_name(size)}',
        );

        final net = tester.getRect(find.byType(FilterChip).first);
        final type = tester.getRect(find.byType(FilterChip).last);
        final sort = tester.getRect(find.byType(PopupMenuButton<DevicesSort>));
        _report(
          'filter bar ${_name(size)}: net=${net.width.toStringAsFixed(1)} '
          'type=${type.width.toStringAsFixed(1)} '
          'sort=${sort.width.toStringAsFixed(1)} '
          'rows=${(sort.center.dy - net.center.dy).abs() > 1 ? 2 : 1}',
        );

        // Order is the docs' order: net, type, sort - never reordered by the
        // wrap, only moved to another line.
        expect(net.right, lessThanOrEqualTo(type.left));

        // The structural rule: one line while the width allows it, otherwise the
        // sort menu drops to the next line (the two chips still share the first
        // one, which is what keeps a 360 px window clear).
        if (size.width >= 800) {
          expect(
            sort.left,
            greaterThanOrEqualTo(type.right),
            reason: 'the wide window keeps the sort menu after the chips',
          );
          expect(
            (sort.center.dy - net.center.dy).abs(),
            lessThan(1),
            reason: 'the wide window keeps all three controls on one line',
          );
        } else {
          expect(
            sort.top,
            greaterThanOrEqualTo(type.bottom - 0.5),
            reason: 'the narrow window moves the sort menu below the chips',
          );
          expect(
            (type.center.dy - net.center.dy).abs(),
            lessThan(1),
            reason: 'the two chips still share the first line at 360 px',
          );
        }
      });
    }
  });

  group('Devices page', () {
    for (final size in _bothOrientations) {
      testWidgets('mounts with its filter bar at ${_name(size)}', (
        tester,
      ) async {
        await _pump(tester, size, const DevicesPage());
        expect(
          tester.takeException(),
          isNull,
          reason: 'the Devices page overflowed at ${_name(size)}',
        );
        // Off-device there is no link, so the body is the empty state and the
        // bar below it is what this case measures.
        expect(find.text('Not connected'), findsOneWidget);
        expect(find.byType(DevicesFilterBar), findsOneWidget);
        expect(find.byType(FilterChip), findsNWidgets(2));
        _report(
          'devices page ${_name(size)}: bar='
          '${tester.getSize(find.byType(DevicesFilterBar)).toString()}',
        );
      });
    }
  });

  group('Device row (DeviceTile)', () {
    for (final size in _bothOrientations) {
      testWidgets('a long name and a signal readout fit ${_name(size)}', (
        tester,
      ) async {
        await _pump(
          tester,
          size,
          Scaffold(
            body: ListView(
              children: [
                DeviceTile(
                  icon: Icons.bluetooth,
                  name: 'SIXTEEN BYTE NM',
                  subtitle: 'MAC AA:BB:CC:DD:EE:FF',
                  trailing: Row(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      const Icon(
                        Icons.signal_cellular_alt,
                        size: 18,
                        color: kError,
                      ),
                      const SizedBox(width: Spacing.xs),
                      const Text('-92 dBm'),
                      IconButton(
                        icon: const Icon(Icons.star_border),
                        onPressed: () {},
                      ),
                    ],
                  ),
                  onTap: () {},
                ),
              ],
            ),
          ),
        );
        expect(
          tester.takeException(),
          isNull,
          reason: 'the device row overflowed at ${_name(size)}',
        );
        expect(find.byType(DeviceTile), findsOneWidget);
        expect(find.text('-92 dBm'), findsOneWidget);

        // The name gives way: its box stays inside the row it is given.
        final row = tester.getRect(find.byType(DeviceTile));
        final name = tester.getRect(find.text('SIXTEEN BYTE NM'));
        expect(name.right, lessThanOrEqualTo(row.right));
        _report(
          'device row ${_name(size)}: row=${row.width.toStringAsFixed(0)} '
          'name=${name.width.toStringAsFixed(0)}',
        );
      });
    }
  });

  group('Graph view', () {
    // A core plus five nodes: enough to show two rows of three in the wide
    // arrangement and three lines of two at 360 px.
    final devices = [
      _device(1, 'Tamu', DeviceType.tamuV20A),
      _device(2, 'DAS Alpha', DeviceType.dualAnalogSensor),
      _device(3, 'DAS Bravo', DeviceType.dualAnalogSensor),
      _device(4, 'Valu Charlie', DeviceType.valuV20),
      _device(5, 'DAS Delta', DeviceType.dualAnalogSensor),
      _device(6, 'DAS Echo', DeviceType.dualAnalogSensor),
    ];

    for (final size in _bothOrientations) {
      testWidgets('the blocks are sized from the width at ${_name(size)}', (
        tester,
      ) async {
        await _pump(tester, size, Scaffold(body: GraphView(devices: devices)));
        expect(
          tester.takeException(),
          isNull,
          reason: 'the graph overflowed at ${_name(size)}',
        );

        // Block width is derived, not the old constant 160: the even share of the
        // width, floored at 128 and capped at 200.
        final block = tester.getSize(find.byType(Card).first);
        final expected = size.width == 800 ? 200.0 : 150.0;
        _report(
          'graph ${_name(size)}: block='
          '${block.width.toStringAsFixed(1)}x${block.height.toStringAsFixed(1)}',
        );
        expect(block.width, closeTo(expected, 0.5));

        // Three per row is the wide arrangement, only: at 360 the same nodes take
        // two per line.
        final alpha = tester.getTopLeft(find.text('DAS Alpha')).dy;
        final bravo = tester.getTopLeft(find.text('DAS Bravo')).dy;
        final charlie = tester.getTopLeft(find.text('Valu Charlie')).dy;
        expect(alpha, closeTo(bravo, 0.5));
        if (size.width >= 800) {
          expect(
            charlie,
            closeTo(alpha, 0.5),
            reason: 'the wide arrangement is rows of three',
          );
        } else {
          expect(
            charlie,
            greaterThan(alpha),
            reason: 'a 360 px wide view holds two blocks per line',
          );
        }

        // The canvas is still pannable, which is what reaches the rest of a tall
        // graph.
        expect(find.byType(InteractiveViewer), findsOneWidget);
      });
    }
  });

  group('Device view', () {
    for (final size in _bothOrientations) {
      for (final scale in const [1.0, 2.0]) {
        testWidgets('a 16-byte name stays inside the app bar at ${_name(size)} '
            '(text scale $scale)', (tester) async {
          // The docs' rename limit is 16 bytes, so this is the longest name the
          // page can be showing; at a 2x text scale it cannot fit the bar's
          // title slot, which is the case the row has to give way in.
          const longName = 'SIXTEEN BYTE NAME';
          tester.platformDispatcher.textScaleFactorTestValue = scale;
          addTearDown(tester.platformDispatcher.clearTextScaleFactorTestValue);
          await _pump(
            tester,
            size,
            Scaffold(
              appBar: AppBar(
                title: DeviceTitle(name: longName, onTap: () {}),
                actions: [
                  IconButton(
                    icon: const Icon(Icons.visibility_outlined),
                    onPressed: () {},
                  ),
                ],
              ),
            ),
          );
          expect(
            tester.takeException(),
            isNull,
            reason:
                'the app-bar title row overflowed at ${_name(size)} '
                'with text scale $scale',
          );

          final title = tester.getRect(find.byType(DeviceTitle));
          final name = tester.getRect(find.text(longName));
          _report(
            'device title ${_name(size)} scale $scale: title='
            '${title.width.toStringAsFixed(0)} '
            'name=${name.width.toStringAsFixed(0)}',
          );
          // The name is clipped to the bar's title slot, not drawn past it.
          expect(name.right, lessThanOrEqualTo(title.right + 0.5));
          expect(
            tester.widget<Text>(find.text(longName)).overflow,
            TextOverflow.ellipsis,
          );
        });
      }

      testWidgets('the page mounts at ${_name(size)}', (tester) async {
        // No link, so the page shows its loading state; this case is the page
        // shell (app bar + title row) laying out at both orientations.
        await _pump(tester, size, const DeviceViewPage(deviceId: 2));
        expect(
          tester.takeException(),
          isNull,
          reason: 'the Device view page overflowed at ${_name(size)}',
        );
        expect(find.byType(DeviceViewPage), findsOneWidget);
      });
    }
  });

  group('Connection page', () {
    for (final size in _bothOrientations) {
      testWidgets('mounts at ${_name(size)}', (tester) async {
        await _pump(tester, size, const ConnectionPage());
        expect(
          tester.takeException(),
          isNull,
          reason: 'the Connection page overflowed at ${_name(size)}',
        );
        // No radio in the test host, so the list is empty: the page's own
        // chrome (app bar, source menu, refresh controls) is what lays out here.
        expect(find.byType(ConnectionPage), findsOneWidget);
      });
    }
  });
}
