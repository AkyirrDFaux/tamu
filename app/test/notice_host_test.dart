/// In-app notices: closable, swipeable, one at a time, and never on top of a control.
///
/// The owner's report (2026-11-05): "the notifications within the app sometimes interfere with
/// buttons and other elements... make them closable". A snack bar is drawn over the page, so a
/// line about one action could cover the control for the next one.
///
/// Measured before, with the same harness this file uses (real `BackupPage`, `tester.view`
/// sized, a snack raised through the old `showSnack`):
///
/// | window  | snack bar rect            | page controls it covered                       |
/// |---------|---------------------------|------------------------------------------------|
/// | 360x640 | (0,506)-(360,564)         | the last two device rows, (0,445)-(360,510) and (0,510)-(360,575) |
/// | 800x360 | (0,230)-(800,288)         | both visible device rows, (0,169)-(800,234) and (0,234)-(800,299) |
///
/// A notice raised from inside a dialog was worse than covered: it was drawn under the dialog
/// route's barrier. Hit-testing the centre of "Failed to save subscription" at 360x640 returned
/// the barrier's `RenderColoredBox`, not the line.
///
/// Now the notice is a strip of its own below the page (`NoticeHost`), so the page ends where the
/// strip begins, nothing is under it, and a dialog cannot cover it. Measured after, same harness:
///
/// | window / page      | before: berthed over the page        | after: its own strip       |
/// |--------------------|--------------------------------------|----------------------------|
/// | 360x640, Backup    | (0,506)-(360,564), 2 rows covered    | (0,576)-(360,640), 64 px, 0 rows covered, the footer moves up to 512 |
/// | 800x360, Backup    | (0,230)-(800,288), 2 rows covered    | (0,304)-(800,360), 56 px, 0 rows covered, the footer moves up to 244 |
/// | 360x640, the shell | over the page body                   | (0,576)-(360,640); 0 of the page's buttons, tiles and fields overlap it |
/// | 800x360, the shell | over the page body                   | (0,304)-(800,360); 0 of the page's buttons, tiles and fields overlap it |
/// | a dialog's failure at 360x640 | (31,586)-(329,616) under the barrier | (46,587)-(300,629) with its close control at (306,584)-(354,632), both tappable while the dialog stays |
///
/// The heights are the two shapes: 64 px for the `card` (a tall window) and 56 px for the
/// `strip` (a short one).
library;

import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart' show connectPairingWindowMessage;
import 'package:tamuapp/core/notice.dart';
import 'package:tamuapp/main.dart';
import 'package:tamuapp/ui/notice_host.dart';
import 'package:tamuapp/ui/theme.dart';

/// Sets the logical window size for one test and restores it afterwards.
void useWindow(WidgetTester tester, Size size) {
  tester.view.physicalSize = size;
  tester.view.devicePixelRatio = 1.0;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
}

/// The app's own chrome around [home]: the notice strip is part of the window, not the page.
Widget app(Widget home) =>
    MaterialApp(theme: buildTheme(), builder: noticeHostBuilder, home: home);

/// A page shaped like the ones this report is about: a scrolling list of rows that carry a
/// control each, and a bottom action bar - the two places a floating notice used to land on.
Widget pageWithControls(void Function(String) onAction) {
  return Scaffold(
    appBar: AppBar(title: const Text('Page')),
    body: Column(
      children: [
        Expanded(
          child: ListView(
            children: [
              for (var i = 0; i < 6; i++)
                ListTile(
                  title: Text('row $i'),
                  trailing: TextButton(
                    onPressed: () => onAction('row $i'),
                    child: const Text('open'),
                  ),
                ),
            ],
          ),
        ),
        Padding(
          padding: const EdgeInsets.all(Spacing.lg),
          child: Row(
            children: [
              Expanded(
                child: FilledButton(
                  onPressed: () => onAction('Save'),
                  child: const Text('Save'),
                ),
              ),
            ],
          ),
        ),
      ],
    ),
  );
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  // The strip remembers the last occurrence per event; a test must not inherit one.
  setUp(NoticeCenter.instance.reset);

  group('one notice, closable', () {
    testWidgets('the close control takes the line away at once', (
      tester,
    ) async {
      final actions = <String>[];
      await tester.pumpWidget(app(pageWithControls(actions.add)));
      await tester.pump();

      showNotice('Restore applied');
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 250));

      expect(find.text('Restore applied'), findsOneWidget);
      expect(find.byType(NoticeBand), findsOneWidget);

      // No timer has run: the control is what takes it away, not the 4 s exit.
      await tester.tap(find.byKey(NoticeBand.closeKey));
      await tester.pump();
      expect(find.text('Restore applied'), findsNothing);
      expect(NoticeCenter.instance.current, isNull);
      expect(actions, isEmpty, reason: 'closing a notice touches nothing else');

      await tester.pumpWidget(const SizedBox());
      await tester.pump();
    });

    testWidgets('a sideways swipe dismisses it', (tester) async {
      await tester.pumpWidget(app(pageWithControls((_) {})));
      await tester.pump();
      showNotice('Restore applied');
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 250));
      expect(find.byType(NoticeBand), findsOneWidget);

      await tester.drag(find.byType(NoticeBand), const Offset(400, 0));
      // The slide, then the strip's own height collapsing: two animations, two pumps, and
      // `pumpAndSettle` is not usable on a page that refreshes.
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      await tester.pump(const Duration(milliseconds: 300));
      await tester.pump(const Duration(milliseconds: 300));

      expect(find.text('Restore applied'), findsNothing);
      expect(NoticeCenter.instance.current, isNull);

      await tester.pumpWidget(const SizedBox());
      await tester.pump();
    });

    testWidgets('its own timer ends it, and the exit is pumped explicitly', (
      tester,
    ) async {
      await tester.pumpWidget(app(pageWithControls((_) {})));
      await tester.pump();
      showNotice('Restore applied');
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 250));
      expect(find.text('Restore applied'), findsOneWidget);

      // Entered, then the strip's own four seconds.
      await tester.pump(AppNotice.defaultDuration);
      await tester.pump();
      expect(NoticeCenter.instance.current, isNull, reason: 'the timer fired');
      // The height animation is what the page reflows through.
      expect(find.text('Restore applied'), findsNothing);

      await tester.pumpWidget(const SizedBox());
      await tester.pump();
    });
  });

  group('the control under a notice', () {
    testWidgets('is still tappable while the notice is up', (tester) async {
      useWindow(tester, const Size(360, 640));
      final actions = <String>[];
      await tester.pumpWidget(app(pageWithControls(actions.add)));
      await tester.pump();

      showNotice('Restore applied');
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 250));

      final band = tester.getRect(find.byType(NoticeBand));
      final page = tester.getRect(find.byType(Scaffold));
      final save = tester.getRect(find.widgetWithText(FilledButton, 'Save'));
      expect(
        band.top,
        greaterThanOrEqualTo(page.bottom),
        reason: 'the page ends where the notice begins',
      );
      expect(
        save.overlaps(band),
        isFalse,
        reason: 'the primary action is not under the notice',
      );

      // A tap at the button's own centre reaches the button, not the strip above it.
      await tester.tap(find.widgetWithText(FilledButton, 'Save'));
      await tester.pump();
      expect(actions, ['Save']);

      await tester.pumpWidget(const SizedBox());
      await tester.pump();
    });

    testWidgets('is reachable on every row of a real page shell', (
      tester,
    ) async {
      for (final size in const [Size(360, 640), Size(800, 360)]) {
        useWindow(tester, size);
        await tester.pumpWidget(app(const ShellPage()));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 100));

        showNotice('Restore applied');
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 250));

        final band = tester.getRect(find.byType(NoticeBand));
        expect(
          band.height,
          lessThan(size.height / 3),
          reason: 'the strip may not eat the page it reflows',
        );
        final controls = <Rect>[
          for (final finder in [
            find.byType(FilledButton),
            find.byType(TextButton),
            find.byType(ListTile),
            find.byType(TextField),
          ])
            for (final element in finder.evaluate())
              tester.getRect(find.byWidget(element.widget)),
        ];
        for (final rect in controls) {
          expect(
            rect.overlaps(band),
            isFalse,
            reason: 'a control at $rect is under the notice $band at $size',
          );
        }
        expect(tester.takeException(), isNull);

        await tester.pumpWidget(const SizedBox());
        await tester.pump();
      }
    });
  });

  group('bounded', () {
    testWidgets('a new notice replaces the one on screen, it does not queue', (
      tester,
    ) async {
      await tester.pumpWidget(app(pageWithControls((_) {})));
      await tester.pump();
      showNotice('first');
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 250));
      expect(find.text('first'), findsOneWidget);

      showNotice('second');
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 250));

      expect(find.text('second'), findsOneWidget);
      expect(find.text('first'), findsNothing);
      expect(find.byType(NoticeBand), findsOneWidget);

      await tester.pumpWidget(const SizedBox());
      await tester.pump();
    });

    testWidgets('the same occurrence reported twice is one notice', (
      tester,
    ) async {
      await tester.pumpWidget(app(pageWithControls((_) {})));
      await tester.pump();

      final first = NoticeCenter.instance.raise(
        event: 'connect',
        sequence: 7,
        message: 'Connection cancelled',
      );
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 250));
      expect(first, isTrue);

      // The listener fires again for the same attempt: same event, same occurrence.
      final second = NoticeCenter.instance.raise(
        event: 'connect',
        sequence: 7,
        message: 'Connection cancelled',
      );
      await tester.pump();

      expect(second, isFalse);
      expect(find.text('Connection cancelled'), findsOneWidget);
      expect(find.byType(NoticeBand), findsOneWidget);

      await tester.pumpWidget(const SizedBox());
      await tester.pump();
    });

    testWidgets('the same text in a new occurrence shows again', (
      tester,
    ) async {
      await tester.pumpWidget(app(pageWithControls((_) {})));
      await tester.pump();

      // Two cancels in a row hand the page the very same `const` notice object; a dedupe
      // on identity would remember the first and drop the second.
      NoticeCenter.instance.raise(
        event: 'connect',
        sequence: 1,
        message: 'Connection cancelled',
      );
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 250));
      await tester.tap(find.byKey(NoticeBand.closeKey));
      await tester.pump();
      expect(find.text('Connection cancelled'), findsNothing);

      final again = NoticeCenter.instance.raise(
        event: 'connect',
        sequence: 2,
        message: 'Connection cancelled',
      );
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 250));

      expect(again, isTrue);
      expect(find.text('Connection cancelled'), findsOneWidget);

      // ...and the occurrence that was already shown is still not shown again.
      await tester.tap(find.byKey(NoticeBand.closeKey));
      await tester.pump();
      expect(
        NoticeCenter.instance.raise(
          event: 'connect',
          sequence: 1,
          message: 'Connection cancelled',
        ),
        isFalse,
        reason: 'a replayed old occurrence is not a new line',
      );

      await tester.pumpWidget(const SizedBox());
      await tester.pump();
    });
  });

  group('shapes', () {
    testWidgets('a short window takes the strip, a tall one the card', (
      tester,
    ) async {
      for (final (size, shape) in const [
        (Size(360, 640), NoticeBandShape.card),
        (Size(800, 360), NoticeBandShape.strip),
      ]) {
        useWindow(tester, size);
        await tester.pumpWidget(app(pageWithControls((_) {})));
        await tester.pump();
        showNotice('Restore applied');
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 250));

        final band = tester.widget<NoticeBand>(find.byType(NoticeBand));
        final resolved =
            band.shape ??
            NoticeBandShape.of(tester.element(find.byType(NoticeBand)));
        expect(resolved, shape, reason: '${size.height} px tall');

        // One API, two explicit variants: what differs is how much of the window the strip
        // takes and how many lines the page's height buys the message.
        final line = tester.widget<Text>(find.text('Restore applied'));
        expect(line.maxLines, NoticeBandShape.maxLines(shape));
        expect(
          tester.getRect(find.byType(NoticeBand)).height,
          lessThanOrEqualTo(shape == NoticeBandShape.strip ? 72 : 96),
          reason: 'the ${shape.name} shape has its own height budget',
        );

        await tester.pumpWidget(const SizedBox());
        await tester.pump();
      }
    });

    testWidgets('the strip is chosen by the window, both ways round', (
      tester,
    ) async {
      useWindow(tester, const Size(800, 360));
      await tester.pumpWidget(
        app(
          Builder(
            builder: (context) => Text(
              NoticeBandShape.of(context).name,
              textDirection: TextDirection.ltr,
            ),
          ),
        ),
      );
      await tester.pump();
      expect(find.text('strip'), findsOneWidget);

      tester.view.physicalSize = const Size(360, 640);
      await tester.pump();
      expect(find.text('card'), findsOneWidget);

      await tester.pumpWidget(const SizedBox());
      await tester.pump();
    });
  });

  testWidgets('a notice raised from a dialog stays visible and closable', (
    tester,
  ) async {
    useWindow(tester, const Size(360, 640));
    late BuildContext dialogContext;
    await tester.pumpWidget(
      app(
        Scaffold(
          body: Center(
            child: ElevatedButton(
              onPressed: () => showDialog<void>(
                context: tester.element(find.byType(ElevatedButton)),
                builder: (context) {
                  dialogContext = context;
                  return AlertDialog(
                    title: const Text('Subscription'),
                    content: TextButton(
                      onPressed: () =>
                          showNotice('Failed to save subscription'),
                      child: const Text('save'),
                    ),
                  );
                },
              ),
              child: const Text('open'),
            ),
          ),
        ),
      ),
    );
    await tester.tap(find.text('open'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 400));
    await tester.tap(find.text('save'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 250));

    // Not behind the barrier (the old snack bar was): the strip is beside the routes.
    expect(find.text('Failed to save subscription'), findsOneWidget);
    expect(
      find.byType(AlertDialog),
      findsOneWidget,
      reason: 'the dialog stays',
    );

    await tester.tap(find.byKey(NoticeBand.closeKey));
    await tester.pump();
    expect(find.text('Failed to save subscription'), findsNothing);
    expect(find.byType(AlertDialog), findsOneWidget);

    // The dialog owns the notice's context, and it is still usable afterwards.
    expect(dialogContext.mounted, isTrue);

    await tester.pumpWidget(const SizedBox());
    await tester.pump();
  });

  testWidgets('a notice that names a step keeps the whole sentence', (
    tester,
  ) async {
    for (final size in const [Size(360, 640), Size(800, 360)]) {
      useWindow(tester, size);
      await tester.pumpWidget(app(pageWithControls((_) {})));
      await tester.pump();

      // The refusal copy names what the user has to do ("open a pairing window from USB").
      // It is guidance, not a title: nothing may shorten it, so it is neither cut down to a
      // headline nor ellipsised by the strip.
      showNotice(connectPairingWindowMessage);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 250));

      expect(find.text(connectPairingWindowMessage), findsOneWidget);
      final line = tester.widget<Text>(find.text(connectPairingWindowMessage));
      expect(line.maxLines, greaterThanOrEqualTo(2));
      final paragraph = tester.renderObject<RenderParagraph>(
        find.text(connectPairingWindowMessage),
      );
      expect(
        paragraph.didExceedMaxLines,
        isFalse,
        reason: 'the step must still be readable at $size',
      );

      await tester.pumpWidget(const SizedBox());
      await tester.pump();
    }
  });

  group('the center', () {
    test('a notice with no host to draw it is not kept for later', () {
      final center = NoticeCenter.forTest();
      expect(center.hosts, 0);
      expect(
        center.raise(event: 'app', message: 'invisible'),
        isFalse,
        reason: 'nothing could have shown it, so it is not a backlog',
      );
      expect(center.current, isNull);

      center.attachHost();
      expect(center.raise(event: 'app', message: 'visible'), isTrue);
      expect(center.current?.message, 'visible');

      // The last host going away drops the line with it.
      center.detachHost();
      expect(center.current, isNull);
    });

    test(
      'dismissing a notice that is no longer current is not a dismissal',
      () {
        final center = NoticeCenter.forTest();
        center.attachHost();
        center.raise(event: 'app', message: 'first');
        final first = center.current!;
        center.raise(event: 'app', message: 'second');
        final second = center.current!;

        center.dismiss(first);
        expect(
          center.current,
          same(second),
          reason: 'the old line is gone already',
        );

        center.dismiss(second);
        expect(center.current, isNull);
        center.detachHost();
      },
    );
  });
}
