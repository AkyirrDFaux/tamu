/// A fake bootloader channel, shared by [BootloaderClient]'s own suite and the Update page's.
///
/// One fake models every link fault the client has to tell apart: a refused write (a `FAIL` with
/// the device's stale last-written offset), a dropped write the core acknowledged but the node
/// never got (the passthrough's offset-less ACK), a read timeout, a node that answers nothing, and
/// a write that lands but does not read back.
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

  /// Offsets whose read comes back with byte 0 flipped: the write landed, the flash (or the read
  /// path) does not return it - the owner's "acknowledged but does not read back".
  final Set<int> corrupt = {};

  /// True to corrupt every chunk's read, so the whole image reads back wrong.
  bool corruptAll = false;

  bool alwaysNullRead = false;

  /// True to refuse every write the way a node that has left bootloader mode does: a FAIL with a
  /// stale last-written offset, for every chunk and every attempt.
  bool alwaysReject = false;

  /// True to mirror the passthrough: writes are ACKed with a success flag but no last-written
  /// offset, so the client cannot reconcile per chunk and the read-back must catch a drop.
  bool nullOffsetAcks = false;

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
    flash[offset] = Uint8List.fromList(data);
    _last = offset;
    return nullOffsetAcks
        ? const WriteAck(true)
        : WriteAck(true, lastWrittenOffset: offset);
  }

  @override
  Future<Uint8List?> readChunk(int offset, Duration timeout) async {
    reads++;
    if (timeoutOnce.remove(offset)) {
      throw const TransportException('timeout');
    }
    if (alwaysNullRead) return null;
    final held = flash[offset];
    final data = held != null
        ? Uint8List.fromList(held)
        : (Uint8List(Bootloader.payloadSize)
            ..fillRange(0, Bootloader.payloadSize, 0xFF));
    if (corruptAll || corrupt.contains(offset)) data[0] = data[0] ^ 0xFF;
    return data;
  }

  @override
  Future<BootloaderIdentity?> identify(Duration timeout) async => identity;
}

/// A test image: `length` bytes of `i & 0xFF`.
Uint8List testImage(int length) =>
    Uint8List.fromList(List.generate(length, (i) => i & 0xFF));
