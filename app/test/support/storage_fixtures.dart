/// The captured `LAY_1` payload and the reply a storage read of it produces.
///
/// `screens_over_ble_test` and `storage_partial_reply_test` each built the same four-fragment
/// reply by hand - the same 64-byte chunk loop, the same `.padName` echo per fragment, the same
/// `missing`/`stopped`/`total` construction - from the same captured bytes
/// (`test/fixtures/lay1.bin`). The reply IS the device's answer, not test logic, so it lives here
/// once.
library;

import 'dart:io';
import 'dart:math' as math;

import 'package:tamuapp/core/fragment_reassembler.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/storage_client.dart';

/// One captured stream from `test/fixtures`, as the bytes the rig gave.
List<int> fixture(String name) => File('test/fixtures/$name').readAsBytesSync();

/// `LAY_1`, read off the rig over BLE on 2026-11-04: an 11 x 10 LED layout, 223 bytes, brightness
/// limit 178.
List<int> lay1Content() => fixture('lay1.bin');

/// The reply a storage read of `LAY_1` produces: four `[name][content]` fragments of 64 content
/// bytes each.
///
/// [loseTheLast] drops the last fragment and its STOP frame, which is the shape a read that lost
/// its tail has: a hole at the last index and `stopped` false.
PacketResponse lay1Reply({bool loseTheLast = false}) {
  const chunk = 64;
  final content = lay1Content();
  final chunks = <FragmentChunk>[];
  for (var i = 0; i * chunk < content.length; i++) {
    final start = i * chunk;
    chunks.add(
      FragmentChunk(i, [
        ...StorageClient.padName('LAY_1'),
        ...content.sublist(start, math.min(start + chunk, content.length)),
      ]),
    );
  }
  final kept = loseTheLast ? chunks.sublist(0, chunks.length - 1) : chunks;
  return PacketResponse(
    payload: [for (final c in kept) ...c.body],
    success: true,
    fail: false,
    chunks: kept,
    missing: loseTheLast ? [chunks.length - 1] : const [],
    stopped: !loseTheLast,
    total: chunks.length,
  );
}
