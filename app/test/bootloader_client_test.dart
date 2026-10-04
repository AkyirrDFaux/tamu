import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader.dart';
import 'package:tamuapp/core/bootloader_client.dart';
import 'package:tamuapp/core/transport.dart';

/// In-memory bootloader channel: records written chunks, caches them, and can simulate a
/// dropped write, a read timeout, or a permanently unreachable node.
class FakeTransport implements BootloaderTransport {
  final Map<int, Uint8List> flash = {};
  final Set<int> dropOnce = {};
  final Set<int> timeoutOnce = {};
  bool alwaysNullRead = false;
  int writes = 0, reads = 0;

  @override
  Future<bool> writeChunk(int offset, List<int> data, Duration timeout) async {
    writes++;
    // A dropped write is acknowledged by the core but never reaches the node.
    if (dropOnce.remove(offset)) return true;
    flash[offset] = Uint8List.fromList(data);
    return true;
  }

  @override
  Future<Uint8List?> readChunk(int offset, Duration timeout) async {
    reads++;
    if (timeoutOnce.remove(offset)) {
      throw const TransportException('timeout');
    }
    if (alwaysNullRead) return null;
    return flash[offset] ?? (Uint8List(Bootloader.payloadSize)..fillRange(0, Bootloader.payloadSize, 0xFF));
  }
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

  test('dropped chunks are corrected and the image re-verifies', () async {
    final t = FakeTransport()..dropOnce.addAll({0, 64});
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
}
