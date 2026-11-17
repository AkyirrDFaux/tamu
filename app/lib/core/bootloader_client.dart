/// Bootloader client, protocol v2 (Docs/Services/Bootloader.md, `c3aabab`).
///
/// One upload is five steps, in this order, and every one of them is bounded:
///
/// 1. **Identify** - the app size, the flash page size and the device name. A bootloader that
///    does not report a plausible page size is a pre-v2 bootloader, which this client refuses
///    rather than guessing at - see [Bootloader.pageSizePlausible]. A bootloader that does not
///    answer is simply not in bootloader mode and the run ends with [FlashFault.noAnswer].
/// 2. **Compare per page** - read each page of the image in 32-byte chunks and compare. Only the
///    pages that differ are erased and written, which is where the reliability comes from: loss
///    is per transaction, so a page that already matches is one the run never has to re-send.
/// 3. **Blank the tail** - the pages from the end of the image to the end of the app space, so
///    the whole space is a function of the image and the CRC is predictable. A page the node
///    REFUSES to erase is not a failure: some devices keep state at the top of the app space (the
///    DAS keeps its 512-byte storage there and its erase command stops at the code space's end),
///    so those bytes are read first and become part of the model both measures are compared with.
/// 4. **Verify, two measures** - the CRC (one exchange, over the whole app space) and the full
///    read-back. They are reported separately and neither wins a disagreement: the CRC depends on
///    the node's own CRC code being right, the read-back does not, so "CRC matches, read-back
///    differs" is its own named outcome ([FlashVerifyOutcome.crcAgreesReadBackDiffers]) and so is
///    the other way round.
/// 5. **Correct, up to five rounds** - the pages the read-back flagged are erased and rewritten,
///    then both measures run again. A clean read-back rewrites nothing. A sixth failure ends the
///    upload naming the offsets that disagree ([FlashFault.verifyMismatch]).
///
/// ## One request at a time, and why a busy answer cannot livelock
///
/// The core relays ONE passthrough request at a time and refuses a second with reason `0x01`
/// (Docs/Services/Bootloader.md "Core Bootloader Passthrough Commands"). The client sends its
/// requests sequentially and retries `0x01` on a backoff of 250 ms, doubling to a 2 s cap, at most
/// six times per request - 250 + 500 + 1000 + 2000 + 2000 = 5.75 s at worst, after which the
/// request fails as [FlashFault.busy]. It cannot livelock because the wait is bounded twice over:
/// a fixed number of attempts per request, and the run's own deadline, which is checked between
/// every request. The 250 ms first step is the settle measured on the bench: an outstanding
/// passthrough settles within a few milliseconds of its reply, and 250 ms is past every one seen.
/// No delay is inserted between ordinary requests - the read-back of the whole DAS space (448
/// chunk reads) completes in 5.9 s, so back-to-back requests are what the core is built for.
///
/// ## The other three reasons are NOT retries
///
/// `0x02` malformed is a bug in this client (the core could not even encode the request): it stops
/// the run as [FlashFault.malformed] instead of hammering the same frame. `0x03` means the request
/// went out and the node never answered: the node is not in bootloader mode or is not there, and
/// re-sending into that is exactly the hammering the reason exists to prevent, so the run stops as
/// [FlashFault.nodeSilent]. `0x04` is the node's own refusal (a misaligned erase, a write into a
/// page that was not erased): it is the node's semantics and maps to [FlashFault.nodeRefused] - or
/// to the "this page cannot be blanked" path of step 3 - never to a retry. A FAIL with no reason
/// byte at all comes from a core older than the reason table and is named as its own fault
/// ([FlashFault.unreportedRefusal]) rather than folded into any of the four.
///
/// A timeout of the APP's own request (no packet came back from the core at all) is a fifth,
/// different silence and is retried: it says nothing about the node.
///
/// ## The upload deadline
///
/// [BootloaderClient.flashDeadline] is 10 minutes, checked between every request in [flash]. The
/// arithmetic: a full read-back of the DAS space (448 chunk reads) is 5.9 s, and verification runs
/// at most six times (the initial one plus one after each of the five correction rounds) - ~36 s;
/// the delta write pass is at most 448 chunk writes and 224 erases (~10 s), and five correction
/// rounds repairing the whole image at worst add ~50 s. That is ~100 s over USB; a BLE link costs
/// ~3.5x per request, so the same worst case is ~350 s. Ten minutes covers it with margin, and it
/// is a backstop rather than the liveness mechanism: every request is already bounded by its own
/// window and attempt count.
library;

import 'dart:async';
import 'dart:math' as math;
import 'dart:typed_data';

import 'bootloader.dart';
import 'ble_bootloader_transport.dart';
import 'connection.dart';
import 'device_db.dart';
import 'firmware_package.dart';
import 'protocol.dart';
import 'request_channel.dart';
import 'transport.dart';
import 'types.dart';

/// Which stage of [BootloaderClient.flash] is running.
enum FlashPhase { comparing, writing, verifying, correcting, done }

/// A progress snapshot emitted by [BootloaderClient.flash].
class FlashProgress {
  final FlashPhase phase;

  /// Steps finished in this phase: pages while comparing/writing, chunk reads while verifying.
  final int completed;
  final int total;

  /// Chunks the current read-back has found different from what should be there.
  final int mismatches;

  const FlashProgress({
    required this.phase,
    required this.completed,
    required this.total,
    this.mismatches = 0,
  });

  double get fraction => total == 0 ? 1.0 : completed / total;
}

/// What the two measures of one verification said.
///
/// The CRC is one exchange whose value depends on the device's own CRC code being right; the
/// read-back does not depend on it. A disagreement is its own named outcome - never resolved by
/// preferring either side.
enum FlashVerifyOutcome {
  /// The CRC matched and every chunk read back identical.
  confirmed,

  /// The CRC matched and the read-back found chunks that differ: the node's CRC and its own bytes
  /// disagree, so one of the two is lying.
  crcAgreesReadBackDiffers,

  /// The CRC did not match and every chunk read back identical: the CRC (or the value it was
  /// compared with) is wrong, the bytes are not.
  crcDiffersReadBackAgrees,

  /// Both measures say the space is wrong.
  bothDiffer,

  /// The node did not answer the CRC request, so the read-back is the only measure
  /// ([FlashVerifyResult.note] says so). A pre-v2 bootloader has no CRC command.
  readBackOnly;

  /// True when the outcome names a space the measures agree on. Use [FlashVerifyResult.clean]:
  /// with no CRC the read-back has to have found nothing as well.
  bool get agrees =>
      this == FlashVerifyOutcome.confirmed ||
      this == FlashVerifyOutcome.readBackOnly;

  String get label => switch (this) {
    FlashVerifyOutcome.confirmed => 'CRC and read-back agree',
    FlashVerifyOutcome.crcAgreesReadBackDiffers =>
      'CRC matches, read-back differs',
    FlashVerifyOutcome.crcDiffersReadBackAgrees =>
      'CRC differs, read-back matches',
    FlashVerifyOutcome.bothDiffer => 'CRC and read-back both differ',
    FlashVerifyOutcome.readBackOnly => 'read-back only (no CRC from the node)',
  };
}

/// The two measures of one verification: the CRC, the read-back, and what they said.
class FlashVerifyResult {
  /// What the two measures said (see [FlashVerifyOutcome]).
  final FlashVerifyOutcome outcome;

  /// The CRC the node answered with, or null when it has no CRC command.
  final int? crc;

  /// The value the CRC was compared against: the client's CRC32 of the model space (the image,
  /// its tail, and any region the device refuses to blank).
  final int expectedCrc;

  /// Read-back passes over the space: 1 whenever the pass ran.
  final int readBacks;

  /// Chunk reads issued.
  final int reads;

  /// Every chunk the read-back found different, when any. Null when the pass found none.
  final FlashVerifyReport? differences;

  /// Chunks a correction round rewrote and then found identical (only ever set by
  /// [BootloaderClient.flash], which is the only caller that writes).
  final int corrected;

  const FlashVerifyResult({
    required this.outcome,
    this.crc,
    required this.expectedCrc,
    required this.readBacks,
    required this.reads,
    this.differences,
    this.corrected = 0,
  });

  /// True when the device holds the space the image describes: both measures agreed, or - with no
  /// CRC from the node - the read-back pass found nothing.
  bool get clean =>
      outcome == FlashVerifyOutcome.confirmed ||
      (outcome == FlashVerifyOutcome.readBackOnly && differing == 0);

  /// True when the node answered the CRC request at all.
  bool get hasCrc => crc != null;

  /// True when the CRC agreed with the expected value.
  bool get crcOk => crc != null && crc == expectedCrc;

  int get differing => differences?.count ?? 0;

  /// `0x0b43b1b7 (expected 0x083a7d36)` - the pair a CRC disagreement is reported with.
  String get crcPair => hasCrc
      ? '${_hex32(crc!)} (expected ${_hex32(expectedCrc)})'
      : 'none (the node did not answer a CRC request)';

  /// The two measures, each on its own terms - the shape both the log line and the report use.
  String get note {
    final crcPart = hasCrc
        ? 'CRC ${_hex32(crc!)} vs ${_hex32(expectedCrc)} - '
              '${crcOk ? 'match' : 'MISMATCH'}'
        : 'CRC none (the node did not answer a CRC request)';
    final readPart = differing == 0
        ? 'read-back $reads chunks, all identical'
        : 'read-back ${differences!.summary}';
    final correctedPart = corrected > 0
        ? '; a correction round rewrote $corrected chunk(s)'
        : '';
    return '$crcPart; $readPart$correctedPart';
  }
}

/// The outcome of [BootloaderClient.flash] or [BootloaderClient.flashPackage].
class FlashResult {
  /// The two measures, as they stood when the run finished.
  final FlashVerifyResult verify;

  /// Image pages that were compared against the device.
  final int pagesCompared;

  /// Image pages that already held what the image wants: never erased, never written.
  final int pagesKept;

  /// Image pages erased and written - the initial delta pass plus every correction round.
  final int pagesWritten;

  /// Tail pages the run blanked (already-blank pages are not counted).
  final int pagesBlanked;

  /// Tail pages the node refuses to erase; their bytes are part of the verified model.
  final int pagesProtected;

  /// 32-byte chunks written over the whole run.
  final int chunksWritten;

  /// Chunks of a rewritten page that were already blank after its erase, so they were not sent.
  final int chunksSkipped;

  /// Correction rounds run (0 when the first verification was clean).
  final int correctionRounds;

  /// Verification rounds run: 1 plus [correctionRounds].
  final int rounds;

  /// The package version from the manifest (when flashed via [flashPackage]).
  final String? version;

  /// The build commit from the manifest (when flashed via [flashPackage]).
  final String? buildCommit;

  /// The target the package was built for (when flashed via [flashPackage]).
  final String? target;

  const FlashResult({
    required this.verify,
    required this.pagesCompared,
    required this.pagesKept,
    required this.pagesWritten,
    required this.pagesBlanked,
    required this.pagesProtected,
    required this.chunksWritten,
    required this.chunksSkipped,
    required this.correctionRounds,
    required this.rounds,
    this.version,
    this.buildCommit,
    this.target,
  });

  int get reads => verify.reads;

  /// Chunks a correction round rewrote over the whole run.
  int get corrections => verify.corrected;

  FlashVerifyOutcome get outcome => verify.outcome;

  bool get clean => verify.clean;

  /// The counts a handoff quotes, in one line.
  String get counts =>
      '${pagesCompared - pagesKept} of $pagesCompared image page(s) rewritten, '
      '$pagesKept kept, $pagesBlanked tail page(s) blanked, '
      '${pagesProtected > 0 ? '$pagesProtected tail page(s) not blankable, ' : ''}'
      '$chunksWritten chunk(s) written, $rounds verification round(s)';

  /// One line stating how the image was confirmed, for a log or a page.
  String get note => '${verify.note}; $counts';
}

String _hex32(int value) =>
    '0x${value.toUnsigned(32).toRadixString(16).padLeft(8, '0')}';

/// A device's identity: the size of its app space, its erase page size, and its name.
class BootloaderIdentity {
  /// The app-space size in bytes (the identify response's offset slot).
  final int appSize;

  /// The flash erase page size in bytes (the identify response's page field).
  final int pageSize;

  /// The device name (the identify response's 28-byte, null-padded field).
  final String name;

  const BootloaderIdentity({
    required this.appSize,
    required this.pageSize,
    required this.name,
  });

  /// Pages in the whole app space.
  int get pages => appSize ~/ pageSize;

  @override
  String toString() => '$name ($appSize B app space, $pageSize B pages)';
}

/// The outcome of one low-level write attempt.
class WriteAck {
  /// True when the device reported this chunk written.
  final bool accepted;

  /// The offset the device's write response carried: this chunk when it landed, the page with
  /// bit 0 set when the page could not be written, or a stale offset when nothing was touched.
  final int? acked;

  const WriteAck(this.accepted, {this.acked});

  /// True when the answer names a PAGE, i.e. the page could not be written (a write into a page
  /// that was never erased).
  bool get pageFailed => acked != null && Bootloader.ackFailed(acked!);
}

/// Why a passthrough request failed (Docs/Services/Bootloader.md, the reason table).
///
/// The five are kept apart because the client does something different with each: only [busy] and
/// [linkLost] are retried, [nodeRefused] is the node's own semantics, and [malformed] is a bug in
/// this client.
enum PassthroughReason {
  /// `0x01` - the core refused it: another passthrough request is outstanding.
  busy(0x01),

  /// `0x02` - the core refused it: the request is malformed (payload too short, unknown CID).
  malformed(0x02),

  /// `0x03` - the request went out and the node never answered before the 1 s deadline.
  nodeSilent(0x03),

  /// `0x04` - the node answered with a failure frame of its own.
  nodeRefused(0x04),

  /// A FAIL with no reason byte: a core older than the reason table. Never folded into the four.
  unreported(0),

  /// No reply from the core at all - the app's own request timed out. Distinct from [nodeSilent]:
  /// this one says nothing about the node, so it is retried.
  linkLost(-1);

  const PassthroughReason(this.code);

  /// The byte the core sends for this reason; null for the two that no byte describes.
  final int code;

  String get label => switch (this) {
    PassthroughReason.busy => 'refused, another request is outstanding',
    PassthroughReason.malformed => 'refused, the request is malformed',
    PassthroughReason.nodeSilent => 'the node did not answer',
    PassthroughReason.nodeRefused => 'the node refused it',
    PassthroughReason.unreported =>
      'refused with no reason (the core predates the reason byte)',
    PassthroughReason.linkLost => 'the core did not answer the request',
  };

  /// The reason a one-byte failure payload carries, or [unreported] for a byte outside the table.
  static PassthroughReason fromCode(int code) => switch (code) {
    0x01 => PassthroughReason.busy,
    0x02 => PassthroughReason.malformed,
    0x03 => PassthroughReason.nodeSilent,
    0x04 => PassthroughReason.nodeRefused,
    _ => PassthroughReason.unreported,
  };
}

/// One failed request, with the reason the core gave (or the absence of one).
class PassthroughFailure implements Exception {
  final PassthroughReason reason;

  /// What was being asked, so a message names the request and not just the verdict.
  final String what;

  /// The reason byte as received, when one arrived.
  final int? code;

  const PassthroughFailure(this.reason, this.what, {this.code});

  String get message => '${reason.label} ($what)';

  @override
  String toString() => message;
}

/// Low-level bootloader channel. One call is one attempt (no retries here); a failure is thrown
/// as [PassthroughFailure] so the client can tell the four reasons and the link's own silence
/// apart. [BootloaderClient] owns the retry policy and the pacing.
abstract class BootloaderTransport {
  /// Asks the device to identify itself. Null when the node did not answer at all - the answer a
  /// node that is not in bootloader mode gives - and thrown for every other failure.
  Future<BootloaderIdentity?> identify(Duration timeout);

  /// Puts the addressed node into bootloader mode by sending Device service CID 4.
  /// Returns true if the device acknowledged and entered bootloader mode.
  Future<bool> enterBootloader(Duration timeout);

  /// Reads 32 bytes at [offset].
  Future<Uint8List> readChunk(int offset, Duration timeout);

  /// Blanks the [Bootloader] page at [offset] and returns the page the device acknowledged.
  Future<int> erasePage(int offset, Duration timeout);

  /// Writes one 32-byte chunk at [offset].
  Future<WriteAck> writeChunk(int offset, List<int> data, Duration timeout);

  /// Asks for the CRC32 of the whole app space (one number, no arguments).
  Future<int> crc(Duration timeout);

  /// Asks the node to jump to its application (no arguments). The node answers with an ack
  /// and then leaves the bootloader. Returns true when the ack was received.
  Future<bool> jump(Duration timeout);
}

/// Core passthrough command IDs (Docs/Services/Bootloader.md "Core Bootloader Passthrough
/// Commands (002x)"). They follow the bootloader command order - identify, read, erase, write, CRC,
/// jump - so a CID and its frame always carry the same rank.
const int coreBootIdentify = 0x20;
const int coreBootRead = 0x21;
const int coreBootErase = 0x22;
const int coreBootWrite = 0x23;
const int coreBootCrc = 0x24;
const int coreBootJump = 0x25;
const int coreBootEnterBootloader = 0x04;

/// The core's `0020`-`0024` passthrough: the core relays each raw bootloader frame onto its RSBus,
/// so the app reaches whatever node is in bootloader mode without knowing its address.
///
/// The core owns the frame encoding, the 1 s reply deadline, and the failure reasons; this
/// transport turns its answers into [WriteAck]s, page numbers and CRCs, and its refusals into
/// [PassthroughFailure]s.
class PassthroughTransport implements BootloaderTransport {
  /// The core that relays our raw frames onto its RSBus.
  final int coreId;

  PassthroughTransport({this.coreId = 1});

  ConnectionManager get _link => ConnectionManager.instance;

  /// The reply to one passthrough request, with a FAIL classified by its reason byte.
  Future<PacketResponse> _ask(
    String what,
    int cid, {
    List<int> payload = const [],
    required Duration timeout,
  }) async {
    final PacketResponse reply;
    try {
      reply = await _link.requestWithFlags(
        coreId,
        ServiceType.device,
        cid,
        payload: payload,
        timeout: timeout,
      );
    } on TransportException {
      throw PassthroughFailure(PassthroughReason.linkLost, what);
    }
    if (!reply.ok) throw _classify(what, reply);
    return reply;
  }

  /// What a FAIL reply says: a one-byte reason, the node's own ack (4 bytes), or neither.
  static PassthroughFailure _classify(String what, PacketResponse reply) {
    final payload = reply.payload;
    if (payload.length == 1) {
      return PassthroughFailure(
        PassthroughReason.fromCode(payload[0]),
        what,
        code: payload[0],
      );
    }
    // A four-byte failure body is the node's own ack, which the core passes on with FAIL: for a
    // write that is the page it could not program, for an erase the page with bit 0 set.
    if (payload.length == 4) {
      return PassthroughFailure(PassthroughReason.nodeRefused, what);
    }
    return PassthroughFailure(PassthroughReason.unreported, what);
  }

  @override
  Future<BootloaderIdentity?> identify(Duration timeout) async {
    final PacketResponse reply;
    try {
      reply = await _ask('identify', coreBootIdentify, timeout: timeout);
    } on PassthroughFailure catch (failure) {
      if (failure.reason == PassthroughReason.nodeSilent) return null;
      rethrow;
    }
    // The core forwards the node's identify body verbatim: app size (4), page size (4), name (28).
    if (reply.payload.length < 4 + 4) {
      throw const PassthroughFailure(
        PassthroughReason.unreported,
        'identify: the reply carried no page size',
      );
    }
    final appSize = uint32FromBytes(reply.payload, 0);
    final pageSize = uint32FromBytes(reply.payload, 4);
    final nameBytes = reply.payload.sublist(8);
    var n = nameBytes.length;
    while (n > 0 && nameBytes[n - 1] == 0) {
      n--;
    }
    return BootloaderIdentity(
      appSize: appSize,
      pageSize: pageSize,
      name: String.fromCharCodes(nameBytes.sublist(0, n)),
    );
  }

  @override
  Future<Uint8List> readChunk(int offset, Duration timeout) async {
    final reply = await _ask(
      'read 0x${offset.toRadixString(16)}',
      coreBootRead,
      payload: uint32ToBytes(offset),
      timeout: timeout,
    );
    if (reply.payload.length < 4 + Bootloader.payloadSize) {
      throw PassthroughFailure(
        PassthroughReason.unreported,
        'read 0x${offset.toRadixString(16)}: the reply was '
        '${reply.payload.length} bytes, not 36',
      );
    }
    return Uint8List.fromList(
      reply.payload.sublist(4, 4 + Bootloader.payloadSize),
    );
  }

  @override
  Future<int> erasePage(int offset, Duration timeout) async {
    final reply = await _ask(
      'erase 0x${offset.toRadixString(16)}',
      coreBootErase,
      payload: uint32ToBytes(offset),
      timeout: timeout,
    );
    if (reply.payload.length < 4) {
      throw PassthroughFailure(
        PassthroughReason.unreported,
        'erase 0x${offset.toRadixString(16)}: the reply carried no page',
      );
    }
    final page = uint32FromBytes(reply.payload, 0);
    if (Bootloader.ackFailed(page)) {
      throw PassthroughFailure(
        PassthroughReason.nodeRefused,
        'erase 0x${offset.toRadixString(16)}',
        code: PassthroughReason.nodeRefused.code,
      );
    }
    return page;
  }

  @override
  Future<WriteAck> writeChunk(
    int offset,
    List<int> data,
    Duration timeout,
  ) async {
    final reply = await _ask(
      'write 0x${offset.toRadixString(16)}',
      coreBootWrite,
      payload: [...uint32ToBytes(offset), ...data],
      timeout: timeout,
    );
    final acked = reply.payload.length >= 4
        ? uint32FromBytes(reply.payload, 0)
        : null;
    // The core sets SUCCESS only when the node's ack names the chunk it was asked to write, so
    // `ok` is the verdict; the offset is kept for a message that says what the node answered.
    return WriteAck(true, acked: acked);
  }

  @override
  Future<int> crc(Duration timeout) async {
    final reply = await _ask('CRC', coreBootCrc, timeout: timeout);
    if (reply.payload.length < 4) {
      throw const PassthroughFailure(
        PassthroughReason.unreported,
        'CRC: the reply carried no value',
      );
    }
    return uint32FromBytes(reply.payload, 0);
  }

  @override
  Future<bool> jump(Duration timeout) async {
    final reply = await _ask('jump', coreBootJump, timeout: timeout);
    // The core forwards the node's jump ack (4 bytes, offset ignored). SUCCESS means the ack arrived.
    return reply.payload.length >= 4;
  }

  @override
  Future<bool> enterBootloader(Duration timeout) async {
    final reply = await _ask('enter bootloader', coreBootEnterBootloader, timeout: timeout);
    // The node answers with ack (0x0E) and then enters bootloader mode.
    return reply.ok;
  }
}

/// The core's own USB bootloader: the raw frames go straight to the USB Serial/JTAG port with no
/// relay, so this transport encodes the requests and parses the answers itself. Each request's
/// response frame is matched on its command, and additionally on the offset it echoes where the
/// frame has one (a late answer from a previous transaction is dropped rather than returned).
class DirectUsbTransport implements BootloaderTransport {
  final UsbTransport usb;

  DirectUsbTransport(this.usb);

  @override
  Future<BootloaderIdentity?> identify(Duration timeout) async {
    final reply = await _requestFrame(
      Bootloader.encodeIdentifyRequest(),
      command: Bootloader.cmdIdentifyResponse,
      size: Bootloader.identifyResponseSize,
      timeout: timeout,
      what: 'identify',
    );
    if (reply == null) return null;
    return BootloaderIdentity(
      appSize: Bootloader.appSize(reply),
      pageSize: Bootloader.pageSize(reply),
      name: Bootloader.name(reply),
    );
  }

  @override
  Future<Uint8List> readChunk(int offset, Duration timeout) async {
    final reply = await _requestFrame(
      Bootloader.encodeReadRequest(offset),
      command: Bootloader.cmdReadResponse,
      size: Bootloader.dataSize,
      matchOffset: offset,
      timeout: timeout,
      what: 'read 0x${offset.toRadixString(16)}',
    );
    if (reply == null) {
      throw PassthroughFailure(
        PassthroughReason.nodeSilent,
        'read 0x${offset.toRadixString(16)}',
      );
    }
    return Uint8List.fromList(
      reply.sublist(
        Bootloader.headerSize,
        Bootloader.headerSize + Bootloader.payloadSize,
      ),
    );
  }

  @override
  Future<int> erasePage(int offset, Duration timeout) async {
    final reply = await _requestFrame(
      Bootloader.encodeEraseRequest(offset),
      command: Bootloader.cmdEraseResponse,
      size: Bootloader.readRequestSize,
      timeout: timeout,
      what: 'erase 0x${offset.toRadixString(16)}',
    );
    if (reply == null) {
      throw PassthroughFailure(
        PassthroughReason.nodeSilent,
        'erase 0x${offset.toRadixString(16)}',
      );
    }
    final page = Bootloader.offset(reply);
    if (Bootloader.ackFailed(page)) {
      throw PassthroughFailure(
        PassthroughReason.nodeRefused,
        'erase 0x${offset.toRadixString(16)}',
      );
    }
    return page;
  }

  @override
  Future<WriteAck> writeChunk(
    int offset,
    List<int> data,
    Duration timeout,
  ) async {
    final reply = await _requestFrame(
      Bootloader.encodeWrite(offset, data),
      command: Bootloader.cmdWriteResponse,
      size: Bootloader.readRequestSize,
      timeout: timeout,
      what: 'write 0x${offset.toRadixString(16)}',
    );
    if (reply == null) {
      throw PassthroughFailure(
        PassthroughReason.nodeSilent,
        'write 0x${offset.toRadixString(16)}',
      );
    }
    final acked = Bootloader.offset(reply);
    if (acked == offset) return WriteAck(true, acked: acked);
    // The ack names a page (bit 0 set) when the page could not be written, and otherwise keeps the
    // last offset it did write - "success only if the offset matches the request".
    return WriteAck(false, acked: acked);
  }

  @override
  Future<int> crc(Duration timeout) async {
    final reply = await _requestFrame(
      Bootloader.encodeCrcRequest(),
      command: Bootloader.cmdCrcResponse,
      size: Bootloader.crcResponseSize,
      timeout: timeout,
      what: 'CRC',
    );
    if (reply == null) {
      throw const PassthroughFailure(PassthroughReason.nodeSilent, 'CRC');
    }
    return Bootloader.crc(reply);
  }

  @override
  Future<bool> jump(Duration timeout) async {
    final reply = await _requestFrame(
      Bootloader.encodeJumpRequest(),
      command: Bootloader.cmdJumpResponse,
      size: Bootloader.readRequestSize,
      timeout: timeout,
      what: 'jump',
    );
    return reply != null;
  }

  @override
  Future<bool> enterBootloader(Duration timeout) async {
    final frame = Bootloader.encodeBootloaderEntryRequest();
    final reply = await _requestFrame(
      frame,
      command: Bootloader.cmdBootloaderEntryResponse,
      size: Bootloader.readRequestSize,
      timeout: timeout,
      what: 'enter bootloader',
    );
    return reply != null;
  }

  /// Writes [request] and waits for its response frame: a valid frame of [command] and [size],
  /// matched additionally on [matchOffset] when given. Returns null on timeout.
  Future<Uint8List?> _requestFrame(
    List<int> request, {
    required int command,
    required int size,
    required String what,
    int? matchOffset,
    required Duration timeout,
  }) async {
    final buffer = <int>[];
    final completer = Completer<Uint8List?>();
    final sub = usb.linkBytes.listen((chunk) {
      if (completer.isCompleted) return;
      buffer.addAll(chunk);
      while (buffer.isNotEmpty) {
        if (buffer[0] != Bootloader.start) {
          buffer.removeAt(0);
          continue;
        }
        if (buffer.length < size) break;
        final frame = buffer.sublist(0, size);
        if (Bootloader.decode(frame) == command &&
            (matchOffset == null || Bootloader.offset(frame) == matchOffset)) {
          buffer.removeRange(0, size);
          completer.complete(Uint8List.fromList(frame));
          return;
        }
        buffer.removeAt(0);
      }
    });
    try {
      usb.writeRaw(request);
      return await completer.future.timeout(timeout);
    } on TimeoutException {
      return null;
    } finally {
      await sub.cancel();
    }
  }
}

/// Why a flash run stopped before the device's image was confirmed as the one that was sent.
///
/// The faults are kept apart because their remedies differ: a silent node and a refused write both
/// need a power cycle, a refused erase may mean the page is not blankable at all, a malformed
/// request is this client's bug, and a busy core is a link someone else is using.
enum FlashFault {
  /// The node answered nothing to identify (the core's reason `0x03`, or no frame at all over
  /// direct USB): it is not in bootloader mode or it is not there.
  noAnswer,

  /// The identify body carried no usable page size: a pre-v2 bootloader, which this client refuses
  /// rather than guessing at. Nothing was sent beyond the identify.
  oldBootloader,

  /// The core refused a request as malformed (`0x02`): a bug in this client, not in the node.
  malformed,

  /// Every bounded retry was refused because another passthrough request was outstanding
  /// (`0x01`). Something else is using the core's relay.
  busy,

  /// A FAIL with no reason byte: a core older than the reason table, so the failure cannot be
  /// classified. Never reported as one of the four it might have been.
  unreportedRefusal,

  /// The node stopped answering (`0x03`, or a request that never came back over direct USB).
  nodeSilent,

  /// The core did not answer the app's own request. The node may well be fine.
  coreSilent,

  /// The node refused an operation (`0x04`): a misaligned erase, a write into a page that was not
  /// erased, or an address outside the space.
  nodeRefused,

  /// Five correction rounds ran and the space still does not read back.
  verifyMismatch,

  /// The caller's `isCancelled` said stop.
  cancelled,

  /// The run outlived its `deadline`.
  deadline;

  /// A short name for the failure class.
  String get label => switch (this) {
    FlashFault.noAnswer => 'no answer',
    FlashFault.oldBootloader => 'old bootloader',
    FlashFault.malformed => 'malformed request',
    FlashFault.busy => 'the relay stayed busy',
    FlashFault.unreportedRefusal => 'refused without a reason',
    FlashFault.nodeSilent => 'node silent',
    FlashFault.coreSilent => 'core silent',
    FlashFault.nodeRefused => 'node refused',
    FlashFault.verifyMismatch => 'image does not read back',
    FlashFault.cancelled => 'cancelled',
    FlashFault.deadline => 'out of time',
  };

  /// What the user has to do about it.
  ///
  /// A bootloader never jumps to the app by itself and no packet can reset a node, so a device
  /// left in bootloader mode needs a power cycle from a human.
  String get remedy => switch (this) {
    FlashFault.noAnswer || FlashFault.nodeSilent =>
      'Power-cycle the node: it is still in its bootloader and nothing the app sends can reset '
          'it. Then hold its button while powering it on and retry.',
    FlashFault.oldBootloader =>
      'Update the bootloader first: this client speaks protocol v2 and refuses to guess at the '
          'older encoding. Nothing was written.',
    FlashFault.malformed =>
      'This is a bug in the app, not in the device: the core could not encode the request. '
          'Nothing was written past that point; report the request named above.',
    FlashFault.busy =>
      'Something else is using the core\'s bootloader relay (another upload, a script, another '
          'app). Wait for it to finish and retry.',
    FlashFault.unreportedRefusal =>
      'The core answered a failure with no reason byte, so the app cannot tell a busy relay from '
          'a dead node. Update the core firmware, which now always carries a reason, and retry.',
    FlashFault.coreSilent =>
      'The core stopped answering the app. Check the link (USB/BLE), then retry: the node holds '
          'whatever was written so far, which is safe to overwrite.',
    FlashFault.nodeRefused =>
      'Power-cycle the node: it is still in its bootloader, and no packet can reset it. A refused '
          'erase or write means the page was not blank when the write arrived; retrying from the '
          'start of the upload erases it again.',
    FlashFault.verifyMismatch =>
      'Power-cycle the node: it is still in its bootloader, and no packet can reset it. If the '
          'same offsets fail again, the node\'s flash or its read path is at fault rather than the '
          'image.',
    FlashFault.cancelled =>
      'The node holds a partial image and is still in its bootloader; it is safe to write over. '
          'Retry when ready.',
    FlashFault.deadline =>
      'The node holds a partial image. Check the link, then retry: a partial image is safe to '
          'overwrite.',
  };
}

/// One chunk a read-back returned different from what should be there.
class FlashMismatch {
  const FlashMismatch(
    this.offset, {
    this.at,
    this.expected,
    this.actual,
    this.length,
  });

  /// The chunk's offset in the app space.
  final int offset;

  /// The first byte that differs: its position in the chunk, the byte wanted and the byte read
  /// back. Null when the read came back a different length than a chunk.
  final int? at;
  final int? expected;
  final int? actual;

  /// The length the read came back with, when it was not a whole chunk.
  final int? length;

  /// `0x3400 (byte 3: wanted 0x00, read 0x4e)`, or `0x3400` when the read had another length.
  String get description {
    final byte = at, want = expected, read = actual;
    if (byte == null || want == null || read == null) {
      return '0x${offset.toRadixString(16)}';
    }
    return '0x${offset.toRadixString(16)} '
        '(byte $byte: wanted ${_hex8(want)}, read ${_hex8(read)})';
  }

  static String _hex8(int b) => '0x${b.toRadixString(16).padLeft(2, '0')}';
}

/// What the read-back pass found: how many chunks differ from the model, and which.
class FlashVerifyReport {
  /// How many offsets [summary] names.
  static const int named = 8;

  /// The app space's chunk count, so the summary can say how much of it was looked at.
  final int total;

  /// Every differing chunk, in offset order.
  final List<FlashMismatch> all;

  /// The erase page size, so the report can say which pages a correction has to rewrite.
  final int pageSize;

  const FlashVerifyReport({
    required this.total,
    required this.all,
    this.pageSize = Bootloader.payloadSize,
  });

  int get count => all.length;

  /// The pages the differing chunks fall in, ascending - what a correction round rewrites.
  List<int> get pages {
    final set = <int>{};
    for (final m in all) {
      set.add(m.offset - m.offset % pageSize);
    }
    final list = set.toList()..sort();
    return list;
  }

  /// The first [named] offsets, in offset order.
  List<FlashMismatch> get first => all.take(named).toList();

  /// The first difference, in offset order.
  FlashMismatch? get firstDifference => all.isEmpty ? null : all.first;

  /// `2 of 448 chunks differ in 1 page (0x3400); first: 0x3400 (byte 3: wanted 0x00, read
  /// 0x4e)` - the sentence a page shows and a handoff quotes.
  String get summary {
    final shown = first.map((m) => m.description).join(', ');
    final rest = count > named ? ' (+${count - named} more)' : '';
    final where = pageSize > Bootloader.payloadSize
        ? ' in ${pages.length} page(s) (${pages.take(named).map((p) => '0x${p.toRadixString(16)}').join(', ')})'
        : '';
    return '$count of $total chunks differ$where; first: $shown$rest';
  }
}

/// A flash run that stopped without confirming the image: the user cancelled it, its deadline
/// passed, the node went silent, the core refused, or the space does not read back.
///
/// Thrown rather than returned because every one of these leaves the device with a partial image
/// and nothing to confirm.
class FlashAborted implements Exception {
  const FlashAborted(
    this.reason, {
    required this.fault,
    this.chunk,
    this.total,
    this.mismatches,
  });

  /// What stopped the run, in one sentence a person can act on.
  final String reason;

  /// Which failure class stopped it; [remedy] follows from it.
  final FlashFault fault;

  /// The app-space chunk being handled when it stopped, when there was one.
  final int? chunk;

  /// The app space's chunk count, when it was known.
  final int? total;

  /// What the read-back found, when a mismatch stopped the run.
  final FlashVerifyReport? mismatches;

  /// What the user has to do about it.
  String get remedy => fault.remedy;

  /// The offsets that disagree, when the run stopped on a mismatch.
  String get offsets => mismatches?.summary ?? '';

  /// `chunk 812 of 13512: the node stopped answering (...)` - the page prefixes its own
  /// `Update stopped (...)` line, so this does not repeat it.
  String get message => chunk == null
      ? reason
      : 'chunk $chunk${total == null ? '' : ' of $total'}: $reason';

  @override
  String toString() => message;
}

/// Flashes a device's main binary with protocol v2: identify, a per-page delta write, the tail
/// blanked, then two measures and up to five correction rounds. See the library doc.
class BootloaderClient {
  /// The low-level channel (passthrough by default, so [coreId] selects the relay core).
  final BootloaderTransport transport;

  /// Window for one request. The core answers within 1 s of a passthrough request (its own
  /// deadline), plus the app's link, so 4 s leaves the core's verdict time to arrive; a bigger
  /// window is grown on a link-level retry.
  final Duration requestTimeout;

  /// Growth applied to the window on a link-level retry.
  final double timeoutGrowth;

  /// Retries for a request the CORE did not answer at all (the app's link). Never used for the
  /// four reasons the core itself reports.
  final int linkAttempts;

  /// The first busy-backoff step: the settle measured between requests on the bench.
  final Duration busyBackoff;

  /// The cap the busy backoff doubles up to.
  final Duration busyBackoffCap;

  /// Attempts per request while the core answers `0x01` (busy).
  final int busyAttempts;

  /// Correction rounds an upload may run before it fails naming the offsets.
  final int correctionRounds;

  /// How long one [flash] run may take as a whole - see the library doc for the arithmetic.
  /// Enforced in [flash], between every request.
  static const Duration flashDeadline = Duration(minutes: 10);

  BootloaderClient({
    int coreId = 1,
    BootloaderTransport? transport,
    this.requestTimeout = const Duration(seconds: 4),
    this.timeoutGrowth = 1.5,
    this.linkAttempts = 3,
    this.busyBackoff = const Duration(milliseconds: 250),
    this.busyBackoffCap = const Duration(seconds: 2),
    this.busyAttempts = 6,
    this.correctionRounds = 5,
  }) : transport = transport ?? PassthroughTransport(coreId: coreId);

  Duration _grow(Duration timeout) {
    final grown = (timeout.inMicroseconds * timeoutGrowth).round();
    return Duration(microseconds: grown.clamp(1000, 30000000));
  }

  /// Runs one request under the retry policy: `0x01` waits and retries, a request the core never
  /// answered retries with a wider window, and every other failure is final.
  Future<T> _exchange<T>(
    String what,
    Future<T> Function(Duration timeout) call,
  ) async {
    var timeout = requestTimeout;
    var wait = busyBackoff;
    var busy = 0;
    var lost = 0;
    while (true) {
      try {
        return await call(timeout);
      } on PassthroughFailure catch (failure) {
        switch (failure.reason) {
          case PassthroughReason.busy:
            busy++;
            if (busy >= busyAttempts) rethrow;
            await Future<void>.delayed(wait);
            wait = Duration(
              microseconds: math.min(
                wait.inMicroseconds * 2,
                busyBackoffCap.inMicroseconds,
              ),
            );
          case PassthroughReason.linkLost:
            lost++;
            if (lost >= linkAttempts) rethrow;
            await Future<void>.delayed(wait);
            timeout = _grow(timeout);
          case PassthroughReason.malformed:
          case PassthroughReason.nodeSilent:
          case PassthroughReason.nodeRefused:
          case PassthroughReason.unreported:
            rethrow;
        }
      }
    }
  }

  /// The fault a failure maps to, for the ones that end a run.
  static FlashFault _faultOf(PassthroughReason reason) => switch (reason) {
    PassthroughReason.busy => FlashFault.busy,
    PassthroughReason.malformed => FlashFault.malformed,
    PassthroughReason.nodeSilent => FlashFault.nodeSilent,
    PassthroughReason.nodeRefused => FlashFault.nodeRefused,
    PassthroughReason.unreported => FlashFault.unreportedRefusal,
    PassthroughReason.linkLost => FlashFault.coreSilent,
  };

  /// The identify body's geometry, or a refusal: a bootloader that reports no plausible page size
  /// is a pre-v2 one and nothing else is sent to it.
  static BootloaderIdentity _checked(BootloaderIdentity id) {
    if (!Bootloader.pageSizePlausible(id.pageSize) ||
        id.appSize <= 0 ||
        id.appSize % id.pageSize != 0) {
      throw FlashAborted(
        'the bootloader reports an app space of ${id.appSize} B and a page size of '
        '${id.pageSize} B, which is not a usable geometry - it is a pre-v2 bootloader '
        '(the page size is where the old protocol kept its 32-byte name), so nothing was '
        'written',
        fault: FlashFault.oldBootloader,
      );
    }
    return id;
  }

  /// Asks the device to identify itself. Null when the node did not answer (the core's `0x03`),
  /// so a page can show "no bootloader reply" for a node that is simply not in bootloader mode;
  /// throws [FlashAborted] when the answer is a pre-v2 bootloader.
  Future<BootloaderIdentity?> identify() async {
    final BootloaderIdentity? id;
    try {
      id = await _exchange('identify', transport.identify);
    } on PassthroughFailure catch (failure) {
      if (failure.reason == PassthroughReason.nodeSilent ||
          failure.reason == PassthroughReason.linkLost) {
        return null;
      }
      throw FlashAborted(failure.message, fault: _faultOf(failure.reason));
    }
    return id == null ? null : _checked(id);
  }

  /// Reads one 32-byte chunk, with the request policy applied.
  Future<Uint8List> readChunk(int offset) =>
      _exchange('read 0x${offset.toRadixString(16)}', (t) {
        return transport.readChunk(offset, t);
      });

  /// Blanks one page, with the request policy applied.
  Future<int> erasePage(int offset) =>
      _exchange('erase 0x${offset.toRadixString(16)}', (t) {
        return transport.erasePage(offset, t);
      });

  /// Writes one 32-byte chunk and says whether the device confirmed it.
  Future<WriteAck> writeChunk(int offset, List<int> data) =>
      _exchange('write 0x${offset.toRadixString(16)}', (t) {
        return transport.writeChunk(offset, Bootloader.padPayload(data), t);
      });

  /// The CRC of the whole app space.
  Future<int> crc() => _exchange('CRC', transport.crc);

  /// Flashes [image] and verifies it with two measures.
  ///
  /// [maxImageSize] refuses an image the device's app space cannot hold before anything is sent;
  /// [expectedCrc] is optional and must equal the client's own whole-space CRC (a caller that
  /// computed it independently - the two must agree, or the value is a stale one and the run is
  /// refused rather than compared against it). [deadline] bounds the run as a whole and
  /// [isCancelled] is the user's way out; both are checked between requests.
  Future<FlashResult> flash(
    Uint8List image, {
    int? maxImageSize,
    int? expectedCrc,
    void Function(FlashProgress progress)? onProgress,
    Duration deadline = flashDeadline,
    bool Function()? isCancelled,
  }) async {
    if (maxImageSize != null && image.length > maxImageSize) {
      throw ArgumentError(
        'image is ${image.length} bytes; the device app space is $maxImageSize',
      );
    }
    if (image.isEmpty) {
      throw ArgumentError('image is empty: there is nothing to upload');
    }
    try {
      return await _flash(
        image,
        expectedCrc: expectedCrc,
        deadline: deadline,
        isCancelled: isCancelled,
        onProgress: onProgress,
      );
    } on PassthroughFailure catch (failure) {
      throw FlashAborted(failure.message, fault: _faultOf(failure.reason));
    }
  }

  /// Flashes a `.fw.zip` package: verifies its manifest, extracts the image, and runs the
  /// flash sequence. Returns a [FlashResult] that includes the manifest's [version],
  /// [buildCommit] and [target] for the progress UI.
  ///
  /// The package bytes are checked by [verifyFirmwarePackage] against [expectedTarget] (the
  /// route's target: [routeTarget]). A package that fails any check throws [PackageRefused]
  /// with every failed check's sentence - nothing is written. A bare image (not a `.fw.zip`)
  /// is refused by name.
  ///
  /// On success the image from the manifest is flashed with the same sequence as [flash]:
  /// identify, delta write, tail blank, two measures, up to five correction rounds.
  /// The [expectedCrc] passed to the flash run is the manifest's `crc32_padded` (the value the
  /// device's CRC command returns for this image), so the final CRC check matches the manifest.
  Future<FlashResult> flashPackage(
    Uint8List packageBytes, {
    required String expectedTarget,
    String name = '',
    void Function(FlashProgress progress)? onProgress,
    Duration deadline = flashDeadline,
    bool Function()? isCancelled,
  }) async {
    // Verify the package and extract the manifest info.
    final package = verifyFirmwarePackage(
      bytes: packageBytes,
      expectedTarget: expectedTarget,
      name: name,
    );

    // Flash the image from the package. The manifest's crc32_padded is what the device
    // answers for this image, so pass it as expectedCrc for the final CRC check.
    // maxImageSize is omitted - the flash run will check against the device's actual
    // app space size after identify.
    final result = await flash(
      package.image,
      expectedCrc: package.crc32Padded,
      deadline: deadline,
      isCancelled: isCancelled,
      onProgress: onProgress,
    );

    // Return a new FlashResult that includes the manifest metadata.
    return FlashResult(
      verify: result.verify,
      pagesCompared: result.pagesCompared,
      pagesKept: result.pagesKept,
      pagesWritten: result.pagesWritten,
      pagesBlanked: result.pagesBlanked,
      pagesProtected: result.pagesProtected,
      chunksWritten: result.chunksWritten,
      chunksSkipped: result.chunksSkipped,
      correctionRounds: result.correctionRounds,
      rounds: result.rounds,
      version: package.version,
      buildCommit: package.buildCommit,
      target: package.target,
    );
  }

  Future<FlashResult> _flash(
    Uint8List image, {
    int? expectedCrc,
    required Duration deadline,
    bool Function()? isCancelled,
    void Function(FlashProgress progress)? onProgress,
  }) async {
    final started = DateTime.now();
    var reads = 0;

    void checkRun(String phase) {
      if (isCancelled?.call() ?? false) {
        throw FlashAborted(
          'you cancelled it while $phase',
          fault: FlashFault.cancelled,
        );
      }
      if (DateTime.now().difference(started) > deadline) {
        throw FlashAborted(
          'it passed its ${deadline.inMinutes} minute deadline while $phase',
          fault: FlashFault.deadline,
        );
      }
    }

    // 1. Identify: nothing is sent to a device that does not answer, and nothing at all to one
    //    that speaks the pre-v2 encoding.
    checkRun('identifying');
    final BootloaderIdentity? identity;
    try {
      identity = await _exchange('identify', transport.identify);
    } on PassthroughFailure catch (failure) {
      throw FlashAborted(
        'identify failed: ${failure.message}',
        fault: _faultOf(failure.reason),
      );
    }
    if (identity == null) {
      throw const FlashAborted(
        'the node did not answer identify: it is not in bootloader mode, or it is not there',
        fault: FlashFault.noAnswer,
      );
    }
    final id = _checked(identity);
    final pageSize = id.pageSize;
    final appSize = id.appSize;
    if (image.length > appSize) {
      throw ArgumentError(
        'image is ${image.length} bytes; ${id.name} has an app space of $appSize',
      );
    }
    // The image's own pages end at a whole page; the rest of the space is the tail.
    final imageEnd = Bootloader.paddedLength(image.length);
    if (imageEnd > appSize) {
      throw ArgumentError(
        'image is ${image.length} bytes, which needs ${imageEnd - appSize} bytes more than '
        'the $appSize byte app space to fill its last page',
      );
    }

    // The model: what the space should hold once the upload is done. The image, erased 0xFF to
    // the end of the space - and, for each tail page the device refuses to blank, the bytes it
    // already holds (read before anything is written). Both measures are compared with THIS.
    final space = Uint8List(appSize)..fillRange(0, appSize, 0xFF);
    space.setRange(0, image.length, image);

    // 2. Compare, page by page: only the pages that differ are written at all.
    final differing = <int>[];
    var kept = 0;
    final pages = imageEnd ~/ pageSize;
    for (var page = 0; page < imageEnd; page += pageSize) {
      checkRun('comparing');
      final want = Uint8List.sublistView(space, page, page + pageSize);
      final same = await _pageMatches(page, pageSize, want, () => reads++);
      if (same) {
        kept++;
      } else {
        differing.add(page);
      }
      onProgress?.call(
        FlashProgress(
          phase: FlashPhase.comparing,
          completed: page ~/ pageSize + 1,
          total: pages,
        ),
      );
    }

    // 3. Write the pages that differ, and blank the tail.
    var chunksWritten = 0;
    var chunksSkipped = 0;
    var pagesWritten = 0;
    for (var i = 0; i < differing.length; i++) {
      checkRun('writing');
      final written = await _rewritePage(
        differing[i],
        pageSize,
        space,
        onChunk: (written, skipped) {
          chunksWritten += written;
          chunksSkipped += skipped;
        },
      );
      pagesWritten += written;
      onProgress?.call(
        FlashProgress(
          phase: FlashPhase.writing,
          completed: i + 1,
          total: differing.length,
        ),
      );
    }

    var pagesBlanked = 0;
    final protectedPages = <int>{};
    for (var page = imageEnd; page < appSize; page += pageSize) {
      checkRun('blanking the tail');
      // Read first: if the page is already blank there is nothing to do, and if the node will not
      // erase it, these bytes are what the space holds and what both measures have to expect.
      final held = await _readPage(page, pageSize, () => reads++);
      if (_allErased(held)) continue;
      try {
        await erasePage(page);
        pagesBlanked++;
        // An erase ack means "this page is blank now", so the model's 0xFF is right.
      } on PassthroughFailure catch (failure) {
        if (failure.reason != PassthroughReason.nodeRefused) rethrow;
        // The node will not blank this page (the DAS's storage region is inside its app space and
        // its erase stops at the code space's end). Take what it holds as the model's content.
        protectedPages.add(page);
        space.setRange(page, page + pageSize, held);
      }
      onProgress?.call(
        FlashProgress(
          phase: FlashPhase.writing,
          completed: pagesBlanked,
          total: (appSize - imageEnd) ~/ pageSize,
        ),
      );
    }

    // The CRC the device has to answer: the model's, not the image's - the difference is any
    // region the device refuses to blank, which the host had to read to know.
    final expected = Bootloader.crc32(space);
    if (expectedCrc != null && expectedCrc != expected) {
      throw ArgumentError(
        'the caller\'s expected CRC ${_hex32(expectedCrc)} is not this space\'s '
        '${_hex32(expected)} - the value does not describe the image plus the '
        '${protectedPages.length} page(s) the device will not blank',
      );
    }

    // 4./5. Two measures, then up to five correction rounds on the pages the read-back names.
    var rounds = 0;
    var corrections = 0;
    var verify = await _measure(
      space,
      pageSize: pageSize,
      expected: expected,
      countRead: () => reads++,
      checkRun: checkRun,
      onProgress: onProgress,
    );
    rounds = 1;
    while (!verify.clean) {
      final report = verify.differences;
      if (report == null || report.count == 0) {
        // The measures disagree but not one byte differs: nothing a rewrite could settle. Report
        // both and stop - the run does not pick a side.
        throw FlashAborted(
          '${verify.outcome.label}: ${verify.note}',
          fault: FlashFault.verifyMismatch,
        );
      }
      if (rounds > correctionRounds) {
        throw FlashAborted(
          'five correction rounds and the space still does not read back: '
          '${report.summary} (${verify.outcome.label}: ${verify.note})',
          fault: FlashFault.verifyMismatch,
          mismatches: report,
        );
      }
      // Correct exactly the pages the read-back flagged - never one the device will not blank,
      // which no rewrite of ours can change.
      final targets = report.pages
          .where((page) => !protectedPages.contains(page))
          .toList();
      if (targets.isEmpty) {
        throw FlashAborted(
          'the only differing pages are the ones the device will not blank: '
          '${report.summary}',
          fault: FlashFault.verifyMismatch,
          mismatches: report,
        );
      }
      for (var i = 0; i < targets.length; i++) {
        checkRun('correcting');
        await _rewritePage(
          targets[i],
          pageSize,
          space,
          onChunk: (written, skipped) {
            chunksWritten += written;
            chunksSkipped += skipped;
            corrections += written;
          },
        );
        pagesWritten++;
        onProgress?.call(
          FlashProgress(
            phase: FlashPhase.correcting,
            completed: i + 1,
            total: targets.length,
            mismatches: report.count,
          ),
        );
      }
      verify = await _measure(
        space,
        pageSize: pageSize,
        expected: expected,
        countRead: () => reads++,
        checkRun: checkRun,
        onProgress: onProgress,
      );
      rounds++;
    }

    onProgress?.call(
      FlashProgress(
        phase: FlashPhase.done,
        completed: appSize ~/ Bootloader.payloadSize,
        total: appSize ~/ Bootloader.payloadSize,
      ),
    );
    return FlashResult(
      verify: FlashVerifyResult(
        outcome: verify.outcome,
        crc: verify.crc,
        expectedCrc: expected,
        readBacks: verify.readBacks,
        reads: reads,
        differences: verify.differences,
        corrected: corrections,
      ),
      pagesCompared: pages,
      pagesKept: kept,
      pagesWritten: pagesWritten,
      pagesBlanked: pagesBlanked,
      pagesProtected: protectedPages.length,
      chunksWritten: chunksWritten,
      chunksSkipped: chunksSkipped,
      correctionRounds: rounds - 1,
      rounds: rounds,
    );
  }

  /// Verifies the device's space against [image] WITHOUT writing a byte.
  ///
  /// The tail past the image is whatever the device holds (a read-only pass cannot blank it), so
  /// the comparison is the image plus those bytes; a clean result means the image is there.
  Future<FlashVerifyResult> verify(Uint8List image) async {
    final BootloaderIdentity? identity;
    try {
      identity = await _exchange('identify', transport.identify);
    } on PassthroughFailure catch (failure) {
      throw FlashAborted(
        'identify failed: ${failure.message}',
        fault: _faultOf(failure.reason),
      );
    }
    if (identity == null) {
      throw const FlashAborted(
        'the node did not answer identify: it is not in bootloader mode, or it is not there',
        fault: FlashFault.noAnswer,
      );
    }
    final id = _checked(identity);
    final imageEnd = Bootloader.paddedLength(image.length);
    if (imageEnd > id.appSize) {
      throw ArgumentError(
        'image is ${image.length} bytes; ${id.name} has an app space of ${id.appSize}',
      );
    }
    final space = Uint8List(id.appSize)..fillRange(0, id.appSize, 0xFF);
    space.setRange(0, image.length, image);
    var reads = 0;
    if (imageEnd < id.appSize) {
      final tail = await _readPage(
        imageEnd,
        id.appSize - imageEnd,
        () => reads++,
      );
      space.setRange(imageEnd, id.appSize, tail);
    }
    final result = await _measure(
      space,
      pageSize: id.pageSize,
      expected: Bootloader.crc32(space),
      countRead: () => reads++,
    );

    return FlashVerifyResult(
      outcome: result.outcome,
      crc: result.crc,
      expectedCrc: result.expectedCrc,
      readBacks: result.readBacks,
      reads: reads,
      differences: result.differences,
    );
  }

  /// Jumps the device from bootloader mode back to its application.
  /// Returns true when the jump ack was received.
  Future<bool> jump() => _exchange('jump', transport.jump);

  /// Puts the addressed node into bootloader mode by sending Device service CID 4.
  /// Returns true if the device acknowledged and entered bootloader mode.
  Future<bool> enterBootloader() => _exchange('enter bootloader', transport.enterBootloader);

  /// Automatically detects the bootloader transport for [deviceId].
  ///
  /// Tries transports in priority order:
  /// 1. [DirectUsbTransport] if USB is connected (core's own USB bootloader)
  /// 2. [PassthroughTransport] if [deviceId] is known on the bus (node via core)
  /// 3. [BleBootloaderTransport] if BLE is connected to that device
  ///
  /// Returns the first transport that answers [BootloaderTransport.identify] with a
  /// valid bootloader response, or null if none respond.
  static Future<BootloaderTransport?> detectTransport(int deviceId) async {
    final link = ConnectionManager.instance;
    const timeout = Duration(seconds: 2);

    // 1. USB connected -> try DirectUsbTransport (core's own USB bootloader)
    if (link.isConnected && link.transport is UsbTransport) {
      final usb = link.transport as UsbTransport;
      final transport = DirectUsbTransport(usb);
      final id = await _tryIdentify(transport, timeout);
      if (id != null) return transport;
    }

    // 2. Device known on the bus -> try PassthroughTransport (node via core)
    final db = DeviceDatabase.instance;
    final entry = db.byId(deviceId);
    if (entry != null && !entry.stale && link.isConnected) {
      final transport = PassthroughTransport(coreId: coreId);
      final id = await _tryIdentify(transport, timeout);
      if (id != null) return transport;
    }

    // 3. BLE connected to that device -> try BleBootloaderTransport
    if (link.isConnected && link.transport is BleTransport) {
      final ble = link.transport as BleTransport;
      if (ble.displayName.contains(idToString(deviceId))) {
        final transport = BleBootloaderTransport(deviceId: ble.id);
        try {
          await transport.connect();
        } catch (_) {
          // Connection failed, try next transport
        }
        final id = await _tryIdentify(transport, timeout);
        if (id != null) return transport;
        await transport.close();
      }
    }

    return null;
  }

  /// Tries [BootloaderTransport.identify] on [transport] with [timeout].
  /// Returns the identity on success, null on any failure.
  static Future<BootloaderIdentity?> _tryIdentify(
    BootloaderTransport transport,
    Duration timeout,
  ) async {
    try {
      final id = await transport.identify(timeout);
      if (id != null && Bootloader.pageSizePlausible(id.pageSize)) {
        return id;
      }
    } catch (_) {
      // Any failure means this transport didn't work
    }
    return null;
  }

  // --- the two measures ------------------------------------------------------

  /// One verification of [space]: the CRC, then the full read-back. Read-only.
  Future<FlashVerifyResult> _measure(
    Uint8List space, {
    required int pageSize,
    required int expected,
    required void Function() countRead,
    void Function(String phase)? checkRun,
    void Function(FlashProgress progress)? onProgress,
  }) async {
    checkRun?.call('verifying');
    final total = space.length ~/ Bootloader.payloadSize;
    onProgress?.call(
      FlashProgress(phase: FlashPhase.verifying, completed: 0, total: 2),
    );

    // (a) The CRC: one exchange over the whole app space. A node that refuses the request (a
    //     bootloader without the CRC command) leaves the read-back as the only measure.
    int? crc;
    try {
      crc = await this.crc();
    } on PassthroughFailure catch (failure) {
      if (failure.reason != PassthroughReason.nodeRefused &&
          failure.reason != PassthroughReason.unreported) {
        rethrow;
      }
    }
    onProgress?.call(
      FlashProgress(phase: FlashPhase.verifying, completed: 1, total: 2),
    );

    // (b) The full read-back, whatever the CRC said: a CRC that matches while a byte differs is a
    //     finding, not something to skip the pass over.
    final found = <FlashMismatch>[];
    for (
      var offset = 0;
      offset < space.length;
      offset += Bootloader.payloadSize
    ) {
      checkRun?.call('verifying');
      final data = await readChunk(offset);
      countRead();
      final byteAt = _firstDifference(
        data,
        space,
        offset,
        Bootloader.payloadSize,
      );
      if (byteAt != null) {
        found.add(
          byteAt < 0
              ? FlashMismatch(offset, length: data.length)
              : FlashMismatch(
                  offset,
                  at: byteAt,
                  expected: space[offset + byteAt],
                  actual: data[byteAt],
                ),
        );
      }
      onProgress?.call(
        FlashProgress(
          phase: FlashPhase.verifying,
          completed: offset ~/ Bootloader.payloadSize + 1,
          total: total,
          mismatches: found.length,
        ),
      );
    }

    final differences = found.isEmpty
        ? null
        : FlashVerifyReport(total: total, all: found, pageSize: pageSize);
    final FlashVerifyOutcome outcome;
    if (crc == null) {
      outcome = FlashVerifyOutcome.readBackOnly;
    } else if (crc == expected) {
      outcome = found.isEmpty
          ? FlashVerifyOutcome.confirmed
          : FlashVerifyOutcome.crcAgreesReadBackDiffers;
    } else {
      outcome = found.isEmpty
          ? FlashVerifyOutcome.crcDiffersReadBackAgrees
          : FlashVerifyOutcome.bothDiffer;
    }
    return FlashVerifyResult(
      outcome: outcome,
      crc: crc,
      expectedCrc: expected,
      readBacks: 1,
      reads: total,
      differences: differences,
    );
  }

  // --- page helpers ----------------------------------------------------------

  /// Whether [want] (a whole page) is what the device holds. Stops at the first differing chunk:
  /// the caller only needs to know whether the page has to be rewritten.
  Future<bool> _pageMatches(
    int page,
    int pageSize,
    Uint8List want,
    void Function() countRead,
  ) async {
    for (var off = 0; off < pageSize; off += Bootloader.payloadSize) {
      final data = await readChunk(page + off);
      countRead();
      if (data.length != Bootloader.payloadSize) return false;
      for (var i = 0; i < Bootloader.payloadSize; i++) {
        if (data[i] != want[off + i]) return false;
      }
    }
    return true;
  }

  /// Reads [length] bytes at [page] into one buffer (the tail's model and the comparison).
  Future<Uint8List> _readPage(
    int page,
    int length,
    void Function() countRead,
  ) async {
    final out = Uint8List(length);
    for (var off = 0; off < length; off += Bootloader.payloadSize) {
      final data = await readChunk(page + off);
      countRead();
      out.setRange(off, off + Bootloader.payloadSize, data);
    }
    return out;
  }

  /// Erases the page at [page] and writes the chunks of [space] that are not already blank.
  ///
  /// Returns how many pages it wrote (1). A chunk that wants all 0xFF is not sent: the erase left
  /// it blank, which is exactly what the chunk would program.
  Future<int> _rewritePage(
    int page,
    int pageSize,
    Uint8List space, {
    required void Function(int written, int skipped) onChunk,
  }) async {
    final acked = await erasePage(page);
    if (acked != page) {
      throw FlashAborted(
        'the node acknowledged an erase of 0x${acked.toRadixString(16)} for the page '
        '0x${page.toRadixString(16)}',
        fault: FlashFault.nodeRefused,
      );
    }
    var written = 0;
    var skipped = 0;
    for (var off = 0; off < pageSize; off += Bootloader.payloadSize) {
      final chunk = space.sublist(
        page + off,
        page + off + Bootloader.payloadSize,
      );
      if (_allErased(chunk)) {
        skipped++;
        continue;
      }
      final ack = await writeChunk(page + off, chunk);
      if (!ack.accepted) {
        throw FlashAborted(
          'the node did not take the chunk at 0x${(page + off).toRadixString(16)} '
          '(it answered 0x${(ack.acked ?? 0).toRadixString(16)}'
          '${ack.pageFailed ? ', the page is not blank' : ''})',
          fault: FlashFault.nodeRefused,
        );
      }
      written++;
    }
    onChunk(written, skipped);
    return 1;
  }

  static bool _allErased(List<int> bytes) {
    for (final b in bytes) {
      if (b != 0xFF) return false;
    }
    return true;
  }

  /// The first byte of [data] that differs from [want] at [offset], or -1 when the lengths
  /// differ, or null when the chunk is identical.
  static int? _firstDifference(
    List<int> data,
    List<int> want,
    int offset,
    int length,
  ) {
    if (data.length != length || offset + length > want.length) return -1;
    for (var i = 0; i < length; i++) {
      if (data[i] != want[offset + i]) return i;
    }
    return null;
  }
}
