// Shared-layer responsive gates: the two-axis shell and a dialog body that
// never hands its content more than the space the dialog has to give it.
//
// Both cases are measured on the pre-fix code (`master` d0d8edc, before the
// fix) with the same harness these tests use — `tester.view.physicalSize` plus
// `devicePixelRatio = 1.0`, a fresh mount, and `tester.takeException()` after a
// painted frame:
//
//   shell, 640x360 and 800x360  -> 'A RenderFlex overflowed by 23 pixels on the
//                                  bottom.' (the labelled NavigationRail needs
//                                  ~389 px of height and Flutter's rail is not
//                                  scrollable)
//   shell, 1024x600 / 1280x400  -> no exception (the rail fits)
//   PickerShell(maxWidth 500, height 400), ListView body
//                               -> DialogBody measures 500x398 at 1024x600 but
//                                  500x158 at 640x360 and 232x260 at 360x480
//   the same shell with a content-sized body (a column of 20 form rows)
//                               -> 'A RenderFlex overflowed by 882 pixels on the
//                                  bottom.' at 640x360 (780 px at 360x480): the
//                                  rows were clipped, not reachable.
//   the same, with the hint line above the body
//                               -> 'A RenderFlex overflowed by 903 pixels on the
//                                  bottom.' at 640x360: the hint wrapper's
//                                  Expanded forced the content-sized body to the
//                                  leftover height.
//
// After the fix a short window gets the Drawer instead of the clipped rail, and
// a content-sized body gets a scroll view, so the last row is reachable by
// scrolling in both cases.

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/main.dart';
import 'package:tamuapp/ui/dialogs.dart';
import 'package:tamuapp/ui/theme.dart';
import 'package:tamuapp/ui/widgets.dart';

/// Sets the logical window size for one test and restores it afterwards.
void useWindow(WidgetTester tester, Size size) {
  tester.view.physicalSize = size;
  tester.view.devicePixelRatio = 1.0;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
}

/// True when [inner] is entirely inside [outer].
bool fitsInside(Rect inner, Rect outer) =>
    inner.top >= outer.top - 0.5 &&
    inner.bottom <= outer.bottom + 0.5 &&
    inner.left >= outer.left - 0.5 &&
    inner.right <= outer.right + 0.5;

void main() {
  group('shell', () {
    // A fresh mount of the real shell at each orientation/size pair.
    Future<void> mountShell(WidgetTester tester, Size size) async {
      useWindow(tester, size);
      await tester.pumpWidget(
        MaterialApp(theme: buildTheme(), home: const ShellPage()),
      );
      // Two frames: the first layout, then a painted one. pumpAndSettle is not
      // usable here — the Connection page's 1 s autorefresh timer never settles.
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 100));
    }

    for (final size in const [Size(640, 360), Size(800, 360)]) {
      testWidgets('a short window gets the drawer, not a clipped rail', (
        tester,
      ) async {
        await mountShell(tester, size);
        expect(
          tester.takeException(),
          isNull,
          reason: 'the shell overflowed at $size',
        );
        // The rail cannot fit its own ~389 px of content here, so the layout
        // must fall back to the drawer.
        expect(find.byType(NavigationRail), findsNothing);

        // ...and the drawer is actually reachable from the app bar.
        expect(find.byIcon(Icons.menu), findsOneWidget);
        await tester.tap(find.byIcon(Icons.menu));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 400));
        expect(find.byType(Drawer), findsOneWidget);
        expect(find.widgetWithText(ListTile, 'Devices'), findsOneWidget);
        expect(tester.takeException(), isNull);
      });
    }

    for (final size in const [Size(1024, 600), Size(1280, 400)]) {
      testWidgets('a window the rail fits keeps the rail', (tester) async {
        await mountShell(tester, size);
        expect(
          tester.takeException(),
          isNull,
          reason: 'the shell overflowed at $size',
        );
        expect(find.byType(NavigationRail), findsOneWidget);
        expect(find.byIcon(Icons.menu), findsNothing);
        expect(tester.getSize(find.byType(NavigationRail)).height, size.height);
      });
    }
  });

  group('dialog body', () {
    /// Opens [shell] in a dialog over a 1-button page and pumps it painted.
    Future<void> openPicker(
      WidgetTester tester,
      Size size,
      PickerShell shell,
    ) async {
      useWindow(tester, size);
      await tester.pumpWidget(
        MaterialApp(
          theme: buildTheme(),
          home: Scaffold(
            body: Builder(
              builder: (context) => Center(
                child: ElevatedButton(
                  onPressed: () => showTamuPicker<int>(context, shell),
                  child: const Text('open'),
                ),
              ),
            ),
          ),
        ),
      );
      await tester.tap(find.text('open'));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 400));
    }

    /// Drags the dialog's body to its end and reports where the last row ended
    /// up and where the body box is.
    Future<(Rect, Rect)> scrollToEnd(
      WidgetTester tester,
      String lastRow,
    ) async {
      await tester.drag(find.byType(Scrollable).last, const Offset(0, -4000));
      await tester.pump();
      return (
        tester.getRect(find.text(lastRow)),
        tester.getRect(find.byType(DialogBody)),
      );
    }

    // The card's fixed-height construct. The body fills its box and scrolls
    // itself, so it must lay out at every window size and keep its last row
    // reachable. Pre-fix the box measured 500x158 at 640x360 and 232x260 at
    // 360x480 — smaller than the 500x398 it gets at 1024x600.
    for (final (size, boxWidth, boxHeight) in const [
      (Size(640, 360), 500.0, 158.0),
      (Size(360, 480), 232.0, 260.0),
    ]) {
      testWidgets('PickerShell(500, 400) with a list body at $size', (
        tester,
      ) async {
        await openPicker(
          tester,
          size,
          PickerShell(
            title: 'Pick something',
            maxWidth: 500,
            height: 400,
            body: ListView(
              children: [
                for (var i = 0; i < 20; i++) ListTile(title: Text('item $i')),
              ],
            ),
          ),
        );
        expect(tester.takeException(), isNull);
        final box = tester.getSize(find.byType(DialogBody));
        expect(box.width, boxWidth);
        expect(box.height, boxHeight);

        final (last, body) = await scrollToEnd(tester, 'item 19');
        expect(
          fitsInside(last, body),
          isTrue,
          reason: 'the last row is not inside the ${body.size} body',
        );
      });

      // The same shell with a content-sized body (a form column, as the pickers
      // that draw their own form use). Before the fix this body was laid out at
      // its natural height inside a 158 px box and overflowed by 882 px at
      // 640x360, clipping the rows instead of scrolling them.
      testWidgets('PickerShell(500, 400) with a form body at $size', (
        tester,
      ) async {
        await openPicker(
          tester,
          size,
          PickerShell(
            title: 'Pick something',
            maxWidth: 500,
            height: 400,
            body: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                for (var i = 0; i < 20; i++) ListTile(title: Text('item $i')),
              ],
            ),
          ),
        );
        expect(tester.takeException(), isNull);
        final box = tester.getSize(find.byType(DialogBody));
        expect(box.width, boxWidth);
        expect(
          box.height,
          lessThanOrEqualTo(size.height),
          reason: 'the body box exceeds the window it was given',
        );

        final (last, body) = await scrollToEnd(tester, 'item 19');
        expect(
          fitsInside(last, body),
          isTrue,
          reason: 'the last row is not reachable inside the ${body.size} body',
        );
      });
    }

    // The hint line wraps the body in a Column with an Expanded, so the body has
    // to get the same fit rule inside that wrapper. Pre-fix the Expanded forced
    // a content-sized body to the leftover height and overflowed by 903 px at
    // 640x360 (body box 500x158), leaving the last row unreachable.
    testWidgets('PickerShell with a hint and a form body at Size(640, 360)', (
      tester,
    ) async {
      await openPicker(
        tester,
        const Size(640, 360),
        PickerShell(
          title: 'Pick something',
          maxWidth: 500,
          height: 400,
          hint: 'pick one',
          body: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              for (var i = 0; i < 20; i++) ListTile(title: Text('item $i')),
            ],
          ),
        ),
      );
      expect(tester.takeException(), isNull);
      // The hint stays pinned above the scrolling body.
      expect(find.text('pick one'), findsOneWidget);

      final body = tester.getRect(find.byType(DialogBody));
      expect(
        body.height,
        lessThanOrEqualTo(360),
        reason: 'the body box exceeds the window it was given',
      );

      await tester.drag(find.byType(Scrollable).last, const Offset(0, -4000));
      await tester.pump();
      expect(
        fitsInside(tester.getRect(find.text('item 19')), body),
        isTrue,
        reason: 'the last row is not reachable inside the ${body.size} body',
      );
    });
  });
}
