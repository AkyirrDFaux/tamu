// The restore result screen: reasons per item, grouped by device, copyable as text.
//
// Two things are gated here, both structural rather than measured:
//
//   * every failure of the run is on the page under its own device heading, with its kind, its
//     address, the code and what that means - a count alone is what the owner's card is about;
//   * the body is ONE scrolling list, so a portrait phone (360x640) and a landscape one (800x360)
//     both reach every row by scrolling and neither overflows. The sizes are the evidence; the rule
//     is that nothing sits in a fixed row that a narrow window could push past its edge.
//
// The Copy action is measured through the platform clipboard channel, so "the text is copyable"
// is asserted on the bytes that would reach the phone's clipboard, not on the button existing.

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/ui/restore_result.dart';
import 'package:tamuapp/ui/theme.dart';

const Size _portrait = Size(360, 640);
const Size _landscape = Size(800, 360);

RestoreReport _report() => RestoreReport(
  at: DateTime(2026, 10, 25, 19, 30),
  written: 240,
  reasons: const [
    RestoreFailure(
      archive: 'Tamu',
      target: 'Tamu (1.1)',
      targetId: 1025,
      kindWord: 'block field',
      subject: 'PWM output "Fan" #0 / Duty [Key 0] (field 1 key 0)',
      code: 'FAIL (FLAG_FAIL 0x40, no reason payload)',
      meaning:
          'the write did not land, so the device keeps the value it held '
          'for this entry',
    ),
    RestoreFailure(
      archive: 'Tamu',
      target: 'Tamu (1.1)',
      targetId: 1025,
      kindWord: 'script file',
      subject: 'SCR_004 (4 B, slot 4)',
      code:
          'no code from the device (no reply: TimeoutException after 0:00:06)',
      meaning:
          'deleting failed, so that script is still stored and still '
          'loadable',
    ),
    RestoreFailure(
      archive: 'DAS',
      target: 'no target',
      kindWord: 'requester row',
      subject:
          'from 1.3 (Resistive measurement / Measured Value) into '
          'PWM output / Duty',
      code: 'no code from the device (the app refused it: No target device)',
      meaning:
          'the restore refused this row before sending anything, so the '
          'device was never asked for it',
    ),
  ],
);

void main() {
  final copies = <String>[];

  setUp(() {
    copies.clear();
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(SystemChannels.platform, (call) async {
          if (call.method == 'Clipboard.setData') {
            copies.add((call.arguments as Map)['text'] as String);
          }
          return null;
        });
  });

  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(SystemChannels.platform, null);
  });

  Future<void> pumpScreen(WidgetTester tester, Size size) async {
    tester.view.physicalSize = size;
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        home: Scaffold(
          body: Builder(
            builder: (context) => Center(
              child: ElevatedButton(
                onPressed: () => showRestoreResult(context, _report()),
                child: const Text('open'),
              ),
            ),
          ),
        ),
      ),
    );
    await tester.tap(find.text('open'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 300));
  }

  for (final size in const [_portrait, _landscape]) {
    final name = '${size.width.toInt()}x${size.height.toInt()}';

    testWidgets('every reason is on the result screen and reachable at $name', (
      tester,
    ) async {
      await pumpScreen(tester, size);

      expect(tester.takeException(), isNull, reason: 'the dialog overflowed');
      // The counts first, then one section per device the rows were written to.
      expect(
        find.text('240 written · 0 skipped (structural) · 3 failed'),
        findsOneWidget,
      );
      expect(find.textContaining('Tamu (1.1) · 2 failure'), findsOneWidget);
      expect(find.textContaining('no target (from DAS)'), findsOneWidget);
      // The item kind, the address, the code and the sentence - the four things the card asks for.
      expect(find.textContaining('PWM output "Fan"'), findsOneWidget);
      expect(
        find.textContaining('FAIL (FLAG_FAIL 0x40, no reason payload)'),
        findsWidgets,
      );
      expect(
        find.textContaining('the app refused it: No target device'),
        findsOneWidget,
      );

      // The list scrolls: the last row is reachable in a short window.
      final scroller = find
          .descendant(
            of: find.byType(Dialog),
            matching: find.byType(SingleChildScrollView),
          )
          .last;
      await tester.drag(scroller, const Offset(0, -600));
      await tester.pump();
      expect(tester.takeException(), isNull);
      expect(
        find.textContaining('device was never asked for it'),
        findsOneWidget,
      );
    });

    testWidgets('the report is copied as text at $name', (tester) async {
      await pumpScreen(tester, size);

      await tester.tap(find.text('Copy as text'));
      await tester.pump();

      expect(copies, hasLength(1), reason: 'the clipboard got one report');
      final text = copies.single;
      expect(text, startsWith('Restore report 2026-10-25 19:30'));
      expect(text, contains('240 written · 0 skipped (structural) · 3 failed'));
      expect(text, contains('Tamu (1.1) · 2 failures'));
      expect(text, contains('block field  PWM output "Fan" #0 / Duty [Key 0]'));
      expect(text, contains('code: FAIL (FLAG_FAIL 0x40, no reason payload)'));
      expect(
        text,
        contains(
          'no code from the device (the app refused it: '
          'No target device)',
        ),
      );
      // No line is wider than a phone: the report wraps its own rows.
      for (final line in text.split('\n')) {
        expect(
          line.length,
          lessThan(120),
          reason: 'too wide to read on a phone: $line',
        );
      }
    });
  }

  testWidgets('a clean run says so instead of showing an empty list', (
    tester,
  ) async {
    tester.view.physicalSize = _portrait;
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final report = RestoreReport(
      at: DateTime(2026, 10, 25, 19, 30),
      written: 240,
      reasons: const [],
    );
    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        home: Scaffold(body: RestoreResultView(report: report)),
      ),
    );
    await tester.pump();

    expect(tester.takeException(), isNull);
    expect(
      find.text('240 written · 0 skipped (structural) · 0 failed'),
      findsOneWidget,
    );
    expect(find.textContaining('Every selected item landed.'), findsOneWidget);
  });

  // A run whose structural rows are reported, not failed: the count line separates the three
  // numbers and the rows get their own heading, so `0 failed` cannot read as "nothing was left
  // out". The reason is wrapped in the copied text (a phone cannot read a 150-character line).
  testWidgets(
    'structural skips are counted and named, never counted as failures',
    (tester) async {
      tester.view.physicalSize = _portrait;
      tester.view.devicePixelRatio = 1.0;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      final report = RestoreReport(
        at: DateTime(2026, 10, 25, 19, 36),
        written: 602,
        reasons: const [],
        skipped: const [
          RestoreSkip(
            archive: 'Tamu',
            target: 'Tamu (1.1)',
            targetId: 1025,
            kindWord: 'block field',
            subject: 'Dynamic "Left Eye" #1 / Field 4 [Key 0] (field 4 key 0)',
            reason:
                "the device carries the row: key 0 is the field's dictionary "
                'marker, which declares the type and holds no value of its own',
          ),
        ],
      );
      await tester.pumpWidget(
        MaterialApp(
          theme: buildTheme(),
          home: Scaffold(body: RestoreResultView(report: report)),
        ),
      );
      await tester.pump();

      expect(tester.takeException(), isNull);
      expect(
        find.text('602 written · 1 skipped (structural) · 0 failed'),
        findsOneWidget,
      );
      expect(find.text('Skipped as structural (1)'), findsOneWidget);
      expect(find.textContaining('Left Eye'), findsWidgets);

      final text = report.text;
      expect(text, contains('602 written · 1 skipped (structural) · 0 failed'));
      expect(text, contains('Skipped as structural (1)'));
      expect(text, contains('why : the device carries the row: key 0'));
      for (final line in text.split('\n')) {
        expect(
          line.length,
          lessThan(120),
          reason: 'too wide to read on a phone: $line',
        );
      }
      // The skip's kind word is a block field, and it is NOT in the failure vocabulary: the report
      // carries no `FAIL` code for it, because nothing was ever sent.
      expect(text, isNot(contains('FAIL (')));
    },
  );
}
