/// The core's Serial Number Database (SNDB): the network roster, and the writes that maintain
/// it (Docs/Services/System Block and Device Commands.md, 00.12/00.13).
///
/// Pure Dart, and the single place the SNDB record format is decoded: the app's in-RAM device
/// database and a restore's SNDB items both come through here.
library;

import 'diagnostics.dart';
import 'protocol.dart';
import 'request_channel.dart';
import 'service_request.dart';
import 'types.dart';

/// The SNDB roster as `[id, serial hex]` records, including id 0 (the unassigned marker -
/// callers filter it). Empty when the core does not answer.
Future<List<({int id, String serial})>> sndbEntries() async =>
    await readSndbEntries() ?? const [];

/// The SNDB roster, or null when the core does not answer.
///
/// [sndbEntries] flattens a lost reply to an empty roster, which is right for a caller that only
/// wants to know what the roster names. A caller that has to tell "the roster holds nothing" from
/// "the roster could not be read" - a restore plan's diff, which must not report a device as
/// missing from a roster nobody managed to look at - reads this instead, or [readSndbRoster] when
/// it has to say WHY.
Future<List<({int id, String serial})>?> readSndbEntries() async =>
    (await readSndbRoster()).entries;

/// The byte length of one roster record: `serial (14) | id (u16)`.
const int sndbRecordSize = 16;

/// How many times one roster read is sent: the first attempt plus two re-requests.
///
/// SNDB Read All (CID 3) is a STREAM - the device answers it with FRAG frames, not one packet
/// (`Docs/Services/System Block and Device Commands.md` , "Fragmentation, SN + ID (stream)"), and
/// like every streamed reply it can lose a fragment or stop before its STOP frame. A roster
/// parsed from a prefix is a SHORTER network: nodes that are alive read as absent, which is
/// exactly the failure this policy exists to prevent (the same ruling as the file reads:
/// "process it correctly no matter what - re-request if something fails, detect it").
const int sndbReadAttempts = 3;

/// The pause before each re-request; [sndbReadAttempts] - 1 entries. Short, because the failure
/// it recovers from is a lost frame, not a device that has gone away.
const List<Duration> sndbReadRetryBackoff = [
  Duration(milliseconds: 200),
  Duration(milliseconds: 600),
];

/// One roster read: the entries, how many sends it took, and why it failed when it did.
class SndbRosterRead {
  /// The roster, or null when no attempt produced a whole one.
  final List<({int id, String serial})>? entries;

  /// How many times the read was sent (0 when there was no link to send it on).
  final int attempts;

  /// One sentence saying why the roster could not be read, or null on success.
  final String? fault;

  const SndbRosterRead({
    required this.entries,
    required this.attempts,
    this.fault,
  });

  /// True when the roster was read whole.
  bool get ok => entries != null;
}

/// Reads the whole roster, re-requesting it while the reply is not whole.
///
/// An attempt fails when a fragment never arrived, when the stream stopped without its STOP
/// frame, when the body is not a whole number of [sndbRecordSize] records, or when the request
/// itself threw (the stall watchdog closes a stream that goes quiet, so this never waits for a
/// stream that is still arriving). The whole read is asked again after the matching
/// [sndbReadRetryBackoff] pause, and after [sndbReadAttempts] sends the last fault is returned.
///
/// A core with an empty registry answers with an empty stream on purpose, so an empty body is a
/// ROSTER THAT HOLDS NOTHING, not a failure.
Future<SndbRosterRead> readSndbRoster() async {
  final channel = requestChannel;
  // A read that cannot go out is not retried: three timeouts would only make the caller wait
  // longer for the same sentence.
  if (channel == null || !channel.isConnected) {
    return const SndbRosterRead(
      entries: null,
      attempts: 0,
      fault: 'there is no link to the device',
    );
  }
  PacketResponse? reply;
  String? fault;
  for (var attempt = 1; attempt <= sndbReadAttempts; attempt++) {
    reply = null;
    fault = null;
    try {
      reply = await _readRosterReply(channel);
    } catch (error) {
      fault = 'the device did not answer ($error)';
    }
    fault ??= _rosterFault(reply!);
    if (fault == null) {
      final entries = parseSndb(reply!.payload);
      AppDiagnostics.log(
        'sndb',
        'roster: ${entries.length} entr(y/ies) from ${_replySummary(reply)}, '
            'attempt $attempt/$sndbReadAttempts',
      );
      return SndbRosterRead(
        entries: entries,
        attempts: attempt,
        fault: null,
      );
    }
    final more = attempt < sndbReadAttempts;
    final wait = more ? sndbReadRetryBackoff[attempt - 1] : Duration.zero;
    AppDiagnostics.log(
      'sndb',
      'roster attempt $attempt/$sndbReadAttempts failed - $fault'
          '${more ? '; re-requesting the whole roster in ${wait.inMilliseconds} ms'
                  : '; giving up'}',
    );
    if (more) await Future<void>.delayed(wait);
  }
  return SndbRosterRead(
    entries: null,
    attempts: sndbReadAttempts,
    fault: fault,
  );
}

/// Device CID 3 (`0x13`) through whichever request the channel offers.
///
/// The app's connection implements [TolerantRequestChannel], which is what lets a roster that
/// lost a fragment arrive at all (the caller checks [PacketResponse.gap] and re-requests); any
/// other channel gets the plain request, whose reply is one payload or throws.
Future<PacketResponse> _readRosterReply(RequestChannel channel) async {
  if (channel is TolerantRequestChannel) {
    return channel.requestTolerant(
      coreId,
      ServiceType.device,
      0x13,
      timeout: const Duration(seconds: 5),
    );
  }
  final payload = await channel.request(
    coreId,
    ServiceType.device,
    0x13,
    timeout: const Duration(seconds: 5),
  );
  return PacketResponse(payload: payload, success: true, fail: false);
}

/// Why [reply] is not a whole roster, or null when it is.
String? _rosterFault(PacketResponse reply) {
  if (reply.fail) return 'the device refused the read (FAIL)';
  final gap = reply.gap;
  if (gap != null) return gap.message;
  final length = reply.payload.length;
  if (length % sndbRecordSize != 0) {
    return 'the reply is $length byte(s), which is not a whole number of '
        '$sndbRecordSize-byte records';
  }
  return null;
}

/// `1 of 1 fragment(s), STOP received, 48 byte(s)` - the shape a rig log quotes, so a run's
/// fragment counts can be read against the reply they belong to.
String _replySummary(PacketResponse reply) {
  final bytes = reply.payload.length;
  if (reply.chunks.isEmpty) return 'a single packet, $bytes byte(s)';
  return '${reply.chunks.length} of ${reply.total} fragment(s), '
      '${reply.stopped ? 'STOP received' : 'no STOP frame'}, $bytes byte(s)';
}

/// The single SNDB record parser: the 16-byte entries are `serial (14) | id (u16)`
/// little-endian. Every record is emitted (including id 0, the unassigned marker); callers
/// filter as needed.
List<({int id, String serial})> parseSndb(List<int> reply) => [
  for (var offset = 0; offset + sndbRecordSize <= reply.length;
      offset += sndbRecordSize)
    (
      id: reply[offset + 14] | (reply[offset + 15] << 8),
      serial: serialNumberToHex(reply.sublist(offset, offset + 14)),
    ),
];

/// SNDB write (00.12): registers [serial] under [id]. The core answers with the updated
/// record, so a reply of a full record is the acceptance signal.
///
/// [trace] is for a caller that reports WHY a write did not land: a short or empty answer where the
/// core echoes the record is recorded as a refusal, which is how the row is reported with a code
/// instead of only as a count.
Future<bool> sndbWrite(List<int> serial, int id, {RequestTrace? trace}) async {
  final reply = await serviceRequest(
    coreId,
    ServiceType.device,
    0x12,
    payload: [...serial, id & 0xFF, (id >> 8) & 0xFF],
    timeout: const Duration(seconds: 3),
    tag: 'sndb',
    trace: trace,
  );
  if (reply == null) return false;
  if (reply.length < 16) {
    trace?.unusable(
      coreId,
      service: ServiceType.device,
      cid: 0x12,
      payload: reply,
      detail:
          'an accepted write answers with the whole 16-byte record; this '
          'reply has ${reply.length} byte(s)',
    );
    return false;
  }
  return true;
}

/// SNDB delete (00.12 with ID 0): drops [serial]'s record.
Future<bool> sndbDelete(List<int> serial) async {
  if (serial.length < 14) return false;
  final reply = await serviceRequest(
    coreId,
    ServiceType.device,
    0x12,
    payload: [...serial.take(14), 0, 0],
    timeout: const Duration(seconds: 3),
    tag: 'sndb',
  );
  return reply != null && reply.length >= 16;
}
