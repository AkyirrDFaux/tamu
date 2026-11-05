/// The SNDB roster is a STREAM (Device CID 3), and the app must read it whole or say why.
///
/// Captured on the rig on 2026-11-05 (`app/.dart_tool/hermes/sndb_probe.py`, an independent
/// probe that speaks the App Interface framing): SNDB Read All answers with **one FRAG frame,
/// fragment 0 of 1, 52 payload bytes (4 frag info + 48 body), STOP set**, and the 48-byte body is
/// the three DAS records of this rig:
///
///     2 / CDABA26400000000000000000000
///     3 / CDABBD6400000000000000000000
///     4 / CDABA06400000000000000000000
///
/// A bigger network is more fragments of the same stream, so the same rule has to hold: a reply
/// with a hole or without its STOP is a FAILED read, re-requested whole, never parsed into a
/// shorter roster - nodes that are alive would read as absent.
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/fragment_reassembler.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/sndb.dart';

/// The three records the rig's core answered with on 2026-11-05, as the probe printed them.
const List<String> _rigSerials = [
  'CDABA26400000000000000000000',
  'CDABBD6400000000000000000000',
  'CDABA06400000000000000000000',
];

List<int> _record(String serial, int id) => [
  for (var i = 0; i < 14; i++) int.parse(serial.substring(i * 2, i * 2 + 2), radix: 16),
  id & 0xFF,
  (id >> 8) & 0xFF,
];

final List<int> _rigBody = [
  for (var i = 0; i < _rigSerials.length; i++) ..._record(_rigSerials[i], i + 2),
];

/// The rig's whole reply: one fragment that is also the last one.
PacketResponse _whole(List<int> body) => PacketResponse(
  payload: body,
  success: true,
  fail: false,
  chunks: [FragmentChunk(0, body)],
  stopped: true,
  total: 1,
);

/// A reply that announced [total] fragments, delivered [delivered] of them, and then went quiet:
/// the link's stall watchdog closes it, so `stopped` is false and the missing indexes are known.
PacketResponse _truncated(int total, List<int> deliveredIndexes) {
  final body = <int>[];
  for (final i in deliveredIndexes) {
    body.addAll([i, i]); // a fragment's body is placed by index, the bytes do not matter here
  }
  final missing = [
    for (var i = 0; i < total; i++)
      if (!deliveredIndexes.contains(i)) i,
  ];
  return PacketResponse(
    payload: body,
    success: true,
    fail: false,
    chunks: [
      for (final i in deliveredIndexes) FragmentChunk(i, [i, i]),
    ],
    missing: missing,
    stopped: false,
    total: total,
  );
}

/// A channel that answers the roster read from a list of replies (the last one repeats); a null
/// entry is "the device did not answer" and is thrown, as a request timeout is.
class _RosterChannel implements TolerantRequestChannel {
  _RosterChannel(this.replies);

  final List<PacketResponse?> replies;
  final List<int> asked = [];

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
    asked.add(functionCid);
    final reply = replies[asked.length - 1 < replies.length
        ? asked.length - 1
        : replies.length - 1];
    if (reply == null) throw StateError('device CID $functionCid request timed out');
    return reply;
  }
}

void main() {
  tearDown(() => installRequestChannel(null));

  test('the rig\'s 1-fragment roster reads as three records on the first attempt', () async {
    final channel = _RosterChannel([_whole(_rigBody)]);
    installRequestChannel(channel);

    final read = await readSndbRoster();

    expect(read.ok, isTrue);
    expect(read.attempts, 1);
    expect(read.fault, isNull);
    expect(channel.asked, [0x13]);
    expect(
      [for (final e in read.entries!) '${e.id}/${e.serial}'],
      ['2/${_rigSerials[0]}', '3/${_rigSerials[1]}', '4/${_rigSerials[2]}'],
    );
  });

  test('a roster that lost a fragment is re-requested whole, then read', () async {
    final channel = _RosterChannel([_truncated(3, [0, 1]), _whole(_rigBody)]);
    installRequestChannel(channel);

    final read = await readSndbRoster();

    expect(read.ok, isTrue);
    expect(read.attempts, 2, reason: 'the whole roster is asked for again');
    expect(channel.asked, [0x13, 0x13]);
    expect(read.entries!.map((e) => e.id), [2, 3, 4]);
  });

  test('a roster that never arrives whole is refused, never parsed short', () async {
    // 3 fragments declared, 2 delivered, no STOP - a network that is really four nodes would
    // otherwise render as three.
    final channel = _RosterChannel([_truncated(3, [0, 1])]);
    installRequestChannel(channel);

    final read = await readSndbRoster();

    expect(read.ok, isFalse);
    expect(read.entries, isNull);
    expect(read.attempts, sndbReadAttempts);
    expect(
      channel.asked.length,
      sndbReadAttempts,
      reason: 'the read is sent exactly sndbReadAttempts times, then given up',
    );
    expect(
      read.fault,
      'fragment 2 of 3 never arrived (1 of 3 missing, 2 received); '
      'the stream stopped without its STOP frame',
    );
  });

  test('a body short of whole records fails instead of dropping the tail', () async {
    final channel = _RosterChannel([_whole(_rigBody.sublist(0, 40))]);
    installRequestChannel(channel);

    final read = await readSndbRoster();

    expect(read.ok, isFalse);
    expect(read.attempts, sndbReadAttempts);
    expect(
      read.fault,
      'the reply is 40 byte(s), which is not a whole number of 16-byte records',
    );
  });

  test('an empty reply is an empty registry, not a failed read', () async {
    // A core with nothing registered answers with an empty stream on purpose
    // (firmware `HandleSNDB`: `SendResponse(Frame, nullptr, 0)` when the count is 0).
    final channel = _RosterChannel([_whole(const [])]);
    installRequestChannel(channel);

    final read = await readSndbRoster();

    expect(read.ok, isTrue);
    expect(read.entries, isEmpty);
    expect(read.attempts, 1);
    expect(channel.asked.length, 1);
  });

  test('with no link the roster is not retried', () async {
    final read = await readSndbRoster();

    expect(read.ok, isFalse);
    expect(read.attempts, 0);
    expect(read.fault, 'there is no link to the device');
  });

  test('a device that never answers is given up after sndbReadAttempts sends', () async {
    final channel = _RosterChannel([null]);
    installRequestChannel(channel);

    final read = await readSndbRoster();

    expect(read.ok, isFalse);
    expect(channel.asked.length, sndbReadAttempts);
    expect(read.fault, contains('the device did not answer'));
  });
}
