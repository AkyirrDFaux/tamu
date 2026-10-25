import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/script_client.dart';
import 'package:tamuapp/core/script_fault.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/script_widgets.dart';
import 'package:tamuapp/ui/theme.dart';

/// The fault row is what the owner reads when a script faults: the sentence must be on screen
/// verbatim and Copy must put exactly that text on the clipboard.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  /// A `SCRIPT_ERR_TYPE` on a `Register read` of slot 2's input 0, at line 4.
  ScriptFault typeFaultOnRegRead() {
    final blockInfo = makeBlockInfo(
      scriptTypeForIndex(2),
      scriptInstanceForIndex(2),
      ScriptField.input,
      0,
    );
    return ScriptFault.fromReport(
      ScriptReport(
        state: ScriptState.error,
        depth: 0,
        line: 4,
        instruction: 1 + ((catService << 8) | 1),
        error: 2,
      ),
      program: [
        for (var i = 0; i < 4; i++)
          ScriptLine(instruction: ScriptSymbol.instruction(catService, 4)),
        ScriptLine(
          destinations: [ScriptSymbol.variable(0)],
          instruction: ScriptSymbol.instruction(catService, 1),
          operands: [ScriptSymbol.constant(0)],
        ),
      ],
      constantOf: (index) => index == 0 ? blockInfo : null,
    );
  }

  testWidgets('renders the decoded fault and copies it as text', (
    tester,
  ) async {
    const sentence =
        'Script fault at line 4 (Register read): '
        'SCRIPT_ERR_TYPE on Script[2].f1.k0 (Input)';
    final copied = <String>[];
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
      SystemChannels.platform,
      (call) async {
        if (call.method == 'Clipboard.setData') {
          copied.add((call.arguments as Map)['text'] as String);
        }
        return null;
      },
    );
    addTearDown(
      () => tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(
        SystemChannels.platform,
        null,
      ),
    );

    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        home: Scaffold(body: ScriptFaultRow(typeFaultOnRegRead())),
      ),
    );

    expect(tester.takeException(), isNull);
    expect(find.text(sentence), findsOneWidget);

    await tester.tap(find.byTooltip('Copy fault as text'));
    await tester.pump();
    expect(copied, [sentence]);
  });

  testWidgets('an engine-level fault names no line', (tester) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        home: Scaffold(
          body: ScriptFaultRow(
            ScriptFault.fromReport(
              const ScriptReport(
                state: ScriptState.error,
                depth: 0,
                line: 7,
                instruction: 0,
                error: 2,
              ),
            ),
          ),
        ),
      ),
    );
    expect(
      find.text(
        'Script fault (engine-level): SCRIPT_ERR_TYPE - no instruction behind it',
      ),
      findsOneWidget,
    );
  });
}
