import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/ui/widgets.dart';

/// Reproduces the symbol-picker dialog structure: AlertDialog with a Row title (Expanded)
/// and a DialogBody(height) wrapping a Column with an Expanded ListView.
void main() {
  testWidgets('symbol picker dialog lays out and settles', (tester) async {
    await tester.pumpWidget(MaterialApp(
      home: Scaffold(
        body: Builder(
          builder: (context) => Center(
            child: ElevatedButton(
              onPressed: () {
                showDialog<Object>(
                  context: context,
                  builder: (_) => AlertDialog(
                    title: Row(children: [
                      IconButton(
                          onPressed: () {},
                          icon: const Icon(Icons.arrow_back, size: 18)),
                      const Expanded(child: Text('Pick operand')),
                    ]),
                    content: DialogBody(
                      maxWidth: 360,
                      height: 380,
                      child: Column(children: [
                        const Padding(
                          padding: EdgeInsets.only(bottom: 4),
                          child: Text('Numeric value'),
                        ),
                        Expanded(
                          child: ListView(children: [
                            for (var i = 0; i < 40; i++) ListTile(title: Text('item $i')),
                          ]),
                        ),
                      ]),
                    ),
                    actions: [
                      TextButton(onPressed: () {}, child: const Text('Cancel')),
                    ],
                  ),
                );
              },
              child: const Text('open'),
            ),
          ),
        ),
      ),
    ));
    await tester.tap(find.text('open'));
    await tester.pumpAndSettle(const Duration(seconds: 5));
    expect(find.text('item 0'), findsOneWidget);
    expect(find.text('Pick operand'), findsOneWidget);
  });
}
