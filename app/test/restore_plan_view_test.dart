// The restore plan viewer's size gate (card t_6ffa42c6, `tamu-app-ui` batch discipline).
//
// The page is pumped as the whole window at a portrait phone (360x640) and a landscape one
// (800x360) in EVERY filter mode, and each case asserts the structural rule as well as
// `takeException() == null`:
//
//   * the filter bar is two Wraps - the five modes on one line when the width allows it, moved
//     onto the next line when it does not - so no control is ever pushed past the window it was
//     given;
//   * the rows scroll inside the list, so a short window reaches every one of them;
//   * the empty state of a plan that already matches offers `All`, and a device with no
//     differences says so.
//
// The measurements printed below are the evidence; the rule is the structure, not a pixel width.
//
// The last case is the PROBE: it rebuilds the shape this viewer replaces - one fixed Row holding
// the five mode controls - and shows it overflowing at 360 px, which is what the gate catches.

import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/backup_script.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/backup_page.dart';
import 'package:tamuapp/ui/theme.dart';

const Size _portrait = Size(360, 640);
const Size _landscape = Size(800, 360);
const List<Size> _bothOrientations = [_portrait, _landscape];

String _name(Size size) => '${size.width.toInt()}x${size.height.toInt()}';

void _useWindow(WidgetTester tester, Size size) {
  tester.view.physicalSize = size;
  tester.view.devicePixelRatio = 1.0;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
}

Future<void> _pump(WidgetTester tester, Size size, Widget child) async {
  _useWindow(tester, size);
  await tester.pumpWidget(MaterialApp(theme: buildTheme(), home: child));
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 100));
}

void _report(String line) => debugPrint('[restore plan gate] $line');

BackupEntry _entry(
  String field,
  int fieldIndex,
  Object? value, {
  int size = 4,
}) => BackupEntry(
  field: field,
  fieldIndex: fieldIndex,
  key: 'Key 0',
  keyIndex: 0,
  type: 'Number',
  flags: const ['Persistent'],
  value: value,
  size: size,
);

LiveEntry _liveEntry(
  String field,
  int fieldIndex, {
  List<int> value = const [],
}) => LiveEntry(
  field: fieldIndex,
  key: 0,
  fieldName: field,
  keyName: 'Key 0',
  meta: ValueInfo(type: DataType.number.value, size: 4),
  value: value,
);

BackupScript _script(int slot, String name) => BackupScript(
  slot: slot,
  functionName: name,
  properties: const [],
  inputs: const [],
  outputs: const [],
  variables: const [],
  constants: const [],
  lines: const [],
);

BackupDevice _archive() => BackupDevice(
  type: 'Tamu v2.0A',
  typeId: 1,
  id: idFromString('0.1')!,
  name: 'Tamu',
  blocks: [
    BackupBlock(
      type: 'PWM output',
      typeIndex: BlockType.pwm.value,
      instance: 0,
      name: 'Fan',
      isDynamic: false,
      entries: [
        _entry('Frequency', 0, 25000.0), // same
        _entry('Duty', 1, 40.0), // replace
        _entry('Extra', 2, 7.0), // add
      ],
    ),
  ],
  scripts: [_script(1, 'Blink'), _script(2, 'Fade')],
  files: [
    BackupFile(name: 'DATA', kind: 'Binary', data: base64Encode([1, 2, 3, 4])),
  ],
);

/// A second source device with nothing live to restore into: its rows cannot be placed, which is
/// the "unavailable" count the footer carries.
BackupDevice _orphan() => BackupDevice(
  type: 'DAS v0.1',
  typeId: 2,
  id: idFromString('0.3')!,
  name: 'DAS',
  blocks: [
    BackupBlock(
      type: 'PWM output',
      typeIndex: BlockType.pwm.value,
      instance: 0,
      name: 'Fan',
      isDynamic: false,
      entries: [_entry('Frequency', 0, 1000.0)],
    ),
  ],
);

LiveDevice _live() => LiveDevice(
  id: idFromString('0.1')!,
  name: 'Tamu',
  capabilities:
      Capability.scripts | Capability.subscriptionRequest | Capability.core,
  blocks: [
    LiveBlock(
      type: BlockType.pwm.value,
      instance: 0,
      name: 'Fan',
      isDynamic: false,
      entries: [
        _liveEntry(
          'Frequency',
          0,
          value: resolveEntryBytes(
            _archive().blocks.first.entries.first,
            _liveEntry('Frequency', 0),
          )!,
        ),
        _liveEntry('Duty', 1, value: const [0, 0, 0x20, 0x42]),
      ],
    ),
  ],
);

/// The plan every case here renders: one row of each class, a deletion, and one device that cannot
/// be placed at all.
RestorePlan _plan({bool includeOrphan = true}) {
  final archive = _archive();
  final live = _live();
  final scriptImage = _script(1, 'Blink').toDraft().toImage();
  return planFromSnapshot(
    devices: [archive, if (includeOrphan) _orphan()],
    live: [live],
    inventories: {
      live.id: LiveInventory(
        fileNames: {'SCR_001', 'SCR_002', 'DATA'},
        fileSizes: {'SCR_001': scriptImage.length, 'DATA': 4},
        files: {
          'SCR_001': scriptImage,
          'SCR_002': _script(2, 'Fade').toDraft().toImage(),
          'DATA': const [1, 2, 3, 9],
        },
        loadedSlots: {9: 9},
        requesters: const [],
      ),
    },
  );
}

/// A plan where every row is the same: the empty state's case.
RestorePlan _matchingPlan() {
  final archive = _archive();
  final live = _live();
  return planFromSnapshot(
    devices: [
      BackupDevice(
        type: archive.type,
        typeId: archive.typeId,
        id: archive.id,
        name: archive.name,
        blocks: [
          BackupBlock(
            type: 'PWM output',
            typeIndex: BlockType.pwm.value,
            instance: 0,
            name: 'Fan',
            isDynamic: false,
            entries: [_entry('Frequency', 0, 25000.0)],
          ),
        ],
      ),
    ],
    live: [live],
    inventories: {
      live.id: LiveInventory(fileNames: const {}, fileSizes: const {}),
    },
  );
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('the page in every filter mode', () {
    for (final size in _bothOrientations) {
      testWidgets('mounts and switches modes at ${_name(size)}', (
        tester,
      ) async {
        final plan = _plan();
        await _pump(tester, size, RestorePlanPage(plan: plan));
        expect(
          tester.takeException(),
          isNull,
          reason: 'the restore plan page overflowed at ${_name(size)}',
        );
        expect(find.byType(RestoreFilterBar), findsOneWidget);
        expect(
          find.byType(ChoiceChip),
          findsNWidgets(PlanFilter.values.length),
        );
        expect(
          find.byType(FilterChip),
          findsNWidgets(RestoreFilterBar.toggledKinds.length),
        );

        // Every mode, one after another, on the same page: nothing may overflow, and the bar must
        // stay inside the window it was given.
        for (final mode in PlanFilter.values) {
          await tester.tap(
            find.byType(ChoiceChip).at(PlanFilter.values.indexOf(mode)),
          );
          await tester.pump();
          await tester.pump(const Duration(milliseconds: 50));
          expect(
            tester.takeException(),
            isNull,
            reason: 'the ${mode.name} view overflowed at ${_name(size)}',
          );
          final bar = tester.getRect(find.byType(RestoreFilterBar));
          for (final chip in find.byType(ChoiceChip).evaluate()) {
            final rect = tester.getRect(find.byWidget(chip.widget));
            expect(
              rect.right,
              lessThanOrEqualTo(bar.right + 0.5),
              reason: 'a mode control was pushed past the bar',
            );
          }
          _report(
            '${_name(size)} ${mode.name}: bar='
            '${bar.width.toStringAsFixed(0)}x${bar.height.toStringAsFixed(0)}',
          );
        }
      });
    }
  });

  testWidgets('the bar wraps instead of overrunning a narrow window', (
    tester,
  ) async {
    await _pump(tester, _portrait, RestorePlanPage(plan: _plan()));
    expect(tester.takeException(), isNull);

    final chips = [
      for (var i = 0; i < PlanFilter.values.length; i++)
        tester.getRect(find.byType(ChoiceChip).at(i)),
    ];
    final rows = <double>[];
    for (final rect in chips) {
      if (!rows.any((dy) => (dy - rect.center.dy).abs() < 1)) {
        rows.add(rect.center.dy);
      }
    }
    _report(
      'portrait: ${chips.length} modes on ${rows.length} line(s), '
      'widths ${[for (final c in chips) c.width.toStringAsFixed(0)]}, '
      'last chip right=${chips.last.right.toStringAsFixed(0)}',
    );
    // The structural rule: nothing leaves the window, and the modes that do not fit the first
    // line are moved DOWN rather than out.
    expect(chips.last.right, lessThanOrEqualTo(_portrait.width));
    expect(rows.length, greaterThan(1));
  });

  group('what each view shows', () {
    testWidgets('Diff hides the same rows, and All brings them back', (
      tester,
    ) async {
      await _pump(tester, _landscape, RestorePlanPage(plan: _plan()));
      expect(tester.takeException(), isNull);
      // Default: diff. The same row is not on the page; the add/replace/delete are.
      expect(find.text('SAME'), findsNothing);
      expect(find.text('REPLACE'), findsWidgets);
      expect(find.text('DELETE'), findsWidgets);
      expect(find.text('ADD'), findsWidgets);

      await tester.tap(find.byType(ChoiceChip).at(PlanFilter.all.index));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));
      expect(tester.takeException(), isNull);
      expect(find.text('SAME'), findsWidgets);
      _report('all view at ${_name(_landscape)}: SAME rows on the page');
    });

    testWidgets('the delete view lists the deletion and its footer count', (
      tester,
    ) async {
      await _pump(tester, _landscape, RestorePlanPage(plan: _plan()));
      await tester.tap(find.byType(ChoiceChip).at(PlanFilter.remove.index));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));
      expect(tester.takeException(), isNull);
      expect(find.text('Delete from the device'), findsOneWidget);
      expect(find.textContaining('Script slot 9 (SCR_009)'), findsOneWidget);
      expect(find.text('1 of 1 to delete'), findsOneWidget);
      _report('delete view: footer reads "1 of 1 to delete"');
    });

    testWidgets('a class toggle unticks exactly that class', (tester) async {
      await _pump(tester, _landscape, RestorePlanPage(plan: _plan()));
      await tester.tap(find.byType(ChoiceChip).at(PlanFilter.remove.index));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));

      // The `Deletes` toggle is the third class chip.
      await tester.tap(find.byType(FilterChip).at(2));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));
      expect(tester.takeException(), isNull);
      final row = tester.widget<CheckboxListTile>(
        find.widgetWithText(CheckboxListTile, 'Script slot 9 (SCR_009)'),
      );
      expect(row.value, isFalse, reason: 'the class toggle cleared the box');
      expect(find.text('0 of 1 to delete'), findsOneWidget);
      _report('class toggle: the delete row is unticked, footer follows');
    });

    testWidgets('a device with no differences says so under Diff', (
      tester,
    ) async {
      // Two source devices onto one target: the first has differences, the second already
      // matches. The matching one keeps its section and says why instead of vanishing.
      final live = _live();
      final matching = BackupDevice(
        type: 'DAS v0.1',
        typeId: 2,
        id: idFromString('0.3')!,
        name: 'DAS',
        blocks: [
          BackupBlock(
            type: 'PWM output',
            typeIndex: BlockType.pwm.value,
            instance: 0,
            name: 'Fan',
            isDynamic: false,
            entries: [_entry('Frequency', 0, 25000.0)],
          ),
        ],
      );
      final plan = planFromSnapshot(
        devices: [_archive(), matching],
        live: [live],
        inventories: {
          live.id: LiveInventory(
            fileNames: {'SCR_001', 'SCR_002', 'DATA'},
            fileSizes: {'SCR_001': 0, 'DATA': 4},
            files: {
              'DATA': const [1, 2, 3, 4],
            },
          ),
        },
      );
      await _pump(tester, _landscape, RestorePlanPage(plan: plan));
      expect(tester.takeException(), isNull);
      // The matching device is the second section, below the fold on a 360 px tall window: the
      // list scrolls, which is what reaches it.
      await tester.drag(find.byType(ListView), const Offset(0, -400));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));
      expect(tester.takeException(), isNull);
      expect(
        find.textContaining('already matches the archive'),
        findsOneWidget,
        reason: 'the matching device section says it has no differences',
      );
      _report('a matching device prints "already matches the archive"');
    });

    testWidgets('a plan with nothing to change offers All', (tester) async {
      await _pump(tester, _portrait, RestorePlanPage(plan: _matchingPlan()));
      expect(tester.takeException(), isNull);
      expect(find.text('No differences'), findsOneWidget);
      expect(find.text('Show all items'), findsOneWidget);

      await tester.tap(find.text('Show all items'));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));
      expect(tester.takeException(), isNull);
      expect(find.text('No differences'), findsNothing);
      expect(find.text('SAME'), findsWidgets);
      _report('the empty diff state offers All, and All shows the same rows');
    });
  });

  group('the probe: the shape this replaces', () {
    testWidgets('a fixed Row of the five modes overflows a 360 px window', (
      tester,
    ) async {
      // The old shape: the controls in one Row, no wrapping. This is what the gate catches - the
      // Wrap is not a preference, it is the difference between a readable bar and a clipped one.
      await _pump(
        tester,
        _portrait,
        Scaffold(
          body: Row(
            children: [
              for (final mode in PlanFilter.values)
                ChoiceChip(
                  label: Text('${mode.label} 12'),
                  selected: false,
                  onSelected: (_) {},
                ),
            ],
          ),
        ),
      );
      final error = tester.takeException();
      _report(
        'probe at ${_name(_portrait)}: ${error is FlutterError ? error.toString().split('\n').first : error}',
      );
      expect(
        error,
        isA<FlutterError>(),
        reason: 'the un-wrapped row must overflow, or the gate tests nothing',
      );
    });

    testWidgets('the wrapped bar fits the same window', (tester) async {
      await _pump(
        tester,
        _portrait,
        Scaffold(
          body: RestoreFilterBar(
            plan: _plan(includeOrphan: false),
            filter: PlanFilter.diff,
            onFilter: (_) {},
            onClass: (_, _) {},
          ),
        ),
      );
      expect(tester.takeException(), isNull);
      expect(find.byType(ChoiceChip), findsNWidgets(PlanFilter.values.length));
      _report('the same controls fit at ${_name(_portrait)} once wrapped');
    });
  });
}
