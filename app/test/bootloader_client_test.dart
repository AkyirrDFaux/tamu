import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader.dart';
import 'package:tamuapp/core/bootloader_client.dart';
import 'package:tamuapp/core/transport.dart';

/// In-memory bootloader channel: records written chunks, tracks the device's last-written
/// offset, and can simulate a rejected write (direct-USB style: a stale write-response offset),
/// a dropped write (passthrough style: ACKed but never programmed), a read timeout, a
/// permanently unreachable node, and an identify reply.
class FakeTransport implements BootloaderTransport {
  final Map<int, Uint8List> flash = {};
  final Set<int> rejectOnce = {};
  final Set<int> dropOnce = {};
  final Set<int> timeoutOnce = {};
  bool alwaysNullRead = false;

  /// True to mirror the passthrough: writes are ACKed with a success flag but no last-written
  /// offset, so the client cannot reconcile per chunk and the verify pass must catch a drop.
  bool nullOffsetAcks = false;

  BootloaderIdentity? identity;
  int writes = 0, reads = 0;
  int _last = Bootloader.nothingWritten;

  @override
  Future<WriteAck> writeChunk(int offset, List<int> data, Duration timeout) async {
    writes++;
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
    return flash[offset] ??
        (Uint8List(Bootloader.payloadSize)
          ..fillRange(0, Bootloader.payloadSize, 0xFF));
  }

  @override
  Future<BootloaderIdentity?> identify(Duration timeout) async => identity;
}

Uint8List _image(int length) =>
    Uint8List.fromList(List.generate(length, (i) => i & 0xFF));

BootloaderClient _client(FakeTransport t) =>
    BootloaderClient(transport: t, writePacing: Duration.zero);

void main() {
  test('a clean write verifies in one pass with no corrections', () async {
    final t = FakeTransport();
    final image = _image(100);
    final result = await _client(t).flash(image);

    expect(result.verified, isTrue);
    expect(result.passes, 1);
    expect(result.corrections, 0);
    // 100 bytes -> 4 chunks (the tail padded).
    expect(result.writes, 4);
    expect(result.reads, 4);
  });

  test('the write response lets a rejected chunk be retried immediately', () async {
    // A rejected write is reported with a stale last-written offset, so the client retries it
    // inside the write loop instead of waiting for the verify pass.
    final t = FakeTransport()..rejectOnce.add(64);
    final result = await _client(t).flash(_image(100));

    expect(result.verified, isTrue);
    expect(result.corrections, 0);
    expect(result.passes, 1);
    // 4 chunks issued, plus one retry of the rejected chunk.
    expect(t.writes, 5);
    expect(t.flash[64], isNotNull);
  });

  test('the passthrough catches a dropped write in the verify pass', () async {
    // The passthrough ACKs without a last-written offset, so a dropped write only shows up as
    // a read mismatch and is corrected.
    final t = FakeTransport()
      ..nullOffsetAcks = true
      ..dropOnce.addAll({0, 64});
    final result = await _client(t).flash(_image(100));

    expect(result.verified, isTrue);
    expect(result.corrections, 2);
    expect(result.passes, 2);
  });

  test('a read timeout is retried, not treated as a mismatch', () async {
    final t = FakeTransport()..timeoutOnce.add(32);
    final result = await _client(t).flash(_image(100));

    expect(result.verified, isTrue);
    expect(result.corrections, 0);
    // The timed-out chunk was re-read by the transport within the same pass.
    expect(t.reads, greaterThan(t.writes));
  });

  test('an unreachable node never confirms and needs no corrections', () async {
    final t = FakeTransport()..alwaysNullRead = true;
    final result = await _client(t).flash(_image(100));

    expect(result.verified, isFalse);
    expect(result.corrections, 0);
    expect(result.passes, greaterThan(1));
  });

  test('identify exposes the app size and name', () async {
    final t = FakeTransport()
      ..identity =
          const BootloaderIdentity(appSize: 0x2A0000, name: 'Tamu v2.0A');
    final id = await _client(t).identify();

    expect(id, isNotNull);
    expect(id!.appSize, 0x2A0000);
    expect(id.name, 'Tamu v2.0A');
  });

  test('identify returns null when the device does not answer', () async {
    final t = FakeTransport();
    expect(await _client(t).identify(), isNull);
  });

  // The passthrough CIDs are wire values shared with the firmware's Device.h; a remap on either
  // side would silently break every passthrough operation (the codec KATs pin the frame bytes,
  // but nothing pinned these). Docs/Services/Bootloader.md "Core Bootloader Passthrough
  // Commands (002x)".
  group('passthrough command IDs match the firmware', () {
    test('0x20 identification, 0x21 write, 0x22 read', () {
      expect(coreBootIdentify, 0x20);
      expect(coreBootWrite, 0x21);
      expect(coreBootRead, 0x22);
    });

    test('the three IDs are distinct and inside the 002x range', () {
      final ids = {coreBootIdentify, coreBootWrite, coreBootRead};
      expect(ids.length, 3);
      for (final id in ids) {
        expect(id, inInclusiveRange(0x20, 0x22));
      }
    });
  });
}
