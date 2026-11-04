/// The truncated file-table reply, replayed through the app's own parser.
///
/// `test/fixtures/ble_filetable_reply.bin` is the packet stream the rig sent on 2026-11-04 for
/// `Storage CID 5` of `.TABLE  `, captured over BLE from THIS host (`E4:B0:63:C8:20:72`,
/// advertised as `Akyirr`) by an independent probe that speaks the App Interface framing but
/// shares no code with the app. It is the failure the owner reported ("the storage and emote
/// editor doesn't even open"), captured once and replayed here so the diagnosis does not need
/// the hardware again:
///
///   * the reply is 256 fragments (16 kB) - the file table is a fixed reservation;
///   * only 223 of them arrived, in bursts, and the STOP frame never came (the core answers the
///     whole file from one handler into an 8 kB transmit ring and drops whole frames when the
///     link back-pressures);
///   * the strict reader aborted the whole reply at the first gap, so `readFileTable` answered
///     null and the Storage page rendered an empty list.
///
/// The tolerant reader (what the storage client asks for now) keeps the fragments it got, placed
/// at their declared offsets, and the table's live records all sit in the first ~100 fragments -
/// well inside the delivered prefix.
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/fragment_reassembler.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/storage_client.dart';

/// The captured stream, split into the chunks the app's parser sees (the probe received 480-byte
/// notifications; the parser is chunk-size independent, which is why a fixed split is faithful).
List<List<int>> _capturedChunks() {
  final bytes = File('test/fixtures/ble_filetable_reply.bin').readAsBytesSync();
  const chunk = 480;
  return [
    for (var off = 0; off < bytes.length; off += chunk)
      bytes.sublist(
        off,
        off + chunk > bytes.length ? bytes.length : off + chunk,
      ),
  ];
}

void main() {
  group('the captured file-table reply', () {
    late List<PacketFrame> frames;
    const txId = 0xF321; // the transaction the probe used

    setUpAll(() {
      final parser = PacketStreamParser();
      frames = [for (final chunk in _capturedChunks()) ...parser.feed(chunk)];
    });

    test('is a fragmented reply that stops without its STOP frame', () {
      expect(frames, hasLength(223), reason: '223 fragments arrived of 256');
      expect(frames.every((f) => f.isFrag), isTrue);
      expect(frames.any((f) => f.isStop), isFalse, reason: 'no closing frame');
      final first = fragInfoOf(frames.first.payload);
      expect(first.current, 0);
      expect(first.total, 256, reason: 'a 16 kB file at 64 bytes per fragment');
      // The first gap is at fragment 202: 33 fragments and the STOP were dropped.
      final indexes = [for (final f in frames) fragInfoOf(f.payload).current];
      expect(indexes.contains(202), isFalse);
      expect(indexes.contains(214), isTrue);
    });

    test('the strict reader aborts it - the pre-fix behaviour', () {
      final strict = FragmentReassembler()..begin(txId);
      FragmentEvent? last;
      for (final frame in frames) {
        last = strict.feed(txId, frame);
        if (last is FragmentAbort) break;
      }
      expect(last, isA<FragmentAbort>());
      expect(
        (last! as FragmentAbort).reason,
        'fragment 214/256 out of order (expected 202/256)',
        reason: 'the exact line this failure logged, quoted verbatim',
      );
    });

    test('the tolerant reader keeps every fragment it got', () {
      final tolerant = FragmentReassembler()..begin(txId, tolerant: true);
      FragmentEvent? last;
      for (final frame in frames) {
        last = tolerant.feed(txId, frame);
      }
      // Nothing completed it: the STOP frame never arrived, so the stall watchdog ends the
      // stream with what it has.
      expect(last, isA<FragmentPending>());
      final done = tolerant.finish(txId)!;
      expect(done.partial, isTrue);
      expect(done.missing, isNotEmpty);
      expect(done.chunks, hasLength(223));

      final placed = StorageClient(deviceId: 1).placeFragments(
        PacketResponse(
          payload: done.payload,
          success: true,
          fail: false,
          chunks: done.chunks,
          missing: done.missing,
        ),
      )!;
      // Placed by fragment index, the prefix runs to the first hole: fragments 0..201 are all
      // present, fragment 202 is not, so 202 * 64 bytes are usable and the rest is not claimed.
      expect(placed.bytes.length, 202 * 64);
      expect(placed.fragmentsUsed, 202);
      expect(placed.holes, greaterThan(0));
    });

    test('the file table parses to the device\'s 32 stored files', () {
      final tolerant = FragmentReassembler()..begin(txId, tolerant: true);
      for (final frame in frames) {
        tolerant.feed(txId, frame);
      }
      final done = tolerant.finish(txId)!;
      final placed = StorageClient(deviceId: 1).placeFragments(
        PacketResponse(
          payload: done.payload,
          success: true,
          fail: false,
          chunks: done.chunks,
          missing: done.missing,
        ),
      )!;
      final records = StorageClient.parseFileTable(placed.bytes);
      final names = [for (final r in records) normalizeFileName(r.name)];
      // `parseFileTable` drops the invalidated (offset 0) generations and stops at the first
      // unwritten record, so what is left is exactly the files the device stores.
      expect(
        names,
        hasLength(32),
        reason: 'the files the device actually stores',
      );
      expect(names, contains('.TABLE'));
      expect(names, contains('.SNREG'));
      // The two LED layouts - the "emote" files whose viewer is the editor the owner meant.
      expect(names, contains('LAY_1'));
      expect(names, contains('LAY_2'));
      final lay = records.firstWhere(
        (r) => normalizeFileName(r.name) == 'LAY_1',
      );
      expect(lay.size, 223);
      expect(lay.offset, 8192);
    });
  });
}
