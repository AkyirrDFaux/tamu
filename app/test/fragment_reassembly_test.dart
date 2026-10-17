/// Host tests for the app-interface fragmentation/reassembly path
/// (Docs/Services/App Interface.md; Docs/RSBus and Packets.md "FRAG").
///
/// CID 0 (slot map) and CID 5 (internal state) of the Script service both exceed one
/// frame and stream through the app interface's fragmentation. The framing state
/// machine is [FragmentReassembler] in `core/fragment_reassembler.dart`; these tests drive it
/// with synthesized frames - no hardware - and pin:
///   * a multi-fragment reply reassembles to the concatenation of its fragments,
///     with a payload larger than one frame;
///   * a single non-FRAG frame is the payload verbatim;
///   * a wrong or short fragment sequence aborts the reply instead of silently
///     truncating or corrupting it;
///   * the round trip through the real wire codec ([PacketFrame.toBytes] +
///     [PacketStreamParser]) reassembles to the same bytes.
library;

import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/fragment_reassembler.dart';
import 'package:tamuapp/core/protocol.dart';

/// One response frame for transaction [txId] carrying fragment [current] of [total].
/// The payload is the 4-byte fragment info (u16 current + u16 total) followed by
/// [data], exactly as the device frames a FRAG packet.
PacketFrame _frag(
  int txId,
  int current,
  int total, {
  List<int> data = const [],
  bool start = false,
  bool stop = false,
  bool success = true,
  bool fail = false,
}) => PacketFrame(
  flags:
      (start ? flagStart : 0) |
      (stop ? flagStop : 0) |
      flagFrag |
      flagType |
      (success ? flagSuccess : 0) |
      (fail ? flagFail : 0),
  priority: defaultPriority,
  idTarget: 0x0001,
  idSource: 0x0001,
  srvTarget: makeService(ServiceType.script, 0), // CID 0: slot map
  srvSource: txId,
  payload: Uint8List.fromList([...writeFragInfo(current, total), ...data]),
);

/// A single-packet (START|STOP, non-FRAG) response frame for transaction [txId].
PacketFrame _single(int txId, List<int> data, {bool success = true}) =>
    PacketFrame(
      flags: flagStart | flagStop | flagType | (success ? flagSuccess : 0),
      priority: defaultPriority,
      idTarget: 0x0001,
      idSource: 0x0001,
      srvTarget: makeService(ServiceType.script, 0),
      srvSource: txId,
      payload: Uint8List.fromList(data),
    );

const int _txId = 0xF123; // a transaction ID from the app's reserved range.

void main() {
  group('FragmentReassembler', () {
    test('reassembles a multi-fragment reply into the concatenation', () {
      final r = FragmentReassembler()..begin(_txId);
      final a = List<int>.generate(
        112,
        (i) => i & 0xFF,
      ); // a full 116-byte frame
      final b = List<int>.generate(50, (i) => 100 + i);
      final c = List<int>.generate(30, (i) => 200 + i);

      expect(
        r.feed(_txId, _frag(_txId, 0, 3, data: a, start: true)),
        isA<FragmentPending>(),
      );
      expect(
        r.feed(_txId, _frag(_txId, 1, 3, data: b)),
        isA<FragmentPending>(),
      );

      final done = r.feed(_txId, _frag(_txId, 2, 3, data: c, stop: true));
      expect(done, isA<FragmentComplete>());
      done as FragmentComplete;
      expect(
        done.payload,
        [...a, ...b, ...c],
        reason: 'fragment info is stripped and the data concatenated in order',
      );
      expect(done.success, isTrue);
      expect(done.fail, isFalse);
    });

    test('a reply larger than one frame reassembles in full', () {
      final r = FragmentReassembler()..begin(_txId);
      // 3 x 112 data bytes = 336 bytes of payload, far past one 116-byte frame.
      final parts = [
        for (var i = 0; i < 3; i++)
          List<int>.generate(112, (j) => (i * 112 + j) & 0xFF),
      ];
      FragmentEvent? event;
      for (var i = 0; i < parts.length; i++) {
        event = r.feed(
          _txId,
          _frag(
            _txId,
            i,
            parts.length,
            data: parts[i],
            start: i == 0,
            stop: i == parts.length - 1,
          ),
        );
      }
      expect(event, isA<FragmentComplete>());
      final got = (event as FragmentComplete).payload;
      expect(
        got.length,
        greaterThan(maxPayloadSize),
        reason: 'the reassembled payload exceeds one frame',
      );
      expect(got, [for (final p in parts) ...p]);
    });

    test('a single non-FRAG frame is the payload verbatim', () {
      final r = FragmentReassembler()..begin(_txId);
      final event = r.feed(_txId, _single(_txId, [1, 2, 3, 4, 5]));
      expect(event, isA<FragmentComplete>());
      expect((event as FragmentComplete).payload, [1, 2, 3, 4, 5]);
    });

    test('round-trips through the wire codec and stream parser', () {
      final tx = _txId;
      final wire = [
        ..._frag(
          tx,
          0,
          2,
          data: List<int>.generate(112, (i) => i & 0xFF),
          start: true,
        ).toBytes(),
        ..._frag(tx, 1, 2, data: [7, 8, 9], stop: true).toBytes(),
      ];
      // The parser splits the concatenated wire bytes back into the two frames.
      final parsed = PacketStreamParser().feed(wire);
      expect(parsed, hasLength(2));

      final r = FragmentReassembler()..begin(tx);
      FragmentEvent? last;
      for (final frame in parsed) {
        last = r.feed(tx, frame);
      }
      expect(last, isA<FragmentComplete>());
      expect((last as FragmentComplete).payload, [
        ...List<int>.generate(112, (i) => i & 0xFF),
        7,
        8,
        9,
      ]);
    });

    test('the STOP frame carries the SUCCESS/FAIL flags', () {
      final r = FragmentReassembler()..begin(_txId);
      r.feed(_txId, _frag(_txId, 0, 2, data: [1], start: true));
      final event = r.feed(
        _txId,
        _frag(_txId, 1, 2, data: [2], stop: true, fail: true),
      );
      expect((event as FragmentComplete).fail, isTrue);
    });

    test('a stream that starts at a non-zero fragment aborts', () {
      final r = FragmentReassembler()..begin(_txId);
      final event = r.feed(_txId, _frag(_txId, 2, 5, data: [1]));
      expect(event, isA<FragmentAbort>());
      expect((event as FragmentAbort).reason, contains('starts at fragment 2'));
    });

    test('an out-of-order fragment aborts', () {
      final r = FragmentReassembler()..begin(_txId);
      expect(
        r.feed(_txId, _frag(_txId, 0, 3, data: [0])),
        isA<FragmentPending>(),
      );
      // Fragment 1 is lost; fragment 2 arrives next.
      final event = r.feed(_txId, _frag(_txId, 2, 3, data: [2]));
      expect(event, isA<FragmentAbort>());
      expect((event as FragmentAbort).reason, contains('out of order'));
    });

    test('a changed fragment total aborts', () {
      final r = FragmentReassembler()..begin(_txId);
      r.feed(_txId, _frag(_txId, 0, 3, data: [0]));
      final event = r.feed(_txId, _frag(_txId, 1, 9, data: [1]));
      expect(event, isA<FragmentAbort>());
    });

    test('a FRAG frame with no fragmentation info aborts', () {
      final r = FragmentReassembler()..begin(_txId);
      final short = PacketFrame(
        flags: flagFrag | flagType,
        priority: defaultPriority,
        idTarget: 0x0001,
        idSource: 0x0001,
        srvTarget: makeService(ServiceType.script, 0),
        srvSource: _txId,
        payload: Uint8List.fromList([1, 2]), // fewer than 4 info bytes
      );
      final event = r.feed(_txId, short);
      expect(event, isA<FragmentAbort>());
      expect(
        (event as FragmentAbort).reason,
        contains('without fragment info'),
      );
    });

    test('an aborted or completed reply leaves no buffered state', () {
      final r = FragmentReassembler();
      // Abort mid-stream...
      r.feed(_txId, _frag(_txId, 0, 3, data: [0]));
      r.feed(_txId, _frag(_txId, 5, 3, data: [5]));
      // ...and a fresh stream from fragment 0 reassembles cleanly afterwards.
      expect(
        r.feed(_txId, _frag(_txId, 0, 2, data: [9], start: true)),
        isA<FragmentPending>(),
      );
      final event = r.feed(_txId, _frag(_txId, 1, 2, data: [8], stop: true));
      expect((event as FragmentComplete).payload, [9, 8]);
    });

    test('reassembles a stream without an explicit begin', () {
      final r = FragmentReassembler();
      expect(
        r.feed(_txId, _frag(_txId, 0, 2, data: [1, 2], start: true)),
        isA<FragmentPending>(),
      );
      final event = r.feed(_txId, _frag(_txId, 1, 2, data: [3], stop: true));
      expect((event as FragmentComplete).payload, [1, 2, 3]);
    });

    test('keeps two concurrent replies on separate transaction IDs apart', () {
      final r = FragmentReassembler();
      r.feed(1, _frag(1, 0, 2, data: [1], start: true));
      r.feed(2, _frag(2, 0, 2, data: [10], start: true));
      expect(
        (r.feed(1, _frag(1, 1, 2, data: [2], stop: true)) as FragmentComplete)
            .payload,
        [1, 2],
      );
      expect(
        (r.feed(2, _frag(2, 1, 2, data: [20], stop: true)) as FragmentComplete)
            .payload,
        [10, 20],
      );
    });
  });
}
