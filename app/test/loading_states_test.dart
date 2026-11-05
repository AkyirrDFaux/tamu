/// Every wait is drawn, named and counted - and every one of them has the same three exits.
///
/// The owner's report (2026-11-05): *"some places just wait and freeze up, some show a blank
/// piece."* The shared layer (`lib/ui/wait.dart`, `lib/ui/loading.dart`) gives a page one wait to
/// drive and three shapes to draw it in; these tests pin both, and then pin the three exits on each
/// screen that reads: it completes (the data is on screen), it fails (a sentence and a retry), or it
/// is stopped where the operation allows it.
library;

import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/fragment_reassembler.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/devices_page.dart';
import 'package:tamuapp/ui/loading.dart';
import 'package:tamuapp/ui/register_page.dart';
import 'package:tamuapp/ui/script_editor_page.dart';
import 'package:tamuapp/ui/scripts_page.dart';
import 'package:tamuapp/ui/storage_page.dart';
import 'package:tamuapp/ui/theme.dart';
import 'package:tamuapp/ui/wait.dart';

/// One 16-byte file-table record: u32 offset, u32 size, 8-byte name.
List<int> _record(int offset, int size, String name) => [
  for (final v in [offset, size]) ...[
    (v & 0xFF),
    ((v >> 8) & 0xFF),
    ((v >> 16) & 0xFF),
    ((v >> 24) & 0xFF),
  ],
  ...name.padRight(8).codeUnits,
];

final List<int> _table = [
  // The table's own record is the FIRST live record (`FileRecord.isFiletable` is `index == 0`),
  // and it holds a real offset like any other file.
  ..._record(0x0100, 16384, '.TABLE'),
  ..._record(0x1000, 64, 'SCR_000'),
];

/// The rig's roster, as the core answers Device CID 0x13: 16 bytes per record
/// (14 serial bytes + the node id, little endian).
List<int> _roster(List<({String serial, int id})> nodes) => [
  for (final node in nodes) ...[
    for (var i = 0; i < 14; i++)
      int.parse(node.serial.substring(i * 2, i * 2 + 2), radix: 16),
    node.id & 0xFF,
    (node.id >> 8) & 0xFF,
  ],
];

/// What a device answers. `null` is "nothing came back" (the request throws), which is the shape a
/// lost reply and a timeout both take at this layer.
enum _Answer { whole, lost }

/// A device for the three read paths these pages use: the file table (Storage CID 5), the loaded
/// script slot map (Script CID 0) and the roster (Device CID 0x13). Everything else answers empty.
class _Device implements TolerantRequestChannel {
  _Device({
    this.table = _Answer.whole,
    this.slots = _Answer.whole,
    this.roster = _Answer.whole,
    this.nodes = const [],
    this.delay = Duration.zero,
    this.script,
  });

  _Answer table;
  _Answer slots;
  _Answer roster;
  List<({String serial, int id})> nodes;
  Duration delay;

  /// The bytes a `SCR_XXX` read answers with, or null for "this device holds no such file".
  List<int>? script;

  int tableSends = 0;

  static const int _rosterCid = 0x13;

  @override
  bool get isConnected => true;

  @override
  int takeTxId() => 0xF700;

  @override
  void setSubscriptionListener(Function(List<int>)? listener) {}

  @override
  Future<List<int>> request(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
    bool requestFrag = false,
    int? transactionId,
  }) async => (await requestTolerant(
    targetId,
    service,
    functionCid,
    payload: payload,
    timeout: timeout,
  )).payload;

  @override
  Future<PacketResponse> requestWithFlags(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
    bool requestFrag = false,
    int? transactionId,
  }) => requestTolerant(
    targetId,
    service,
    functionCid,
    payload: payload,
    timeout: timeout,
  );

  @override
  Future<PacketResponse> requestTolerant(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
  }) async {
    if (delay > Duration.zero) await Future<void>.delayed(delay);
    if (service == ServiceType.storage && functionCid == 5) {
      tableSends++;
      if (table != _Answer.whole) throw StateError('the device did not answer');
      final name = StorageClient.unpadName(payload.sublist(0, 8));
      final bytes = name == '.TABLE'
          ? _table
          : (name == 'SCR_000' ? (script ?? const <int>[]) : const <int>[]);
      if (bytes.isEmpty) {
        // FLAG_FAIL with no payload: the name is not in its file table.
        return const PacketResponse(payload: [], success: false, fail: true);
      }
      final body = [...StorageClient.padName(name), ...bytes];
      return PacketResponse(
        payload: body,
        success: true,
        fail: false,
        chunks: [FragmentChunk(0, body)],
        stopped: true,
        total: 1,
      );
    }
    if (service == ServiceType.script && functionCid == 0) {
      if (slots != _Answer.whole) {
        throw StateError('the slot map did not answer');
      }
      // An empty slot map: every slot free, which is "nothing is loaded".
      return PacketResponse(
        payload: List<int>.filled(32, 0xFF),
        success: true,
        fail: false,
        chunks: [FragmentChunk(0, List<int>.filled(32, 0xFF))],
        stopped: true,
        total: 1,
      );
    }
    if (service == ServiceType.device && functionCid == _rosterCid) {
      if (roster != _Answer.whole) {
        throw StateError('the roster did not answer');
      }
      final body = _roster(nodes);
      return PacketResponse(
        payload: body,
        success: true,
        fail: false,
        chunks: [FragmentChunk(0, body)],
        stopped: true,
        total: 1,
      );
    }
    return const PacketResponse(payload: [], success: true, fail: false);
  }
}

/// A channel that reports fragment arrivals while a read streams, as the app's connection does.
class _ProgressDevice extends _Device implements ProgressReportingChannel {
  _ProgressDevice();

  final _progress = StreamController<ReplyStreamProgress>.broadcast();

  int emitted = 0;

  @override
  Stream<ReplyStreamProgress> get streamProgress => _progress.stream;

  @override
  Future<PacketResponse> requestTolerant(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
  }) async {
    if (service == ServiceType.storage && functionCid == 5) {
      final body = [...StorageClient.padName('.TABLE'), ..._table];
      emitted++;
      _progress.add(
        ReplyStreamProgress(
          deviceId: targetId,
          service: service,
          cid: functionCid,
          txId: 1,
          received: 1,
          total: 4,
        ),
      );
      // Fragments arrive in separate turns of the event loop, not in one synchronous burst - and
      // the read's listener is torn down when the reply completes, so each one is delivered while
      // the read is still in flight, exactly as the link delivers them.
      await Future<void>.delayed(const Duration(milliseconds: 1));
      emitted++;
      _progress.add(
        ReplyStreamProgress(
          deviceId: targetId,
          service: service,
          cid: functionCid,
          txId: 1,
          received: 4,
          total: 4,
        ),
      );
      await Future<void>.delayed(const Duration(milliseconds: 1));
      return PacketResponse(
        payload: body,
        success: true,
        fail: false,
        chunks: [FragmentChunk(0, body)],
        stopped: true,
        total: 1,
      );
    }
    return super.requestTolerant(
      targetId,
      service,
      functionCid,
      payload: payload,
      timeout: timeout,
    );
  }
}

Widget _app(Widget home) => MaterialApp(theme: buildTheme(), home: home);

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('the shared wait', () {
    test('a wait with no total is indeterminate and still names itself', () {
      const wait = WaitProgress('Reading the device roster');
      expect(wait.value, isNull);
      expect(wait.determinate, isFalse);
      expect(wait.counter, isNull);
      expect(wait.label, 'Reading the device roster');
    });

    test('a wait with a total is determinate and counts', () {
      const wait = WaitProgress('Writing item 3 of 8', completed: 3, total: 8);
      expect(wait.value, closeTo(0.375, 1e-9));
      expect(wait.determinate, isTrue);
      expect(wait.counter, '3/8');
      const capped = WaitProgress('done', completed: 9, total: 8);
      expect(capped.value, 1.0, reason: 'the bar never overshoots its track');
    });

    test('a controller steps, ends, and ignores a step after the end', () {
      final controller = WaitController();
      var repaints = 0;
      controller.addListener(() => repaints++);

      controller.begin('Reading SCR_000', total: 4);
      expect(controller.active, isTrue);
      controller.step(completed: 2, note: 'fragment 2 of 4');
      expect(controller.progress!.counter, '2/4');
      expect(controller.progress!.note, 'fragment 2 of 4');

      controller.end();
      expect(controller.active, isFalse);
      final after = repaints;
      // A read that finishes after the page closed its wait must not reopen it.
      controller.step(completed: 4);
      expect(controller.active, isFalse);
      expect(repaints, after);
    });

    test('cancel is sticky for the operation and clears the control', () {
      final controller = WaitController()
        ..begin('Reading', cancelLabel: 'Cancel');
      expect(controller.cancelled, isFalse);
      controller.cancel();
      expect(controller.cancelled, isTrue);
      expect(controller.progress!.cancelling, isTrue);
      expect(
        controller.progress!.cancellable,
        isFalse,
        reason: 'a stopped wait does not offer to stop again',
      );
      controller.end();
      expect(
        controller.cancelled,
        isFalse,
        reason: 'the next wait starts unstoppable',
      );
    });

    testWidgets(
      'a panel draws a labelled, determinate bar and nothing when idle',
      (tester) async {
        final controller = WaitController();
        await tester.pumpWidget(_app(WaitView(controller: controller)));
        expect(find.byType(LinearProgressIndicator), findsNothing);

        controller.begin('Reading the file table', total: 8);
        controller.step(completed: 3);
        await tester.pump();

        expect(find.text('Reading the file table'), findsOneWidget);
        expect(find.text('3/8'), findsOneWidget);
        final bar = tester.widget<LinearProgressIndicator>(
          find.byType(LinearProgressIndicator),
        );
        expect(bar.value, closeTo(0.375, 1e-9));
      },
    );

    testWidgets(
      'a banner without a total draws an indeterminate bar and a cancel',
      (tester) async {
        final controller = WaitController()
          ..begin(
            'Connecting',
            note: 'Establishing session',
            cancelLabel: 'Cancel',
          );
        var cancelled = 0;
        await tester.pumpWidget(
          _app(
            WaitView(
              controller: controller,
              shape: WaitShape.banner,
              onCancel: () => cancelled++,
            ),
          ),
        );
        await tester.pump();

        final bar = tester.widget<LinearProgressIndicator>(
          find.byType(LinearProgressIndicator),
        );
        expect(
          bar.value,
          isNull,
          reason: 'no total means a moving bar, not a fake one',
        );
        expect(find.text('Establishing session'), findsOneWidget);
        await tester.tap(find.widgetWithText(TextButton, 'Cancel'));
        expect(cancelled, 1);
      },
    );
  });

  group('the wait fits both orientations', () {
    // The standing rule: a layout decision never rests on one pixel width. The wait is a compound
    // element with two variants (panel and band), and both must lay out in a portrait phone and a
    // short landscape window.
    for (final size in const [Size(360, 640), Size(800, 360)]) {
      for (final shape in const [WaitShape.panel, WaitShape.banner]) {
        testWidgets('$shape at ${size.width.toInt()}x${size.height.toInt()}', (
          tester,
        ) async {
          tester.view.physicalSize = size;
          tester.view.devicePixelRatio = 1.0;
          addTearDown(tester.view.resetPhysicalSize);
          addTearDown(tester.view.resetDevicePixelRatio);

          await tester.pumpWidget(
            _app(
              Scaffold(
                body: WaitView(
                  progress: const WaitProgress(
                    'Reading the stored backup (16.0 kB)',
                    completed: 12,
                    total: 20,
                    note: 'file table: fragment 12 of 20',
                    cancelLabel: 'Cancel',
                  ),
                  shape: shape,
                  onCancel: () {},
                ),
              ),
            ),
          );
          await tester.pump();

          expect(
            tester.takeException(),
            isNull,
            reason: 'the $shape wait overflowed at $size',
          );
          expect(find.byType(LinearProgressIndicator), findsOneWidget);
        });
      }
    }
  });

  group('the Storage page', () {
    testWidgets('loading names the read, then the list arrives', (
      tester,
    ) async {
      final device = _Device(delay: const Duration(milliseconds: 100));
      installRequestChannel(device);
      addTearDown(() => installRequestChannel(null));

      await tester.pumpWidget(
        _app(const StoragePage(deviceId: 1, isConnected: _isConnected)),
      );
      await tester.pump();

      // Loading: not a blank panel - the operation is named and its bar is up.
      expect(find.text('Reading the file table'), findsOneWidget);
      expect(find.byType(LinearProgressIndicator), findsOneWidget);

      await tester.pump(const Duration(milliseconds: 150));
      await tester.pump();
      expect(find.text('Reading the file table'), findsNothing);
      expect(find.text('SCR_000'), findsOneWidget);
      expect(find.text('No file table'), findsNothing);
    });

    testWidgets('a failed read says why and offers the retry', (tester) async {
      final device = _Device(table: _Answer.lost);
      installRequestChannel(device);
      addTearDown(() => installRequestChannel(null));

      await tester.pumpWidget(
        _app(const StoragePage(deviceId: 1, isConnected: _isConnected)),
      );
      await tester.pump();
      // The reader re-requests the whole file twice (200 ms then 600 ms) before giving up.
      await tester.pump(const Duration(milliseconds: 250));
      await tester.pump(const Duration(milliseconds: 700));

      expect(find.text('The file table could not be read'), findsOneWidget);
      expect(find.text('Try again'), findsOneWidget);
      expect(find.byType(LinearProgressIndicator), findsNothing);
    });

    test(
      'a whole-file read reports fragments received of expected as they land',
      () async {
        final device = _ProgressDevice();
        installRequestChannel(device);
        addTearDown(() => installRequestChannel(null));

        final seen = <(int, int)>[];
        final read = await StorageClient(deviceId: 1).readFileWhole(
          '.TABLE',
          onProgress: (received, total) => seen.add((received, total)),
        );

        expect(read.ok, isTrue);
        expect(device.emitted, 2);
        expect(
          seen,
          [(1, 4), (4, 4)],
          reason:
              'the count is the device\'s declared fragments, as they arrive',
        );
      },
    );

    testWidgets(
      'a file read behind the dialog reports fragments received of expected',
      (tester) async {
        final device = _Device(delay: const Duration(milliseconds: 100));
        installRequestChannel(device);
        addTearDown(() => installRequestChannel(null));

        await tester.pumpWidget(
          _app(const StoragePage(deviceId: 1, isConnected: _isConnected)),
        );
        await tester.pump();
        expect(find.text('Reading the file table'), findsOneWidget);
        await tester.pump(const Duration(milliseconds: 150));
        await tester.pump();
        expect(find.text('SCR_000'), findsOneWidget);

        await tester.tap(find.text('SCR_000'));
        await tester.pump();

        // The modal wait names the file it is reading and offers the way out.
        expect(find.text('Reading SCR_000'), findsOneWidget);
        expect(find.widgetWithText(TextButton, 'Cancel'), findsOneWidget);

        await tester.pump(const Duration(milliseconds: 150));
        await tester.pump();
        await tester.pump();
        expect(find.text('Reading SCR_000'), findsNothing);
      },
    );
  });

  group('the Scripts page', () {
    testWidgets('loading names the list, then an empty list is a device state', (
      tester,
    ) async {
      final device = _Device(
        delay: const Duration(milliseconds: 100),
        slots: _Answer.whole,
      );
      installRequestChannel(device);
      addTearDown(() => installRequestChannel(null));

      await tester.pumpWidget(
        _app(
          ScriptsPage(deviceId: 1, deviceName: 'Core', isConnected: () => true),
        ),
      );
      await tester.pump();

      // The bug this replaces: the page said "No scripts loaded" while the read was in flight.
      expect(find.text('Reading the loaded script list'), findsOneWidget);
      expect(find.text('No scripts loaded'), findsNothing);

      await tester.pump(const Duration(milliseconds: 150));
      await tester.pump(const Duration(milliseconds: 50));

      expect(find.text('No scripts loaded'), findsOneWidget);
      expect(find.text('Reading the loaded script list'), findsNothing);
    });

    testWidgets(
      'a lost slot map is a failure with a retry, never "nothing loaded"',
      (tester) async {
        final device = _Device(slots: _Answer.lost);
        installRequestChannel(device);
        addTearDown(() => installRequestChannel(null));

        await tester.pumpWidget(
          _app(
            ScriptsPage(
              deviceId: 1,
              deviceName: 'Core',
              isConnected: () => true,
            ),
          ),
        );
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 50));

        expect(
          find.text('The loaded scripts could not be read'),
          findsOneWidget,
        );
        expect(find.text('Try again'), findsOneWidget);
        expect(find.text('No scripts loaded'), findsNothing);
      },
    );
  });

  group('the script editor', () {
    testWidgets('loading names the read, then the draft is on screen', (
      tester,
    ) async {
      final image = ScriptFileBuilder(functionName: 'blink').build();
      final device = _Device(
        delay: const Duration(milliseconds: 100),
        script: image,
      );
      installRequestChannel(device);
      addTearDown(() => installRequestChannel(null));

      await tester.pumpWidget(
        _app(
          const ScriptEditorPage(
            deviceId: 1,
            fileId: 0,
            name: 'blink',
            loaded: false,
          ),
        ),
      );
      await tester.pump();

      // The load is one wait from the file table to the decoded draft, and it says which step it
      // is on - "Decoding SCR_000" is the part that used to be a silent gap.
      expect(find.text('Reading the file table'), findsOneWidget);
      expect(find.byType(LinearProgressIndicator), findsOneWidget);

      // The table read, then the script file: the wait names each step while it runs, and the
      // page leaves it only when the draft is built.
      await tester.pump(const Duration(milliseconds: 150));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 150));
      await tester.pump();
      await tester.pump();

      expect(find.text('Reading the file table'), findsNothing);
      expect(find.byType(LinearProgressIndicator), findsNothing);
      expect(find.text('Could not load SCR_000'), findsNothing);
    });

    testWidgets('a failed read is a load error with a retry', (tester) async {
      final device = _Device(table: _Answer.lost);
      installRequestChannel(device);
      addTearDown(() => installRequestChannel(null));

      await tester.pumpWidget(
        _app(
          const ScriptEditorPage(
            deviceId: 1,
            fileId: 0,
            name: 'blink',
            loaded: false,
          ),
        ),
      );
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 250));
      await tester.pump(const Duration(milliseconds: 700));

      expect(find.text('Could not load SCR_000'), findsOneWidget);
      expect(find.widgetWithText(FilledButton, 'Retry'), findsOneWidget);
      expect(find.byType(LinearProgressIndicator), findsNothing);
    });
  });

  group('the Register page', () {
    testWidgets('loading names the tree read, then the tree arrives', (
      tester,
    ) async {
      final client = _TopologyClient(
        deviceId: 1,
        delay: const Duration(milliseconds: 100),
      );
      await tester.pumpWidget(
        _app(
          RegisterPage(
            deviceId: 1,
            isConnected: _isConnected,
            clientFactory: (_) => client,
          ),
        ),
      );
      await tester.pump();

      expect(find.text('Reading the register tree'), findsOneWidget);
      expect(find.byType(LinearProgressIndicator), findsOneWidget);

      await tester.pump(const Duration(milliseconds: 150));
      await tester.pump(const Duration(milliseconds: 50));

      expect(find.text('Reading the register tree'), findsNothing);
      expect(find.text('System'), findsWidgets);

      // The page's 0.5 s auto-refresh is a periodic timer: dispose it before the test ends.
      await tester.pumpWidget(const SizedBox());
      await tester.pump();
    });

    testWidgets('a failed topology read is an error with a retry', (
      tester,
    ) async {
      final client = _TopologyClient(deviceId: 1, fail: true);
      await tester.pumpWidget(
        _app(
          RegisterPage(
            deviceId: 1,
            isConnected: _isConnected,
            clientFactory: (_) => client,
          ),
        ),
      );
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));

      expect(find.text('The register could not be read'), findsOneWidget);
      expect(find.text('Try again'), findsOneWidget);

      await tester.pumpWidget(const SizedBox());
      await tester.pump();
    });
  });

  group('the Devices page', () {
    testWidgets(
      'a sweep in flight names the roster instead of claiming it is empty',
      (tester) async {
        final device = _Device(
          delay: const Duration(milliseconds: 100),
          nodes: const [(serial: 'CDABA26400000000000000000000', id: 2)],
        );
        installRequestChannel(device);
        addTearDown(() => installRequestChannel(null));
        final db = DeviceDatabase.forTest()..linkIsUpOverride = true;

        await tester.pumpWidget(
          _app(DevicesPage(database: db, isConnected: _isConnected)),
        );
        await tester.pump();

        // A sweep in flight is named and barred; the roster on screen stays where it is.
        expect(find.text('Refreshing the device roster'), findsOneWidget);
        expect(find.byType(LinearProgressIndicator), findsOneWidget);
        expect(find.text('No devices discovered yet'), findsNothing);

        await tester.pump(const Duration(seconds: 3));
        await tester.pump(const Duration(milliseconds: 50));

        expect(find.text('Refreshing the device roster'), findsNothing);

        await tester.pumpWidget(const SizedBox());
        await tester.pump();
      },
    );

    testWidgets('a failed roster read is an error with a retry', (
      tester,
    ) async {
      final device = _Device(roster: _Answer.lost);
      installRequestChannel(device);
      addTearDown(() => installRequestChannel(null));
      final db = DeviceDatabase.forTest()..linkIsUpOverride = true;

      await tester.pumpWidget(
        _app(DevicesPage(database: db, isConnected: _isConnected)),
      );
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 800));

      expect(
        find.textContaining('The device roster could not be read'),
        findsOneWidget,
      );
      expect(find.text('Try again'), findsOneWidget);
      expect(find.byType(LinearProgressIndicator), findsNothing);

      await tester.pumpWidget(const SizedBox());
      await tester.pump();
    });
  });
}

bool _isConnected() => true;

/// A Register client that answers the topology the page needs, slowly or not at all.
class _TopologyClient extends RegisterClient {
  _TopologyClient({
    required super.deviceId,
    this.delay = Duration.zero,
    this.fail = false,
  });

  final Duration delay;
  final bool fail;

  @override
  Future<BlocksRead?> readBlocks({List<int>? scriptSlots}) async {
    if (delay > Duration.zero) await Future<void>.delayed(delay);
    if (fail) return null;
    return (
      blocks: <ReportedBlock>[
        (
          type: 0,
          inst: 0,
          meta: const ValueInfo(type: 0, size: 9),
          name: 'System',
        ),
      ],
      unreadTypes: const <int>{},
    );
  }

  @override
  Future<StaticFieldLayout> readStaticFieldLayout() async => const {};
}
