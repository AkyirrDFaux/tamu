/// A whole-file read that lost fragments: detected, re-requested, and never decoded from a
/// prefix.
///
/// The failure is the owner's report ("the storage and emote editor doesn't even open"), captured
/// on the rig on 2026-11-04:
///
///   * `test/fixtures/ble_filetable_reply.bin` - the `.TABLE  ` read over BLE from THIS host
///     (`E4:B0:63:C8:20:72`, advertised as `Akyirr`), captured by an independent probe that speaks
///     the App Interface framing and shares no code with the app. It is **223 of 256 fragments**,
///     the first hole at fragment **202**, and the STOP frame never came: the core answers a bolt
///     of 16 kB from one handler into an 8 kB transmit ring and drops whole frames when the link
///     back-pressures (Issues.md E74).
///   * `test/fixtures/table_reply_usb.bin` - the same read over the rig's USB serial link
///     (`dart run tool/storage_read.dart --capture test/fixtures/table_reply_usb.bin` on
///     2026-11-04): **256 of 256 fragments with the STOP frame**, 16384 bytes of table.
///
/// The app's answer is the owner's ruling - "process it correctly no matter what (re-request if
/// something fails, detect it)": a reply with a hole or without its STOP is a FAILED read that
/// names the hole's index and is re-requested whole (`StorageClient.readAttempts` sends,
/// `readRetryBackoff` apart); a `.TABLE` parsed from a fragment set without its STOP is never
/// rendered as the file list, because a truncated table showing 32 of N files is worse than an
/// error.
library;

import 'dart:io';
import 'dart:math' as math;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/fragment_reassembler.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/storage_client.dart';

const int _txId = 0xF321; // the transaction the probe used

List<int> _fixture(String name) =>
    File('test/fixtures/$name').readAsBytesSync();

/// Replays a captured packet stream the way the app's link does: parse the frames, feed the
/// reassembler, and close a stream that stopped without its STOP frame.
///
/// A strict stream aborts at the first gap, which is the pre-fix behaviour this file still pins;
/// a tolerant one yields every fragment it got plus the gap.
PacketResponse _replay(List<int> stream, {bool tolerant = true}) {
  final frames = PacketStreamParser().feed(stream);
  final reassembler = FragmentReassembler()..begin(_txId, tolerant: tolerant);
  FragmentEvent? last;
  for (final frame in frames) {
    last = reassembler.feed(_txId, frame);
    if (last is FragmentAbort) break;
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
  final done = reassembler.finish(_txId)!;
  return PacketResponse(
    payload: done.payload,
    success: true,
    fail: false,
    chunks: done.chunks,
    missing: done.missing,
    stopped: false,
    total: done.total,
  );
}

/// A channel that answers a storage read from a list of replies, repeating the last one; a null
/// entry is "the device did not answer".
class _ReplyChannel implements TolerantRequestChannel {
  _ReplyChannel(this.replies);

  final List<PacketResponse?> replies;
  final List<String> asked = [];

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
    asked.add(StorageClient.unpadName(payload));
    final reply = replies[math.min(asked.length - 1, replies.length - 1)];
    if (reply == null) throw StateError('the device did not answer');
    return reply;
  }
}

void main() {
  final truncated = _fixture('ble_filetable_reply.bin');
  final whole = _fixture('table_reply_usb.bin');

  group('the captured BLE reply (223 of 256, no STOP)', () {
    late List<PacketFrame> frames;

    setUpAll(() {
      // The probe received 480-byte notifications; the parser is chunk-size independent, which
      // is why a fixed split is faithful.
      final parser = PacketStreamParser();
      frames = <PacketFrame>[];
      for (var off = 0; off < truncated.length; off += 480) {
        frames.addAll(
          parser.feed(
            truncated.sublist(off, math.min(off + 480, truncated.length)),
          ),
        );
      }
    });

    test('is a fragmented reply that stops without its STOP frame', () {
      expect(frames, hasLength(223), reason: '223 fragments arrived of 256');
      expect(frames.every((f) => f.isFrag), isTrue);
      expect(frames.any((f) => f.isStop), isFalse, reason: 'no closing frame');
      final first = fragInfoOf(frames.first.payload);
      expect(first.current, 0);
      expect(first.total, 256, reason: 'a 16 kB file at 64 bytes per fragment');
      final indexes = [for (final f in frames) fragInfoOf(f.payload).current];
      expect(indexes.contains(202), isFalse, reason: 'the first hole');
      expect(indexes.contains(214), isTrue);
    });

    test('a strict stream still aborts at the hole', () {
      final strict = FragmentReassembler()..begin(_txId);
      FragmentEvent? last;
      for (final frame in frames) {
        last = strict.feed(_txId, frame);
        if (last is FragmentAbort) break;
      }
      expect(last, isA<FragmentAbort>());
      expect(
        (last! as FragmentAbort).reason,
        'fragment 214/256 out of order (expected 202/256)',
        reason: 'the exact line this failure logged, quoted verbatim',
      );
    });

    test('the gap names the hole index and the missing STOP', () {
      final response = _replay(truncated);
      expect(response.partial, isTrue);
      expect(response.gap!.firstHole, 202);
      expect(response.missing, hasLength(33));
      expect(
        response.gap!.message,
        'fragment 202 of 256 never arrived (33 of 256 missing, 223 received); '
        'the stream stopped without its STOP frame',
      );
    });

    test('a whole-file read re-requests it and then fails visibly', () async {
      final channel = _ReplyChannel([_replay(truncated)]);
      installRequestChannel(channel);
      addTearDown(() => installRequestChannel(null));

      final client = StorageClient(deviceId: 1);
      final table = await client.readFileTable();

      expect(table, isNull, reason: 'a partial table is never data');
      expect(
        channel.asked,
        hasLength(StorageClient.readAttempts),
        reason: 'the read is re-requested, and bounded',
      );
      expect(channel.asked, everyElement('.TABLE'));
      expect(client.lastReadAttempts, StorageClient.readAttempts);
      expect(
        client.lastReadError,
        contains('fragment 202 of 256 never arrived'),
      );
      expect(client.lastReadError, contains('asked 3 time(s)'));
    });
  });

  group('recovery', () {
    test('a truncated first answer is recovered by the re-request', () async {
      final channel = _ReplyChannel([_replay(truncated), _replay(whole)]);
      installRequestChannel(channel);
      addTearDown(() => installRequestChannel(null));

      final client = StorageClient(deviceId: 1);
      final table = await client.readFileTable();

      expect(table, isNotNull, reason: 'the second read arrived whole');
      expect(channel.asked, hasLength(2), reason: 'one re-request, not three');
      expect(client.lastReadAttempts, 2);
      expect(client.lastReadError, isNull);
      expect(table, hasLength(32));
    });

    test('the whole reply is the device\'s 32 files, LAY_1 included', () async {
      final channel = _ReplyChannel([_replay(whole)]);
      installRequestChannel(channel);
      addTearDown(() => installRequestChannel(null));

      final client = StorageClient(deviceId: 1);
      final table = await client.readFileTable();
      expect(channel.asked, hasLength(1), reason: 'whole first time');
      expect(client.lastReadAttempts, 1);

      final names = [for (final r in table!) normalizeFileName(r.name)];
      expect(names, hasLength(32), reason: 'the files the device stores');
      expect(names, contains('.TABLE'));
      expect(names, contains('.SNREG'));
      // The two LED layouts - the "emote" files whose viewer is the editor the owner meant.
      expect(names, contains('LAY_1'));
      expect(names, contains('LAY_2'));
      final lay = table.firstWhere((r) => normalizeFileName(r.name) == 'LAY_1');
      expect(lay.size, 223);
      expect(lay.offset, 8192);
    });
  });

  group('a file read', () {
    /// `LAY_1`'s reply: four `[name][content]` fragments of 64 content bytes.
    PacketResponse layoutReply({bool loseTheLast = false}) {
      final content = _fixture('lay1.bin');
      final chunks = <FragmentChunk>[];
      for (var i = 0; i * 64 < content.length; i++) {
        chunks.add(
          FragmentChunk(i, [
            ...StorageClient.padName('LAY_1'),
            ...content.sublist(i * 64, math.min((i + 1) * 64, content.length)),
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
        // A hole means the stream cannot have ended on a clean STOP either.
        stopped: !loseTheLast,
        total: chunks.length,
      );
    }

    test('a body that lost its tail is re-requested, never returned', () async {
      final channel = _ReplyChannel([
        layoutReply(loseTheLast: true),
        layoutReply(),
      ]);
      installRequestChannel(channel);
      addTearDown(() => installRequestChannel(null));

      final client = StorageClient(deviceId: 1);
      final bytes = await client.readFile('LAY_1', size: 223);
      expect(bytes, isNotNull);
      expect(bytes, hasLength(223));
      expect(client.lastReadAttempts, 2);
    });

    test('a body that never arrives whole is null with the reason', () async {
      final channel = _ReplyChannel([layoutReply(loseTheLast: true)]);
      installRequestChannel(channel);
      addTearDown(() => installRequestChannel(null));

      final client = StorageClient(deviceId: 1);
      final bytes = await client.readFile('LAY_1', size: 223);
      expect(bytes, isNull);
      expect(channel.asked, hasLength(StorageClient.readAttempts));
      expect(client.lastReadError, contains('fragment 3 of 4 never arrived'));
    });
  });

  test('the retry policy is bounded and stated', () {
    expect(
      StorageClient.readAttempts,
      3,
      reason: 'one attempt + two re-requests',
    );
    expect(StorageClient.readRetryBackoff, hasLength(2));
    expect(
      StorageClient.readRetryBackoff.map((d) => d.inMilliseconds).toList(),
      [200, 600],
    );
  });
}
