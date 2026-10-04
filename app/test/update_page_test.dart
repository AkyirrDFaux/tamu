import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/ui/update_page.dart';

void main() {
  testWidgets('update page lays out and gates the Update button', (tester) async {
    // The page is a tall ListView; give it a surface big enough to build every card.
    tester.view.physicalSize = const Size(1000, 2400);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    await tester.pumpWidget(const MaterialApp(home: UpdatePage()));
    await tester.pumpAndSettle();

    // The guide, target, image and log sections all render.
    expect(find.text('How to update'), findsOneWidget);
    expect(find.text('Target'), findsOneWidget);
    expect(find.text('Image'), findsOneWidget);
    expect(find.text('Log'), findsOneWidget);
    expect(find.text('Choose file'), findsOneWidget);

    // Without a chosen image (and with no link) the Update button is disabled.
    final button = tester.widget<FilledButton>(
        find.widgetWithText(FilledButton, 'Update'));
    expect(button.onPressed, isNull);
  });
}
