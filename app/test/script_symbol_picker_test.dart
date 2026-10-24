import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/script_draft.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/script_symbol_picker.dart';
import 'package:tamuapp/ui/theme.dart';

/// `Log`'s single operand is the log code, a 32-bit integer: the picker must offer
/// integer-typed values (Index/Uint32) and must not offer a Q16.16 Number or a String,
/// whose bits/handle would otherwise go on the wire (E40).
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  String nameOf(ScriptDraft draft, ScriptSymbol s) => switch (s.type) {
    symInput => draft.inputs[s.value].name,
    symOutput => draft.outputs[s.value].name,
    symVariable => draft.variables[s.value].name,
    symConstant => draft.constants[s.value].name,
    symPredefine => 'pre${s.subtype}:${s.value}',
    _ => '?',
  };

  Future<void> pumpPicker(WidgetTester tester, ScriptDraft draft) async {
    final log = scriptInstructions.firstWhere(
      (d) => d.category == catService && d.op == 0,
    );
    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        home: Scaffold(
          body: ScriptSymbolPicker(
            draft: draft,
            destination: false,
            def: log,
            operandIndex: 0,
            deviceId: 1,
            nameOf: nameOf,
          ),
        ),
      ),
    );
    await tester.pump();
  }

  ScriptDraft draftWith() => ScriptDraft(functionName: 'Log')
    ..variables.addAll([
      ScriptDraftValue(name: 'numVar', type: DataType.number),
      ScriptDraftValue(name: 'codeVar', type: DataType.uint32),
      ScriptDraftValue(name: 'textVar', type: DataType.text),
    ])
    ..constants.addAll([
      ScriptDraftValue(name: 'numConst', type: DataType.number),
      ScriptDraftValue(name: 'codeConst', type: DataType.uint32),
    ]);

  testWidgets('Log code recommendations are integers only', (tester) async {
    await pumpPicker(tester, draftWith());
    expect(tester.takeException(), isNull);
    expect(find.text('Log code: an integer (Index/Uint32)'), findsOneWidget);
    // Index/Uint32 values are recommended.
    expect(find.text('codeVar'), findsOneWidget);
    expect(find.text('codeConst'), findsOneWidget);
    // A Q16.16 Number and a String are not.
    expect(find.text('numVar'), findsNothing);
    expect(find.text('textVar'), findsNothing);
    expect(find.text('numConst'), findsNothing);
  });

  testWidgets('Log code group lists filter numbers and strings out', (
    tester,
  ) async {
    await pumpPicker(tester, draftWith());
    await tester.tap(find.text('Variables'));
    await tester.pumpAndSettle();
    expect(find.text('codeVar'), findsOneWidget);
    expect(find.text('numVar'), findsNothing);
    expect(find.text('textVar'), findsNothing);
  });
}
