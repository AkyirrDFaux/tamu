import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/subscription_client.dart';
import 'package:tamuapp/ui/script_editor_page.dart';
import 'package:tamuapp/ui/subscriptions_dialog.dart';
import 'package:tamuapp/ui/theme.dart';

/// Smoke tests for the two views whose bodies were split into part-file extensions
/// (register_page.dart has its own suite). They mount without a device - the point is that
/// every member still resolves and the widgets build, which the analyzer alone cannot prove.
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  testWidgets(
    'ScriptEditorPage mounts and reports the load failure off-device',
    (tester) async {
      await tester.pumpWidget(
        MaterialApp(
          theme: buildTheme(),
          home: const ScriptEditorPage(
            deviceId: 1,
            fileId: 0,
            name: 'SCR_000',
            loaded: false,
          ),
        ),
      );
      await tester.pump();
      expect(
        tester.takeException(),
        isNull,
        reason: 'ScriptEditorPage threw while building',
      );
      // Its own widgets are reachable (the split methods are not dead code).
      expect(find.byType(ScriptEditorPage), findsOneWidget);
    },
  );

  // The failure copy and its Retry belong to the load-error state: the shell draws
  // whatever it is handed for every kind, so passing them unconditionally put
  // "Could not load ..." over the spinner of the loading frame.
  testWidgets(
    'ScriptEditorPage keeps the failure copy out of the loading frame',
    (tester) async {
      await tester.pumpWidget(
        MaterialApp(
          theme: buildTheme(),
          home: const ScriptEditorPage(
            deviceId: 1,
            fileId: 0,
            name: 'SCR_000',
            loaded: false,
          ),
        ),
      );
      // First frame: the read is still in flight.
      expect(find.byType(CircularProgressIndicator), findsOneWidget);
      expect(find.text('Could not load SCR_000'), findsNothing);
      expect(find.widgetWithText(FilledButton, 'Retry'), findsNothing);

      // Once the (off-device) read fails, the error state still shows both.
      await tester.pump();
      expect(find.text('Could not load SCR_000'), findsOneWidget);
      expect(find.widgetWithText(FilledButton, 'Retry'), findsOneWidget);
    },
  );

  testWidgets('SubscriptionDialog mounts and lays out', (tester) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        home: Scaffold(
          body: SubscriptionDialog(
            client: SubscriptionClient(deviceId: 1),
            regClient: RegisterClient(deviceId: 1),
            currentSubs: const [],
            onSaved: () async {},
          ),
        ),
      ),
    );
    await tester.pump();
    expect(
      tester.takeException(),
      isNull,
      reason: 'SubscriptionDialog threw while building',
    );
    expect(find.byType(SubscriptionDialog), findsOneWidget);
  });
}
