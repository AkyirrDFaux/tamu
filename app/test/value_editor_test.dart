import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/script_draft.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/dialogs.dart';
import 'package:tamuapp/ui/script_value_dialog.dart';
import 'package:tamuapp/ui/theme.dart';
import 'package:tamuapp/ui/value_editor.dart';

/// The value editing family's contracts, pinned through the one entry point
/// (`showValueEditor`) after the 1c.3 merge. The shared blocks batch 3 added -
/// `_SizeStepper` (the vector/matrix size control), `_NumberGrid` (the matrix and
/// transform cell grid), `_hexBytes` (the hex fields) - are exercised by the
/// editors that used to carry their own copy, so a regression in any of them
/// shows up here as a semantics change, not as a compile error.
class _Recorder {
  List<int>? bytes;
  ScriptDraftValue? draft;
}

/// Mounts a button that runs [open] and leaves the dialog on screen.
Future<void> _host(
  WidgetTester tester,
  Future<void> Function(BuildContext context) open,
) async {
  await tester.pumpWidget(
    MaterialApp(
      theme: buildTheme(),
      home: Scaffold(
        body: Builder(
          builder: (context) => Center(
            child: ElevatedButton(
              onPressed: () => open(context),
              child: const Text('open'),
            ),
          ),
        ),
      ),
    ),
  );
  await tester.tap(find.text('open'));
  await tester.pumpAndSettle();
}

/// Runs a value editor through the host and returns the recorded result.
Future<void> _openValueEditor(
  WidgetTester tester,
  _Recorder out,
  DataType type,
  List<int> current, {
  FieldInfo? info,
  int? deviceId,
}) => _host(tester, (context) async {
  out.bytes = await showValueEditor(
    context,
    type,
    current,
    info: info,
    deviceId: deviceId,
  );
});

Future<void> _confirm(WidgetTester tester) async {
  await tester.tap(find.widgetWithText(FilledButton, 'OK'));
  await tester.pumpAndSettle();
}

void main() {
  testWidgets('number editor keeps the value it was handed', (tester) async {
    final out = _Recorder();
    await _openValueEditor(
      tester,
      out,
      DataType.number,
      numberToBytes(1.5),
      info: const FieldInfo('Level', unit: 'V', min: 0, max: 10, step: 0.5),
    );

    expect(find.text('Level [V]'), findsOneWidget);
    expect(
      tester.widget<TextField>(find.byType(TextField)).controller!.text,
      '1.50',
    );
    // A ranged Number gets its slider, with both ends printed.
    expect(find.byType(Slider), findsOneWidget);
    expect(find.text('0'), findsOneWidget);
    expect(find.text('10'), findsOneWidget);

    await _confirm(tester);
    expect(out.bytes, numberToBytes(1.5));
  });

  testWidgets('number editor returns the typed value', (tester) async {
    final out = _Recorder();
    await _openValueEditor(tester, out, DataType.number, numberToBytes(1.5));

    expect(find.text('Number'), findsOneWidget);
    await tester.enterText(find.byType(TextField), '2.25');
    await _confirm(tester);

    expect(out.bytes, numberToBytes(2.25));
  });

  testWidgets('bool editor toggles and returns the new state', (tester) async {
    final out = _Recorder();
    await _openValueEditor(tester, out, DataType.bool_, [1]);

    expect(find.text('Bool'), findsOneWidget);
    expect(find.text('true'), findsOneWidget);
    await tester.tap(find.byType(SwitchListTile));
    await tester.pumpAndSettle();
    expect(find.text('false'), findsOneWidget);

    await _confirm(tester);
    expect(out.bytes, [0]);
  });

  testWidgets('colour editor round-trips the RGBA hex field', (tester) async {
    final out = _Recorder();
    await _openValueEditor(tester, out, DataType.colour, [255, 0, 0, 128]);

    expect(find.text('Colour'), findsOneWidget);
    expect(
      tester.widget<TextField>(find.byType(TextField)).controller!.text,
      'ff000080',
    );

    await tester.enterText(find.byType(TextField), '00ff00ff');
    await tester.pumpAndSettle();
    await _confirm(tester);

    expect(out.bytes, [0, 255, 0, 255]);
  });

  testWidgets('vector editor grows through the size stepper', (tester) async {
    final out = _Recorder();
    await _openValueEditor(tester, out, DataType.vector, []);

    expect(find.text('Vector'), findsOneWidget);
    // The stepper's own field plus three components.
    expect(find.byType(TextField), findsNWidgets(4));
    expect(find.text('Size: '), findsOneWidget);

    await tester.tap(find.byIcon(Icons.add_circle_outline));
    await tester.pumpAndSettle();
    expect(find.byType(TextField), findsNWidgets(5));
    expect(
      tester.widget<TextField>(find.byType(TextField).first).controller!.text,
      '4',
    );

    await _confirm(tester);
    expect(out.bytes, List<int>.filled(16, 0));
  });

  testWidgets('matrix editor keeps its header and cells', (tester) async {
    final out = _Recorder();
    final current = <int>[
      2,
      0,
      2,
      0,
      ...numberToBytes(1),
      ...numberToBytes(2),
      ...numberToBytes(3),
      ...numberToBytes(4),
    ];
    await _openValueEditor(tester, out, DataType.matrix, current);

    expect(find.text('Matrix 2x2'), findsOneWidget);
    expect(find.text('Rows: '), findsOneWidget);
    expect(find.text('Cols: '), findsOneWidget);
    // Two stepper fields plus the four cells: the grid is the shared one.
    expect(find.byType(TextField), findsNWidgets(6));

    await _confirm(tester);
    expect(out.bytes, current);
  });

  testWidgets('matrix editor shrinks through either stepper', (tester) async {
    final out = _Recorder();
    await _openValueEditor(tester, out, DataType.matrix, []);

    // A brand-new entry offers the identity-like 3x3.
    expect(find.byType(TextField), findsNWidgets(2 + 9));
    await tester.tap(find.byIcon(Icons.remove_circle_outline).first);
    await tester.pumpAndSettle();

    expect(find.byType(TextField), findsNWidgets(2 + 6));
    await _confirm(tester);

    expect(out.bytes!.length, 4 + 2 * 3 * 4);
    expect(out.bytes!.sublist(0, 4), [2, 0, 3, 0]);
  });

  testWidgets('transform editor keeps Transform/Raw and Reset', (tester) async {
    // A phone-height window: the default 600 px test surface clamps the dialog
    // body below the transform editor's own ~430 px of rows, which clips it (the
    // clamp is pre-existing and unrelated to this batch).
    tester.view.physicalSize = const Size(800, 1000);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.reset);

    final out = _Recorder();
    await _openValueEditor(
      tester,
      out,
      DataType.matrix,
      [],
      info: const FieldInfo('Position', transform: true),
    );

    expect(find.text('Matrix (2x3)'), findsOneWidget);
    expect(find.text('Transform'), findsOneWidget);
    expect(find.text('Offset X'), findsOneWidget);
    expect(find.byType(Checkbox), findsNWidgets(2));

    // The raw mode is the shared number grid, with the element names.
    await tester.tap(find.text('Raw'));
    await tester.pumpAndSettle();
    expect(find.byType(TextField), findsNWidgets(6));
    for (final cell in ['a', 'b', 'tx', 'c', 'd', 'ty']) {
      expect(find.text(cell), findsOneWidget);
    }

    // A sign and a decimal point survive the cell round trip: -2.5 in the `a`
    // cell is Scale X 2.5 with Mirror X set.
    await tester.enterText(find.byType(TextField).first, '-2.5');
    await tester.pumpAndSettle();
    await tester.tap(find.text('Transform'));
    await tester.pumpAndSettle();
    // The transform rows in order: Offset X/Y, Rotation, Scale X/Y, Skew.
    expect(
      tester
          .widgetList<TextField>(find.byType(TextField))
          .elementAt(3)
          .controller!
          .text,
      '2.500',
    );

    await tester.tap(find.text('Raw'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Reset'));
    await tester.pumpAndSettle();
    await _confirm(tester);
    expect(out.bytes, [
      2,
      0,
      3,
      0,
      ...numberToBytes(1),
      ...numberToBytes(0),
      ...numberToBytes(0),
      ...numberToBytes(0),
      ...numberToBytes(1),
      ...numberToBytes(0),
    ]);
  });

  testWidgets('enum picker returns the chosen index', (tester) async {
    final out = _Recorder();
    await _openValueEditor(
      tester,
      out,
      DataType.enum_,
      uint32ToBytes(0),
      info: const FieldInfo('Mode', enumValues: {0: 'Off', 1: 'On'}),
    );

    expect(find.text('Mode'), findsOneWidget);
    await tester.tap(find.text('On'));
    await tester.pumpAndSettle();

    expect(out.bytes, [1, 0, 0, 0]);
  });

  testWidgets('unlabelled enum keeps its numeric entry and byte width', (
    tester,
  ) async {
    final out = _Recorder();
    await _openValueEditor(tester, out, DataType.enum_, [7, 0]);

    expect(find.text('Enum value (number)'), findsOneWidget);
    expect(
      tester.widget<TextField>(find.byType(TextField)).controller!.text,
      '7',
    );

    await tester.enterText(find.byType(TextField), '9');
    await _confirm(tester);

    // The field's own width, not the editor's default.
    expect(out.bytes, [9, 0]);
  });

  testWidgets('serial number editor still validates 28 hex chars', (
    tester,
  ) async {
    final out = _Recorder();
    await _openValueEditor(
      tester,
      out,
      DataType.sn,
      List<int>.filled(14, 0xAB),
    );

    expect(
      tester.widget<TextField>(find.byType(TextField)).controller!.text,
      'AB' * 14,
    );

    // A non-hex entry is refused: the dialog stays open, nothing is returned.
    await tester.enterText(find.byType(TextField), 'G' * 28);
    await _confirm(tester);
    expect(out.bytes, isNull);
    expect(find.text('Serial number (28 hex chars)'), findsOneWidget);

    await tester.enterText(
      find.byType(TextField),
      '000102030405060708090A0B0C0D',
    );
    await _confirm(tester);
    expect(out.bytes, List<int>.generate(14, (i) => i));
  });

  testWidgets('hex fallback reads separated hex bytes', (tester) async {
    final out = _Recorder();
    // A matrix buffer too short to carry its header falls back to the hex field.
    await _openValueEditor(tester, out, DataType.matrix, [1, 2, 3]);

    expect(find.text('Matrix 513x0 (hex)'), findsOneWidget);
    await tester.enterText(find.byType(TextField), 'AB CD EF');
    await tester.pumpAndSettle();
    await _confirm(tester);

    expect(out.bytes, [0xAB, 0xCD, 0xEF]);
  });

  testWidgets('a BlockInfo type outside the picker still renders', (
    tester,
  ) async {
    final out = _Recorder();
    // A script block type is not one of the addressable types the picker offers,
    // so the dropdown has to carry the current value itself instead of asserting.
    final current = uint32ToBytes(
      makeBlockInfo(BlockType.script.value, 1, 2, 3),
    );
    await _openValueEditor(tester, out, DataType.blockInfo, current);

    // The frame is also the guard for the dialog body the dropdown needs: at the
    // 360 px this editor used to bound itself to, the widest label overflowed it
    // by 4.5 px.
    expect(tester.takeException(), isNull);
    expect(find.text('BlockInfo'), findsOneWidget);
    expect(find.text(blockTypeLabel(BlockType.script.value)), findsOneWidget);

    await _confirm(tester);
    expect(out.bytes, current);
  });

  testWidgets('script value dialog edits through the entry point', (
    tester,
  ) async {
    final out = _Recorder();
    final initial = ScriptDraftValue(
      name: 'temp',
      type: DataType.number,
      value: Uint8List.fromList(numberToBytes(1.5)),
      spec: const ScriptInputSpec(
        uiType: ScriptUiType.slider,
        min: 0,
        max: 10,
        step: 0.5,
      ),
    );
    await _host(tester, (context) async {
      out.draft = await showTamuPicker<ScriptDraftValue>(
        context,
        ScriptValueDialog(
          category: ScriptValueCategory.input,
          initial: initial,
          index: 1,
          deviceId: 9,
        ),
      );
    });

    expect(find.text('Input 1'), findsOneWidget);
    expect(
      tester.widget<TextField>(find.byType(TextField).first).controller!.text,
      'temp',
    );
    // The limits print through the same field text they were loaded from.
    for (final label in ['Min', 'Max', 'Step']) {
      expect(find.text(label), findsOneWidget);
    }
    expect(find.text('Value: 1.500'), findsOneWidget);

    // The nested editor is the one entry point, and its result comes back here.
    await tester.tap(find.text('Edit value'));
    await tester.pumpAndSettle();
    // Both dialogs are on the stack now, and both carry a 'Number' ('Type' shows
    // the same word), so the entry point's dialog is the last one in the overlay.
    final nested = find.byType(AlertDialog).last;
    await tester.enterText(
      find.descendant(of: nested, matching: find.byType(TextField)),
      '2.5',
    );
    await tester.tap(
      find.descendant(
        of: nested,
        matching: find.widgetWithText(FilledButton, 'OK'),
      ),
    );
    await tester.pumpAndSettle();
    expect(find.text('Value: 2.500'), findsOneWidget);

    await _confirm(tester);
    expect(out.draft!.name, 'temp');
    expect(out.draft!.type, DataType.number);
    expect(out.draft!.value, numberToBytes(2.5));
    expect(out.draft!.spec.uiType, ScriptUiType.slider);
    expect(out.draft!.spec.max, 10);
    expect(out.draft!.spec.step, 0.5);
  });
}
