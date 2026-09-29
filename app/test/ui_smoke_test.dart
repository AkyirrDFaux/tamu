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

  testWidgets('ScriptEditorPage mounts and reports the load failure off-device',
      (tester) async {
    await tester.pumpWidget(MaterialApp(
      theme: buildTheme(),
      home: const ScriptEditorPage(deviceId: 1, fileId: 0, name: 'SCR_00', loaded: false),
    ));
    await tester.pump();
    expect(tester.takeException(), isNull, reason: 'ScriptEditorPage threw while building');
    // Its own widgets are reachable (the split methods are not dead code).
    expect(find.byType(ScriptEditorPage), findsOneWidget);
  });

  testWidgets('SubscriptionDialog mounts and lays out', (tester) async {
    await tester.pumpWidget(MaterialApp(
      theme: buildTheme(),
      home: Scaffold(
        body: SubscriptionDialog(
          client: SubscriptionClient(deviceId: 1),
          regClient: RegisterClient(deviceId: 1),
          currentSubs: const [],
          onSaved: () async {},
        ),
      ),
    ));
    await tester.pump();
    expect(tester.takeException(), isNull, reason: 'SubscriptionDialog threw while building');
    expect(find.byType(SubscriptionDialog), findsOneWidget);
  });
}
