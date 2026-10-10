/// Bootloader flashing client (Docs/Services/Bootloader.md).
///
/// The update is written first, then confirmed by reading it back. Writes are now
/// acknowledged: every write request draws a write-response carrying the offset of the last
/// successfully written chunk, so the client can reconcile a chunk against the device's
/// acknowledged progress and resynchronise after a rejected chunk without read-back-verifying
/// every chunk individually. Final verification of the image remains the app's job: one full
/// read-back pass must come back identical before the image is confirmed. A read timeout (or
/// a relay `FAIL`) is retried with a growing timeout and never counts as a mismatch; only a
/// successful read whose bytes differ is a mistake, and mistakes are rewritten before the next
/// pass.
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

  /// 1-based verification pass (0 during the write phase).
  final int round;

  /// Mistakes found in the last completed pass.
  final int mismatches;

  const FlashProgress({
    required this.phase,
    required this.completed,
    required this.total,
    this.round = 0,
    this.mismatches = 0,
  });

  double get fraction => total == 0 ? 1.0 : completed / total;
}

/// The outcome of [BootloaderClient.flash].
class FlashResult {
  final bool verified;

  /// Verification passes run (a pass is a full read-back of every chunk).
  final int passes;

  /// Chunks rewritten while correcting mistakes.
  final int corrections;

  /// Write operations issued (the initial pass plus corrections).
  final int writes;

  /// Chunk reads issued across all verification passes.
  final int reads;

  const FlashResult({
    required this.verified,
    required this.passes,
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
const int coreBootIdentify = 0x20; // request identification (response: app size + name)
const int coreBootWrite =
    0x21; // send write packet      (request: offset + payload; response: last-written offset)
const int coreBootRead = 0x22; // request read           (request: offset; response: offset + payload)

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
  Future<WriteAck> writeChunk(int offset, List<int> data, Duration timeout) async {
    final payload = <int>[...uint32ToBytes(offset), ...data];
    final reply = await _link.requestWithFlags(coreId, ServiceType.device, coreBootWrite,
        payload: payload, timeout: timeout);
    // The core relays the node's write-response offset; the flag is SUCCESS only when it names
    // this chunk. A FAIL still carries the offset (the node's last successfully written one),
    // and a timeout carries none, so the client can retry or resynchronise.
    final last = reply.payload.length >= 4 ? uint32FromBytes(reply.payload, 0) : null;
    return WriteAck(reply.ok, lastWrittenOffset: last);
  }

  @override
  Future<Uint8List?> readChunk(int offset, Duration timeout) async {
    final reply = await _link.request(coreId, ServiceType.device, coreBootRead,
        payload: uint32ToBytes(offset), timeout: timeout);
    if (reply.length < 4 + Bootloader.payloadSize) return null;
    return Uint8List.fromList(reply.sublist(4, 4 + Bootloader.payloadSize));
  }

  @override
  Future<BootloaderIdentity?> identify(Duration timeout) async {
    final reply = await _link.requestWithFlags(
        coreId, ServiceType.device, coreBootIdentify,
        timeout: timeout);
    if (!reply.ok || reply.payload.length < 4) return null;
    final appSize = uint32FromBytes(reply.payload, 0);
    final nameBytes = reply.payload.sublist(4);
    var n = nameBytes.length;
    while (n > 0 && nameBytes[n - 1] == 0) {
      n--;
    }
    return BootloaderIdentity(
        appSize: appSize, name: String.fromCharCodes(nameBytes.sublist(0, n)));
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
  Future<WriteAck> writeChunk(int offset, List<int> data, Duration timeout) async {
    final reply = await _requestFrame(Bootloader.encodeWrite(offset, data),
        command: Bootloader.cmdWriteResponse,
        size: Bootloader.readRequestSize,
        timeout: timeout);
    if (reply == null) return const WriteAck(false);
    final last = Bootloader.offset(reply);
    return WriteAck(last == offset, lastWrittenOffset: last);
  }

  @override
  Future<Uint8List?> readChunk(int offset, Duration timeout) async {
    final reply = await _requestFrame(Bootloader.encodeReadRequest(offset),
        command: Bootloader.cmdReadResponse,
        size: Bootloader.dataSize,
        matchOffset: offset,
        timeout: timeout);
    if (reply == null) return null;
    return Uint8List.fromList(reply.sublist(
        Bootloader.headerSize, Bootloader.headerSize + Bootloader.payloadSize));
  }

  @override
  Future<BootloaderIdentity?> identify(Duration timeout) async {
    final reply = await _requestFrame(Bootloader.encodeIdentifyRequest(),
        command: Bootloader.cmdIdentifyResponse,
        size: Bootloader.dataSize,
        timeout: timeout);
    if (reply == null) return null;
    return BootloaderIdentity(
        appSize: Bootloader.appSize(reply), name: Bootloader.name(reply));
  }

  /// Writes [request] and waits for its response frame: a valid frame of [command] and
  /// [size], matched additionally on [matchOffset] when given (a late reply from a previous
  /// transaction at another offset is dropped rather than returned). Returns null on timeout.
  Future<Uint8List?> _requestFrame(List<int> request,
      {required int command,
      required int size,
      int? matchOffset,
      required Duration timeout}) async {
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

/// Flashes a device's main binary: write it all, then read it back and correct the chunks
/// that differ until one full pass is clean.
class BootloaderClient {
  /// The low-level channel (passthrough by default, so [coreId] selects the relay core).
  final BootloaderTransport transport;

  /// Delay between write requests. A node erases the 64-byte page on the first half of a
  /// chunk and programs both halves; that cycle outlasts a request (measured ~40 ms), so a
  /// frame arriving too soon is lost. Writes are paced past it; with the write acknowledgement
  /// a dropped write is retried immediately, and the verify/correct pass still catches any
  /// that slipped through.
  final Duration writePacing;

  /// Timeout growth factor applied on each retry ("dynamic adjustment").
  final double timeoutGrowth;

  /// Attempts for a read before it is treated as unverified (a timeout alone is never a
  /// mismatch, so this is generous).
  final int readAttempts;

  /// Attempts for a write before it is dropped.
  final int writeAttempts;

  /// Upper bound on verification passes, so a device that never converges still returns.
  final int maxPasses;

  BootloaderClient({
    int coreId = 1,
    BootloaderTransport? transport,
    this.writePacing = const Duration(milliseconds: 50),
    this.timeoutGrowth = 1.5,
    this.readAttempts = 5,
    this.writeAttempts = 3,
    this.maxPasses = 20,
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

  /// Writes one 32-byte chunk, retrying a timeout or a rejected write with a growing window
  /// and pacing after the attempt. When the channel exposes the device's last-written offset,
  /// a write is only accepted once that offset names this chunk - so a rejected or dropped
  /// chunk is retried here instead of being left for the verify pass.
  Future<bool> writeChunk(int offset, List<int> data) async {
    var timeout = const Duration(seconds: 1);
    var ok = false;
    for (var attempt = 0; attempt < writeAttempts; attempt++) {
      try {
        final ack =
            await transport.writeChunk(offset, Bootloader.padPayload(data), timeout);
        ok = ack.accepted;
        final last = ack.lastWrittenOffset;
        // Reconcile against the device's acknowledged progress: a last-written offset that
        // does not name this chunk means the device did not program it, so retry.
        if (ok && last != null && last != offset) ok = false;
        if (ok) break;
      } on TransportException {
        // Timeout: retry with a longer window.
      }
      timeout = _grow(timeout);
    }
    if (writePacing > Duration.zero) {
      await Future<void>.delayed(writePacing);
    }
    return ok;
  }

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

  /// Flashes [image] and verifies it by read-back. The whole image is written first (each
  /// chunk confirmed against the device's write acknowledgement); then every chunk is read
  /// back in one pass, mistakes are rewritten, and the pass repeats until one pass finds
  /// nothing to correct. Returns [FlashResult.verified] only then.
  Future<FlashResult> flash(
    Uint8List image, {
    int? maxImageSize,
    void Function(FlashProgress progress)? onProgress,
  }) async {
    // Refuse an image the device's app space cannot hold, before spending a write pass on it.
    // The caller gets the size from identify(); the device would reject the out-of-range writes
    // anyway, but only after the whole pass had run and failed verification.
    if (maxImageSize != null && image.length > maxImageSize) {
      throw ArgumentError(
          'image is ${image.length} bytes; the device app space is $maxImageSize');
    }
    final chunks = _chunk(image);
    final total = chunks.length;
    var writes = 0, reads = 0, corrections = 0;

    // 1. Write the entire binary. Each write is acknowledged, so a rejected/dropped chunk is
    //    reconciled and retried right here rather than deferred to the verify pass.
    for (var i = 0; i < total; i++) {
      await writeChunk(chunks[i].offset, chunks[i].data);
      writes++;
      onProgress?.call(FlashProgress(
          phase: FlashPhase.writing, completed: i + 1, total: total));
    }

    // 2. Verify the entire binary in one pass; correct mistakes and verify again until a
    //    full pass reads every chunk back clean.
    for (var pass = 1; pass <= maxPasses; pass++) {
      final mistakes = <_Chunk>[];
      var unread = 0;
      for (var i = 0; i < total; i++) {
        final data = await readChunk(chunks[i].offset);
        reads++;
        // A null read is a timeout, not a mismatch: retry it on the next pass rather than
        // rewrite it, but it still means this pass did not verify the whole image.
        if (data == null) {
          unread++;
        } else if (!_sameBytes(data, chunks[i].data)) {
          mistakes.add(chunks[i]);
        }
        onProgress?.call(FlashProgress(
            phase: FlashPhase.verifying,
            completed: i + 1,
            total: total,
            round: pass,
            mismatches: mistakes.length));
      }

      // Confirmed only when one pass read every chunk back identical in one go.
      if (mistakes.isEmpty && unread == 0) {
        onProgress?.call(FlashProgress(
            phase: FlashPhase.done,
            completed: total,
            total: total,
            round: pass));
        return FlashResult(
            verified: true,
            passes: pass,
            corrections: corrections,
            writes: writes,
            reads: reads);
      }

      for (var i = 0; i < mistakes.length; i++) {
        await writeChunk(mistakes[i].offset, mistakes[i].data);
        writes++;
        corrections++;
        onProgress?.call(FlashProgress(
            phase: FlashPhase.correcting,
            completed: i + 1,
            total: mistakes.length,
            round: pass,
            mismatches: mistakes.length));
      }
    }

    return FlashResult(
        verified: false,
        passes: maxPasses,
        corrections: corrections,
        writes: writes,
        reads: reads);
  }

  // --- helpers ---------------------------------------------------------------

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

  static bool _sameBytes(List<int> a, List<int> b) {
    if (a.length != b.length) return false;
    for (var i = 0; i < a.length; i++) {
      if (a[i] != b[i]) return false;
    }
    return true;
  }
}
