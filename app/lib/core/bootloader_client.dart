/// Bootloader flashing client (Docs/Services/Bootloader.md).
///
/// The update is written first, then confirmed by reading it back. Writes are acknowledged:
/// every write request draws a write-response carrying the offset of the last successfully
/// written chunk, so the client can reconcile a chunk against the device's acknowledged progress
/// and end the run at the first chunk the node does not take. Final verification of the image is
/// the app's job too, and it is READ-ONLY: one read-back of the whole image, never a correction
/// loop. A read timeout (or a relay `FAIL`) is retried with a growing timeout and never counts as
/// a mismatch; only a successful read whose bytes differ is a mistake, and a mistake ends the run
/// - the one exception is a chunk whose write was never confirmed, which one correction round may
/// rewrite (see [BootloaderClient.flash] and [FlashFault]).
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
enum FlashPhase { writing, verifying, correcting, done }

/// A progress snapshot emitted by [BootloaderClient.flash].
class FlashProgress {
  final FlashPhase phase;

  /// Chunks finished in this phase (out of [total]; `total` is the mistake count while
  /// [phase] is [FlashPhase.correcting]).
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

/// The outcome of [BootloaderClient.flash]: the image the device holds is the image that was
/// handed in, read back and identical. A run that cannot confirm that throws [FlashAborted]
/// instead of returning a result.
class FlashResult {
  /// Read-backs of the image, and nothing else: 1 for an image that read back clean, 2 when one
  /// correction round ran (that second read covers only the corrected chunks).
  final int readBacks;

  /// Chunks rewritten by a correction round.
  final int corrections;

  /// Write operations issued (the initial pass plus corrections).
  final int writes;

  /// Chunk reads issued (the read-back plus the re-read of corrected chunks).
  final int reads;

  const FlashResult({
    required this.readBacks,
    required this.corrections,
    required this.writes,
    required this.reads,
  });
}

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

/// The core's Device `0020/0021/0022` passthrough: the core relays each raw bootloader frame
/// onto its RSBus, so the app reaches whatever node is in bootloader mode without knowing its
/// address. A write reply carries the node's last-written offset (the flag is SUCCESS only when
/// that offset names the requested chunk); reads return `offset + payload`; identification
/// returns `app size + name`.
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
  /// but the bytes do not read back identical. The erase/program cycle or the read path is at
  /// fault, not the link.
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

/// What one read-back of the whole image found: how many chunks differ, the first few of them,
/// and how many of those the write pass itself had confirmed.
class FlashVerifyReport {
  /// How many offsets [summary] names.
  static const int named = 8;

  /// The image's chunk count, so the summary can say how much of it is wrong.
  final int total;

  /// Every differing chunk, in image order.
  final List<FlashMismatch> all;

  const FlashVerifyReport({required this.total, required this.all});

  int get count => all.length;

  /// Chunks the write pass acknowledged per chunk and that still do not read back: the device
  /// took the data and lost it, which no rewrite can fix.
  int get confirmed => all.where((m) => m.writeConfirmed).length;

  /// Chunks whose write the channel only accepted without confirming it: a rewrite can still land
  /// these, which is what the one correction round is for.
  int get unconfirmed => count - confirmed;

  /// The first [named] offsets, in image order.
  List<FlashMismatch> get first => all.take(named).toList();

  /// `2 of 100 chunks differ (0 confirmed by the write pass, 2 not confirmed); first: 0x0, 0x40`
  String get summary {
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

/// Flashes a device's main binary: write it all, then read it back once and confirm it. A
/// verification read never writes and never re-runs; see [flash].
class BootloaderClient {
  /// The low-level channel (passthrough by default, so [coreId] selects the relay core).
  final BootloaderTransport transport;

  /// Delay between write requests. A node erases the 64-byte page on the first half of a
  /// chunk and programs both halves; that cycle outlasts a request (measured ~40 ms), so a
  /// frame arriving too soon is lost. Writes are paced past it; with the write acknowledgement
  /// a dropped write is retried immediately, and the read-back catches any that slipped through
  /// with an acknowledgement that did not name the chunk.
  final Duration writePacing;

  /// Timeout growth factor applied on each retry ("dynamic adjustment").
  final double timeoutGrowth;

  /// Attempts for a read before it is treated as unverified (a timeout alone is never a
  /// mismatch, so this is generous).
  final int readAttempts;

  /// Attempts for a write before it is dropped.
  final int writeAttempts;

  /// How long one [flash] run may take as a whole. The app space is 32-byte chunks and the
  /// passthrough paces writes 50 ms apart, so a full image is ~11 minutes of paced writes alone,
  /// plus one read-back and at most one correction round. Forty-five minutes is past any run that
  /// is making progress, and far short of the hours a node that left bootloader mode used to
  /// cost - that case is stopped at the first chunk that does not land, so this is the backstop,
  /// not the fix.
  static const Duration flashDeadline = Duration(minutes: 45);

  BootloaderClient({
    int coreId = 1,
    BootloaderTransport? transport,
    this.writePacing = const Duration(milliseconds: 50),
    this.timeoutGrowth = 1.5,
    this.readAttempts = 5,
    this.writeAttempts = 3,
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

  /// Flashes [image] and confirms it with ONE read-back.
  ///
  /// Three stages, and none of them repeats:
  ///
  /// 1. One write pass over the image, each chunk confirmed against the device's write
  ///    acknowledgement. The first chunk the node does not take ends the run
  ///    ([FlashFault.writeRefused]) instead of walking the rest of the image into a node that has
  ///    left bootloader mode.
  /// 2. One read-back of every chunk. It only reads - it never writes a chunk, and it is never
  ///    re-run because it found a mistake. A chunk that reads back different ends the run
  ///    ([FlashFault.verifyMismatch]) with a [FlashVerifyReport] carrying the count, the first
  ///    offsets and whether the write pass itself had acknowledged them: a chunk the device
  ///    confirmed and that still does not read back is a flash or read-path fault, not a lost
  ///    write, and rewriting it cannot make it hold.
  /// 3. At most one correction round, covering only the chunks whose write the channel accepted
  ///    WITHOUT confirming it (a passthrough relay timeout carries no last-written offset, so the
  ///    device may never have seen the chunk). Those get one rewrite and one re-read; one that
  ///    still differs aborts with [FlashFault.verifyMismatch].
  ///
  /// The bound, and why it replaced `maxPasses = 20`: writes are at most 2 x the image (only when
  /// a correction round runs), reads are at most the image plus the corrected chunks, and no pass
  /// is repeated. The old loop read the whole image back, rewrote every mismatched chunk and
  /// verified again up to twenty times, so a link failing at the core - where every read comes
  /// back wrong - landed twenty erase/write cycles of the whole image on the node and made the
  /// owner's "it gets worse with each verification pass" true.
  ///
  /// [deadline] bounds the run as a whole and [isCancelled] is the user's way out; both are
  /// checked between chunks, so neither waits for a stage to finish.
  Future<FlashResult> flash(
    Uint8List image, {
    int? maxImageSize,
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
    var writes = 0, reads = 0, corrections = 0;
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
    //    read-back is allowed to rewrite that one - and only that one - once.
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

    // 2. The one read-back. Read-only by construction: nothing in this loop writes, and it is not
    //    wrapped in a retry - a read-back that finds a mistake has done its job.
    final found = <FlashMismatch>[];
    for (var i = 0; i < total; i++) {
      checkRun(i + 1, 'verifying');
      final data = await readChunk(chunks[i].offset);
      reads++;
      // A read that exhausted its own attempts (5 tries with growing windows, ~8 s of silence) is
      // the node being gone, not a mismatch: end the run here instead of reading the rest of the
      // image.
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

    if (found.isEmpty) {
      onProgress?.call(
        FlashProgress(phase: FlashPhase.done, completed: total, total: total),
      );
      return FlashResult(
        readBacks: 1,
        corrections: 0,
        writes: writes,
        reads: reads,
      );
    }

    final report = FlashVerifyReport(total: total, all: found);

    // A chunk the write pass acknowledged per chunk and that still reads back different is the
    // node's flash (or its read path) failing, not a lost write: rewriting it cannot make it
    // hold, and rewriting it is exactly the multiplication the card is about. Report and stop.
    if (report.confirmed > 0) {
      throw FlashAborted(
        'the write pass acknowledged ${report.confirmed} of the differing chunks with their '
        'own offset and they still do not read back - ${report.summary}',
        fault: FlashFault.verifyMismatch,
        chunk: found.first.offset ~/ _payloadSize + 1,
        total: total,
        mismatches: report,
      );
    }

    // 3. The one correction round, for the chunks whose write was never confirmed - nothing else
    //    is rewritten. The re-read below decides; there is no second round.
    final byOffset = <int, _Chunk>{
      for (final chunk in chunks) chunk.offset: chunk,
    };
    for (var i = 0; i < found.length; i++) {
      final chunk = byOffset[found[i].offset]!;
      checkRun(chunk.offset ~/ _payloadSize + 1, 'correcting');
      final ack = await writeChunkAck(chunk.offset, chunk.data);
      if (!ack.accepted) {
        throw FlashAborted(
          'the node never took the correction for the chunk at offset 0x'
          '${chunk.offset.toRadixString(16)} after $writeAttempts attempts',
          fault: FlashFault.writeRefused,
          // The image's chunk index, not the correction's: one field, one meaning.
          chunk: chunk.offset ~/ _payloadSize + 1,
          total: total,
          mismatches: report,
        );
      }
      writes++;
      corrections++;
      onProgress?.call(
        FlashProgress(
          phase: FlashPhase.correcting,
          completed: i + 1,
          total: found.length,
          mismatches: found.length,
        ),
      );
    }

    // One re-read of the corrected chunks, not another pass over the image.
    for (var i = 0; i < found.length; i++) {
      final chunk = byOffset[found[i].offset]!;
      checkRun(chunk.offset ~/ _payloadSize + 1, 're-reading the corrections');
      final data = await readChunk(chunk.offset);
      reads++;
      if (data == null) {
        throw FlashAborted(
          'the node stopped answering while the corrections were read back (offset 0x'
          '${chunk.offset.toRadixString(16)}, $readAttempts read attempts)',
          fault: FlashFault.nodeSilent,
          chunk: chunk.offset ~/ _payloadSize + 1,
          total: total,
          mismatches: report,
        );
      }
      if (!sameBytes(data, chunk.data)) {
        final again = _compare(chunk, data, writeConfirmed: true);
        throw FlashAborted(
          'the corrected chunk still does not read back (${again.description})',
          fault: FlashFault.verifyMismatch,
          chunk: chunk.offset ~/ _payloadSize + 1,
          total: total,
          mismatches: FlashVerifyReport(total: total, all: [again]),
        );
      }
    }

    onProgress?.call(
      FlashProgress(phase: FlashPhase.done, completed: total, total: total),
    );
    return FlashResult(
      readBacks: 2,
      corrections: corrections,
      writes: writes,
      reads: reads,
    );
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
