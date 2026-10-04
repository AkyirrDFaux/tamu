/// Bootloader flashing client (Docs/Services/Bootloader.md).
///
/// Drives the raw bootloader frames through the core's Device service passthrough
/// (CID 0x20 write / 0x21 read): the core relays each raw frame onto its RSBus, so the app
/// talks to whatever device is in bootloader mode without knowing its address.
///
/// The update is written first, then confirmed by reading it back. A read timeout is
/// retried with a growing timeout and never counts as a mismatch; only a successful read
/// whose bytes differ is a mistake, and mistakes are rewritten before the next pass. The
/// image is confirmed only when one full pass reads every chunk back identical in one go.
library;

import 'dart:typed_data';

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

  /// Write requests issued, including retries.
  final int writes;

  /// Read requests issued, including retries.
  final int reads;

  const FlashResult({
    required this.verified,
    required this.passes,
    required this.corrections,
    required this.writes,
    required this.reads,
  });
}

/// One 32-byte chunk of the image and its bootloader offset.
class _Chunk {
  final int offset;
  final Uint8List data;

  const _Chunk(this.offset, this.data);
}

/// Flashes a device's main binary through the connected core's Device passthrough.
class BootloaderClient {
  /// The core that relays our raw frames onto its RSBus.
  final int coreId;

  /// Delay between write requests. A node erases the 64-byte page on the first half of a
  /// chunk and programs both halves; that cycle outlasts a request (measured ~40 ms), so a
  /// frame arriving too soon is lost. Writes are paced past it; a dropped write is still
  /// acceptable because the verify/correct pass catches it.
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
    this.coreId = 1,
    this.writePacing = const Duration(milliseconds: 50),
    this.timeoutGrowth = 1.5,
    this.readAttempts = 5,
    this.writeAttempts = 3,
    this.maxPasses = 20,
  });

  ConnectionManager get _link => ConnectionManager.instance;

  Duration _grow(Duration timeout) {
    final grown = (timeout.inMicroseconds * timeoutGrowth).round();
    return Duration(microseconds: grown.clamp(1000, 10000000));
  }

  /// Sends one bootloader frame through Device CID [cid] and returns the reply payload.
  /// A timeout is retried with a growing timeout; when [accept] rejects a reply (the relay
  /// can answer FAIL while the bus is still settling) the attempt is repeated too. Returns
  /// null once [attempts] are spent.
  Future<List<int>?> _send(
      int cid, List<int> payload, Duration base, int attempts,
      {bool Function(List<int> reply)? accept}) async {
    var timeout = base;
    for (var attempt = 0; attempt < attempts; attempt++) {
      try {
        final reply = await _link.request(coreId, ServiceType.device, cid,
            payload: payload, timeout: timeout);
        if (accept == null || accept(reply)) return reply;
      } on TransportException {
        // Timeout: retry with a longer window.
      }
      timeout = _grow(timeout);
    }
    return null;
  }

  /// Writes one 32-byte chunk (Device CID 0x20). Returns true when the core answered.
  Future<bool> writeChunk(int offset, List<int> data) async {
    final payload = <int>[...uint32ToBytes(offset), ..._chunkBytes(data)];
    final reply =
        await _send(0x20, payload, const Duration(seconds: 1), writeAttempts);
    if (writePacing > Duration.zero) {
      await Future<void>.delayed(writePacing);
    }
    return reply != null;
  }

  /// Reads one 32-byte chunk (Device CID 0x21). Returns the 32 bytes, or null when every
  /// attempt was missed (a timeout or a FAIL reply - both are retried, never counted as a
  /// mismatch by [flash]).
  Future<Uint8List?> readChunk(int offset) async {
    final reply = await _send(0x21, uint32ToBytes(offset),
        const Duration(seconds: 1), readAttempts,
        accept: (r) => r.length >= 4 + _payloadSize);
    if (reply == null) return null;
    return Uint8List.fromList(reply.sublist(4, 4 + _payloadSize));
  }

  /// Flashes [image] and verifies it by read-back. The whole image is written first; then
  /// every chunk is read back in one pass, mistakes are rewritten, and the pass repeats
  /// until one pass finds nothing to correct. Returns [FlashResult.verified] only then.
  Future<FlashResult> flash(
    Uint8List image, {
    void Function(FlashProgress progress)? onProgress,
  }) async {
    final chunks = _chunk(image);
    final total = chunks.length;
    var writes = 0, reads = 0, corrections = 0;

    // 1. Write the entire binary.
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

  static const int _payloadSize = 32;

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

  static Uint8List _chunkBytes(List<int> data) {
    final out = Uint8List(_payloadSize)..fillRange(0, _payloadSize, 0xFF);
    final n = data.length > _payloadSize ? _payloadSize : data.length;
    out.setRange(0, n, data);
    return out;
  }

  static bool _sameBytes(List<int> a, List<int> b) {
    if (a.length != b.length) return false;
    for (var i = 0; i < a.length; i++) {
      if (a[i] != b[i]) return false;
    }
    return true;
  }
}
