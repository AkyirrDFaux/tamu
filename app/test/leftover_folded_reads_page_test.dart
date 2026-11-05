/// The three leftover folded reads (Akyirr, 2026-11-05: "never present a failure as data").
///
/// `test/table_read_failure_page_test.dart` fixed the reads whose empty result reads as device
/// state. These are the secondary paths that folded a failed read into a device state all the
/// same, and each is pinned here against a fake transport that FAILS ONCE and then answers:
///
///   * Scripts - the load action's slot map read (`slotMap()` -> empty list said "No free
///     script slot" for a device that never answered). Now `readSlotMap()` -> null is said out
///     loud, while a slot map the device really answered keeps the old wording.
///   * Register - a failed topology refresh kept the previous tree on screen looking current.
///     The tree is kept but marked stale with the moment it was read, and the retry replaces it.
///   * File viewer - a failed `.DT_` sibling read fell into the same raw-hex fallback as "this
///     block has no table". Absent stays benign; failed and partial say so, with the re-read.
library;

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/fragment_reassembler.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/file_viewers.dart';
import 'package:tamuapp/ui/formatters.dart';
import 'package:tamuapp/ui/register_page.dart';
import 'package:tamuapp/ui/scripts_page.dart';
import 'package:tamuapp/ui/theme.dart';

/// A fake device for the Scripts page: the slot map, the file table and the load command.
class _ScriptDevice implements TolerantRequestChannel {
  _ScriptDevice({required this.slotMap});

  /// The CID 0 slot map payload: one uint16 file id per slot, `0xFFFF` = empty.
  final List<int> slotMap;

  /// False makes the slot map request go unanswered (the request throws).
  bool slotMapAnswers = true;

  int slotMapSends = 0;

  /// Every load command the page sent, as `(fileId, slot)`.
  final List<({int fileId, int slot})> loads = [];

  /// The `.TABLE` records the Available view lists.
  List<int> table = const [];

  @override
  bool get isConnected => true;

  @override
  int takeTxId() => 0xF800;

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
    if (service == ServiceType.script) {
      if (functionCid == 0) {
        slotMapSends++;
        if (!slotMapAnswers) throw StateError('the device did not answer');
        return PacketResponse(payload: slotMap, success: true, fail: false);
      }
      if (functionCid == 1) {
        loads.add((
          fileId: payload[0] | (payload[1] << 8),
          slot: payload.length > 2 ? payload[2] : -1,
        ));
        return const PacketResponse(payload: [], success: true, fail: false);
      }
    }
    if (service == ServiceType.storage && functionCid == 5) {
      final echo = StorageClient.padName('.TABLE');
      final body = [...echo, ...table];
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

/// A fake device for the Register page's topology: the script slot map, the block type
/// enumeration and the per-block metadata reads.
class _TopologyDevice implements TolerantRequestChannel {
  /// False makes the block-type enumeration go unanswered (the first topology read fails).
  bool enumerationAnswers = true;

  /// False makes the per-block metadata reads go unanswered (a refresh that half-finishes:
  /// every block it asked about comes back unread).
  bool metaAnswers = true;

  /// Whether the enumeration names a `Button` block (a type that is not the System block).
  bool withButton = false;

  /// The name the `Button` block reports - changed between reads so a refresh is visible.
  String buttonName = 'Button';

  int enumerateSends = 0;

  @override
  bool get isConnected => true;

  @override
  int takeTxId() => 0xF900;

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
    if (service == ServiceType.script && functionCid == 0) {
      // Nothing loaded: one empty slot. Only the topology read is under test.
      return const PacketResponse(
        payload: [0xFF, 0xFF],
        success: true,
        fail: false,
      );
    }
    if (service != ServiceType.register) {
      return const PacketResponse(payload: [], success: true, fail: false);
    }
    switch (functionCid) {
      case RegisterCid.enumerateBlocks:
        enumerateSends++;
        if (!enumerationAnswers) throw StateError('the device did not answer');
        // One word per type: (type << 6) | highest instance.
        return PacketResponse(
          payload: withButton ? [0x40, 0x02] : const [],
          success: true,
          fail: false,
        );
      case RegisterCid.enumerateFields:
        return const PacketResponse(payload: [], success: true, fail: false);
      case RegisterCid.read:
        // A block meta read: the BlockInfo prefix, then ValueInfo (4) + the block's name.
        if (!metaAnswers) throw StateError('the device did not answer');
        final field = blockInfoField(bytesToInt(payload) ?? 0);
        if (field != 0xFF) {
          return const PacketResponse(payload: [], success: true, fail: false);
        }
        final name = withButton ? buttonName : 'System';
        return PacketResponse(
          payload: [
            0, 0, 0, 0, // header
            0x09, 0x00, 0x00, 0x00, // ValueInfo: type 0x09, size 0, no flags
            ...name.codeUnits,
          ],
          success: true,
          fail: false,
        );
      default:
        return const PacketResponse(payload: [], success: true, fail: false);
    }
  }
}

/// What the fake device answers for the `.DT_<hex2>` sibling read.
enum _DtMode {
  /// FLAG_FAIL with no payload: the block has no table stored (absent - benign).
  absent,

  /// Nothing comes back at all (the request throws - failed).
  lost,

  /// One fragment of a two-fragment stream (partial).
  hole,

  /// The whole table.
  whole,
}

/// A fake device that answers the `.DV_` viewer's sibling `.DT_` read from [_DtMode].
class _SiblingDevice implements TolerantRequestChannel {
  _SiblingDevice(this.mode, this.table);

  _DtMode mode;
  final List<int> table;
  int sends = 0;

  @override
  bool get isConnected => true;

  @override
  int takeTxId() => 0xFA00;

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
      return const PacketResponse(payload: [], success: true, fail: false);
    }
    sends++;
    final echo = StorageClient.padName('.DT_00');
    switch (mode) {
      case _DtMode.absent:
        return const PacketResponse(payload: [], success: false, fail: true);
      case _DtMode.lost:
        throw StateError('the device did not answer');
      case _DtMode.hole:
        final half = table.length ~/ 2;
        final body = [...echo, ...table.sublist(0, half)];
        return PacketResponse(
          payload: body,
          success: true,
          fail: false,
          chunks: [FragmentChunk(0, body)],
          missing: const [1],
          stopped: true,
          total: 2,
        );
      case _DtMode.whole:
        final body = [...echo, ...table];
        return PacketResponse(
          payload: body,
          success: true,
          fail: false,
          chunks: [FragmentChunk(0, body)],
          stopped: true,
          total: 1,
        );
    }
  }
}

List<int> _u16(int v) => [v & 0xFF, (v >> 8) & 0xFF];

/// One 16-byte file-table record: u32 offset, u32 size, 8-byte name.
List<int> _record(int offset, int size, String name) => [
  for (final v in [offset, size]) ...[
    v & 0xFF,
    (v >> 8) & 0xFF,
    (v >> 16) & 0xFF,
    (v >> 24) & 0xFF,
  ],
  ...name.padRight(8).codeUnits,
];

/// The `.DT_00` table the viewer decodes: two persistent entries, 4 + 5 = 9 bytes of values.
final List<int> _dtTable = [
  ...'Box'.codeUnits, ...List.filled(13, 0), // 16-byte block name
  ..._u16(2),
  0, 0, // reserved
  ..._u16((0 << 8) | 0), ..._u16(0), // f0.k0 @ offset 0
  ..._u16(DataType.number.value), 4, ValueFlags.persistent,
  ..._u16((1 << 8) | 0), ..._u16(4), // f1.k0 @ offset 4
  ..._u16(DataType.text.value), 5, ValueFlags.persistent,
];

/// The `.DV_00` bytes that pair with [_dtTable]: exactly `persistentSize` (9) long.
final List<int> _dvValues = [...numberToBytes(1.5), ...'hello'.codeUnits];

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  final scriptTable = <int>[
    ..._record(0x1000, 200, 'SCR_000'),
    ..._record(0x2000, 300, 'SCR_001'),
  ];

  // ---------------------------------------------------------------------------
  // 1. Scripts: the load action's slot map read
  // ---------------------------------------------------------------------------
  group('the Scripts page, load action', () {
    Future<void> pumpAvailable(WidgetTester tester) async {
      await tester.pumpWidget(
        MaterialApp(
          theme: buildTheme(),
          home: const ScriptsPage(deviceId: 1, deviceName: 'Core'),
        ),
      );
      await tester.pump(); // initState's Loaded read (the slot map)
      await tester.pump(const Duration(milliseconds: 50));
      await tester.tap(find.text('Available'));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));
    }

    testWidgets('an unanswered slot map says so instead of "No free script '
        'slot", and the retry loads', (tester) async {
      // Eight empty slots: a whole map, with slot 0 free.
      final device = _ScriptDevice(slotMap: [for (var i = 0; i < 8; i++) 0xFF])
        ..table = scriptTable;
      installRequestChannel(device);
      addTearDown(() => installRequestChannel(null));
      await pumpAvailable(tester);

      // The device goes quiet for THIS read only.
      device.slotMapAnswers = false;
      await tester.tap(find.text('Load').first);
      await tester.pump();

      expect(
        find.textContaining('The script slot map could not be read'),
        findsOneWidget,
        reason: 'a failed CID 0 read is not a device with no free slot',
      );
      expect(find.text('No free script slot'), findsNothing);
      expect(
        device.loads,
        isEmpty,
        reason: 'no slot is picked from a map that was never read',
      );

      // The retry re-reads the map and loads into the lowest free slot. The failure's snack
      // bar is still up (ScaffoldMessenger queues), so let it go away first.
      for (
        var i = 0;
        i < 20 &&
            find
                .textContaining('The script slot map could not be read')
                .evaluate()
                .isNotEmpty;
        i++
      ) {
        await tester.pump(const Duration(milliseconds: 500));
      }
      device.slotMapAnswers = true;
      await tester.tap(find.text('Load').first);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));

      expect(
        device.loads,
        [(fileId: 0, slot: 0)],
        reason:
            'the retry ends with the real device action, in the lowest free slot',
      );
      expect(find.text('Loaded SCR_000 into slot 0'), findsOneWidget);
    });

    testWidgets('a slot map the device answers with no free slot still says '
        '"No free script slot"', (tester) async {
      // Two slots, both occupied: this IS a device state, and it keeps the old wording.
      final device = _ScriptDevice(slotMap: [..._u16(0), ..._u16(1)])
        ..table = scriptTable;
      installRequestChannel(device);
      addTearDown(() => installRequestChannel(null));
      await pumpAvailable(tester);

      await tester.tap(find.text('Load').first);
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));

      expect(find.text('No free script slot'), findsOneWidget);
      expect(
        find.textContaining('The script slot map could not be read'),
        findsNothing,
      );
      expect(
        device.slotMapSends,
        2,
      ); // initState's Loaded read, then the load action
      expect(device.loads, isEmpty);
    });
  });

  // ---------------------------------------------------------------------------
  // 2. Register: a failed topology refresh must not leave a tree looking current
  // ---------------------------------------------------------------------------
  group('the Register page, topology refresh', () {
    Future<void> pumpPage(WidgetTester tester) async {
      await tester.pumpWidget(
        MaterialApp(
          theme: buildTheme(),
          home: RegisterPage(deviceId: 1, isConnected: () => true),
        ),
      );
      await tester.pump(); // the initial topology read
      await tester.pump(const Duration(milliseconds: 50));
    }

    Future<void> unmount(WidgetTester tester) async {
      // The page's 0.5 s auto-refresh is a periodic timer: dispose it before the test ends.
      await tester.pumpWidget(const SizedBox());
      await tester.pump();
    }

    testWidgets(
      'a first topology read that fails shows the error page, never an '
      'empty tree, and its retry reads the tree',
      (tester) async {
        final device = _TopologyDevice()
          ..enumerationAnswers = false
          ..withButton = true;
        installRequestChannel(device);
        addTearDown(() => installRequestChannel(null));
        await pumpPage(tester);

        expect(find.text('The register could not be read'), findsOneWidget);
        expect(find.text('No blocks found'), findsNothing);
        expect(find.text('Try again'), findsOneWidget);

        device.enumerationAnswers = true;
        await tester.tap(find.text('Try again'));
        await tester.pump();
        await tester.pump(const Duration(milliseconds: 50));

        expect(find.text('The register could not be read'), findsNothing);
        expect(find.text('Button'), findsWidgets);
        await unmount(tester);
      },
    );

    testWidgets('a failed refresh keeps the tree but marks it stale with when '
        'it was read, and the retry replaces it', (tester) async {
      final device = _TopologyDevice()..withButton = true;
      installRequestChannel(device);
      addTearDown(() => installRequestChannel(null));
      await pumpPage(tester);

      expect(
        find.text('Button'),
        findsWidgets,
        reason: 'the first good read gives the tree that will then go stale',
      );
      expect(find.textContaining('This register tree is stale'), findsNothing);

      // The device stops answering the per-block metadata reads. The client caches the type
      // list, so this is the reachable half-finished topology read: every block it asked about
      // comes back unread and the tree would silently lose them.
      device.metaAnswers = false;
      await tester.pump(const Duration(milliseconds: 5500));
      await tester.pump(const Duration(milliseconds: 10));

      expect(
        find.textContaining('This register tree is stale - last read at'),
        findsOneWidget,
        reason: 'the tree is marked stale, with the moment it was read',
      );
      expect(
        find.textContaining('The device did not answer for'),
        findsOneWidget,
        reason: 'the reason names what the refresh could not read',
      );
      expect(
        find.text('Button'),
        findsWidgets,
        reason: 'the previous tree is kept, never replaced by an empty page',
      );
      expect(find.text('No blocks found'), findsNothing);
      expect(find.text('Try again'), findsOneWidget);

      // Recovery: the device answers, and with different data - so the rendered tree proves
      // the retry re-read it instead of re-showing the stale one.
      device.metaAnswers = true;
      device.buttonName = 'Button B';
      await tester.tap(find.text('Try again'));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));

      expect(find.textContaining('This register tree is stale'), findsNothing);
      expect(find.text('Button B'), findsOneWidget);
      await unmount(tester);
    });
  });

  // ---------------------------------------------------------------------------
  // 3. File viewer: the `.DV_` view's sibling `.DT_` read
  // ---------------------------------------------------------------------------
  group('the .DV_ viewer, sibling .DT_ read', () {
    Future<void> pumpViewer(WidgetTester tester) async {
      await tester.pumpWidget(
        MaterialApp(
          theme: buildTheme(),
          home: FileViewPage(
            deviceId: 1,
            name: '.DV_00',
            size: _dvValues.length,
            data: _dvValues,
          ),
        ),
      );
      await tester.pump();
    }

    testWidgets('a failed read says so and the retry shows the decoded table', (
      tester,
    ) async {
      final device = _SiblingDevice(_DtMode.lost, _dtTable);
      installRequestChannel(device);
      addTearDown(() => installRequestChannel(null));
      await pumpViewer(tester);

      // The shared reader re-requests the whole file twice (200 ms then 600 ms).
      await tester.pump(const Duration(milliseconds: 250));
      await tester.pump(const Duration(milliseconds: 700));

      expect(
        device.sends,
        StorageClient.readAttempts,
        reason: 'a failed read is bounded by the shared re-request policy',
      );
      expect(
        find.text('The block table (.DT_) could not be read'),
        findsOneWidget,
        reason: 'a failed sibling read is not the benign hex fallback',
      );
      expect(find.textContaining('asked 3 time(s)'), findsOneWidget);
      expect(
        find.text(hex32(0)),
        findsNothing,
        reason: 'the failure is not drawn as the viewer\'s raw-hex shape',
      );
      expect(find.textContaining('Box'), findsNothing);
      expect(find.text('Try again'), findsOneWidget);

      // The retry re-reads the table, and this time the device answers whole.
      device.mode = _DtMode.whole;
      await tester.tap(find.text('Try again'));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 50));

      expect(
        find.text('The block table (.DT_) could not be read'),
        findsNothing,
      );
      expect(find.textContaining('Box'), findsWidgets);
      expect(find.text('f0.k0'), findsOneWidget);
      expect(find.text('f1.k0'), findsOneWidget);
    });

    testWidgets('a partial reply names the hole instead of decoding a prefix', (
      tester,
    ) async {
      final device = _SiblingDevice(_DtMode.hole, _dtTable);
      installRequestChannel(device);
      addTearDown(() => installRequestChannel(null));
      await pumpViewer(tester);
      await tester.pump(const Duration(milliseconds: 250));
      await tester.pump(const Duration(milliseconds: 700));

      expect(
        find.text('The block table (.DT_) could not be read'),
        findsOneWidget,
      );
      expect(
        find.textContaining('fragment 1 of 2 never arrived'),
        findsOneWidget,
        reason: 'the reason names the fragment that never arrived',
      );
      expect(find.textContaining('Box'), findsNothing);
      expect(find.text('Try again'), findsOneWidget);
    });

    testWidgets('a block with no table on the device still takes the benign '
        'hex fallback', (tester) async {
      final device = _SiblingDevice(_DtMode.absent, _dtTable);
      installRequestChannel(device);
      addTearDown(() => installRequestChannel(null));
      await pumpViewer(tester);
      await tester.pump(const Duration(milliseconds: 50));

      expect(
        device.sends,
        1,
        reason: 'an absent table is answered once - there is nothing to retry',
      );
      expect(
        find.text('The block table (.DT_) could not be read'),
        findsNothing,
      );
      expect(find.text('Try again'), findsNothing);
      expect(
        find.text(hex32(0)),
        findsOneWidget,
        reason: 'no stored table is a device state: the raw bytes are shown',
      );
    });
  });
}
