/// Storage service client (Docs/Services/Storage.md): file table access and
/// file create/delete/resize/rename/read/write.
library;

import 'dart:typed_data';

import 'diagnostics.dart';
import 'protocol.dart';
import 'request_channel.dart';
import 'service_request.dart';
import 'types.dart';

/// The single file-name normalizer: strips only the 8-byte wire padding (trailing
/// spaces or, from older renames, NULs), never interior characters, so a stored name
/// such as `A B` survives intact.
String normalizeFileName(String name) {
  var end = name.length;
  while (end > 0) {
    final c = name.codeUnitAt(end - 1);
    if (c == 0x20 || c == 0x00) {
      end--;
    } else {
      break;
    }
  }
  return name.substring(0, end);
}

/// One file table record (FileRecord: Offset u32 | Filesize u32 | Name 8 bytes).
class FileRecord {
  final int index;
  final int offset;
  final int size;
  final String name;

  FileRecord({
    required this.index,
    required this.offset,
    required this.size,
    required this.name,
  });

  bool get isFiletable => index == 0;
}

/// One file reply placed by fragment index (see [StorageClient.placeFragments]).
class PlacedFile {
  /// The file's bytes, from offset 0 up to the first missing fragment.
  final List<int> bytes;

  /// How many fragments the prefix covers (1 for a single-packet reply).
  final int fragmentsUsed;

  /// How many gaps the reply had after the prefix - the bytes that are NOT in [bytes].
  final int holes;

  const PlacedFile({
    required this.bytes,
    required this.fragmentsUsed,
    required this.holes,
  });
}

class StorageClient {
  final int deviceId;

  StorageClient({required this.deviceId});

  /// The first failure of this client's requests since the caller last cleared it, so a caller that
  /// must report WHY a file write did not land can say what the device answered (see
  /// [RequestTrace]).
  final RequestTrace trace = RequestTrace();

  Future<List<int>?> _request(
    int cid, {
    List<int> payload = const [],
    Duration? timeout,
    bool requestFrag = false,
  }) => serviceRequest(
    deviceId,
    ServiceType.storage,
    cid,
    payload: payload,
    timeout: timeout,
    // Mutating ops (create/resize/delete) trigger flash erases on slow nodes and can
    // legitimately exceed the default transaction timeout.
    defaultTimeout: const Duration(seconds: 6),
    requestFrag: requestFrag,
    tag: 'storage',
    trace: trace,
  );

  /// The firmware answers status commands with the packet success/fail flags instead of a
  /// payload, so the outcome is in the frame rather than the bytes.
  Future<bool> _status(
    int cid, {
    List<int> payload = const [],
    Duration? timeout,
    bool requestFrag = false,
  }) async {
    final link = requestChannel;
    if (link == null) {
      AppDiagnostics.log('storage', 'no link');
      trace.record(
        RequestFault(
          kind: RequestFaultKind.noLink,
          deviceId: deviceId,
          service: ServiceType.storage,
          cid: cid,
        ),
      );
      return false;
    }
    try {
      final response = await link.requestWithFlags(
        deviceId,
        ServiceType.storage,
        cid,
        payload: payload,
        timeout: timeout ?? const Duration(seconds: 6),
        requestFrag: requestFrag,
      );
      // A FAIL is the device saying no to this command; the flag is the only status it sends.
      if (response.fail) {
        trace.refused(
          deviceId,
          service: ServiceType.storage,
          cid: cid,
          payload: response.payload,
        );
      }
      return response.success;
    } catch (error) {
      AppDiagnostics.log('storage', 'request failed: $error');
      trace.record(
        RequestFault(
          kind: RequestFaultKind.noReply,
          deviceId: deviceId,
          service: ServiceType.storage,
          cid: cid,
          detail: '$error',
        ),
      );
      return false;
    }
  }

  static const nameLength = 8;

  /// The documented "maximum 64 byte stream fragment" content cap (Docs/Services/Storage.md).
  static const fragmentContentMax = 64;

  /// Pads/truncates a file name to the wire format (8 bytes, null padded).
  static Uint8List padName(String name) {
    final bytes = Uint8List(nameLength)..fillRange(0, nameLength, 0x00);
    final raw = name.codeUnits;
    for (var i = 0; i < nameLength && i < raw.length; i++) {
      bytes[i] = raw[i] & 0xFF;
    }
    return bytes;
  }

  static String unpadName(List<int> bytes) =>
      normalizeFileName(String.fromCharCodes(bytes.take(nameLength)));

  /// Strips the per-fragment name echo from a reassembled CID-5 stream (B12). Every fragment
  /// is [name (8)][content, at most fragmentContentMax] and the reassembly layer already
  /// removed the 4-byte frag info, so the groups are walked from the front (each fragment but
  /// the last is full).
  static List<int> stripFragmentNames(List<int> reply) {
    final out = <int>[];
    var pos = 0;
    while (pos + nameLength <= reply.length) {
      pos += nameLength; // drop this fragment's name echo
      final end = (pos + fragmentContentMax > reply.length)
          ? reply.length
          : pos + fragmentContentMax;
      out.addAll(reply.sublist(pos, end));
      pos = end;
    }
    return out;
  }

  /// Reads the file table by reading the ".TABLE  " file directly (CID 5).
  /// The file table is self-describing: the first entry points to itself with its size.
  ///
  /// The read is sent tolerantly ([TolerantRequestChannel.requestTolerant]) so a reply that lost
  /// fragments still ARRIVES - the core answers this file as a 16 kB stream of 256 fragments and
  /// over BLE its transmit ring drops whole frames (measured on the rig: 218-223 of 256 and no
  /// STOP frame). A partial stream is never decoded here: [readFileTable] sees the gap, logs the
  /// hole's index and RE-REQUESTS the whole file (see [_readWhole], [readAttempts] sends in
  /// total). A truncated table parsed from a prefix would show 32 of N files as if it were the
  /// device's directory, which is worse than an error (Akyirr, 2026-11-04: "the app must process
  /// it correctly no matter what").
  ///
  /// Returns null only when no complete table could be read - no link, a device that does not
  /// answer, or every attempt truncated. [lastReadError] then says which, in one sentence the
  /// page can show.
  Future<List<FileRecord>?> readFileTable() async {
    const tableName = '.TABLE  ';
    final read = await _readWhole(tableName);
    if (read.fault != null) {
      _recordReadFault(read, cid: 5, detail: read.fault!);
      lastReadError =
          'The device could not send its file table: ${read.fault}. '
          'It was asked $lastReadAttempts time(s); try refreshing, or '
          'move closer if the link is Bluetooth.';
      AppDiagnostics.log('storage', 'file table: ${lastReadError!}');
      return null;
    }
    final contents = read.placed!.bytes;
    final records = parseFileTable(contents);
    AppDiagnostics.log(
      'storage',
      'file table: ${records.length} record(s) from '
          '${read.placed!.fragmentsUsed} fragment(s), '
          '${contents.length} byte(s), attempt $lastReadAttempts/$readAttempts',
    );
    return records;
  }

  /// Storage CID 5 for one file name, through whichever request the channel offers.
  ///
  /// The app's connection implements [TolerantRequestChannel], which is what lets a 16 kB reply
  /// arrive at all when the link drops fragments; any other channel gets the plain request, whose
  /// reply is one unfragmented payload (or throws, which the retry loop treats as a failed
  /// attempt).
  Future<PacketResponse> _readFileReply(
    RequestChannel channel,
    String name,
  ) async {
    if (channel is TolerantRequestChannel) {
      return channel.requestTolerant(
        deviceId,
        ServiceType.storage,
        5,
        payload: padName(name),
        timeout: const Duration(seconds: 10),
      );
    }
    final payload = await channel.request(
      deviceId,
      ServiceType.storage,
      5,
      payload: padName(name),
      timeout: const Duration(seconds: 10),
    );
    return PacketResponse(payload: payload, success: true, fail: false);
  }

  /// How many times one whole-file read is sent: the first attempt plus two re-requests.
  ///
  /// Storage CID 5 asks a file BY NAME and has no offset form, so a re-request is the whole file
  /// again - there is nothing cheaper to retry. Three sends is the owner's "re-request if
  /// something fails" made bounded: a truncated 16 kB reply is cut by the link's 500 ms stall
  /// watchdog ([ConnectionManager.fragmentStallTimeout]), so the whole policy costs ~2 s over BLE
  /// before the error is shown instead of a hang.
  static const int readAttempts = 3;

  /// The pause before each re-request; [readAttempts] - 1 entries. Short, because the failure it
  /// recovers from is a busy transmit ring on the device, not a device that has gone away.
  static const List<Duration> readRetryBackoff = [
    Duration(milliseconds: 200),
    Duration(milliseconds: 600),
  ];

  /// Reads one file whole, with the recovery policy.
  ///
  /// An attempt fails when the reply lost a fragment, when its STOP frame never came, when the
  /// placed bytes are shorter than [size], or when the request itself threw. A failed attempt is
  /// logged with the hole's index and the whole file is asked for again after the matching
  /// [readRetryBackoff] pause; after [readAttempts] sends the last fault is returned and the
  /// caller surfaces it. This never waits forever: every attempt is bounded by the request
  /// timeout and a stream that stops by the stall watchdog.
  Future<({PacketResponse? reply, PlacedFile? placed, String? fault})>
  _readWhole(String name, {int? size}) async {
    lastReadAttempts = 0;
    lastReadError = null;
    final channel = requestChannel;
    // A read that cannot go out is not retried: there is no link, and three timeouts would only
    // make the screen wait longer for the same sentence. (A link that DROPS mid-read is a failed
    // attempt like any other - the request throws.)
    if (channel == null || !channel.isConnected) {
      return (
        reply: null,
        placed: null,
        fault: 'there is no link to the device',
      );
    }
    PacketResponse? reply;
    String? fault;
    for (var attempt = 1; attempt <= readAttempts; attempt++) {
      lastReadAttempts = attempt;
      reply = null;
      fault = null;
      try {
        reply = await _readFileReply(channel, name);
      } catch (error) {
        fault = 'the device did not answer ($error)';
      }
      PlacedFile? placed;
      if (fault == null) {
        final gap = reply!.gap;
        if (gap != null) {
          fault = gap.message;
        } else {
          placed = placeFragments(reply);
          if (placed == null || placed.bytes.isEmpty) {
            fault = 'the reply carried no file bytes';
          } else if (size != null && placed.bytes.length < size) {
            // A gap-free fragment set can still come up short of the record's size when the
            // device answered with a prefix of the file: same loss, same recovery.
            fault =
                'only ${placed.bytes.length} of $size byte(s) arrived '
                '(${placed.fragmentsUsed} fragment(s))';
          }
        }
      }
      if (fault == null) {
        AppDiagnostics.log(
          'storage',
          'read "$name": complete on attempt $attempt/$readAttempts - '
              '${_fragmentSummary(reply!)}',
        );
        return (reply: reply, placed: placed, fault: null);
      }
      final more = attempt < readAttempts;
      final wait = more ? readRetryBackoff[attempt - 1] : Duration.zero;
      AppDiagnostics.log(
        'storage',
        'read "$name": attempt $attempt/$readAttempts failed - $fault'
            '${more ? '; re-requesting the whole file in '
                      '${wait.inMilliseconds} ms' : '; giving up'}',
      );
      if (more) await Future<void>.delayed(wait);
    }
    return (reply: reply, placed: null, fault: fault);
  }

  /// `256 of 256 fragments, STOP received, 16384 byte(s)` - the shape a rig log quotes, so a
  /// run's fragment counts can be read against the reply they belong to.
  static String _fragmentSummary(PacketResponse reply) {
    final bytes = reply.payload.length;
    if (reply.chunks.isEmpty) return 'single packet, $bytes byte(s)';
    return '${reply.chunks.length} of ${reply.total} fragments, '
        'STOP received, $bytes byte(s)';
  }

  /// Records why a read ended without data, on the client's [trace] (the restore report's reason).
  void _recordReadFault(
    ({PacketResponse? reply, PlacedFile? placed, String? fault}) read, {
    required int cid,
    required String detail,
  }) {
    if (read.reply == null) {
      trace.record(
        RequestFault(
          kind: RequestFaultKind.noReply,
          deviceId: deviceId,
          service: ServiceType.storage,
          cid: cid,
          detail: detail,
        ),
      );
      return;
    }
    trace.unusable(
      deviceId,
      service: ServiceType.storage,
      cid: cid,
      payload: read.reply!.payload,
      detail: detail,
    );
  }

  /// The file bytes a fragmented reply carries, placed by fragment index.
  ///
  /// Each fragment is `[name echo (8)][content, at most [fragmentContentMax]]` after the 4-byte
  /// fragmentation info, and fragment `i` covers content bytes
  /// `[i * fragmentContentMax, i * fragmentContentMax + length)`. Placing by index is what makes
  /// a reply with a lost fragment measurable: the bytes after the hole keep their offsets instead
  /// of shifting left by the lost fragment's length. [bytes] stops at the first hole, so nothing
  /// is ever parsed across one; [hole index][holes] is left for the caller's log, and a whole-file
  /// read treats a non-empty [PlacedFile.holes] or a short body as a failure to be re-requested.
  PlacedFile? placeFragments(PacketResponse response) {
    final chunks = response.chunks;
    if (chunks.isEmpty) {
      // A single-packet reply: no fragmentation info, no name echo to strip.
      return PlacedFile(
        bytes: stripFragmentNames(response.payload),
        fragmentsUsed: 1,
        holes: 0,
      );
    }
    final byIndex = <int, List<int>>{};
    for (final chunk in chunks) {
      if (chunk.body.length < nameLength) continue;
      byIndex[chunk.index] = chunk.body.sublist(nameLength);
    }
    if (byIndex.isEmpty) return null;
    final out = <int>[];
    var index = 0;
    while (byIndex.containsKey(index)) {
      out.addAll(byIndex[index]!);
      index++;
    }
    // Every received fragment past the first gap starts a new run; the run count minus one is
    // the number of holes that were cut off.
    var runs = 0;
    var previous = -2;
    for (final i in byIndex.keys.toList()..sort()) {
      if (i > index && i != previous + 1) runs++;
      previous = i;
    }
    return PlacedFile(bytes: out, fragmentsUsed: index, holes: runs);
  }

  /// How many times the last whole-file read was sent (1 = it arrived whole first time).
  int lastReadAttempts = 0;

  /// Why the last whole-file read did not produce a complete file, in one sentence a page can
  /// show - null when it did. A truncated reply is never rendered as data: the read is
  /// re-requested ([readAttempts] sends) and only then does this get a value.
  String? lastReadError;

  /// Parses 16-byte file-table records. Unwritten entries are all 0xFF and terminate the
  /// table. A record with offset 0 is a superseded/invalidated entry (rename/delete/table
  /// moves zero the 4-byte offset but leave the size), so it is skipped - on every device,
  /// since all targets run the full multi-file filesystem now.
  static List<FileRecord> parseFileTable(List<int> contents) {
    final records = <FileRecord>[];
    for (var offset = 0; offset + 16 <= contents.length; offset += 16) {
      final recOffset = uint32FromBytes(contents, offset);
      final size = uint32FromBytes(contents, offset + 4);
      if (recOffset == 0xFFFFFFFF && size == 0xFFFFFFFF) break;
      if (recOffset == 0) continue; // invalidated record
      records.add(
        FileRecord(
          index: records.length,
          offset: recOffset,
          size: size,
          name: unpadName(contents.sublist(offset + 8)),
        ),
      );
    }
    return records;
  }

  /// Creates a file per docs 03.01 CID1
  Future<bool> createFile(String name, int size) async {
    final payload = <int>[...padName(name), ...uint32ToBytes(size)];
    return _status(1, payload: payload);
  }

  Future<bool> deleteFile(String name) async {
    return _status(2, payload: padName(name));
  }

  Future<bool> renameFile(String oldName, String newName) async {
    final payload = <int>[...padName(oldName), ...padName(newName)];
    return _status(4, payload: payload);
  }

  /// Resizes a file per Docs 03.03 CID3.
  Future<bool> resizeFile(String name, int size) async {
    final payload = <int>[...padName(name), ...uint32ToBytes(size)];
    return _status(3, payload: payload);
  }

  /// Formats the whole filesystem per docs 03.00 CID0 (wipes every file).
  Future<bool> format() async {
    return _status(0);
  }

  /// Reads the whole file per docs 03.05 CID5
  ///
  /// A file is only usable when EVERY byte arrived: unlike a short status reply, a layout or a
  /// script file with a hole in the middle cannot be decoded. A reply that lost a fragment, lost
  /// its STOP frame or came up short of [size] therefore fails that attempt, and the whole file is
  /// requested again ([readAttempts] sends, [readRetryBackoff] apart). When every attempt fails
  /// this returns null with the reason in [lastReadError] - never a shifted or truncated body.
  Future<List<int>?> readFile(String name, {int? size}) async {
    final read = await _readWhole(name, size: size);
    if (read.fault != null) {
      _recordReadFault(read, cid: 5, detail: read.fault!);
      lastReadError =
          'Could not read "$name": ${read.fault}. '
          'It was asked $lastReadAttempts time(s); try again.';
      AppDiagnostics.log('storage', 'read "$name": ${lastReadError!}');
      return null;
    }
    var contents = read.placed!.bytes;
    if (size != null && contents.length > size) {
      contents = contents.sublist(0, size);
    }
    return contents;
  }

  /// Writes a whole file per docs 03.06 CID6. Stream fragments are capped at 64
  /// content bytes (docs: "maximum 64 byte stream fragment"); every fragment carries
  /// the 4-byte frag info so the device can detect out-of-order delivery.
  ///
  /// The new content is staged under a temporary name first and only swapped in after a
  /// successful write, so a failed create/write cannot destroy the original.
  Future<bool> writeFile(String name, List<int> bytes) async {
    final temp = _tempName(name);
    await deleteFile(temp); // clear a stale temp from a previous failed write
    if (!await createFile(temp, bytes.length)) return false;
    if (!await _writeFragments(temp, bytes)) {
      await deleteFile(temp);
      return false;
    }
    // Replace only once the new content is safely staged. Try the rename first: the device
    // replaces an existing name atomically, so a failure leaves the original file untouched.
    // Only when that is refused - a device with no spare table record cannot rename onto an
    // existing name - is the name freed first, which is the one case where a failure here can
    // cost the original.
    if (await renameFile(temp, name)) return true;
    await deleteFile(name);
    if (!await renameFile(temp, name)) {
      await deleteFile(temp);
      return false;
    }
    return true;
  }

  /// The CID-6 fragment stream; every fragment carries the file name (B12).
  Future<bool> _writeFragments(String name, List<int> bytes) async {
    const dataMax = fragmentContentMax;
    var next = 0;
    var offset = 0;
    while (offset < bytes.length) {
      final end = (offset + dataMax > bytes.length)
          ? bytes.length
          : offset + dataMax;
      final payload = <int>[
        ...writeFragInfo(next, 0xFFFF),
        ...padName(name), // every fragment carries the name
        ...bytes.sublist(offset, end),
      ];
      final reply = await _request(6, payload: payload, requestFrag: true);
      if (reply == null || reply.length < 2) {
        if (reply != null) {
          trace.unusable(
            deviceId,
            service: ServiceType.storage,
            cid: 6,
            payload: reply,
            detail:
                'a write fragment is answered with the u16 sequence it '
                'accepted; this reply has ${reply.length} byte(s)',
          );
        }
        return false;
      }
      final lastSeq = reply[0] | (reply[1] << 8);
      if (lastSeq == 0xFFFF) {
        // The device's own "I have no writable target for this fragment" answer.
        trace.unusable(
          deviceId,
          service: ServiceType.storage,
          cid: 6,
          payload: reply,
          detail:
              'the device answered 0xFFFF: no writable target for the '
              'fragment',
        );
        return false;
      }
      next = lastSeq + 1;
      offset = end;
    }
    return true;
  }

  /// A staging name that fits the 8-byte wire field and is unlikely to collide.
  static String _tempName(String name) {
    final base = name.length <= 7 ? name : name.substring(0, 7);
    return '~$base';
  }
}
