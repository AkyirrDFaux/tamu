/// Both screens the owner reported, rendered from data captured off the rig.
///
/// The data comes from the same captures the diagnosis used:
///   * `test/fixtures/ble_filetable_reply.bin` - the `.TABLE  ` reply over BLE (223 of 256
///     fragments, first hole at 202, no STOP frame);
///   * `test/fixtures/table_reply_usb.bin` - the same read over the rig's USB link (256 of 256
///     fragments with the STOP frame), captured with `dart run tool/storage_read.dart --capture`;
///   * the `LAY_1` reply below - the LED layout ("emote") file, 4 fragments, 223 bytes, read
///     whole over BLE in 27 ms;
///   * the Left Eye block - 11 fields, 53 field/key rows, 11 of them size-0 dictionary markers
///     (Geometry 0x101 / Texture 0x102 at key 0), read field by field off the rig.
///
/// What is pinned here is what each screen ends in: data from a COMPLETE reply, or a visible error
/// with a retry. A truncated file-table reply is never rendered as a file list (a table showing 32
/// of N files is worse than an error), so the Storage page either recovers through the client's
/// re-request or says the read failed.
library;

import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/fragment_reassembler.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/render_dict.dart'
    show geometryDictType, textureDictType;
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/file_viewers.dart';
import 'package:tamuapp/ui/register_page.dart';
import 'package:tamuapp/ui/storage_page.dart';
import 'package:tamuapp/ui/theme.dart';

/// Replays a captured packet stream the way the app's link does.
PacketResponse _replay(String fixture) {
  final bytes = File('test/fixtures/$fixture').readAsBytesSync();
  final parser = PacketStreamParser();
  final frames = <PacketFrame>[];
  for (var off = 0; off < bytes.length; off += 480) {
    frames.addAll(
      parser.feed(
        bytes.sublist(off, off + 480 > bytes.length ? bytes.length : off + 480),
      ),
    );
  }
  final reassembler = FragmentReassembler()..begin(0xF321, tolerant: true);
  FragmentEvent? last;
  for (final frame in frames) {
    last = reassembler.feed(0xF321, frame);
  }
  if (last is FragmentComplete) {
    return PacketResponse(
      payload: last.payload,
      success: true,
      fail: false,
      chunks: last.chunks,
      missing: last.missing,
      stopped: last.stopped,
      total: last.total,
    );
  }
  final done = reassembler.finish(0xF321)!;
  return PacketResponse(
    payload: done.payload,
    success: true,
    fail: false,
    chunks: done.chunks,
    missing: done.missing,
    // No STOP frame: the device never finished this reply.
    stopped: false,
    total: done.total,
  );
}

/// The `.TABLE  ` read over BLE on 2026-11-04: 223 of 256 fragments, first hole at 202, no STOP.
PacketResponse _truncatedTableReply() => _replay('ble_filetable_reply.bin');

/// The same read over the rig's USB link: 256 of 256 fragments and the STOP frame
/// (`dart run tool/storage_read.dart --capture test/fixtures/table_reply_usb.bin`).
PacketResponse _wholeTableReply() => _replay('table_reply_usb.bin');

/// `LAY_1`, read off the rig over BLE on 2026-11-04: 4 fragments, 223 bytes (an 11 x 10 LED
/// layout, brightness limit 178). Captured with the independent probe and stored as
/// `test/fixtures/lay1.bin` so the viewer test runs against the device's own bytes.
List<int> _lay1Content() => File('test/fixtures/lay1.bin').readAsBytesSync();

/// The reply a storage read of `LAY_1` produces: 4 fragments of [name][content].
PacketResponse _lay1Reply({bool loseTheLast = false}) {
  const chunk = 64;
  final content = _lay1Content();
  final chunks = <FragmentChunk>[];
  for (var i = 0; i * chunk < content.length; i++) {
    final start = i * chunk;
    final end = (start + chunk).clamp(0, content.length);
    chunks.add(
      FragmentChunk(i, [
        ...StorageClient.padName('LAY_1'),
        ...content.sublist(start, end),
      ]),
    );
  }
  final kept = loseTheLast ? chunks.sublist(0, chunks.length - 1) : chunks;
  return PacketResponse(
    payload: [for (final c in kept) ...c.body],
    success: true,
    fail: false,
    chunks: kept,
    missing: loseTheLast ? [chunks.length - 1] : const [],
    stopped: !loseTheLast,
    total: chunks.length,
  );
}

/// A channel with the rig's answers: the file table as a queue of replies (so one run can be
/// truncated and the next whole, which is what the re-request does), and whole files.
class _RigChannel implements TolerantRequestChannel {
  _RigChannel(this.table);

  final List<PacketResponse> table;
  final List<({ServiceType service, int cid, String name})> asked = [];

  int _tableAsks = 0;

  int get tableAsks => _tableAsks;

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
  }) async => (await requestWithFlags(
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
  }) async => _answer(functionCid, payload);

  @override
  Future<PacketResponse> requestTolerant(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
  }) async => _answer(functionCid, payload);

  PacketResponse _answer(int cid, List<int> payload) {
    final name = payload.length >= 8 ? StorageClient.unpadName(payload) : '';
    asked.add((service: ServiceType.storage, cid: cid, name: name));
    if (name == '.TABLE') {
      final index = _tableAsks < table.length ? _tableAsks : table.length - 1;
      _tableAsks++;
      return table[index];
    }
    if (name == 'LAY_1') return _lay1Reply();
    return const PacketResponse(payload: [], success: true, fail: false);
  }
}

/// The Left Eye block as the rig reports it: 11 fields, and key 0 of every field is a
/// size-0 dictionary marker (Geometry or Texture) - the rows the card asked about.
class _EyeRegisterClient extends RegisterClient {
  _EyeRegisterClient({required super.deviceId});

  static const int _dynType = 0x3F0;
  static const int _fields = 11;
  static const int _persistentCollection = 0x02;

  static const List<int> _fieldTypes = [
    geometryDictType,
    textureDictType,
    geometryDictType,
    textureDictType,
    geometryDictType,
    geometryDictType,
    textureDictType,
    geometryDictType,
    textureDictType,
    geometryDictType,
    textureDictType,
  ];

  @override
  Future<BlocksRead?> readBlocks({List<int>? scriptSlots}) async => (
    blocks: <ReportedBlock>[
      (
        type: _dynType,
        inst: 1,
        meta: ValueInfo(
          type: _dynType,
          size: _fields,
          flags: _persistentCollection,
        ),
        name: 'Left Eye',
      ),
    ],
    unreadTypes: const <int>{},
  );

  @override
  Future<StaticFieldLayout> readStaticFieldLayout() async => const {};

  /// One enumeration returns every row: key 0 (the marker) plus keys 1 and 4 where the rig
  /// reported them.
  @override
  Future<List<int>?> enumerateFieldKeys(int blockType, int instance) async => [
    for (var f = 0; f < _fields; f++) ...<int>[
      (f << 8) | 0,
      (f << 8) | 1,
      if (f != 1 && f != 9 && f != 10) (f << 8) | 4,
    ],
  ];

  @override
  Future<({ValueInfo meta, List<int> value})?> readBlockField(
    int blockType,
    int instance,
    int field,
    int key,
  ) async {
    final type = _fieldTypes[field];
    if (key == 0) {
      // The marker itself: size 0, the declared dictionary type, no value bytes.
      return (
        meta: ValueInfo(type: type, size: 0, flags: _persistentCollection),
        value: const <int>[],
      );
    }
    return (
      meta: ValueInfo(
        type: DataType.number.value,
        size: 4,
        flags: _persistentCollection,
      ),
      value: numberToBytes(field + key / 10),
    );
  }
}

bool _alwaysConnected() => true;

/// Lets the client's bounded retry policy run to completion in the test's fake clock: the two
/// pauses are 200 ms and 600 ms, so this advances past both.
Future<void> _settleRetries(WidgetTester tester) async {
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 150));
  await tester.pump(const Duration(milliseconds: 250));
  await tester.pump(const Duration(milliseconds: 700));
  await tester.pump(const Duration(milliseconds: 10));
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('the Storage page', () {
    testWidgets('lists the device files from a complete reply', (tester) async {
      tester.view.physicalSize = const Size(1000, 1800);
      tester.view.devicePixelRatio = 1.0;
      addTearDown(tester.view.reset);

      final channel = _RigChannel([_wholeTableReply()]);
      installRequestChannel(channel);
      addTearDown(() => installRequestChannel(null));

      await tester.pumpWidget(
        const MaterialApp(
          home: StoragePage(deviceId: 1, isConnected: _alwaysConnected),
        ),
      );
      await _settleRetries(tester);

      // The rows the owner needs: the directory itself, the serial registry and the emotes.
      expect(find.textContaining('LAY_1'), findsWidgets);
      expect(find.textContaining('.TABLE'), findsWidgets);
      expect(find.textContaining('.SNREG'), findsWidgets);
      expect(channel.tableAsks, 1, reason: 'a whole reply is not re-requested');
    });

    testWidgets('recovers a truncated reply with one re-request', (
      tester,
    ) async {
      tester.view.physicalSize = const Size(1000, 1800);
      tester.view.devicePixelRatio = 1.0;
      addTearDown(tester.view.reset);

      final channel = _RigChannel([_truncatedTableReply(), _wholeTableReply()]);
      installRequestChannel(channel);
      addTearDown(() => installRequestChannel(null));

      await tester.pumpWidget(
        const MaterialApp(
          home: StoragePage(deviceId: 1, isConnected: _alwaysConnected),
        ),
      );
      await _settleRetries(tester);

      expect(channel.tableAsks, 2, reason: 'the BLE hole triggered a re-read');
      expect(find.textContaining('LAY_1'), findsWidgets);
    });

    testWidgets('a table that never completes ends in an error, not a list', (
      tester,
    ) async {
      tester.view.physicalSize = const Size(1000, 1800);
      tester.view.devicePixelRatio = 1.0;
      addTearDown(tester.view.reset);

      final channel = _RigChannel([_truncatedTableReply()]);
      installRequestChannel(channel);
      addTearDown(() => installRequestChannel(null));

      await tester.pumpWidget(
        const MaterialApp(
          home: StoragePage(deviceId: 1, isConnected: _alwaysConnected),
        ),
      );
      await _settleRetries(tester);

      expect(channel.tableAsks, StorageClient.readAttempts);
      expect(find.text('The file table could not be read'), findsOneWidget);
      // The hole is named, and there is an action - never an empty directory.
      expect(
        find.textContaining('fragment 202 of 256 never arrived'),
        findsOneWidget,
      );
      expect(find.text('Try again'), findsOneWidget);
      expect(find.textContaining('LAY_1'), findsNothing);
    });
  });

  group('the emote (LED layout) editor', () {
    testWidgets('opens on LAY_1 from a complete read', (tester) async {
      tester.view.physicalSize = const Size(1000, 1800);
      tester.view.devicePixelRatio = 1.0;
      addTearDown(tester.view.reset);

      final channel = _RigChannel([_wholeTableReply()]);
      installRequestChannel(channel);
      addTearDown(() => installRequestChannel(null));

      final data = await StorageClient(
        deviceId: 1,
      ).readFile('LAY_1', size: 223);
      expect(data, isNotNull, reason: 'the whole layout file arrived');
      expect(data!.length, 223);

      await tester.pumpWidget(
        MaterialApp(
          home: FileViewPage(deviceId: 1, name: 'LAY_1', size: 223, data: data),
        ),
      );
      await tester.pump();

      // The layout viewer's own header line: 11 x 10 LEDs at limit 178 (70 %).
      expect(find.textContaining('11x10 LEDs'), findsOneWidget);
    });

    testWidgets('a layout that never arrives whole is an error with a retry', (
      tester,
    ) async {
      tester.view.physicalSize = const Size(1000, 1800);
      tester.view.devicePixelRatio = 1.0;
      addTearDown(tester.view.reset);

      var failures = 0;
      final channel = _RigChannel([_wholeTableReply()]);
      installRequestChannel(channel);
      addTearDown(() => installRequestChannel(null));

      await tester.pumpWidget(
        MaterialApp(
          theme: buildTheme(),
          home: FileViewPage(
            deviceId: 1,
            name: 'LAY_1',
            size: 223,
            data: null,
            error:
                'Could not read "LAY_1": fragment 3 of 4 never arrived '
                '(1 of 4 missing, 3 received); the stream stopped without its '
                'STOP frame. It was asked 3 time(s); try again.',
            onRetry: () async {
              failures++;
              // The second read is a re-request the device answers whole.
              return _lay1Content();
            },
          ),
        ),
      );
      await tester.pump();

      expect(find.text('Could not read this file'), findsOneWidget);
      expect(
        find.textContaining('fragment 3 of 4 never arrived'),
        findsOneWidget,
      );

      await tester.tap(find.text('Try again'));
      await tester.pump();
      await tester.pump();

      expect(failures, 1);
      expect(find.textContaining('11x10 LEDs'), findsOneWidget);
    });
  });

  testWidgets('the Register page renders the Left Eye block and its markers', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(1000, 2400);
    tester.view.devicePixelRatio = 1.0;
    addTearDown(tester.view.reset);
    addTearDown(() => tester.pumpWidget(const SizedBox()));

    await tester.pumpWidget(
      MaterialApp(
        theme: buildTheme(),
        home: RegisterPage(
          deviceId: 1,
          isConnected: () => true,
          hasDynamicMemory: true,
          clientFactory: (_) => _EyeRegisterClient(deviceId: 1),
        ),
      ),
    );
    await tester.pump();
    await tester.pump();
    expect(find.text('Left Eye'), findsOneWidget);

    await tester.tap(find.text('Left Eye'));
    await tester.pump();
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 50));

    // The block's own line: 11 fields, as the rig's block meta reports (meta.size = 11).
    expect(find.text('11 fields'), findsOneWidget);
    // Every field renders as the dictionary its key-0 marker declares: 6 Geometry (fields 0, 2,
    // 4, 5, 7, 9) and 5 Texture (1, 3, 6, 8, 10). Field 9 and field 10 - the two size-0
    // markers the card asked about - are the last Geometry and the last Texture row, and their
    // rows carry no value bytes at all; the old descriptor path had no shape for them.
    expect(find.text('Geometry'), findsNWidgets(6));
    expect(find.text('Texture'), findsNWidgets(5));
    expect(
      find.textContaining('keys'),
      findsNWidgets(11),
      reason: 'one row per field, each listing the keys the device reports',
    );
    expect(tester.takeException(), isNull);
  });
}
