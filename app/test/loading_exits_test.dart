/// The exits every wait has, proven on the screens the loading card touched.
///
/// The card's rule (Akyirr, 2026-11-05): a wait either COMPLETES, FAILS with a message, or is
/// CANCELLED where the operation allows it - and a wait that outlives its page must not draw (or
/// assert) after the page is gone. `loading_states_test.dart` pins the shared layer and the pages
/// that read; this file pins the five waits it left untested at the widget layer - the script load
/// modal, the register's stored-backup read, the backup capture, the restore plan build and the
/// restore apply - plus the pop-mid-read case.
///
/// Two of them exist because review round 1 found the waits had no exit at all:
///   * the register Backup view's Retry left "Reading the stored backup" up for ever, because the
///     wait was opened in `_loadBackup` and ended only by `_refreshAll`'s `finally`;
///   * `end()`/`step()` after `dispose()` threw in every debug run that popped a page mid-read,
///     because `ChangeNotifier.notifyListeners` asserts not-disposed.
library;

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/notice.dart';
import 'package:tamuapp/core/request_channel.dart';

import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/backup_page.dart';
import 'package:tamuapp/ui/notice_host.dart';
import 'package:tamuapp/ui/register_page.dart';
import 'package:tamuapp/ui/scripts_page.dart';
import 'package:tamuapp/ui/storage_page.dart';
import 'package:tamuapp/ui/theme.dart';
import 'package:tamuapp/ui/wait.dart';

import 'support/fake_core.dart';
import 'support/read_device.dart';
import 'support/topology_client.dart';

bool _connected() => true;

// The app's own chrome around a page: notices are drawn by the window's strip, so
// a page test that asserts one has to mount it.
Widget _app(Widget home) =>
    MaterialApp(theme: buildTheme(), builder: noticeHostBuilder, home: home);

/// Pumps [steps] frames of [slice] each, so a chain of short awaits (a read per register field, a
/// read per file) can finish without guessing the exact number of turns.
Future<void> _drain(
  WidgetTester tester, {
  int steps = 40,
  Duration slice = const Duration(milliseconds: 50),
}) async {
  for (var i = 0; i < steps; i++) {
    await tester.pump(slice);
  }
}

final List<int> _scriptImage = ScriptFileBuilder(functionName: 'blink').build();

final List<int> _table = [
  ...fileRecord(
    0x0100,
    32,
    '.TABLE',
  ), // its declared size is the 32 bytes the bus returns
  ...fileRecord(0x1000, _scriptImage.length, 'SCR_000'),
];

// --- A restore plan with rows a live device can take -------------------------------------------

BackupEntry _entry(String field, int fieldIndex, Object? value) => BackupEntry(
  field: field,
  fieldIndex: fieldIndex,
  key: 'Key 0',
  keyIndex: 0,
  type: 'Number',
  flags: const ['Persistent'],
  value: value,
  size: 4,
);

LiveEntry _liveEntry(String field, int fieldIndex) => LiveEntry(
  field: fieldIndex,
  key: 0,
  fieldName: field,
  keyName: 'Key 0',
  meta: ValueInfo(type: DataType.number.value, size: 4),
  value: const [0, 0, 0x20, 0x42],
);

/// An archive of [rows] PWM rows, and the live device that can take them: every row is a REPLACE,
/// so every row is READY - which is what puts the Restore button in reach.
RestorePlan _readyPlan({int rows = 1}) {
  final live = LiveDevice(
    id: idFromString('0.1')!,
    name: 'Tamu',
    blocks: [
      LiveBlock(
        type: 22,
        instance: 0,
        name: 'Fan',
        isDynamic: false,
        entries: [for (var i = 0; i < rows; i++) _liveEntry('Duty $i', i)],
      ),
    ],
  );
  return planFromSnapshot(
    devices: [
      BackupDevice(
        type: 'Tamu v2.0A',
        typeId: 1,
        id: idFromString('0.1')!,
        name: 'Tamu',
        blocks: [
          BackupBlock(
            type: 'PWM output',
            typeIndex: 22,
            instance: 0,
            name: 'Fan',
            isDynamic: false,
            entries: [
              for (var i = 0; i < rows; i++) _entry('Duty $i', i, 40.0 + i),
            ],
          ),
        ],
      ),
    ],
    live: [live],
  );
}

/// An archive the Backup page can hand to the real plan build: one device, one row.
BackupDevice _archive() => BackupDevice(
  type: 'Tamu v2.0A',
  typeId: 1,
  id: idFromString('0.1')!,
  name: 'Tamu',
  blocks: [
    BackupBlock(
      type: 'PWM output',
      typeIndex: 22,
      instance: 0,
      name: 'Fan',
      isDynamic: false,
      entries: [_entry('Duty', 0, 40.0)],
    ),
  ],
);

DeviceDatabase _databaseWithDevice() {
  final db = DeviceDatabase.forTest();
  final entry = DeviceEntry(id: 1)
    ..name = 'Tamu'
    ..serialNumber = 'E4B063C820700000000000000000';
  db.seedDeviceForTest(entry);
  return db;
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  // The strip remembers the last occurrence per event; a page that reports one
  // occurrence per action starts its own count, so the memory must not cross tests.
  setUp(NoticeCenter.instance.reset);

  group('a wait whose page is gone', () {
    test('a disposed controller ignores begin, step, cancel and end', () {
      final controller = WaitController()..begin('Reading the file table');
      controller.end();
      controller.dispose();

      // Nothing of this may reach notifyListeners: ChangeNotifier asserts not-disposed in debug.
      controller.step(completed: 3, total: 8);
      controller.cancel();
      controller.end();
      controller.begin('Reading again');
      expect(
        controller.active,
        isFalse,
        reason: 'the read ended while nobody watched',
      );
    });

    testWidgets(
      'popping a Storage page mid-read does not use its wait after dispose',
      (tester) async {
        final bus = ReadDevice(
          table: _table,
          script: _scriptImage,
          delay: const Duration(milliseconds: 100),
        );
        installRequestChannel(bus);
        addTearDown(() => installRequestChannel(null));

        await tester.pumpWidget(
          _app(const StoragePage(deviceId: 1, isConnected: _connected)),
        );
        await tester.pump();
        expect(find.text('Reading the file table'), findsOneWidget);

        // The user walks away while the 16 kB table is still streaming: the read finishes into a
        // disposed controller. Before the fix this threw "A WaitController was used after being
        // disposed" in every debug run.
        await tester.pumpWidget(
          const MaterialApp(home: Scaffold(body: SizedBox())),
        );
        await tester.pump(const Duration(milliseconds: 400));

        expect(tester.takeException(), isNull);
      },
    );
  });

  group('the Scripts page load', () {
    /// The page, with its Stored list read, about to tap `Load` on SCR_000.
    Future<void> openStored(WidgetTester tester, ReadDevice bus) async {
      installRequestChannel(bus);
      addTearDown(() => installRequestChannel(null));
      await tester.pumpWidget(
        _app(
          ScriptsPage(deviceId: 1, deviceName: 'Core', isConnected: () => true),
        ),
      );
      await tester.pump();
      await _drain(tester, steps: 4);
      await tester.tap(find.text('Available'));
      await tester.pump();
      await _drain(tester, steps: 4);
      expect(find.text('SCR_000'), findsOneWidget);
    }

    testWidgets('the load modal names the operation, then the load lands', (
      tester,
    ) async {
      final bus = ReadDevice(
        table: _table,
        script: _scriptImage,
        delay: const Duration(milliseconds: 100),
      );
      await openStored(tester, bus);

      await tester.tap(find.widgetWithText(TextButton, 'Load'));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 150));
      await tester.pump();

      // Loading: the modal says which file goes where, and it is not a blank screen.
      // Drawn twice on purpose: the modal, and the page's own wait band behind it.
      expect(find.text('Loading SCR_000 into slot 0'), findsWidgets);
      expect(find.byType(LinearProgressIndicator), findsWidgets);

      await _drain(tester, steps: 6);
      expect(find.text('Loading SCR_000 into slot 0'), findsNothing);
      expect(find.text('Loaded SCR_000 into slot 0'), findsOneWidget);

      await tester.pumpWidget(const SizedBox());
      await tester.pump();
    });

    testWidgets('a refused load says so instead of leaving the modal up', (
      tester,
    ) async {
      final bus = ReadDevice(
        table: _table,
        script: _scriptImage,
        delay: const Duration(milliseconds: 100),
        loadFails: true,
      );
      await openStored(tester, bus);

      await tester.tap(find.widgetWithText(TextButton, 'Load'));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 150));
      await tester.pump();

      expect(find.text('Loading SCR_000 into slot 0'), findsWidgets);

      await _drain(tester, steps: 6);
      expect(find.text('Loading SCR_000 into slot 0'), findsNothing);
      expect(find.text('Load failed'), findsOneWidget);
      expect(find.byType(LinearProgressIndicator), findsNothing);

      await tester.pumpWidget(const SizedBox());
      await tester.pump();
    });
  });

  group('the Register page stored backup', () {
    Future<void> openBackupView(WidgetTester tester, ReadDevice bus) async {
      installRequestChannel(bus);
      addTearDown(() => installRequestChannel(null));
      final client = TopologyClient(
        deviceId: 1,
        delay: const Duration(milliseconds: 100),
      );
      await tester.pumpWidget(
        _app(
          RegisterPage(
            deviceId: 1,
            isConnected: _connected,
            clientFactory: (_) => client,
          ),
        ),
      );
      await tester.pump();
      await _drain(tester, steps: 8);
      await tester.tap(find.widgetWithText(TextButton, 'Current'));
      await tester.pump();
    }

    testWidgets(
      'the read names what it is doing, then the stored table arrives',
      (tester) async {
        final bus = ReadDevice(
          table: _table,
          script: _scriptImage,
          delay: const Duration(milliseconds: 100),
        );
        await openBackupView(tester, bus);

        expect(find.text('Reading the stored backup'), findsOneWidget);
        expect(find.byType(LinearProgressIndicator), findsOneWidget);

        await _drain(tester, steps: 20);
        expect(find.text('Reading the stored backup'), findsNothing);
        expect(find.text('The stored backup could not be read'), findsNothing);

        await tester.pumpWidget(const SizedBox());
        await tester.pump();
      },
    );

    testWidgets('a failed read says why, and the Retry ENDS its wait', (
      tester,
    ) async {
      final bus = ReadDevice(
        table: _table,
        script: _scriptImage,
        delay: const Duration(milliseconds: 100),
        tableAnswer: ReadAnswer.lost,
      );
      await openBackupView(tester, bus);

      await _drain(tester, steps: 60);
      expect(find.text('The stored backup could not be read'), findsOneWidget);
      expect(find.text('Reading the stored backup'), findsNothing);

      // The reader answers now; the error state's own Retry calls _loadBackup directly, which is
      // the path that used to leave the band up for ever.
      bus.tableAnswer = ReadAnswer.whole;
      await tester.tap(find.widgetWithText(FilledButton, 'Try again'));
      await tester.pump();
      expect(find.text('Reading the stored backup'), findsOneWidget);

      await _drain(tester, steps: 8);
      expect(find.text('Reading the stored backup'), findsNothing);
      expect(find.text('The stored backup could not be read'), findsNothing);

      await tester.pumpWidget(const SizedBox());
      await tester.pump();
    });
  });

  group('the Backup page capture', () {
    testWidgets('the capture counts its devices, then the archive is saved', (
      tester,
    ) async {
      final bus = ReadDevice(
        table: _table,
        script: _scriptImage,
        delay: const Duration(milliseconds: 20),
      );
      ConnectionManager.instance; // installs the app's own channel
      installRequestChannel(bus);
      addTearDown(() => installRequestChannel(null));
      final saved = <String>[];

      await tester.pumpWidget(
        _app(
          BackupPage(
            database: _databaseWithDevice(),
            isConnected: _connected,
            saveArchive: (name, bytes) async {
              saved.add(name);
              return '/tmp/$name';
            },
          ),
        ),
      );
      await tester.pump();
      await tester.tap(find.text('Select all'));
      await tester.pump();
      await tester.tap(find.text('Create backup'));
      await tester.pump();

      // Loading: one device to capture, so the bar is determinate from the first step.
      expect(find.textContaining('Capturing'), findsOneWidget);
      expect(find.byType(LinearProgressIndicator), findsOneWidget);

      await _drain(tester, steps: 60);
      expect(find.textContaining('Capturing'), findsNothing);
      expect(find.textContaining('Backup saved'), findsOneWidget);
      expect(saved, hasLength(1));
    });

    testWidgets('a device that answers nothing is a message, not a stuck bar', (
      tester,
    ) async {
      final bus = ReadDevice(
        table: _table,
        script: _scriptImage,
        delay: const Duration(milliseconds: 20),
        tableAnswer: ReadAnswer.lost,
      );
      ConnectionManager.instance;
      installRequestChannel(bus);
      addTearDown(() => installRequestChannel(null));

      await tester.pumpWidget(
        _app(
          BackupPage(
            database: _databaseWithDevice(),
            isConnected: _connected,
            saveArchive: (name, bytes) async => '/tmp/$name',
          ),
        ),
      );
      await tester.pump();
      await tester.tap(find.text('Select all'));
      await tester.pump();
      await tester.tap(find.text('Create backup'));
      await tester.pump();
      expect(find.textContaining('Capturing'), findsOneWidget);

      await _drain(tester, steps: 60);
      expect(find.textContaining('Capturing'), findsNothing);
      expect(find.text('No device responded'), findsOneWidget);
    });
  });

  group('the restore plan build', () {
    Future<void> tapRestore(WidgetTester tester, BackupPage page) async {
      await tester.pumpWidget(_app(page));
      await tester.pump();
      await tester.tap(find.text('Restore'));
      await tester.pump();
    }

    testWidgets(
      'the build names its phase and its counter, then the plan opens',
      (tester) async {
        final bus = ReadDevice(
          table: _table,
          script: _scriptImage,
          delay: const Duration(milliseconds: 20),
        );
        ConnectionManager.instance;
        installRequestChannel(bus);
        addTearDown(() => installRequestChannel(null));
        final zip = buildBackupZip([_archive()]);

        await tapRestore(
          tester,
          BackupPage(
            database: _databaseWithDevice(),
            isConnected: _connected,
            pickArchive: () async => zip,
          ),
        );

        expect(find.text('Reading the network'), findsWidgets);
        expect(find.byType(LinearProgressIndicator), findsWidgets);

        await _drain(tester, steps: 40);
        expect(find.text('Reading the network'), findsNothing);
        expect(find.text('Restore plan'), findsOneWidget);
      },
    );

    testWidgets('a build that fails says so and closes its dialog', (
      tester,
    ) async {
      ConnectionManager.instance;
      installRequestChannel(ReadDevice(table: _table, script: _scriptImage));
      addTearDown(() => installRequestChannel(null));
      final zip = buildBackupZip([_archive()]);

      await tapRestore(
        tester,
        BackupPage(
          database: _databaseWithDevice(),
          isConnected: _connected,
          pickArchive: () async => zip,
          planBuilder: (devices, onProgress) async {
            throw StateError('the network could not be read');
          },
        ),
      );

      await _drain(tester, steps: 6);
      expect(find.text('Reading the network'), findsNothing);
      expect(
        find.textContaining('Could not build the restore plan'),
        findsOneWidget,
      );
      expect(find.byType(LinearProgressIndicator), findsNothing);
    });
  });

  group('the restore apply', () {
    testWidgets('the run counts items of N and ends with its report', (
      tester,
    ) async {
      final bus = ReadDevice(
        table: _table,
        script: _scriptImage,
        delay: const Duration(milliseconds: 20),
      );
      installRequestChannel(bus);
      addTearDown(() => installRequestChannel(null));

      await tester.pumpWidget(_app(RestorePlanPage(plan: _readyPlan())));
      await tester.pump();
      await tester.tap(find.widgetWithText(FilledButton, 'Restore'));
      await tester.pump();

      // Loading: named, counted, and stoppable - the first step lands before the first frame.
      expect(find.text('Writing item 1 of 1'), findsOneWidget);
      expect(find.text('0/1'), findsOneWidget);
      expect(find.widgetWithText(TextButton, 'Stop'), findsOneWidget);
      final bar = tester.widget<LinearProgressIndicator>(
        find.byType(LinearProgressIndicator),
      );
      expect(
        bar.value,
        isNotNull,
        reason: 'the apply knows how many items it has to write',
      );

      await _drain(tester, steps: 20);
      expect(find.text('Applying the restore plan'), findsNothing);
      expect(
        find.text('Copy as text'),
        findsOneWidget,
        reason: 'the run ended in its report, which is its completion exit',
      );
    });

    testWidgets('Stop ends the run and the report says what was never written', (
      tester,
    ) async {
      final bus = ReadDevice(
        table: _table,
        script: _scriptImage,
        delay: const Duration(milliseconds: 20),
      );
      installRequestChannel(bus);
      addTearDown(() => installRequestChannel(null));

      await tester.pumpWidget(_app(RestorePlanPage(plan: _readyPlan(rows: 6))));
      await tester.pump();
      await tester.tap(find.widgetWithText(FilledButton, 'Restore'));
      await tester.pump();
      expect(find.text('Writing item 1 of 6'), findsOneWidget);

      await tester.tap(find.widgetWithText(TextButton, 'Stop'));
      await tester.pump();
      // The stop closes the wait at once, and the run ends in its report rather than a dead bar.
      expect(find.text('Writing item 1 of 6'), findsNothing);
      expect(find.widgetWithText(TextButton, 'Stop'), findsNothing);

      await _drain(tester, steps: 20);
      expect(
        find.text('Copy as text'),
        findsOneWidget,
        reason: 'the stopped run still reports, which is its cancelled exit',
      );
      // What the stopped run never wrote is `report.notRun`, pinned at the core layer by
      // restore_progress_test.dart ('the two items past the stop are counted, not dropped'); the
      // result screen shows counts and reasons, and carries that sentence in "Copy as text".
    });
  });
}
