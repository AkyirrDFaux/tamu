import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/notice.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/core/unsaved_settings.dart';
import 'package:tamuapp/ui/notice_host.dart';
import 'package:tamuapp/ui/theme.dart';
import 'package:tamuapp/ui/unsaved_settings_watcher.dart';

/// The shell's app-wide voice for unsaved device settings (`ui/unsaved_settings_watcher.dart`).
///
/// The panel says its own piece while it is on screen; these are the two moments it cannot cover,
/// because they happen with the panel closed: the link going away and the link coming back.
class _FakeLink extends ChangeNotifier {
  bool up = false;

  void drop() {
    up = false;
    notifyListeners();
  }

  void raise() {
    up = true;
    notifyListeners();
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  PendingSetting pending(int field, String name) => PendingSetting(
    deviceId: 1,
    blockName: 'Bluetooth',
    blockType: 0x0B,
    inst: 0,
    field: field,
    fieldName: name,
    meta: const ValueInfo(type: 0, flags: ValueFlags.persistent),
    bytes: const [0],
  );

  Future<UnsavedSettings> pump(
    WidgetTester tester,
    _FakeLink link, {
    UnsavedSettings? store,
  }) async {
    final settings = store ?? UnsavedSettings.forTest();
    // A notice is raised only while a host is mounted, and the host is what draws it.
    NoticeCenter.instance.reset();
    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        builder: noticeHostBuilder,
        home: Scaffold(
          body: UnsavedSettingsWatcher(
            link: link,
            isConnected: () => link.up,
            store: settings,
            child: const Text('the shell'),
          ),
        ),
      ),
    );
    await tester.pumpAndSettle();
    addTearDown(() async {
      await tester.pumpWidget(const SizedBox());
      NoticeCenter.instance.reset();
    });
    return settings;
  }

  testWidgets('it draws the shell it wraps and nothing else', (tester) async {
    await pump(tester, _FakeLink());

    expect(find.text('the shell'), findsOneWidget);
    expect(NoticeCenter.instance.current, isNull);
  });

  testWidgets('a link that returns with unsaved writes says so, naming them', (
    tester,
  ) async {
    final link = _FakeLink();
    final store = await pump(tester, link);
    store.mark(pending(0, 'Enabled'));

    link.raise();
    await tester.pump();

    expect(
      NoticeCenter.instance.current?.message,
      unsavedLinkBackNotice(['Enabled']),
    );
    // The record itself is what warns: the notice is a pointer at it.
    expect(store.isDirty(1), isTrue);
  });

  testWidgets('a link that drops with unsaved writes says so, naming them', (
    tester,
  ) async {
    final link = _FakeLink()..up = true;
    final store = await pump(tester, link);
    store.mark(pending(0, 'Enabled'));
    store.mark(pending(3, 'Display name'));

    link.drop();
    await tester.pump();

    expect(
      NoticeCenter.instance.current?.message,
      unsavedLinkLostNotice(['Enabled', 'Display name']),
    );
  });

  testWidgets('a link change with nothing unsaved is silent', (tester) async {
    final link = _FakeLink();
    await pump(tester, link);

    link.raise();
    await tester.pump();

    expect(NoticeCenter.instance.current, isNull);
  });

  testWidgets(
    'a second notification for the same state is not a second voice',
    (tester) async {
      final link = _FakeLink()..up = true;
      final store = await pump(tester, link);
      store.mark(pending(0, 'Enabled'));
      link.drop();
      await tester.pump();
      final first = NoticeCenter.instance.current;

      link.drop(); // a further event for a link that is already down
      await tester.pump();

      expect(NoticeCenter.instance.current, same(first));
    },
  );
}
