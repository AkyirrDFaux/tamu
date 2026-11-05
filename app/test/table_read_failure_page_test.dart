/// A failed table read is never rendered as an empty list (Akyirr, 2026-11-05).
///
/// The owner's rule - "the app must process it correctly no matter what (re-request if something
/// fails, detect it), never present a failure as data" - applied to the two screens where an empty
/// list is the most misleading thing the app can draw: an empty Scripts list or an empty Register
/// backup table reads as a DEVICE STATE ("nothing is stored"), not as an error.
///
/// Both pages read the table through the one shared reader (`StorageClient.readFileTableResult`),
/// which keeps the three cases apart:
///
///   * absent  - the device answered FAIL: it holds no `.TABLE` file. A legitimate empty list.
///   * partial - the reply lost a fragment / its STOP frame: an error naming the hole, re-requested
///               `StorageClient.readAttempts` times, never decoded.
///   * failed  - nothing usable at all (no link, no answer): an error with the reason.
///
/// The fakes below are transports, not clients: the pages build their own `StorageClient`, which
/// asks whatever channel is installed, exactly as it does on the app's live link.
library;

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/fragment_reassembler.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/register_page.dart';
import 'package:tamuapp/ui/scripts_page.dart';
import 'package:tamuapp/ui/theme.dart';

/// What the fake device answers for a `.TABLE` read.
enum _Mode {
  /// FLAG_FAIL with no payload: the name is not in its file table (absent).
  absent,

  /// Nothing comes back at all (the request throws).
  lost,

  /// One fragment with a hole in the stream (partial).
  hole,

  /// The whole table, one fragment.
  whole,
}

/// A device that answers the file table from [_Mode], and everything else (the Script slot map)
/// with an empty success - the pages' other reads are not what these tests are about.
class _TableDevice implements TolerantRequestChannel {
  _TableDevice(this.mode, this.tableBytes);

  _Mode mode;
  final List<int> tableBytes;

  /// How many `.TABLE` reads the client sent (the retry policy's sends included).
  int tableSends = 0;

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
    if (service != ServiceType.storage || functionCid != 5) {
      return PacketResponse(payload: const [], success: true, fail: false);
    }
    tableSends++;
    switch (mode) {
      case _Mode.absent:
        // The firmware's answer for a name its file table does not carry: FLAG_FAIL, no payload.
        return PacketResponse(payload: const [], success: false, fail: true);
      case _Mode.lost:
        throw StateError('the device did not answer');
      case _Mode.hole:
        final echo = StorageClient.padName('.TABLE');
        final half = tableBytes.length ~/ 2;
        return PacketResponse(
          payload: [...echo, ...tableBytes.sublist(0, half)],
          success: true,
          fail: false,
          chunks: [
            FragmentChunk(0, [...echo, ...tableBytes.sublist(0, half)]),
          ],
          missing: const [1],
          stopped: true,
          total: 2,
        );
      case _Mode.whole:
        final echo = StorageClient.padName('.TABLE');
        return PacketResponse(
          payload: [...echo, ...tableBytes],
          success: true,
          fail: false,
          chunks: [
            FragmentChunk(0, [...echo, ...tableBytes]),
          ],
          stopped: true,
          total: 1,
        );
    }
  }
}

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

/// A Register client that only needs to report the topology: the Backup view decodes the stored
/// `.SV` against it.
class _TopologyClient extends RegisterClient {
  _TopologyClient({required super.deviceId});

  @override
  Future<BlocksRead?> readBlocks({List<int>? scriptSlots}) async => (
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

  @override
  Future<StaticFieldLayout> readStaticFieldLayout() async => const {};
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  final scriptTable = <int>[
    ..._record(0x1000, 200, 'SCR_000'),
    ..._record(0x2000, 300, 'SCR_001'),
  ];

  /// The table the Register Backup view decodes: no `.SV`, so every row is "not backed up" -
  /// which is the legitimate empty backup this test contrasts with a failed read.
  final backupTable = <int>[..._record(0x1000, 16384, '.TABLE')];

  Future<void> pumpScripts(WidgetTester tester) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        home: const ScriptsPage(deviceId: 1, deviceName: 'Core'),
      ),
    );
    await tester
        .pump(); // initState's Loaded read (a legitimately empty slot map)
    await tester.pump(const Duration(milliseconds: 50));
  }

  /// Switches the Scripts page to the Available view, which reads the file table.
  Future<void> showAvailable(WidgetTester tester) async {
    await tester.tap(find.text('Available'));
    await tester.pump();
  }

  group('the Scripts page, Available view', () {
    testWidgets('a failed table read shows the error and its retry, then the '
        'real list', (tester) async {
      final device = _TableDevice(_Mode.lost, scriptTable);
      installRequestChannel(device);
      addTearDown(() => installRequestChannel(null));
      await pumpScripts(tester);
      await showAvailable(tester);

      // The reader re-requests the whole file twice (200 ms then 600 ms) and then gives up.
      await tester.pump(const Duration(milliseconds: 250));
      await tester.pump(const Duration(milliseconds: 700));

      expect(
        device.tableSends,
        StorageClient.readAttempts,
        reason: 'a failed read is bounded by the shared re-request policy',
      );
      expect(
        find.text('The stored script list could not be read'),
        findsOneWidget,
        reason: 'the failure is shown, not rendered as an empty list',
      );
      expect(find.textContaining('asked 3 time(s)'), findsOneWidget);
      expect(find.text('No stored scripts'), findsNothing);
      expect(find.text('SCR_000'), findsNothing);
      expect(find.text('Try again'), findsOneWidget);

      // The retry re-reads, and this time the device answers whole.
      device.mode = _Mode.whole;
      await tester.tap(find.text('Try again'));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));

      expect(find.text('SCR_000'), findsOneWidget);
      expect(find.text('SCR_001'), findsOneWidget);
      expect(
        find.text('The stored script list could not be read'),
        findsNothing,
      );
    });

    testWidgets('a table the device does not hold is an empty list, not an '
        'error', (tester) async {
      final device = _TableDevice(_Mode.absent, scriptTable);
      installRequestChannel(device);
      addTearDown(() => installRequestChannel(null));
      await pumpScripts(tester);
      await showAvailable(tester);
      await tester.pump(const Duration(milliseconds: 50));

      expect(
        device.tableSends,
        1,
        reason: 'an absent file is answered once - there is nothing to retry',
      );
      expect(find.text('No stored scripts'), findsOneWidget);
      expect(find.textContaining('holds no file table'), findsOneWidget);
      expect(find.text('Try again'), findsNothing);
      expect(
        find.text('The stored script list could not be read'),
        findsNothing,
      );
    });

    testWidgets('a partial table reply is an error naming the hole, never a '
        'short list', (tester) async {
      // Both records sit past the hole, so a decoded prefix would have shown an EMPTY list -
      // exactly the "there is nothing here" lie this card removes.
      final device = _TableDevice(_Mode.hole, scriptTable);
      installRequestChannel(device);
      addTearDown(() => installRequestChannel(null));
      await pumpScripts(tester);
      await showAvailable(tester);
      await tester.pump(const Duration(milliseconds: 250));
      await tester.pump(const Duration(milliseconds: 700));

      expect(
        find.text('The stored script list could not be read'),
        findsOneWidget,
      );
      expect(
        find.textContaining('fragment 1 of 2 never arrived'),
        findsOneWidget,
        reason: 'the reason names the fragment that never arrived',
      );
      expect(find.text('SCR_000'), findsNothing);
      expect(find.text('SCR_001'), findsNothing);
      expect(find.text('Try again'), findsOneWidget);
    });
  });

  group('the Register page, Backup view', () {
    /// Mounts the page (Current view) and switches it to Backup, whose rows are the stored data.
    Future<void> pumpBackup(WidgetTester tester) async {
      final client = _TopologyClient(deviceId: 1);
      await tester.pumpWidget(
        MaterialApp(
          theme: buildTheme(),
          home: RegisterPage(
            deviceId: 1,
            isConnected: () => true,
            clientFactory: (_) => client,
          ),
        ),
      );
      await tester.pump(); // the initial topology read
      await tester.pump(const Duration(milliseconds: 50));
      await tester.tap(find.text('Current'));
      await tester.pump();
    }

    Future<void> unmount(WidgetTester tester) async {
      // The page's 0.5 s auto-refresh is a periodic timer: dispose it before the test ends.
      await tester.pumpWidget(const SizedBox());
      await tester.pump();
    }

    testWidgets('a failed table read shows the error and its retry, then the '
        'decoded backup', (tester) async {
      final device = _TableDevice(_Mode.lost, backupTable);
      installRequestChannel(device);
      addTearDown(() => installRequestChannel(null));
      await pumpBackup(tester);

      // Three sends, 200 ms then 600 ms apart. The pumps stay inside the page's next 0.5 s
      // auto-refresh tick (t = 1 s) so the error state is observed before the tick re-reads.
      await tester.pump(const Duration(milliseconds: 210));
      await tester.pump(const Duration(milliseconds: 650));

      expect(device.tableSends, StorageClient.readAttempts);
      expect(
        find.text('The stored backup could not be read'),
        findsOneWidget,
        reason: 'a failed table read is shown, not an empty backup table',
      );
      expect(find.textContaining('asked 3 time(s)'), findsOneWidget);
      expect(find.textContaining('not backed up'), findsNothing);
      expect(find.text('Try again'), findsOneWidget);

      device.mode = _Mode.whole;
      await tester.tap(find.text('Try again'));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));

      expect(find.text('The stored backup could not be read'), findsNothing);
      expect(
        find.textContaining('not backed up'),
        findsWidgets,
        reason: 'the decoded backup is on show once the read succeeds',
      );
      await unmount(tester);
    });

    testWidgets('a device with no file table is an empty backup, not an '
        'error', (tester) async {
      final device = _TableDevice(_Mode.absent, backupTable);
      installRequestChannel(device);
      addTearDown(() => installRequestChannel(null));
      await pumpBackup(tester);
      await tester.pump(const Duration(milliseconds: 50));

      expect(
        device.tableSends,
        1,
        reason: 'an absent table is answered once - no retry storm',
      );
      expect(find.text('The stored backup could not be read'), findsNothing);
      expect(
        find.textContaining('not backed up'),
        findsWidgets,
        reason:
            'nothing saved is a device state the backup table says out loud',
      );
      await unmount(tester);
    });

    testWidgets('a partial table reply is an error naming the hole, never a '
        'partial backup', (tester) async {
      final device = _TableDevice(_Mode.hole, backupTable);
      installRequestChannel(device);
      addTearDown(() => installRequestChannel(null));
      await pumpBackup(tester);
      await tester.pump(const Duration(milliseconds: 210));
      await tester.pump(const Duration(milliseconds: 650));

      expect(find.text('The stored backup could not be read'), findsOneWidget);
      expect(
        find.textContaining('fragment 1 of 2 never arrived'),
        findsOneWidget,
      );
      expect(find.textContaining('not backed up'), findsNothing);
      await unmount(tester);
    });
  });

  test('the absent answer is one send, never a retry storm', () async {
    // The policy itself, without a page in the way: the device's FAIL answer for a name it does
    // not carry ends the read at the first send.
    final device = _TableDevice(_Mode.absent, scriptTable);
    installRequestChannel(device);
    addTearDown(() => installRequestChannel(null));

    final client = StorageClient(deviceId: 1);
    final read = await client.readFileTableResult();

    expect(read.absent, isTrue);
    expect(read.failed, isFalse);
    expect(read.records, isNull, reason: 'absent is not data');
    expect(read.attempts, 1);
    expect(device.tableSends, 1);
    expect(client.lastReadError, isNull);
  });

  test(
    'a partial table is re-requested whole and never returned as data',
    () async {
      final device = _TableDevice(_Mode.hole, scriptTable);
      installRequestChannel(device);
      addTearDown(() => installRequestChannel(null));

      final client = StorageClient(deviceId: 1);
      final read = await client.readFileTableResult();

      expect(read.partial, isTrue);
      expect(read.ok, isFalse);
      expect(device.tableSends, StorageClient.readAttempts);
      expect(read.detail, contains('fragment 1 of 2 never arrived'));
      expect(read.detail, contains('asked 3 time(s)'));
    },
  );

  test('a whole table decodes to the records, one send', () async {
    final device = _TableDevice(_Mode.whole, scriptTable);
    installRequestChannel(device);
    addTearDown(() => installRequestChannel(null));

    final client = StorageClient(deviceId: 1);
    final read = await client.readFileTableResult();

    expect(read.ok, isTrue);
    expect(device.tableSends, 1);
    expect(
      [for (final r in read.records!) normalizeFileName(r.name)],
      ['SCR_000', 'SCR_001'],
    );
  });

  test('the retry policy is the shared one: 3 sends, 200 then 600 ms', () {
    expect(StorageClient.readAttempts, 3);
    expect(
      StorageClient.readRetryBackoff.map((d) => d.inMilliseconds).toList(),
      [200, 600],
    );
  });
}
