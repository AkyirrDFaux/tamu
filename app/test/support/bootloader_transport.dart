/// A fake bootloader channel, shared by [BootloaderClient]'s own suite and the Update page's.
///
/// One fake models every link fault the client has to tell apart: a refused write (a `FAIL` with
/// the device's stale last-written offset), a dropped write the core acknowledged but the node
/// never got (the passthrough's offset-less ACK), a read timeout, a node that answers nothing, a
/// write that lands but is lost by the flash, and a node whose bootloader has no digest command.
///
/// The device's content is what the digest is computed over, so a chunk the flash did not keep
/// shows up in the digest - exactly as on the real node, where the digest is computed from flash
/// and not through the read command.
library;

import 'dart:typed_data';

import 'package:tamuapp/core/bootloader.dart';
import 'package:tamuapp/core/bootloader_client.dart';
import 'package:tamuapp/core/transport.dart';

class FakeBootloaderTransport implements BootloaderTransport {
  /// The bytes the node holds, by offset.
  final Map<int, Uint8List> flash = {};

  /// Offsets whose next write is refused once (direct-USB style: a stale offset).
  final Set<int> rejectOnce = {};

  /// Offsets whose next write is acknowledged but never programmed.
  final Set<int> dropOnce = {};

  /// Offsets whose next read times out.
  final Set<int> timeoutOnce = {};

  /// Offsets whose stored chunk is programmed with byte 0 flipped: the device took the data and
  /// lost it, which is what the owner saw as "acknowledged but does not read back".
  final Set<int> corrupt = {};

  /// True to corrupt every chunk's stored bytes, so the whole image is wrong on the device.
  bool corruptAll = false;

  /// True when [corruptAll] / [corrupt] only hit a chunk's FIRST write, so one correction round can
  /// fix it. False (the default) keeps corrupting every write: a flash that will not hold the data.
  bool corruptFirstWriteOnly = false;

  /// True to make every read flip a byte after the fact: the stored bytes and the digest are right,
  /// the read path is not - the node's own CRC and its own bytes disagree.
  bool readBackLies = false;

  bool alwaysNullRead = false;

  /// True to refuse every write the way a node that has left bootloader mode does: a FAIL with a
  /// stale last-written offset, for every chunk and every attempt.
  bool alwaysReject = false;

  /// True to mirror the passthrough: writes are ACKed with a success flag but no last-written
  /// offset, so the client cannot reconcile per chunk and the digest must catch a drop.
  bool nullOffsetAcks = false;

  /// True to model a bootloader older than the digest command: the digest request is ignored,
  /// which is indistinguishable from silence for the caller - that is what the fallback is for.
  bool noDigest = false;

  /// Digest requests issued (the client sends one, and retries a miss).
  int digests = 0;

  /// Offsets that have been programmed at least once (for [corruptFirstWriteOnly]).
  final Set<int> _programmed = {};

  BootloaderIdentity? identity;
  int writes = 0, reads = 0;
  int _last = Bootloader.nothingWritten;

  @override
  Future<WriteAck> writeChunk(
    int offset,
    List<int> data,
    Duration timeout,
  ) async {
    writes++;
    if (alwaysReject) {
      return nullOffsetAcks
          ? const WriteAck(false)
          : WriteAck(false, lastWrittenOffset: _last);
    }
    // A rejected write is acknowledged with the device's (stale) last-written offset.
    if (rejectOnce.remove(offset)) {
      return nullOffsetAcks
          ? const WriteAck(false)
          : WriteAck(false, lastWrittenOffset: _last);
    }
    // A dropped write is acknowledged by the core but never reaches the node.
    if (dropOnce.remove(offset)) {
      return nullOffsetAcks
          ? const WriteAck(true)
          : WriteAck(false, lastWrittenOffset: _last);
    }
    final stored = Uint8List.fromList(data);
    final corruptThis =
        (corruptAll || corrupt.contains(offset)) &&
        (!corruptFirstWriteOnly || !_programmed.contains(offset));
    if (corruptThis) stored[0] = stored[0] ^ 0xFF;
    flash[offset] = stored;
    _programmed.add(offset);
    _last = offset;
    return nullOffsetAcks
        ? const WriteAck(true)
        : WriteAck(true, lastWrittenOffset: offset);
  }

  /// The device's chunk at [offset]: what it holds, or erased 0xFF where nothing was written.
  Uint8List _held(int offset) =>
      flash[offset] ??
      (Uint8List(Bootloader.payloadSize)
        ..fillRange(0, Bootloader.payloadSize, 0xFF));

  @override
  Future<Uint8List?> readChunk(int offset, Duration timeout) async {
    reads++;
    if (timeoutOnce.remove(offset)) {
      throw const TransportException('timeout');
    }
    if (alwaysNullRead) return null;
    final data = Uint8List.fromList(_held(offset));
    if (readBackLies) data[0] = data[0] ^ 0xFF;
    return data;
  }

  @override
  Future<int?> digest(int offset, int length, Duration timeout) async {
    digests++;
    if (noDigest || alwaysNullRead) return null;
    // The node computes the CRC from its flash: bytes past the app region read as erased 0xFF.
    var crc = 0xFFFFFFFF;
    for (var i = 0; i < length; i++) {
      final chunk = _held(offset + i - (offset + i) % Bootloader.payloadSize);
      crc = _Crc.step(crc, chunk[(offset + i) % Bootloader.payloadSize]);
    }
    return crc ^ 0xFFFFFFFF;
  }

  @override
  Future<BootloaderIdentity?> identify(Duration timeout) async => identity;
}

/// The reflected CRC-32 step, so the fake's digest is the device's value and not the client's.
class _Crc {
  static int step(int crc, int byte) {
    var c = crc ^ (byte & 0xFF);
    for (var bit = 0; bit < 8; bit++) {
      c = (c >> 1) ^ ((c & 1) != 0 ? 0xEDB88320 : 0);
    }
    return c;
  }
}

/// A test image: `length` bytes of `i & 0xFF`.
Uint8List testImage(int length) =>
    Uint8List.fromList(List.generate(length, (i) => i & 0xFF));
