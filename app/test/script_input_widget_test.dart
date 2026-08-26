import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/script_input_widget.dart';

void main() {
  group('switch', () {
    Widget switchHarness(
        {List<int>? live, Future<void> Function(List<int>)? onWrite}) =>
        MaterialApp(
          home: Scaffold(
            body: ScriptInputControl(
              index: 0,
              input: ScriptInput(
                key: 1,
                flagsAndType: DataType.bool_.value,
                defaultValue: [0],
                style: InputStyle.switch_,
              ),
              liveValue: live,
              onWrite: onWrite ?? (_) async {},
            ),
          ),
        );

    testWidgets('toggling the switch commits the new value', (tester) async {
      final writes = <List<int>>[];
      await tester.pumpWidget(switchHarness(onWrite: (v) async { writes.add(v); }));

      final sw = find.byType(Switch);
      expect(tester.widget<Switch>(sw).value, isFalse);
      await tester.tap(sw);
      await tester.pump();
      expect(writes.single, [1]);
    });

    testWidgets(
        'switch keeps the toggled position when the live value lags (no return)',
        (tester) async {
      final writes = <List<int>>[];
      await tester.pumpWidget(switchHarness(onWrite: (v) async { writes.add(v); }));
      final sw = find.byType(Switch);
      await tester.tap(sw);
      await tester.pump();
      expect(tester.widget<Switch>(sw).value, isTrue,
          reason: 'switch must stay on after toggling');

      // A poll rebuild arrives with the stale (default) live value before the
      // device echoes - the switch must not snap back.
      await tester.pumpWidget(switchHarness(live: [0]));
      await tester.pump();
      expect(tester.widget<Switch>(sw).value, isTrue,
          reason: 'switch must not return to off while the write is un-echoed');

      // Once the device echoes the value, the switch tracks the live value.
      await tester.pumpWidget(switchHarness(live: [1]));
      await tester.pump();
      expect(tester.widget<Switch>(sw).value, isTrue);
    });
  });

  testWidgets('slider input commits on drag release', (tester) async {
    final commits = <double>[];
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: ScriptInputControl(
          index: 0,
          input: ScriptInput(
            key: 1,
            flagsAndType: DataType.number.value,
            defaultValue: [0, 0, 0, 0],
            style: InputStyle.slider,
          ),
          onWrite: (value) async {
            commits.add(numberFromBytes(value));
          },
        ),
      ),
    ));

    final slider = find.byType(Slider);
    expect(slider, findsOneWidget);

    // Drag toward max and release; the value should be committed.
    await tester.drag(slider, const Offset(120, 0));
    await tester.pumpAndSettle();

    expect(commits, isNotEmpty, reason: 'drag release should commit a value');
    expect(commits.last, greaterThan(0));
  });

  testWidgets('slider thumb follows the drag before release', (tester) async {
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: ScriptInputControl(
          index: 0,
          input: ScriptInput(
            key: 1,
            flagsAndType: DataType.number.value,
            defaultValue: [0, 0, 0, 0],
            style: InputStyle.slider,
          ),
          onWrite: (_) async {},
        ),
      ),
    ));

    final slider = find.byType(Slider);
    final start = tester.widget<Slider>(slider).value;
    // Move the pointer onto the slider and drag a little without releasing.
    final gesture = await tester.startGesture(tester.getCenter(slider));
    await gesture.moveBy(const Offset(80, 0));
    await tester.pump();
    final mid = tester.widget<Slider>(slider).value;
    await gesture.up();
    await tester.pump();

    // The thumb (the Slider's value prop) moved with the drag before release.
    expect(mid, greaterThan(start),
        reason: 'thumb should follow the drag');
  });

  testWidgets('slider survives poll rebuilds inside a scrollable', (tester) async {
    // Faithful to the script list/editor: the control sits in a ListView and the
    // parent is rebuilt every tick with the (still stale) live value - like the 1 s
    // live poll. The drag must keep working and the thumb must stay put.
    double live = 0;
    var ticks = 0;
    Widget buildIt() => MaterialApp(
          home: Scaffold(
            body: StatefulBuilder(
              builder: (context, setState) {
                final liveBytes = ticks == 0 ? null : numberToBytes(live);
                return ListView(
                  children: [
                    ScriptInputControl(
                      index: 0,
                      input: ScriptInput(
                        key: 1,
                        flagsAndType: DataType.number.value,
                        defaultValue: [0, 0, 0, 0],
                        style: InputStyle.slider,
                      ),
                      liveValue: liveBytes,
                      onWrite: (value) async {
                        live = numberFromBytes(value);
                      },
                    ),
                  ],
                );
              },
            ),
          ),
        );
    await tester.pumpWidget(buildIt());
    final slider = find.byType(Slider);
    final start = tester.widget<Slider>(slider).value;

    // Drag halfway, then have the poll rebuild the parent a few times mid-drag.
    final gesture = await tester.startGesture(tester.getCenter(slider));
    await gesture.moveBy(const Offset(60, 0));
    await tester.pump();
    ticks++;
    await tester.pumpWidget(buildIt()); // poll rebuild during the drag
    await tester.pump();
    final midDrag = tester.widget<Slider>(slider).value;
    expect(midDrag, greaterThan(start),
        reason: 'thumb must follow the drag despite poll rebuilds');

    // Release and let the write settle, then more poll rebuilds.
    await gesture.up();
    await tester.pump();
    for (var i = 0; i < 3; i++) {
      ticks++;
      await tester.pumpWidget(buildIt());
      await tester.pump();
    }
    final settled = tester.widget<Slider>(slider).value;
    expect(settled, greaterThan(start),
        reason: 'thumb must not return after release');
  });

  testWidgets('slider keeps the committed value when the live value lags',
      (tester) async {
    final commits = <double>[];
    Widget build({List<int>? live}) => MaterialApp(
          home: Scaffold(
            body: ScriptInputControl(
              index: 0,
              input: ScriptInput(
                key: 1,
                flagsAndType: DataType.number.value,
                defaultValue: [0, 0, 0, 0],
                style: InputStyle.slider,
              ),
              liveValue: live,
              onWrite: (value) async {
                commits.add(numberFromBytes(value));
              },
            ),
          ),
        );
    await tester.pumpWidget(build());
    await tester.drag(find.byType(Slider), const Offset(120, 0));
    await tester.pump();
    final committed = commits.last;
    expect(committed, greaterThan(0));

    // A poll rebuild arrives with a stale (default) live value before the device
    // echoes the write - the thumb must stay where the user put it.
    await tester.pumpWidget(build(live: [0, 0, 0, 0]));
    await tester.pump();
    expect(tester.widget<Slider>(find.byType(Slider)).value,
        closeTo(committed, 0.001),
        reason: 'thumb must not snap back while the write is un-echoed');

    // Once the device echoes the committed value, the thumb tracks it again.
    await tester.pumpWidget(build(live: numberToBytes(committed)));
    await tester.pump();
    expect(tester.widget<Slider>(find.byType(Slider)).value,
        closeTo(committed, 0.001));
  });
}