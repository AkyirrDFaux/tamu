/// The device the loading/lifecycle page suites read from.
///
/// `loading_states_test` and `loading_exits_test` each declared their own fake - one a `_Device`
/// with an `_Answer` knob per read, the other a `_Bus` with a `bool` per read - and the two
/// answered identically: the file table (Storage CID 5) from a `.TABLE` body, the loaded slot map
/// (Script CID 0) as an empty one, a script load (Script CID 1) as accepted or refused, and the
/// roster (Device CID 0x13) as the nodes it knows. They differed only in which knob each happened
/// to have, which is how a fake drifts: one suite's "device" answering a read the other's refused.
///
/// A suite now names the answer it measures with [ReadAnswer] (a read that answers, or one that
/// never comes back) and passes its own `.TABLE` body.
library;

import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/storage_client.dart';

import 'fake_channel.dart';

/// Whether a read answers, or comes back as nothing at all - which is the shape a lost reply and a
/// timeout both take at the request layer (the request throws).
enum ReadAnswer { whole, lost }

/// A device that answers the four reads these pages make, and nothing else: the file table
/// (Storage CID 5), the script slot map (Script CID 0), a script load (Script CID 1) and the
/// roster (Device CID 0x13). Every other request answers an empty success, which the clients read
/// as "the device said nothing usable" rather than as a lost reply.
class ReadDevice extends FakeTolerantChannel {
  ReadDevice({
    required List<int> table,
    List<int>? script,
    List<({String serial, int id})> nodes = const [],
    this.tableAnswer = ReadAnswer.whole,
    this.slotsAnswer = ReadAnswer.whole,
    this.rosterAnswer = ReadAnswer.whole,
    this.loadFails = false,
    this.delay = Duration.zero,
  }) : _table = table,
       _script = script,
       _nodes = nodes;

  /// The bytes a `.TABLE` read answers with: one record per file the device holds.
  final List<int> _table;

  /// The bytes a `SCR_000` read answers with, or null when the device holds no such file.
  final List<int>? _script;

  /// The nodes a roster read reports.
  final List<({String serial, int id})> _nodes;

  /// The file table's answer - [ReadAnswer.lost] for a device that never answered. Mutable
  /// because a case flips it mid-run: the failed read's own Retry has to be answered.
  ReadAnswer tableAnswer;

  /// The slot map's answer.
  final ReadAnswer slotsAnswer;

  /// The roster's answer.
  final ReadAnswer rosterAnswer;

  /// Script CID 1 (load into a slot) answers FAIL, which is a load the device refused.
  final bool loadFails;

  /// How long every answer takes; a page test that asserts its wait needs this to be non-zero.
  final Duration delay;

  /// How many `.TABLE` reads arrived (the client's re-requests included).
  int tableSends = 0;

  int _nextTxId = 0xF700;

  @override
  int takeTxId() => _nextTxId++;

  @override
  Future<PacketResponse> answer(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
  }) async {
    if (delay > Duration.zero) await Future<void>.delayed(delay);
    if (service == ServiceType.storage && functionCid == 5) {
      tableSends++;
      if (tableAnswer != ReadAnswer.whole) {
        throw StateError('the device did not answer');
      }
      final name = StorageClient.unpadName(payload.sublist(0, 8));
      final bytes = switch (name) {
        '.TABLE' => _table,
        'SCR_000' => _script ?? const <int>[],
        _ => const <int>[],
      };
      if (bytes.isEmpty) {
        // FLAG_FAIL with no payload: the name is not in its file table.
        return const PacketResponse(payload: [], success: false, fail: true);
      }
      return wholeReply([...StorageClient.padName(name), ...bytes]);
    }
    if (service == ServiceType.script && functionCid == 0) {
      if (slotsAnswer != ReadAnswer.whole) {
        throw StateError('the slot map did not answer');
      }
      // An empty slot map: every slot free, which is "nothing is loaded".
      return wholeReply(List<int>.filled(32, 0xFF));
    }
    if (service == ServiceType.script && functionCid == 1) {
      return PacketResponse(
        payload: const [],
        success: !loadFails,
        fail: loadFails,
      );
    }
    if (service == ServiceType.device && functionCid == 0x13) {
      if (rosterAnswer != ReadAnswer.whole) {
        throw StateError('the roster did not answer');
      }
      return wholeReply(rosterBody(_nodes));
    }
    return const PacketResponse(payload: [], success: true, fail: false);
  }
}

/// The roster as the core answers Device CID 0x13: 16 bytes per record - 14 serial bytes and the
/// node's id, little endian.
List<int> rosterBody(List<({String serial, int id})> nodes) => [
  for (final node in nodes) ...[
    for (var i = 0; i < 14; i++)
      int.parse(node.serial.substring(i * 2, i * 2 + 2), radix: 16),
    node.id & 0xFF,
    (node.id >> 8) & 0xFF,
  ],
];
