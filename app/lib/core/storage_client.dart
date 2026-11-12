/// Storage service client (Docs/Services/Storage.md): file table access and
/// file create/delete/resize/rename/read/write.
library;

import 'dart:async';
import 'dart:typed_data';

import 'diagnostics.dart';
import 'protocol.dart';
import 'request_channel.dart';
import 'request_gate.dart';
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
  /// The record's position among the records the parse RETURNED, so a dropped
  /// invalidated record does not consume an index and the first returned record is 0.
  final int index;

  /// The record's own 16-byte slot in the table (0 = the table's own entry), which is
  /// what a table view numbers its rows with. It does NOT renumber when a record is
  /// dropped, so a listing can show the gap an invalidated record left behind.
  final int slot;

  final int offset;
  final int size;
  final String name;

  /// The table zeroed this record's offset: a rename, delete or table move superseded it.
  /// Only produced with `includeInvalidated` - see [StorageClient.parseFileTable].
  final bool invalidated;

  FileRecord({
    required this.index,
    required this.slot,
    required this.offset,
    required this.size,
    required this.name,
    this.invalidated = false,
  });

  bool get isFiletable => slot == 0;
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

/// Why a whole-file read produced no bytes.
///
/// The three cases are NOT the same thing and must never be collapsed into one (Akyirr,
/// 2026-11-05: "the app must process it correctly no matter what ... never present a failure as
/// data"). A page that renders [absent] as an error nags about a device that simply holds no such
/// file; a page that renders [partial] or [failed] as an empty list claims the device holds
/// nothing, which is worse than an error because it reads as device state.
enum FileReadFault {
  /// The device answered FAIL for the name, with no bytes at all: it holds no such file. This is
  /// DATA - an empty listing - so the read is NOT re-requested. The firmware's answer to a read of
  /// a name its file table does not carry (`Core/Services/Storage.h`, `BeginRead`).
  absent('the device holds no such file'),

  /// The file exists but the reply lost a fragment or its STOP frame, or came up short of the
  /// record's size. The bytes that arrived are never decoded as if they were the file.
  partial('the reply was incomplete'),

  /// Nothing usable came back: no link, no answer, or an answer this read could not use.
  failed('the read failed');

  const FileReadFault(this.label);

  /// The one wording a log line and a page both use for this case.
  final String label;
}

/// One whole-file read: the bytes when the device sent them whole, otherwise WHY not.
///
/// The single result type every page reads through - the Storage list, the Scripts page's
/// available list, the Register page's Backup view and the script editor - so "what does an empty
/// answer mean" is answered in one place instead of once per page.
class FileRead {
  /// The file name as asked for (unpadded).
  final String name;

  /// The whole file, or null when [fault] says why not.
  final List<int>? bytes;

  /// Null only when [bytes] is set.
  final FileReadFault? fault;

  /// One sentence a page can show: what failed, naming the file and - for a [FileReadFault.partial]
  /// reply - the fragment index that never arrived. Null when the read is whole.
  final String? detail;

  /// How many sends the bounded re-request policy used (1 = whole first time, 0 = never sent).
  final int attempts;

  const FileRead({
    required this.name,
    this.bytes,
    this.fault,
    this.detail,
    this.attempts = 0,
  });

  /// The device sent the whole file.
  bool get ok => bytes != null;

  /// The device holds no file by this name - a legitimate empty result, not a failure.
  bool get absent => fault == FileReadFault.absent;

  /// The file exists but the reply arrived incomplete; it was reported once and never decoded.
  bool get partial => fault == FileReadFault.partial;

  /// Nothing usable came back (no link, no answer, a refusal this read cannot read as "absent").
  bool get failed => fault == FileReadFault.failed;

  @override
  String toString() => ok
      ? 'FileRead("$name", ${bytes!.length} byte(s), attempt $attempts)'
      : 'FileRead("$name", ${fault!.name}: $detail)';
}

/// The file table, read through the same policy as [FileRead]: the records when the `.TABLE` file
/// arrived whole, otherwise which of the three cases it was - an absent table (a device with no
/// filesystem is an empty directory) or a read that failed / came back partial.
class FileTableRead {
  /// Every live record, or null when [fault] says why not.
  final List<FileRecord>? records;
  final FileReadFault? fault;

  /// One sentence a page can show (which file, which fragment if the reply was short).
  final String? detail;

  /// How many sends the bounded re-request policy used.
  final int attempts;

  const FileTableRead({
    this.records,
    this.fault,
    this.detail,
    this.attempts = 0,
  });

  bool get ok => records != null;
  bool get absent => fault == FileReadFault.absent;
  bool get partial => fault == FileReadFault.partial;
  bool get failed => fault == FileReadFault.failed;

  /// The records, or an empty list for a legitimately absent table. A failed or partial read
  /// returns an empty list here only if the caller ignored [fault] - the named getter is the
  /// honest one.
  List<FileRecord> get recordsOrEmpty => records ?? const <FileRecord>[];
}

/// How a [StorageClient.writeFile] put a file's content on the device.
///
/// The three are kept apart because they cost the device's FILE TABLE different amounts, and a
/// node's table is the thing that runs out: a DAS holds 8 sixteen-byte records, and every append
/// consumes one of them FOR GOOD - "Invalidated (0x00) holes are never reused"
/// (`firmware/src/Core/Functions/StorageBlockFS.h`). Creating a staging file and renaming it over
/// the target appends two records per write; a resize (Storage CID 3) appends one and keeps the
/// data area; a write into a file that is already the right length appends none.
enum FileWritePath {
  /// The file was already the right length: the fragments went into the file itself. No record.
  inPlace,

  /// Storage CID 3 put the file at its new length in place (verified by reading the size back) and
  /// the fragments went into it. One record, same data area.
  resize,

  /// The content was staged under a temporary name and renamed over the target. What a first
  /// creation takes, and the fallback for a refused resize or a failed in-place write. Two records.
  staged;

  /// How many file-table records this path appends on a device that never compacts, in one word a
  /// report can print.
  String get recordCost => switch (this) {
    FileWritePath.inPlace => 'no record',
    FileWritePath.resize => '1 record',
    FileWritePath.staged => '2 records',
  };
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
    } on RequestQueueTimeout catch (error) {
      // The link held every slot for the whole queue bound, so this command was never sent. That
      // is not a device verdict: the device was never asked (see [RequestQueueTimeout]).
      AppDiagnostics.log(
        'storage',
        'request $cid not sent - the link is busy: $error',
      );
      trace.record(
        RequestFault(
          kind: RequestFaultKind.busy,
          deviceId: deviceId,
          service: ServiceType.storage,
          cid: cid,
          detail: '$error',
        ),
      );
      return false;
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

  /// Reads the file table by reading the ".TABLE  " file directly (CID 5), through the shared
  /// whole-file reader, and returns the records or the reason there are none.
  ///
  /// The file table is self-describing: the first entry points to itself with its size.
  ///
  /// The read is sent tolerantly ([TolerantRequestChannel.requestTolerant]) so a reply that lost
  /// fragments still ARRIVES - the core answers this file as a 16 kB stream of 256 fragments and
  /// over BLE its transmit ring drops whole frames (measured on the rig: 218-223 of 256 and no
  /// STOP frame). A partial stream is never decoded here: [readFileTableResult] sees the gap, logs
  /// the hole's index and RE-REQUESTS the whole file (see [_readWhole], [readAttempts] sends in
  /// total). A truncated table parsed from a prefix would show 32 of N files as if it were the
  /// device's directory, which is worse than an error (Akyirr, 2026-11-04: "the app must process
  /// it correctly no matter what").
  ///
  /// Three outcomes, kept apart: [FileTableRead.records] (whole), [FileTableRead.absent] (the
  /// device holds no `.TABLE` file, which is an empty filesystem and renders as an empty list), and
  /// [FileTableRead.failed] / [FileTableRead.partial] (an error with the reason in
  /// [FileTableRead.detail], never an empty list).
  ///
  /// [attempts] is the re-send budget, [readAttempts] unless a caller is reading the table as a
  /// PROBE rather than for the user. `StorageClient.writeFile` is such a caller: it reads the table
  /// only to learn whether the file is already there and how long it is, and one attempt is enough
  /// - a probe that failed means the staging path runs, which needs no table at all. A probe at the
  /// full budget would add three request timeouts (30 s) to a single write on a node that answers
  /// nothing.
  Future<FileTableRead> readFileTableResult({
    void Function(int received, int total)? onProgress,
    bool Function()? isCancelled,
    int attempts = readAttempts,
  }) async {
    const tableName = '.TABLE  ';
    final read = await _readWhole(
      tableName,
      onProgress: onProgress,
      isCancelled: isCancelled,
      attempts: attempts,
    );
    if (read.kind == FileReadFault.absent) {
      final detail =
          'The device holds no file table: it reports an empty filesystem.';
      AppDiagnostics.log(
        'storage',
        'file table: ${FileReadFault.absent.label} '
            '($lastReadAttempts send(s), not retried)',
      );
      return FileTableRead(
        fault: FileReadFault.absent,
        detail: detail,
        attempts: lastReadAttempts,
      );
    }
    if (read.fault != null) {
      _recordReadFault(read, cid: 5, detail: read.fault!);
      lastReadError =
          'The device could not send its file table: ${read.fault}. '
          'It was asked $lastReadAttempts time(s).'
          '${_notResentNote(read.kind)} '
          'Try again, or move closer if the link is Bluetooth.';
      AppDiagnostics.log(
        'storage',
        'file table: FAILED (${read.kind?.label ?? FileReadFault.failed.label}) '
            '- ${lastReadError!}',
      );
      return FileTableRead(
        fault: read.kind ?? FileReadFault.failed,
        detail: lastReadError,
        attempts: lastReadAttempts,
      );
    }
    final contents = read.placed!.bytes;
    final records = parseFileTable(contents);
    AppDiagnostics.log(
      'storage',
      'file table: ${records.length} record(s) from '
          '${read.placed!.fragmentsUsed} fragment(s), '
          '${contents.length} byte(s), attempt $lastReadAttempts/$attempts',
    );
    return FileTableRead(records: records, attempts: lastReadAttempts);
  }

  /// The records, or null for BOTH an absent table and a failed read.
  ///
  /// Kept for the callers whose answer to "no table" and "unreadable table" is the same abort
  /// (the restore's snapshot sweeps, the CLI tools). A PAGE must use [readFileTableResult]:
  /// rendering a failed read as "there is nothing here" is the defect this card fixed.
  Future<List<FileRecord>?> readFileTable() async =>
      (await readFileTableResult()).records;

  /// Storage CID 5 for one file name, through whichever request the channel offers.
  ///
  /// The app's connection implements [TolerantRequestChannel], which is what lets a 16 kB reply
  /// arrive at all when the link drops fragments, and it hands back the packet's SUCCESS/FAIL
  /// flags - the device answers a name its file table does not carry with FLAG_FAIL and no
  /// payload, which is how [FileReadFault.absent] is told from a failed read.
  ///
  /// Any other channel gets the plain request, whose reply is one unfragmented payload (or
  /// throws, which the retry loop treats as a failed attempt). That form carries no flags, so an
  /// EMPTY reply is read as the same answer: the device has no bytes for this name. (The app's
  /// live link never takes this path; it is the CLI tools and the test fakes.)
  Future<PacketResponse> _readFileReply(
    RequestChannel channel,
    String name, {
    void Function(int received, int total)? onProgress,
  }) async {
    // A long reply is a fragment stream: when the channel can say how many fragments the device
    // declared and how many have arrived, publish that while the reply is still being reassembled
    // (a 16 kB `.TABLE` is 256 fragments over BLE). Only this read's own reply counts - the
    // channel is shared with the roster pings and the subscribe traffic around it.
    StreamSubscription<ReplyStreamProgress>? progress;
    if (onProgress != null && channel is ProgressReportingChannel) {
      progress = (channel as ProgressReportingChannel).streamProgress.listen((
        event,
      ) {
        if (event.deviceId != deviceId) return;
        if (event.service != ServiceType.storage || event.cid != 5) return;
        onProgress(event.received, event.total);
      });
    }
    try {
      if (channel is TolerantRequestChannel) {
        return await channel.requestTolerant(
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
      if (payload.isEmpty) {
        return const PacketResponse(payload: [], success: false, fail: true);
      }
      return PacketResponse(payload: payload, success: true, fail: false);
    } finally {
      await progress?.cancel();
    }
  }

  /// How many times one whole-file read is SENT when the attempt ended in
  /// [FileReadFault.failed]: the first attempt plus two re-sends.
  ///
  /// Storage CID 5 asks a file BY NAME and has no offset form, so a re-request is the whole file
  /// again - there is nothing cheaper to retry. What is retried is [FileReadFault.failed]: the
  /// send threw (the frame was lost, the link dropped mid-request), the reply carried no usable
  /// file bytes, or NOTHING of the reply ever arrived. That last one is the HEAD of a reply and it
  /// is the device's queue, not silence (`connection.dart` `fragmentStallTimeout`: the device
  /// forwards one request at a time, so a read can sit behind up to eight of them or behind a 25 s
  /// Save All), so it is bounded by the request deadline (10 s) and re-sent like any other failed
  /// attempt.
  ///
  /// A reply that came back SHORT or with a HOLE is NOT re-sent - see [_readWhole]: it arrived
  /// incomplete from the ring that just dropped it, and asking the same request again lands in the
  /// same ring. That is the one shape that is reported rather than retried.
  static const int readAttempts = 3;

  /// The pause before each re-send; [readAttempts] - 1 entries. Short, because the failure it
  /// recovers from is a lost frame on a link that is otherwise up.
  static const List<Duration> readRetryBackoff = [
    Duration(milliseconds: 200),
    Duration(milliseconds: 600),
  ];

  /// Reads one file whole, with the recovery policy.
  ///
  /// An attempt fails when the reply lost a fragment, when its STOP frame never came, when the
  /// placed bytes are shorter than [size], or when the request itself threw. Which of those is
  /// re-sent depends on [kind]: a `partial` reply (a hole, a missing STOP, a short body - a stream
  /// that STARTED and then stopped) is logged with the hole's index and returned after ONE send,
  /// while a `failed` attempt (the request threw, its reply carried no file bytes, or nothing of it
  /// ever arrived) is asked for again after the matching [readRetryBackoff] pause. After
  /// [readAttempts] sends the last fault is returned and the caller surfaces it. This never waits
  /// forever: every attempt is bounded by the request timeout, a stream that stops by the stream
  /// watchdog, and a link whose slots are all taken by the queue bound (which is reported as a busy
  /// link, not as a device that did not answer).
  ///
  /// [kind] says which of the three cases the attempt ended in: `absent` is the device answering
  /// FAIL with no bytes (the name is not in its file table) - a whole-file read does NOT retry
  /// that, because the answer will be the same three times; `partial` is a reply that started and
  /// lost a fragment / its STOP frame / its tail; `failed` is nothing usable at all.
  Future<
    ({
      PacketResponse? reply,
      PlacedFile? placed,
      String? fault,
      FileReadFault? kind,
      bool busy,
    })
  >
  _readWhole(
    String name, {
    int? size,
    void Function(int received, int total)? onProgress,
    bool Function()? isCancelled,
    int attempts = readAttempts,
  }) async {
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
        kind: FileReadFault.failed,
        busy: false,
      );
    }
    PacketResponse? reply;
    String? fault;
    FileReadFault? kind;
    for (var attempt = 1; attempt <= attempts; attempt++) {
      // A cancelled read sends nothing further: the page has stopped waiting and the user asked it
      // to stop, so only the attempt already in flight can still land.
      if (isCancelled?.call() ?? false) {
        return (
          reply: reply,
          placed: null,
          fault: 'the read was cancelled',
          kind: FileReadFault.failed,
          busy: false,
        );
      }
      lastReadAttempts = attempt;
      reply = null;
      fault = null;
      kind = null;
      try {
        reply = await _readFileReply(channel, name, onProgress: onProgress);
      } on RequestQueueTimeout catch (error) {
        // The link never gave this read a turn, so the device was never asked. Re-sending costs the
        // whole queue bound again (5 s) for a queue that is still held by whatever was ahead of it,
        // so the read ends here and says which of the two things happened: a busy link, not a
        // device that did not answer.
        AppDiagnostics.log(
          'storage',
          'read "$name": never got a slot on the link: $error',
        );
        return (
          reply: null,
          placed: null,
          fault:
              'the link was busy holding other requests (${error.inFlight} of '
              '${error.limit} slots taken, nothing came free in '
              '${error.waited.inMilliseconds} ms), so this read was never sent',
          kind: FileReadFault.failed,
          busy: true,
        );
      } catch (error) {
        fault = 'the device did not answer ($error)';
        kind = FileReadFault.failed;
      }
      PlacedFile? placed;
      if (fault == null) {
        final gap = reply!.gap;
        if (gap != null) {
          // A reply that carried NOT ONE fragment is a HEAD that was cut, not a hole in a stream:
          // nothing of the answer arrived, so asking again is the same question once the link has
          // had time to move, and it is retried below. A reply that STARTED and then stopped is
          // the reported hole ([FileReadFault.partial]) and is never re-burst. The link's stream
          // watchdog is armed by the first fragment that lands, so a head is normally closed by the
          // request deadline instead - this branch is the shape for a channel that reports it.
          fault = gap.message;
          kind = gap.received == 0
              ? FileReadFault.failed
              : FileReadFault.partial;
        } else if (!reply.ok && reply.payload.isEmpty && reply.chunks.isEmpty) {
          // The device's answer for a name its file table does not carry: FLAG_FAIL with no
          // payload and no fragment (firmware `Core/Services/Storage.h`, `BeginRead`). That is
          // DATA, not a lost read: the file is absent, the same answer three times over, and the
          // page renders an empty listing instead of an error.
          AppDiagnostics.log(
            'storage',
            'read "$name": ${FileReadFault.absent.label} - the device answered FAIL with no '
                'bytes (1 send, not retried)',
          );
          return (
            reply: reply,
            placed: null,
            fault: null,
            kind: FileReadFault.absent,
            busy: false,
          );
        } else {
          placed = placeFragments(reply);
          if (placed == null || placed.bytes.isEmpty) {
            fault = 'the reply carried no file bytes';
            kind = FileReadFault.failed;
          } else if (size != null && placed.bytes.length < size) {
            // A gap-free fragment set can still come up short of the record's size when the
            // device answered with a prefix of the file: same loss, same recovery.
            fault =
                'only ${placed.bytes.length} of $size byte(s) arrived '
                '(${placed.fragmentsUsed} fragment(s))';
            kind = FileReadFault.partial;
          }
        }
      }
      if (fault == null) {
        AppDiagnostics.log(
          'storage',
          'read "$name": complete on attempt $attempt/$attempts - '
              '${_fragmentSummary(reply!)}',
        );
        return (
          reply: reply,
          placed: placed,
          fault: null,
          kind: null,
          busy: false,
        );
      }
      if (kind == FileReadFault.partial) {
        // A reply that ARRIVED with a hole is reported, not asked for again. Storage CID 5 asks a
        // file BY NAME: there is no offset form to resume from, so a re-request is the WHOLE file
        // again - and it lands in the same 24 KiB transmit ring that just dropped fragments,
        // before that ring has drained. Three whole re-requests never recovered anything a quiet
        // second would not, and a page that lists N files paid it per file. The retry is the
        // page's Try again (or the next refresh), at the moment the user chooses, on a link that
        // has had time to settle.
        AppDiagnostics.log(
          'storage',
          'read "$name": attempt $attempt failed (${kind!.label}) - $fault; '
              'not re-requested (an incomplete reply is reported, never re-burst)',
        );
        return (
          reply: reply,
          placed: null,
          fault: fault,
          kind: kind,
          busy: false,
        );
      }
      final more = attempt < attempts;
      final wait = more ? readRetryBackoff[attempt - 1] : Duration.zero;
      AppDiagnostics.log(
        'storage',
        'read "$name": attempt $attempt/$attempts failed '
            '(${kind?.label ?? FileReadFault.failed.label}) - $fault'
            '${more ? '; re-requesting the whole file in '
                      '${wait.inMilliseconds} ms' : '; giving up'}',
      );
      if (more) await Future<void>.delayed(wait);
    }
    return (reply: reply, placed: null, fault: fault, kind: kind, busy: false);
  }

  /// Why a reply that came back short was not asked for again: the read has no offset form, so a
  /// re-request is the whole file - back down the link that just failed to deliver it, whether it
  /// delivered a hole or nothing at all.
  static String _notResentNote(FileReadFault? kind) =>
      kind == FileReadFault.partial
      ? ' It was not asked again: this read has no offset form, so a re-request is the '
            'whole file again, back down the link that just failed to deliver it.'
      : '';

  /// `256 of 256 fragments, STOP received, 16384 byte(s)` - the shape a rig log quotes, so a
  /// run's fragment counts can be read against the reply they belong to.
  static String _fragmentSummary(PacketResponse reply) {
    final bytes = reply.payload.length;
    if (reply.chunks.isEmpty) return 'single packet, $bytes byte(s)';
    return '${reply.chunks.length} of ${reply.total} fragments, '
        'STOP received, $bytes byte(s)';
  }

  /// Records why a read ended without data, on the client's [trace] (the restore report's reason).
  ///
  /// A read that never got a slot on the link ([RequestQueueTimeout]) is [RequestFaultKind.busy],
  /// not [RequestFaultKind.noReply]: not one frame went out, so the device was never asked. The
  /// read's own sentence already says so (`_readWhole`); this keeps the recorded KIND agreeing
  /// with it, which is what `restoreCodeText` renders.
  void _recordReadFault(
    ({
      PacketResponse? reply,
      PlacedFile? placed,
      String? fault,
      FileReadFault? kind,
      bool busy,
    })
    read, {
    required int cid,
    required String detail,
  }) {
    if (read.reply == null) {
      trace.record(
        RequestFault(
          kind: read.busy ? RequestFaultKind.busy : RequestFaultKind.noReply,
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
  /// show - null when it did. A truncated reply is never rendered as data: it is reported after
  /// one send, a request that threw after [readAttempts], and only then does this get a value.
  String? lastReadError;

  /// Parses 16-byte file-table records. Unwritten entries are all 0xFF and terminate the
  /// table. A record with offset 0 is a superseded/invalidated entry (rename/delete/table
  /// moves zero the 4-byte offset but leave the size), so it is skipped - on every device,
  /// since all targets run the full multi-file filesystem now.
  ///
  /// [includeInvalidated] is the ONE way a caller asks for those superseded records too, and
  /// it is the only place the rule "is an invalidated record a row?" is decided: the Storage
  /// view's file-table page asks for them (it folds them away under a collapsible section so
  /// the view agrees with the file list instead of drowning in stale records), and every other
  /// caller takes the default and keeps the live rows only. An invalidated record comes back
  /// with [FileRecord.invalidated] set, `offset` 0, and the size and name bytes the table still
  /// holds; rendering them as "invalidated" is the page's choice, not this parser's.
  static List<FileRecord> parseFileTable(
    List<int> contents, {
    bool includeInvalidated = false,
  }) {
    final records = <FileRecord>[];
    for (var offset = 0; offset + 16 <= contents.length; offset += 16) {
      final recOffset = uint32FromBytes(contents, offset);
      final size = uint32FromBytes(contents, offset + 4);
      if (recOffset == 0xFFFFFFFF && size == 0xFFFFFFFF) break;
      final invalidated = recOffset == 0;
      if (invalidated && !includeInvalidated) continue;
      records.add(
        FileRecord(
          index: records.length,
          slot: offset ~/ 16,
          offset: recOffset,
          size: size,
          name: unpadName(contents.sublist(offset + 8)),
          invalidated: invalidated,
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
  ///
  /// On a block filesystem this is the CHEAP way to change a file's length: the record is appended
  /// against the file's existing data area (`StorageBlockFS.h` `ResizeFile`), where a
  /// delete+create allocates (and erases) a fresh area for the same bytes. Shrinking always
  /// works; growing only works when the free run behind the file can be extended in place, and
  /// both refuse a zero size (B11).
  ///
  /// [writeFile] sends it as part of its in-place path; nothing else calls it directly.
  Future<bool> resizeFile(String name, int size) async {
    final payload = <int>[...padName(name), ...uint32ToBytes(size)];
    return _status(3, payload: payload);
  }

  /// Formats the whole filesystem per docs 03.00 CID0 (wipes every file).
  Future<bool> format() async {
    return _status(0);
  }

  /// Reads the whole file per docs 03.05 CID5 and says what happened.
  ///
  /// This is the ONE whole-file read every page goes through. A file is only usable when EVERY
  /// byte arrived: unlike a short status reply, a layout or a script file with a hole in the middle
  /// cannot be decoded. Which case the attempt ended in then decides what happens next (see
  /// [_readWhole]): a reply that lost a fragment, lost its STOP frame or came up short of [size]
  /// arrived INCOMPLETE and is reported after ONE send, while an attempt that produced nothing
  /// usable (the request threw, the reply carried no file bytes) is re-sent, [readAttempts] sends
  /// in all, [readRetryBackoff] apart. The result keeps the three cases apart - whole bytes, an
  /// ABSENT file (the device answered FAIL: a legitimate empty result), or a failed/partial read
  /// with the reason in [FileRead.detail] - and a partial body is never returned as data.
  Future<FileRead> readFileWhole(
    String name, {
    int? size,
    void Function(int received, int total)? onProgress,
    bool Function()? isCancelled,
  }) async {
    final read = await _readWhole(
      name,
      size: size,
      onProgress: onProgress,
      isCancelled: isCancelled,
    );
    if (read.kind == FileReadFault.absent) {
      return FileRead(
        name: name,
        fault: FileReadFault.absent,
        detail: 'The device holds no file "$name".',
        attempts: lastReadAttempts,
      );
    }
    if (read.fault != null) {
      _recordReadFault(read, cid: 5, detail: read.fault!);
      lastReadError =
          'Could not read "$name": ${read.fault}. '
          'It was asked $lastReadAttempts time(s); try again.';
      AppDiagnostics.log(
        'storage',
        'read "$name": FAILED (${read.kind?.label ?? FileReadFault.failed.label}) '
            '- ${lastReadError!}',
      );
      return FileRead(
        name: name,
        fault: read.kind ?? FileReadFault.failed,
        detail: lastReadError,
        attempts: lastReadAttempts,
      );
    }
    var contents = read.placed!.bytes;
    if (size != null && contents.length > size) {
      contents = contents.sublist(0, size);
    }
    return FileRead(name: name, bytes: contents, attempts: lastReadAttempts);
  }

  /// The whole file's bytes, or null for BOTH an absent file and a failed read.
  ///
  /// Kept for callers whose answer to "absent" and "unreadable" is the same abort (a write that
  /// must not proceed, a download that reports "failed"). A surface that shows the three cases
  /// differently uses [readFileWhole] and reads [FileRead.fault].
  Future<List<int>?> readFile(String name, {int? size}) async =>
      (await readFileWhole(name, size: size)).bytes;

  /// How the last [writeFile] put a file's content on the device, so a caller (a restore report, a
  /// page notice) can say which path ran instead of assuming one did.
  FileWritePath lastWritePath = FileWritePath.staged;

  /// Why the last [writeFile] did not take its in-place path, in one sentence - null when it did,
  /// or when the file was simply not there yet (a first creation is staged by design).
  String? lastWriteNote;

  /// Set once this client has seen a device refuse the resize (a FAIL reply, or no reply at all to
  /// what is a documented CID). An image older than the resize capability answers NOTHING to CID 3,
  /// which costs this client a whole request timeout PER WRITE; a refusal is a property of the
  /// device, so it is remembered for the rest of this client's life and not re-probed.
  bool _resizeRefused = false;

  /// The sentence [lastWriteNote] gets when the resize is skipped because this device already
  /// refused it. Size-independent on purpose: the sizes of later writes are not the one that was
  /// refused.
  static const String _resizeRefusedNote =
      'the device refused Storage CID 3 earlier';

  /// Writes a whole file per docs 03.06 CID6. Stream fragments are capped at 64
  /// content bytes (docs: "maximum 64 byte stream fragment"); every fragment carries
  /// the 4-byte frag info so the device can detect out-of-order delivery.
  ///
  /// TWO paths, and which one ran is in [lastWritePath]:
  ///
  /// * IN PLACE ([FileWritePath.inPlace] / [FileWritePath.resize]) - the file is already on the
  ///   device, so its length is changed with Storage CID 3 (one appended record, same data area)
  ///   or not at all when it is already the right length (no record), and the fragments are
  ///   written into it. This is the path that stops the app filling a node's file table: a DAS
  ///   holds 8 sixteen-byte records and **an invalidated (0x00) hole is never reused**
  ///   (`firmware/src/Core/Functions/StorageBlockFS.h`), so a write that appends two records per
  ///   save is what made nodes start refusing Save All.
  /// * STAGED ([FileWritePath.staged]) - the content is staged under a temporary name and renamed
  ///   over the target once the write succeeded. This is the path a FIRST creation takes (there is
  ///   no length to change and nothing to preserve), and the fallback whenever the resize is
  ///   refused, does not take, or the in-place write fails. It appends two records (create +
  ///   rename) and reallocates the data area, and it is the only path that leaves the original
  ///   untouched until the new content is complete.
  ///
  /// The in-place path trades that last guarantee for the records: a link that dies mid-write
  /// leaves the file at its new length with part of the new content. Everything short of that is
  /// repaired by the staged path, which rewrites the file whole from the bytes the caller holds.
  Future<bool> writeFile(String name, List<int> bytes) async {
    lastWritePath = FileWritePath.staged;
    lastWriteNote = null;
    final ok =
        await _writeInPlace(name, bytes) || await _stageAndSwap(name, bytes);
    // The path is a fact about the device, not an implementation detail: a file table fills up
    // WATCHING which path ran, and the diagnostics ring is where a rig session reads it back.
    AppDiagnostics.log(
      'storage',
      'write "$name" (${bytes.length} B): ${lastWritePath.name}, '
          '${lastWritePath.recordCost}'
          '${lastWriteNote == null ? '' : ' - $lastWriteNote'}',
    );
    return ok;
  }

  /// The in-place half of [writeFile]. True only when the whole file is on the device; false means
  /// the caller must stage and swap (and [lastWriteNote] says why, when there is something to say).
  ///
  /// The length change is verified by READING THE FILE'S SIZE BACK, never by the resize reply
  /// alone: a device that answers success and leaves the old size would then take the fragments as
  /// an over-run (`Core/Services/Storage.h` refuses a fragment past the file's end) and the write
  /// would leave the file at its old length - the worst outcome there is, because nothing reports
  /// it.
  Future<bool> _writeInPlace(String name, List<int> bytes) async {
    // A zero-size file cannot exist (Docs 03.01/03.03), so there is nothing to write in place.
    if (bytes.isEmpty) return false;

    final before = await _readTableQuietly();
    if (before == null) {
      lastWriteNote =
          'the file table could not be read, so the file was staged and renamed '
          'instead of resized';
      return false;
    }
    final existing = _liveRecord(before, name);
    if (existing == null) {
      // A first creation: there is no length to change and nothing to preserve, so the staging
      // path is the normal one and needs no explanation.
      return false;
    }

    if (existing.size != bytes.length) {
      if (_resizeRefused) {
        lastWriteNote = _resizeRefusedNote;
        return false;
      }
      // The resize is a PROBE for a capability this device may not have. Its fault is not the
      // reason a later failure is reported with, so the trace starts clean for the path that runs.
      trace.clear();
      if (!await resizeFile(name, bytes.length)) {
        _resizeRefused = true;
        lastWriteNote =
            'the device refused the resize (Storage CID 3) of "$name" to '
            '${bytes.length} byte(s), so the file was staged and renamed instead';
        trace.clear();
        return false;
      }
      final after = await _readTableQuietly();
      final resized = after == null ? null : _liveRecord(after, name);
      if (resized == null || resized.size != bytes.length) {
        final what = resized == null
            ? 'no longer lists the file'
            : 'still says ${resized.size} byte(s)';
        lastWriteNote =
            'the device accepted the resize of "$name" to ${bytes.length} '
            'byte(s) but its file table $what, so the file was staged and '
            'renamed instead';
        trace.clear();
        return false;
      }
      lastWritePath = FileWritePath.resize;
    } else {
      lastWritePath = FileWritePath.inPlace;
    }

    if (!await _writeFragments(name, bytes)) {
      // The file is now at the new length with part of the content. The staged path rewrites it
      // whole from the caller's bytes, so falling back here repairs the file as well as it can.
      lastWritePath = FileWritePath.staged;
      lastWriteNote =
          'the write into "$name" did not land, so the file was staged and '
          'renamed instead';
      return false;
    }
    return true;
  }

  /// Today's path: stage under a temporary name, write, then rename over the target only once the
  /// content is safely staged, so a failed create/write cannot destroy the original.
  Future<bool> _stageAndSwap(String name, List<int> bytes) async {
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

  /// The live record for [name], or null when the table does not list it.
  static FileRecord? _liveRecord(List<FileRecord> records, String name) {
    final wanted = normalizeFileName(name);
    for (final record in records) {
      if (record.invalidated) continue;
      if (normalizeFileName(record.name) == wanted) return record;
    }
    return null;
  }

  /// A file-table read that leaves [lastReadError] and [lastReadAttempts] alone.
  ///
  /// A write is not a read: those two fields are the answer a PAGE shows for its last read, and a
  /// table read taken inside a write must not put a stale sentence on a view that never asked for
  /// one. Null means the table could not be read whole. One attempt only: this is a probe (see
  /// [readFileTableResult]'s `attempts`), and its answer decides between two paths, not a page.
  Future<List<FileRecord>?> _readTableQuietly() async {
    final error = lastReadError;
    final attempts = lastReadAttempts;
    try {
      final read = await readFileTableResult(attempts: 1);
      return read.ok ? read.recordsOrEmpty : null;
    } finally {
      lastReadError = error;
      lastReadAttempts = attempts;
    }
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
