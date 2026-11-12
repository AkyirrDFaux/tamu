/// Bootloader flashing client (Docs/Services/Bootloader.md).
///
/// The update is written first, then confirmed by ONE digest request. Writes are acknowledged:
/// every write request draws a write-response carrying the offset of the last successfully
/// written chunk, so the client can reconcile a chunk against the device's acknowledged progress
/// and end the run at the first chunk the node does not take.
///
/// Final verification is the app's job too, and it is READ-ONLY: one digest request over the
/// whole padded image and one comparison against the expected CRC32 (the package manifest's
/// `crc32_padded`, or the CRC of the image handed in). A node whose bootloader predates the
/// digest command answers nothing, so the client falls back to ONE read-back pass and says so
/// in the result. Either way the run never loops and never rewrites on a failed verify: a
/// mismatch only buys one read-back pass to name the first differing offset, and then the run
/// stops. A read timeout (or a relay `FAIL`) is retried with a growing timeout and never counts
/// as a mismatch; only a successful read whose bytes differ is a mistake.
///
/// The low-level channel lives behind [BootloaderTransport] so the same algorithm drives both
/// a node over the core's Device passthrough ([PassthroughTransport]) and the core over its
/// own USB bootloader ([DirectUsbTransport]).
library;

import 'dart:async';
import 'dart:typed_data';

import 'bootloader.dart';
import 'connection.dart';
import 'protocol.dart';
import 'transport.dart';
import 'types.dart';

/// Which stage of [BootloaderClient.flash] is running.
enum FlashPhase { writing, verifying, done }

/// A progress snapshot emitted by [BootloaderClient.flash].
class FlashProgress {
  final FlashPhase phase;

  /// Steps finished in this phase: image chunks while writing, and `0`/`1` while verifying (a
  /// digest request is one step; a read-back pass reports its chunks instead).
  final int completed;
  final int total;

  /// Chunks the current read-back has found different from what was written so far.
  final int mismatches;

  const FlashProgress({
    required this.phase,
    required this.completed,
    required this.total,
    this.mismatches = 0,
  });

  double get fraction => total == 0 ? 1.0 : completed / total;
}

/// Which pass confirmed the device's image.
enum FlashVerify {
  /// One digest request over the whole padded image, compared against the expected CRC32.
  digest,

  /// No digest from the node (a bootloader older than the digest command): one read-back pass,
  /// which is also why [FlashResult.note] names that fallback.
  readBackNoDigest;

  String get label => switch (this) {
    FlashVerify.digest => 'one digest',
    FlashVerify.readBackNoDigest => 'one read-back pass (no digest from the node)',
  };
}

/// The outcome of [BootloaderClient.flash]: the image the device holds is the image that was
/// handed in, confirmed by one digest request (or, for a bootloader without the digest command,
/// by one read-back pass). A run that cannot confirm that throws [FlashAborted] instead of
/// returning a result.
class FlashResult {
  /// Which pass confirmed the image.
  final FlashVerify method;

  /// The digest the node answered with, when it has the digest command.
  final int? digest;

  /// The value the digest was compared against: the caller's expected CRC32 (the package
  /// manifest's `crc32_padded`) or the client's own CRC of the padded image.
  final int expectedDigest;

  /// Write operations issued (the initial pass only: nothing is rewritten).
  final int writes;

  /// Chunk reads issued: 0 on the digest path, the image's chunk count on the fallback.
  final int reads;

  const FlashResult({
    required this.method,
    this.digest,
    required this.expectedDigest,
    required this.writes,
    required this.reads,
  });

  /// One line stating how the image was confirmed, for a log or a page.
  String get note => switch (method) {
    FlashVerify.digest =>
      'confirmed by ${method.label} over ${_hex32(expectedDigest)} '
          '(${_hex32(digest ?? 0)})',
    FlashVerify.readBackNoDigest =>
      'the node answered no digest - its bootloader predates the digest command - '
          'so the image was confirmed by ${method.label} ($reads chunks)',
  };
}

/// The outcome of [BootloaderClient.verify], which never writes: either the device holds the
/// image, or the first chunk that differs from it.
class FlashVerifyResult {
  /// Which pass looked.
  final FlashVerify method;

  /// The digest the node answered with, or null when it has no digest command.
  final int? digest;

  /// The value compared against (see [FlashResult.expectedDigest]).
  final int expectedDigest;

  /// Read-back passes over the image: 0 when the digest confirmed it.
  final int readBacks;

  /// Chunk reads issued.
  final int reads;

  /// The first differing chunk, when the image does not match. Null when it does.
  final FlashVerifyReport? mismatches;

  const FlashVerifyResult({
    required this.method,
    this.digest,
    required this.expectedDigest,
    required this.readBacks,
    required this.reads,
    this.mismatches,
  });

  /// True when the device holds exactly the image that was handed in: the digests agree, or -
  /// when the node has no digest command - the read-back found no difference.
  bool get clean => method == FlashVerify.digest
      ? digest == expectedDigest
      : (mismatches == null || mismatches!.count == 0);

  /// `digest 0x1234abcd, expected 0x1234abce` - the pair a mismatch is reported with.
  String get digestPair => method == FlashVerify.digest
      ? '${_hex32(digest ?? 0)} (expected ${_hex32(expectedDigest)})'
      : 'none (the node has no digest command)';

  /// `1` on the digest path, `0` when the node had no digest.
  int get digests => method == FlashVerify.digest ? 1 : 0;

  /// One line stating how the image was checked, for a log, a page or a rig run.
  String get note => switch (method) {
    FlashVerify.digest => clean
        ? 'confirmed by one digest over the padded image (${_hex32(expectedDigest)})'
        : 'the digest ${_hex32(digest ?? 0)} does not match '
              '${_hex32(expectedDigest)}',
    FlashVerify.readBackNoDigest =>
      'the node answered no digest - its bootloader predates the digest command - '
          'so the image was checked by one read-back pass ($reads chunks)',
  };
}

String _hex32(int value) =>
    '0x${value.toUnsigned(32).toRadixString(16).padLeft(8, '0')}';

/// A device's identity: the size of its main-binary space and its name.
class BootloaderIdentity {
  /// The app-space size in bytes (the identify-response's offset slot).
  final int appSize;

  /// The device name (the identify-response's 32-byte, null-padded payload).
  final String name;

  const BootloaderIdentity({required this.appSize, required this.name});

  @override
  String toString() => '$name ($appSize bytes)';
}

/// The outcome of one low-level write attempt.
class WriteAck {
  /// True when the channel reported the write accepted (and, when [lastWrittenOffset] is
  /// known, that the device reached this chunk).
  final bool accepted;

  /// The offset of the device's last successfully written chunk, or null when the channel
  /// could not expose it (a relay timeout carries no payload). A value of
  /// [Bootloader.nothingWritten] means nothing has been written yet.
  final int? lastWrittenOffset;

  const WriteAck(this.accepted, {this.lastWrittenOffset});
}

/// Low-level bootloader channel. One call is one attempt (no retries here); a timeout is
/// reported as null (or [TransportException] when the transport chooses to throw).
/// [BootloaderClient] adds the pacing and the retry policy.
abstract class BootloaderTransport {
  /// Writes one 32-byte chunk at [offset]. Both channels report the device's
  /// [WriteAck.lastWrittenOffset]; it is null only when a relay timeout carried no payload,
  /// in which case the verify pass catches a dropped write.
  Future<WriteAck> writeChunk(int offset, List<int> data, Duration timeout);

  /// Reads 32 bytes at [offset]. Returns null when the node answered FAIL or the request was
  /// missed; throws [TransportException] on timeout.
  Future<Uint8List?> readChunk(int offset, Duration timeout);

  /// Asks for the CRC32 of `[offset, offset + length)` of the device's app region (the digest
  /// command). Returns the CRC, or null when no digest came back: the node answered FAIL, the
  /// request was missed, or its bootloader predates the digest command and stayed silent.
  Future<int?> digest(int offset, int length, Duration timeout);

  /// Asks the device to identify itself. Returns null when it did not answer.
  Future<BootloaderIdentity?> identify(Duration timeout);
}

/// Core passthrough command IDs (Docs/Services/Bootloader.md "Core Bootloader Passthrough
/// Commands (002x)"): the core relays the app's request onto its RSBus and echoes the reply.
const int coreBootIdentify =
    0x20; // request identification (response: app size + name)
const int coreBootWrite =
    0x21; // send write packet      (request: offset + payload; response: last-written offset)
const int coreBootRead =
    0x22; // request read           (request: offset; response: offset + payload)
const int coreBootDigest =
    0x23; // request digest         (request: offset + length; response: the node's frame as-is)

/// The core's Device `0020/0021/0022/0023` passthrough: the core relays each raw bootloader
/// frame onto its RSBus, so the app reaches whatever node is in bootloader mode without knowing
/// its address. A write reply carries the node's last-written offset (the flag is SUCCESS only
/// when that offset names the requested chunk); reads return `offset + payload`; identification
/// returns `app size + name`; the digest returns the node's 11-byte digest response verbatim,
/// so this client decodes and compares it.
class PassthroughTransport implements BootloaderTransport {
  /// The core that relays our raw frames onto its RSBus.
  final int coreId;

  PassthroughTransport({this.coreId = 1});

  ConnectionManager get _link => ConnectionManager.instance;

  @override
  Future<WriteAck> writeChunk(
    int offset,
    List<int> data,
    Duration timeout,
  ) async {
    final payload = <int>[...uint32ToBytes(offset), ...data];
    final reply = await _link.requestWithFlags(
      coreId,
      ServiceType.device,
      coreBootWrite,
      payload: payload,
      timeout: timeout,
    );
    // The core relays the node's write-response offset; the flag is SUCCESS only when it names
    // this chunk. A FAIL still carries the offset (the node's last successfully written one),
    // and a timeout carries none, so the client can retry or resynchronise.
    final last = reply.payload.length >= 4
        ? uint32FromBytes(reply.payload, 0)
        : null;
    return WriteAck(reply.ok, lastWrittenOffset: last);
  }

  @override
  Future<Uint8List?> readChunk(int offset, Duration timeout) async {
    final reply = await _link.request(
      coreId,
      ServiceType.device,
      coreBootRead,
      payload: uint32ToBytes(offset),
      timeout: timeout,
    );
    if (reply.length < 4 + Bootloader.payloadSize) return null;
    return Uint8List.fromList(reply.sublist(4, 4 + Bootloader.payloadSize));
  }

  @override
  Future<int?> digest(int offset, int length, Duration timeout) async {
    final payload = <int>[
      ...uint32ToBytes(offset),
      length & 0xFF,
      (length >> 8) & 0xFF,
    ];
    final reply = await _link.requestWithFlags(
      coreId,
      ServiceType.device,
      coreBootDigest,
      payload: payload,
      timeout: timeout,
    );
    // The core forwards the node's digest response verbatim, framing included, so the frame
    // itself says whether it is this request's answer: the digest command, a valid parity, and
    // the offset that was asked about.
    if (!reply.ok || reply.payload.length < Bootloader.digestResponseSize) {
      return null;
    }
    final frame = reply.payload.sublist(0, Bootloader.digestResponseSize);
    if (Bootloader.decode(frame) != Bootloader.cmdDigestResponse) return null;
    if (Bootloader.offset(frame) != offset) return null;
    return Bootloader.digestCrc(frame);
  }

  @override
  Future<BootloaderIdentity?> identify(Duration timeout) async {
    final reply = await _link.requestWithFlags(
      coreId,
      ServiceType.device,
      coreBootIdentify,
      timeout: timeout,
    );
    if (!reply.ok || reply.payload.length < 4) return null;
    final appSize = uint32FromBytes(reply.payload, 0);
    final nameBytes = reply.payload.sublist(4);
    var n = nameBytes.length;
    while (n > 0 && nameBytes[n - 1] == 0) {
      n--;
    }
    return BootloaderIdentity(
      appSize: appSize,
      name: String.fromCharCodes(nameBytes.sublist(0, n)),
    );
  }
}

/// The core's own USB bootloader (Docs/Services/Bootloader.md): the raw 0xCA..0xBC frames go
/// straight to the USB Serial/JTAG port with no core relay. Each request's response frame is
/// parsed off the same port; a write is acknowledged by a write-response carrying the offset
/// of the last successfully written chunk, so the client can reconcile its progress.
class DirectUsbTransport implements BootloaderTransport {
  final UsbTransport usb;

  DirectUsbTransport(this.usb);

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
    );
    if (reply == null) return const WriteAck(false);
    final last = Bootloader.offset(reply);
    return WriteAck(last == offset, lastWrittenOffset: last);
  }

  @override
  Future<Uint8List?> readChunk(int offset, Duration timeout) async {
    final reply = await _requestFrame(
      Bootloader.encodeReadRequest(offset),
      command: Bootloader.cmdReadResponse,
      size: Bootloader.dataSize,
      matchOffset: offset,
      timeout: timeout,
    );
    if (reply == null) return null;
    return Uint8List.fromList(
      reply.sublist(
        Bootloader.headerSize,
        Bootloader.headerSize + Bootloader.payloadSize,
      ),
    );
  }

  @override
  Future<int?> digest(int offset, int length, Duration timeout) async {
    final reply = await _requestFrame(
      Bootloader.encodeDigestRequest(offset, length),
      command: Bootloader.cmdDigestResponse,
      size: Bootloader.digestResponseSize,
      matchOffset: offset,
      timeout: timeout,
    );
    if (reply == null) return null;
    return Bootloader.digestCrc(reply);
  }

  @override
  Future<BootloaderIdentity?> identify(Duration timeout) async {
    final reply = await _requestFrame(
      Bootloader.encodeIdentifyRequest(),
      command: Bootloader.cmdIdentifyResponse,
      size: Bootloader.dataSize,
      timeout: timeout,
    );
    if (reply == null) return null;
    return BootloaderIdentity(
      appSize: Bootloader.appSize(reply),
      name: Bootloader.name(reply),
    );
  }

  /// Writes [request] and waits for its response frame: a valid frame of [command] and
  /// [size], matched additionally on [matchOffset] when given (a late reply from a previous
  /// transaction at another offset is dropped rather than returned). Returns null on timeout.
  Future<Uint8List?> _requestFrame(
    List<int> request, {
    required int command,
    required int size,
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
        // Match the reply to the request: a valid frame (structure + parity) of the expected
        // command, and the requested offset when one is expected.
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

/// One 32-byte chunk of the image and its bootloader offset.
class _Chunk {
  final int offset;
  final Uint8List data;

  const _Chunk(this.offset, this.data);
}

/// Why a flash run stopped before the device's image was confirmed as the one that was sent.
///
/// The three device faults are separate faults with separate remedies, and the page names them
/// apart: [nodeSilent] is a link that died or a node that left bootloader mode, [writeRefused] is
/// a node that answered and would not program the chunk, [verifyMismatch] is a node that took the
/// data and did not keep it. [cancelled] and [deadline] are the user's own clock, not the device.
enum FlashFault {
  /// The node stopped answering: a write was never taken through all its attempts, or a read-back
  /// exhausted its attempts. Nothing the app sends can reset it.
  nodeSilent,

  /// The node answered and refused the chunk (a `FLAG_FAIL`, or a last-written offset naming a
  /// different chunk), through every attempt. Nothing was programmed there.
  writeRefused,

  /// The node accepted the chunk - and, for a confirmed write, named it in the write response -
  /// but the image it holds does not digest to what was sent. The erase/program cycle or the
  /// read path is at fault, not the link.
  verifyMismatch,

  /// The caller's `isCancelled` said stop.
  cancelled,

  /// The run outlived its `deadline`.
  deadline;

  /// A short name for the failure class, so a log line or a report cannot fold the three device
  /// faults into one message.
  String get label => switch (this) {
    FlashFault.nodeSilent => 'no answer',
    FlashFault.writeRefused => 'write refused',
    FlashFault.verifyMismatch => 'image does not read back',
    FlashFault.cancelled => 'cancelled',
    FlashFault.deadline => 'out of time',
  };

  /// What the user has to do about it.
  ///
  /// A bootloader never jumps to the app by itself and no packet can reset a node, so a device
  /// left in bootloader mode needs a power cycle from a human: a message that only says "retry"
  /// leaves it silently in its bootloader, which the owner cannot tell apart from a dead node.
  String get remedy => switch (this) {
    FlashFault.nodeSilent =>
      'Power-cycle the node: it is still in its bootloader and nothing the app sends can '
          'reset it. Then hold its button while powering it on and retry.',
    FlashFault.writeRefused =>
      'Power-cycle the node: nothing was programmed and no packet can reset it. Then put it '
          'back in bootloader mode and retry.',
    FlashFault.verifyMismatch =>
      'Power-cycle the node: it is still in its bootloader, and no packet can reset it. If '
          'the same offsets fail again, the node\'s flash or its read path is at fault rather '
          'than the image.',
    FlashFault.cancelled =>
      'The node holds a partial image and is still in its bootloader; it is safe to write '
          'over. Retry when ready.',
    FlashFault.deadline =>
      'The node holds a partial image. Check the link, then retry: a partial image is safe '
          'to overwrite.',
  };
}

/// One chunk a read-back returned different from what was written.
class FlashMismatch {
  const FlashMismatch(
    this.offset, {
    required this.writeConfirmed,
    this.at,
    this.expected,
    this.actual,
  });

  /// The chunk's offset in the app image.
  final int offset;

  /// True when the write pass got a per-chunk acknowledgement for this offset: the device named
  /// it as its last successfully written chunk, so it took the data and lost it. False when the
  /// channel only accepted the write without saying how far the device got (a passthrough relay
  /// timeout carries no offset), so the device may never have seen this chunk.
  final bool writeConfirmed;

  /// The first byte that differs: its position in the chunk, the byte written and the byte read
  /// back. Null when the read came back a different length than the chunk.
  final int? at;
  final int? expected;
  final int? actual;

  /// `0x40 (byte 3: wrote 0x00, read 0x4e)`, or `0x40` when the read had another length.
  String get description {
    final byte = at, wrote = expected, read = actual;
    if (byte == null || wrote == null || read == null) {
      return '0x${offset.toRadixString(16)}';
    }
    return '0x${offset.toRadixString(16)} '
        '(byte $byte: wrote ${_hex(wrote)}, read ${_hex(read)})';
  }

  static String _hex(int b) => '0x${b.toRadixString(16).padLeft(2, '0')}';
}

/// What the locating read-back found: the first chunk whose bytes differ from the image, and
/// whether the pass read the whole image or stopped at that first difference.
class FlashVerifyReport {
  /// How many offsets [summary] names.
  static const int named = 8;

  /// The image's chunk count, so the summary can say how much of it was looked at.
  final int total;

  /// Every differing chunk, in image order.
  final List<FlashMismatch> all;

  /// True when the pass read the whole image (the no-digest fallback, where the read-back is
  /// the only evidence). False when it stopped at the first difference - the digest already said
  /// the image is wrong, so the pass only had to name an offset.
  final bool complete;

  const FlashVerifyReport({
    required this.total,
    required this.all,
    this.complete = true,
  });

  int get count => all.length;

  /// Chunks the write pass acknowledged per chunk and that still do not match: the device took
  /// the data and lost it, which no rewrite can fix.
  int get confirmed => all.where((m) => m.writeConfirmed).length;

  /// Chunks whose write the channel only accepted without confirming it: the device may never
  /// have seen them.
  int get unconfirmed => count - confirmed;

  /// The first [named] offsets, in image order.
  List<FlashMismatch> get first => all.take(named).toList();

  /// The first difference, which is the one a stopped pass found.
  FlashMismatch? get firstDifference => all.isEmpty ? null : all.first;

  /// `first differing chunk: 0x40 (byte 3: wrote 0x00, read 0x4e)` when the pass stopped at the
  /// first difference, or `2 of 100 chunks differ (...); first: 0x0, 0x40` when it read the
  /// whole image.
  String get summary {
    if (!complete) {
      final only = firstDifference;
      return only == null
          ? 'no chunk differs'
          : 'first differing chunk: ${only.description}';
    }
    final shown = first.map((m) => m.description).join(', ');
    final rest = count > named ? ' (+${count - named} more)' : '';
    return '$count of $total chunks differ ($confirmed confirmed by the write pass, '
        '$unconfirmed not confirmed); first: $shown$rest';
  }
}

/// A flash run that stopped without confirming the image: the user cancelled it, its deadline
/// passed, a chunk was never programmed, the node stopped answering, or a chunk does not read
/// back.
///
/// Thrown rather than returned because every one of these leaves the device with a partial image
/// and nothing to confirm: the caller must hear it as a failure, not read a result as "it finished
/// and did not check out".
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

  /// The image chunk being handled when it stopped, when there was one.
  final int? chunk;

  /// The image's chunk count, when it was known.
  final int? total;

  /// What the read-back found, when a mismatch stopped the run.
  final FlashVerifyReport? mismatches;

  /// What the user has to do about it - a power cycle for the three device faults.
  String get remedy => fault.remedy;

  /// `chunk 812 of 13512: the node stopped answering (...)` - the page prefixes its own
  /// `Update stopped (...)` line, so this does not repeat it.
  String get message => chunk == null
      ? reason
      : 'chunk $chunk${total == null ? '' : ' of $total'}: $reason';

  @override
  String toString() => message;
}

/// Flashes a device's main binary: write it all, then confirm it with one digest request. A
/// verification never writes and never re-runs; see [flash] and [verify].
class BootloaderClient {
  /// The low-level channel (passthrough by default, so [coreId] selects the relay core).
  final BootloaderTransport transport;

  /// Delay between write requests. A node erases the 64-byte page on the first half of a
  /// chunk and programs both halves; that cycle outlasts a request (measured ~40 ms), so a
  /// frame arriving too soon is lost. Writes are paced past it; with the write acknowledgement
  /// a dropped write is retried immediately, and the digest at the end catches any that slipped
  /// through with an acknowledgement that did not name the chunk.
  final Duration writePacing;

  /// Timeout growth factor applied on each retry ("dynamic adjustment").
  final double timeoutGrowth;

  /// Attempts for a read before it is treated as unverified (a timeout alone is never a
  /// mismatch, so this is generous).
  final int readAttempts;

  /// Attempts for a write before it is dropped.
  final int writeAttempts;

  /// Attempts for a digest request before the client concludes the node has no digest command
  /// (an older bootloader stays silent) and falls back to one read-back pass. Two: a lost digest
  /// request gets a second, longer window, and the fallback still costs the image's reads.
  final int digestAttempts;

  /// How long one [flash] run may take as a whole. The app space is 32-byte chunks and the
  /// passthrough paces writes 50 ms apart, so a full image is ~11 minutes of paced writes alone,
  /// plus one digest request. Forty-five minutes is past any run that is making progress, and far
  /// short of the hours a node that left bootloader mode used to cost - that case is stopped at
  /// the first chunk that does not land, so this is the backstop, not the fix.
  static const Duration flashDeadline = Duration(minutes: 45);

  BootloaderClient({
    int coreId = 1,
    BootloaderTransport? transport,
    this.writePacing = const Duration(milliseconds: 50),
    this.timeoutGrowth = 1.5,
    this.readAttempts = 5,
    this.writeAttempts = 3,
    this.digestAttempts = 2,
  }) : transport = transport ?? PassthroughTransport(coreId: coreId);

  Duration _grow(Duration timeout) {
    final grown = (timeout.inMicroseconds * timeoutGrowth).round();
    return Duration(microseconds: grown.clamp(1000, 10000000));
  }

  /// Asks the device to identify itself (app size + name), retrying a miss with a growing
  /// window. Returns null when every attempt was missed.
  Future<BootloaderIdentity?> identify() async {
    var timeout = const Duration(seconds: 1);
    for (var attempt = 0; attempt < readAttempts; attempt++) {
      try {
        final id = await transport.identify(timeout);
        if (id != null) return id;
      } on TransportException {
        // Timeout: retry with a longer window.
      }
      timeout = _grow(timeout);
    }
    return null;
  }

  /// Writes one 32-byte chunk, retrying a timeout or a rejected write with a growing window and
  /// pacing after the attempt. Returns what the device acknowledged: [WriteAck.accepted] is false
  /// when no attempt was taken, and [WriteAck.lastWrittenOffset] is null when the channel carried
  /// an acceptance without saying which chunk the device reached (a passthrough relay timeout
  /// carries no offset) - the one case in which the read-back may find a chunk worth rewriting.
  ///
  /// When the channel exposes the device's last-written offset, a write is only accepted once
  /// that offset names this chunk, so a rejected chunk is retried here rather than read back.
  Future<WriteAck> writeChunkAck(int offset, List<int> data) async {
    var timeout = const Duration(seconds: 1);
    var ack = const WriteAck(false);
    for (var attempt = 0; attempt < writeAttempts; attempt++) {
      try {
        final reply = await transport.writeChunk(
          offset,
          Bootloader.padPayload(data),
          timeout,
        );
        final last = reply.lastWrittenOffset;
        // Reconcile against the device's acknowledged progress: a last-written offset that
        // does not name this chunk means the device did not program it, so retry.
        if (reply.accepted && (last == null || last == offset)) {
          ack = reply;
          break;
        }
      } on TransportException {
        // Timeout: retry with a longer window.
      }
      timeout = _grow(timeout);
    }
    if (writePacing > Duration.zero) {
      await Future<void>.delayed(writePacing);
    }
    return ack;
  }

  /// Writes one 32-byte chunk and says only whether the device confirmed it.
  Future<bool> writeChunk(int offset, List<int> data) async =>
      (await writeChunkAck(offset, data)).accepted;

  /// Reads one 32-byte chunk, retrying a timeout or a FAIL with a growing window. Returns the
  /// 32 bytes, or null when every attempt was missed (never counted as a mismatch by [flash]).
  Future<Uint8List?> readChunk(int offset) async {
    var timeout = const Duration(seconds: 1);
    for (var attempt = 0; attempt < readAttempts; attempt++) {
      try {
        final data = await transport.readChunk(offset, timeout);
        if (data != null) return data;
      } on TransportException {
        // Timeout: retry with a longer window.
      }
      timeout = _grow(timeout);
    }
    return null;
  }

  /// Flashes [image] and confirms it with ONE digest request.
  ///
  /// Two stages, and neither repeats:
  ///
  /// 1. One write pass over the image, each chunk confirmed against the device's write
  ///    acknowledgement. The first chunk the node does not take ends the run
  ///    ([FlashFault.writeRefused]) instead of walking the rest of the image into a node that has
  ///    left bootloader mode.
  /// 2. One verification, which never writes ([_verify]): a single digest request over
  ///    `[0, padded length)` and one comparison against [expectedDigest] (the package manifest's
  ///    `crc32_padded`) or, when the caller has none, the client's own CRC of the padded image.
  ///    A mismatch ends the run ([FlashFault.verifyMismatch]) naming the digest pair and the
  ///    first differing offset, which one read-back pass locates - it does not rewrite anything.
  ///    A node whose bootloader predates the digest command answers nothing, so the image is
  ///    confirmed by ONE read-back pass instead and the result says so.
  ///
  /// The bound, and why it replaced `maxPasses = 20`: writes are exactly the image, reads are at
  /// most the image (and none on the digest path), and no pass is repeated. The old loop read the
  /// whole image back, rewrote every mismatched chunk and verified again up to twenty times, so a
  /// link failing at the core - where every read comes back wrong - landed twenty erase/write
  /// cycles of the whole image on the node and made the owner's "it gets worse with each
  /// verification pass" true.
  ///
  /// [deadline] bounds the run as a whole and [isCancelled] is the user's way out; both are
  /// checked between chunks, so neither waits for a stage to finish.
  Future<FlashResult> flash(
    Uint8List image, {
    int? maxImageSize,
    int? expectedDigest,
    void Function(FlashProgress progress)? onProgress,
    Duration deadline = flashDeadline,
    bool Function()? isCancelled,
  }) async {
    // Refuse an image the device's app space cannot hold, before spending a write pass on it.
    // The caller gets the size from identify(); the device would reject the out-of-range writes
    // anyway, but only after the whole pass had run and failed verification.
    if (maxImageSize != null && image.length > maxImageSize) {
      throw ArgumentError(
        'image is ${image.length} bytes; the device app space is $maxImageSize',
      );
    }
    final chunks = _chunk(image);
    final total = chunks.length;
    final expected = expectedDigest ?? Bootloader.crc32Padded(image);
    var writes = 0;
    final started = DateTime.now();

    /// The two ways out that are not the device's fault, checked between chunks.
    void checkRun(int chunk, String phase) {
      if (isCancelled?.call() ?? false) {
        throw FlashAborted(
          'you cancelled it while $phase',
          fault: FlashFault.cancelled,
          chunk: chunk,
          total: total,
        );
      }
      if (DateTime.now().difference(started) > deadline) {
        throw FlashAborted(
          'it passed its ${deadline.inMinutes} minute deadline while $phase',
          fault: FlashFault.deadline,
          chunk: chunk,
          total: total,
        );
      }
    }

    // 1. One write pass. Each write is acknowledged, so a rejected chunk is retried right here;
    //    a chunk the node never takes ends the run. A chunk the channel accepted without naming
    //    it in the acknowledgement is remembered: the device may never have seen it, so the
    //    verification below can say whether the device had confirmed the chunk it finds wrong.
    final unconfirmed = <int>{};
    for (var i = 0; i < total; i++) {
      checkRun(i + 1, 'writing');
      final ack = await writeChunkAck(chunks[i].offset, chunks[i].data);
      if (!ack.accepted) {
        throw FlashAborted(
          'the node never confirmed the chunk at offset 0x'
          '${chunks[i].offset.toRadixString(16)} after $writeAttempts attempts',
          fault: FlashFault.writeRefused,
          chunk: i + 1,
          total: total,
        );
      }
      if (ack.lastWrittenOffset == null) unconfirmed.add(i);
      writes++;
      onProgress?.call(
        FlashProgress(
          phase: FlashPhase.writing,
          completed: i + 1,
          total: total,
        ),
      );
    }

    // 2. The one verification: a digest request and a comparison, or - for a node without the
    //    digest command - one read-back pass. Read-only by construction, and never re-run.
    final verify = await _verify(
      chunks,
      expected: expected,
      unconfirmed: unconfirmed,
      checkRun: checkRun,
      onProgress: onProgress,
    );

    if (verify.clean) {
      onProgress?.call(
        FlashProgress(phase: FlashPhase.done, completed: total, total: total),
      );
      return FlashResult(
        method: verify.method,
        digest: verify.digest,
        expectedDigest: expected,
        writes: writes,
        reads: verify.reads,
      );
    }

    // A mismatch ends the run: the write pass is not repeated, and nothing is rewritten. The
    // report names the first differing offset; the digest pair says the whole image is wrong.
    final report = verify.mismatches!;
    final first = report.firstDifference;
    throw FlashAborted(
      verify.method == FlashVerify.digest
          ? 'the image does not match: digest ${_hex32(verify.digest ?? 0)} '
                '!= expected ${_hex32(expected)}; ${report.summary}'
          : 'the image does not match: ${report.summary}',
      fault: FlashFault.verifyMismatch,
      chunk: first == null ? null : first.offset ~/ _payloadSize + 1,
      total: total,
      mismatches: report,
    );
  }

  /// Verifies the device's image against [image] WITHOUT writing a byte: one digest request over
  /// `[0, padded length)` and one comparison against [expectedDigest] (or the client's own CRC of
  /// the padded image). A mismatch costs one read-back pass, which stops at the first chunk that
  /// differs, and then the answer is returned - it is never re-run. A node whose bootloader has
  /// no digest command is confirmed by one read-back pass instead and the result says so.
  Future<FlashVerifyResult> verify(Uint8List image, {int? expectedDigest}) =>
      _verify(
        _chunk(image),
        expected: expectedDigest ?? Bootloader.crc32Padded(image),
        unconfirmed: const <int>{},
      );

  /// Asks for the CRC32 of `[offset, offset + length)` of the device's image, retrying a miss with
  /// a growing window. Returns null when no digest came back after [digestAttempts] - a node whose
  /// bootloader predates the digest command never answers, which is what the caller falls back on.
  Future<int?> digest(int offset, int length) async {
    var timeout = const Duration(seconds: 1);
    for (var attempt = 0; attempt < digestAttempts; attempt++) {
      try {
        final crc = await transport.digest(offset, length, timeout);
        if (crc != null) return crc;
      } on TransportException {
        // Timeout: retry with a longer window, then let the caller fall back.
      }
      timeout = _grow(timeout);
    }
    return null;
  }

  /// The whole verification: one digest request and one comparison, one read-back pass to locate a
  /// mismatch, or one read-back pass when the node has no digest command. Reads only.
  Future<FlashVerifyResult> _verify(
    List<_Chunk> chunks, {
    required int expected,
    required Set<int> unconfirmed,
    void Function(int chunk, String phase)? checkRun,
    void Function(FlashProgress)? onProgress,
  }) async {
    final total = chunks.length;
    var reads = 0;
    void onRead() => reads++;

    checkRun?.call(0, 'verifying');
    onProgress?.call(
      const FlashProgress(
        phase: FlashPhase.verifying,
        completed: 0,
        total: 1,
      ),
    );

    // One digest over the whole padded image: the app region's bytes past the image read as
    // erased 0xFF, so the padded range is what a correct image digests to.
    final digest = await this.digest(0, total * _payloadSize);
    onProgress?.call(
      const FlashProgress(
        phase: FlashPhase.verifying,
        completed: 1,
        total: 1,
      ),
    );

    if (digest != null) {
      if (digest == expected) {
        return FlashVerifyResult(
          method: FlashVerify.digest,
          digest: digest,
          expectedDigest: expected,
          readBacks: 0,
          reads: 0,
        );
      }
      // One read-back pass, stopping at the first differing chunk, to name the offset the digest
      // cannot. It never writes and it is not re-run.
      final found = await _readBack(
        chunks,
        stopAtFirst: true,
        unconfirmed: unconfirmed,
        onRead: onRead,
        checkRun: checkRun,
      );
      return FlashVerifyResult(
        method: FlashVerify.digest,
        digest: digest,
        expectedDigest: expected,
        readBacks: 1,
        reads: reads,
        mismatches: FlashVerifyReport(
          total: total,
          all: found,
          complete: false,
        ),
      );
    }

    // No digest: a bootloader without the digest command stays silent, so the image is confirmed
    // the old way - ONE read-back pass over every chunk - and the result names that reason.
    final found = await _readBack(
      chunks,
      stopAtFirst: false,
      unconfirmed: unconfirmed,
      onRead: onRead,
      checkRun: checkRun,
      onProgress: onProgress,
    );
    return FlashVerifyResult(
      method: FlashVerify.readBackNoDigest,
      expectedDigest: expected,
      readBacks: 1,
      reads: reads,
      mismatches: found.isEmpty
          ? null
          : FlashVerifyReport(total: total, all: found),
    );
  }

  /// Reads the image back, chunk by chunk, and returns the chunks whose bytes differ. Never writes.
  ///
  /// [stopAtFirst] ends the pass at the first difference (the digest has already said the image is
  /// wrong, so this only names an offset); false reads every chunk, which is the fallback path
  /// where the read-back is the only evidence. A chunk that exhausts its own attempts is a node
  /// that stopped answering, not a mismatch, so that ends the run ([FlashFault.nodeSilent]).
  Future<List<FlashMismatch>> _readBack(
    List<_Chunk> chunks, {
    required bool stopAtFirst,
    required Set<int> unconfirmed,
    required void Function() onRead,
    void Function(int chunk, String phase)? checkRun,
    void Function(FlashProgress)? onProgress,
  }) async {
    final total = chunks.length;
    final found = <FlashMismatch>[];
    for (var i = 0; i < total; i++) {
      checkRun?.call(i + 1, 'verifying');
      final data = await readChunk(chunks[i].offset);
      onRead();
      // A read that exhausted its own attempts (5 tries with growing windows, ~8 s of silence) is
      // the node being gone, not a mismatch: end the run here instead of reading the rest.
      if (data == null) {
        throw FlashAborted(
          'the node stopped answering (offset 0x'
          '${chunks[i].offset.toRadixString(16)}, $readAttempts read attempts)',
          fault: FlashFault.nodeSilent,
          chunk: i + 1,
          total: total,
        );
      }
      if (!sameBytes(data, chunks[i].data)) {
        found.add(
          _compare(chunks[i], data, writeConfirmed: !unconfirmed.contains(i)),
        );
        if (stopAtFirst) break;
      }
      onProgress?.call(
        FlashProgress(
          phase: FlashPhase.verifying,
          completed: i + 1,
          total: total,
          mismatches: found.length,
        ),
      );
    }
    return found;
  }


  // --- helpers ---------------------------------------------------------------

  /// The first byte [read] got wrong against [chunk], for a report a person can act on.
  static FlashMismatch _compare(
    _Chunk chunk,
    List<int> read, {
    required bool writeConfirmed,
  }) {
    if (read.length == chunk.data.length) {
      for (var i = 0; i < read.length; i++) {
        if (read[i] != chunk.data[i]) {
          return FlashMismatch(
            chunk.offset,
            writeConfirmed: writeConfirmed,
            at: i,
            expected: chunk.data[i],
            actual: read[i],
          );
        }
      }
    }
    return FlashMismatch(chunk.offset, writeConfirmed: writeConfirmed);
  }

  static const int _payloadSize = Bootloader.payloadSize;

  /// Splits [image] into 32-byte chunks, padding the tail with 0xFF (erased flash) so every
  /// chunk is a full write.
  static List<_Chunk> _chunk(Uint8List image) {
    final chunks = <_Chunk>[];
    for (var offset = 0; offset < image.length; offset += _payloadSize) {
      final end = offset + _payloadSize;
      final data = Uint8List(_payloadSize)..fillRange(0, _payloadSize, 0xFF);
      final n = end > image.length ? image.length - offset : _payloadSize;
      data.setRange(0, n, image, offset);
      chunks.add(_Chunk(offset, data));
    }
    return chunks;
  }
}
