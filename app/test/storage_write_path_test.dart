/// The two paths a file write can take, and what each costs a node's file table.
///
/// The owner's ruling (2026-11-12) is that `StorageClient.resizeFile` (Storage CID 3) is the way a
/// file's LENGTH changes. The reason is in the firmware: a DAS file table holds 8 sixteen-byte
/// records, every append consumes one, and "Invalidated (0x00) holes are never reused"
/// (`firmware/src/Core/Functions/StorageBlockFS.h`). The staged path - create a temporary file,
/// write it, rename it over the target - appends TWO records per write (create + rename) and
/// erases a fresh data area; a resize appends ONE and keeps the file's own area; a write into a
/// file that is already the right length appends none.
///
/// The device below is a model, not a mock: it appends a record for exactly the three commands the
/// firmware appends one for (`CreateFile`, `ResizeFile`, `RenameFile` - see the same header), and
/// it applies the firmware's own rules for a CID-6 stream: fragment 0 opens it, a fragment may not
/// run past the file's end, and every fragment carries the sequence it accepted. That is what lets
/// [appends] stand in for the record count a rig session would read out of `.TABLE  `.
library;

import 'dart:math' as math;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/diagnostics.dart';
import 'package:tamuapp/core/fragment_reassembler.dart' show FragmentChunk;
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/storage_client.dart';

import 'support/fake_bus.dart';
import 'support/fake_channel.dart';
import 'support/fake_core.dart';

/// A node's storage service, modelling the file table's record accounting.
class _Device extends FakeTolerantChannel {
  /// The files the node holds, name -> its bytes (the length IS the record's size).
  final Map<String, List<int>> files = {};

  /// How many file records the table has gained: one per create, resize and rename.
  int appends = 0;

  /// Every command the node answered, in order, as `cid:name[:detail]`.
  final List<String> ops = [];

  /// Answer FAIL to a resize (an image with no CID 3, or one that refuses this growth).
  bool refuseResize = false;

  /// Answer SUCCESS to a resize and leave the file at its old size - the silent failure the
  /// read-back exists for.
  bool ignoreResize = false;

  /// The reply to the `.TABLE  ` read is a stream that lost its last fragment.
  bool tablePartial = false;

  /// Refuse this many fragment writes before accepting them (`-1` = never refuse).
  int refuseFragments = -1;

  /// How many times the `.TABLE  ` file was asked for.
  int tableReads = 0;

  int _writeOffset = 0;
  String? _writeName;

  List<String> get names => files.keys.toList();

  @override
  Future<PacketResponse> answer(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
  }) async {
    expect(
      service,
      ServiceType.storage,
      reason: 'only storage is modelled here',
    );
    final name = StorageClient.unpadName(payload);
    switch (functionCid) {
      case 0: // format - not used here
        return busAccepted;
      case 1: // create
        final size = _u32(payload, 8);
        if (files.containsKey(name) || size == 0) return busRefused;
        files[name] = List<int>.filled(size, 0);
        appends++;
        ops.add('create $name $size');
        return busAccepted;
      case 2: // delete
        files.remove(name);
        ops.add('delete $name');
        return busAccepted;
      case 3: // resize
        ops.add('resize $name ${_u32(payload, 8)}');
        if (refuseResize || !files.containsKey(name)) return busRefused;
        final size = _u32(payload, 8);
        if (!ignoreResize) files[name] = _resized(files[name]!, size);
        appends++;
        return busAccepted;
      case 4: // rename
        final newName = _name(payload, 8);
        ops.add('rename $name->$newName');
        if (!files.containsKey(name)) return busRefused;
        files[newName] = files.remove(name)!;
        appends++;
        return busAccepted;
      case 5: // read
        if (name == '.TABLE') {
          tableReads++;
          final reply = _tableResponse();
          return tablePartial ? _lostTail(reply) : reply;
        }
        final content = files[name];
        if (content == null) return busRefused;
        return _chunked([
          for (var off = 0; off < content.length; off += 64) ...[
            ...StorageClient.padName(name),
            ...content.sublist(off, math.min(off + 64, content.length)),
          ],
        ]);
      case 6: // write stream
        return _writeFragment(payload);
    }
    return busRefused;
  }

  PacketResponse _writeFragment(List<int> payload) {
    final info = fragInfoOf(payload);
    final name = _name(payload, 4);
    final content = payload.sublist(12);
    if (info.current == 0) {
      _writeName = name;
      _writeOffset = 0;
    } else if (_writeName != name) {
      return busRefused;
    }
    final held = files[name];
    if (held == null) return busRefused;
    ops.add('write $name ${content.length}@$_writeOffset');
    if (refuseFragments > 0) {
      refuseFragments--;
      return busRefused;
    }
    if (content.length > held.length - _writeOffset) return busRefused;
    held.setRange(_writeOffset, _writeOffset + content.length, content);
    _writeOffset += content.length;
    return PacketResponse(
      payload: [info.current & 0xFF, (info.current >> 8) & 0xFF],
      success: true,
      fail: false,
    );
  }

  PacketResponse _tableResponse() {
    final payload = tableReply(names, (n) => files[n]!.length);
    return _chunked(payload);
  }

  /// The device's reply shape: `[name][content, at most 64 bytes]` per fragment, the payload being
  /// their concatenation (B12).
  PacketResponse _chunked(List<int> payload) {
    const stride = 8 + 64;
    final chunks = <FragmentChunk>[];
    for (var off = 0; off < payload.length; off += stride) {
      chunks.add(
        FragmentChunk(
          chunks.length,
          payload.sublist(off, math.min(off + stride, payload.length)),
        ),
      );
    }
    return PacketResponse(
      payload: payload,
      success: true,
      fail: false,
      chunks: chunks,
      stopped: true,
      total: chunks.length,
    );
  }

  /// The same reply with its last fragment gone and no STOP frame - the shape the rig's link
  /// produces when a stream is cut.
  PacketResponse _lostTail(PacketResponse reply) {
    final kept = reply.chunks.sublist(0, reply.chunks.length - 1);
    return PacketResponse(
      payload: [for (final chunk in kept) ...chunk.body],
      success: true,
      fail: false,
      chunks: kept,
      missing: [reply.chunks.length - 1],
      stopped: false,
      total: reply.total,
    );
  }

  static List<int> _resized(List<int> old, int size) {
    final next = List<int>.filled(size, 0);
    next.setRange(0, math.min(old.length, size), old);
    return next;
  }

  static int _u32(List<int> payload, int at) =>
      payload[at] |
      (payload[at + 1] << 8) |
      (payload[at + 2] << 16) |
      (payload[at + 3] << 24);

  static String _name(List<int> payload, int at) =>
      StorageClient.unpadName(payload.sublist(at, at + 8));
}

void main() {
  late _Device device;
  late StorageClient client;

  setUp(() {
    device = _Device();
    installRequestChannel(device);
    client = StorageClient(deviceId: 3);
  });

  tearDown(() => installRequestChannel(null));

  test('a first creation is staged: create + rename, two records', () async {
    final ok = await client.writeFile('NEW', List<int>.filled(8, 0xAB));

    expect(ok, isTrue);
    expect(client.lastWritePath, FileWritePath.staged);
    expect(device.appends, 2, reason: 'create + rename');
    expect(device.ops, [
      'delete ~NEW',
      'create ~NEW 8',
      'write ~NEW 8@0',
      'rename ~NEW->NEW',
    ]);
    expect(device.files['NEW'], List<int>.filled(8, 0xAB));
    expect(client.lastWriteNote, isNull);
  });

  test(
    'a rewrite of the same length writes in place: no record at all',
    () async {
      device.files['SCR_007'] = List<int>.filled(16, 0x11);
      device.appends = 0;

      final ok = await client.writeFile('SCR_007', List<int>.filled(16, 0x22));

      expect(ok, isTrue);
      expect(client.lastWritePath, FileWritePath.inPlace);
      expect(device.appends, 0, reason: 'no create, no resize, no rename');
      expect(device.ops.where((op) => !op.startsWith('write')), isEmpty);
      expect(device.files['SCR_007'], List<int>.filled(16, 0x22));
    },
  );

  test(
    'a length change is one resize, not a delete+create: one record',
    () async {
      device.files['SCR_007'] = List<int>.filled(16, 0x11);
      device.appends = 0;
      final image = [for (var i = 0; i < 148; i++) i & 0xFF];

      final ok = await client.writeFile('SCR_007', image);

      expect(ok, isTrue);
      expect(client.lastWritePath, FileWritePath.resize);
      expect(
        device.appends,
        1,
        reason: 'one resize record, the same data area',
      );
      expect(device.ops.first, 'resize SCR_007 148');
      expect(device.ops.where((op) => op.startsWith('create')), isEmpty);

      // Verified by reading the file's SIZE back, not by the resize reply: a fresh client, a fresh
      // table read, the number the device now reports.
      final table = await StorageClient(deviceId: 3).readFileTableResult();
      expect(table.ok, isTrue);
      final record = table.recordsOrEmpty.firstWhere(
        (r) => r.name == 'SCR_007',
      );
      expect(record.size, 148);
      expect(record.invalidated, isFalse);
      expect(device.files['SCR_007'], image);
    },
  );

  test(
    'a refused resize falls back to the staged path and says which ran',
    () async {
      device.files['SCR_007'] = List<int>.filled(16, 0x11);
      device.appends = 0;
      device.refuseResize = true;
      final image = List<int>.filled(40, 0x5A);

      final ok = await client.writeFile('SCR_007', image);

      expect(ok, isTrue, reason: 'the fallback still writes the file');
      expect(client.lastWritePath, FileWritePath.staged);
      expect(client.lastWriteNote, contains('refused the resize'));
      expect(
        device.appends,
        2,
        reason: 'create + rename (the refused resize appends nothing)',
      );
      expect(device.files['SCR_007'], image);
    },
  );

  test(
    'a resize the device accepts but does not apply is caught by the read-back',
    () async {
      device.files['SCR_007'] = List<int>.filled(16, 0x11);
      device.appends = 0;
      device.ignoreResize = true;
      final image = List<int>.filled(40, 0x5A);

      final ok = await client.writeFile('SCR_007', image);

      expect(ok, isTrue);
      expect(
        client.lastWritePath,
        FileWritePath.staged,
        reason:
            'the table still said 16 byte(s), so the file was rewritten whole',
      );
      expect(client.lastWriteNote, contains('still says 16 byte(s)'));
      expect(device.files['SCR_007'], image);
    },
  );

  test('a failed in-place write is repaired by the staged path', () async {
    device.files['SCR_007'] = List<int>.filled(16, 0x11);
    device.appends = 0;
    device.refuseFragments = 1;
    final image = List<int>.filled(40, 0x5A);

    final ok = await client.writeFile('SCR_007', image);

    expect(ok, isTrue, reason: 'the staged path rewrote the file whole');
    expect(client.lastWritePath, FileWritePath.staged);
    expect(client.lastWriteNote, contains('did not land'));
    expect(
      device.files['SCR_007'],
      image,
      reason: 'no partial content is left behind',
    );
  });

  test(
    'a refusal is remembered: the next write does not probe CID 3 again',
    () async {
      device.files['SCR_007'] = List<int>.filled(16, 0x11);
      device.refuseResize = true;

      await client.writeFile('SCR_007', List<int>.filled(40, 0x5A));
      await client.writeFile('SCR_007', List<int>.filled(80, 0x5A));

      expect(
        device.ops.where((op) => op.startsWith('resize')),
        hasLength(1),
        reason: 'a CID 3 the device does not answer costs a timeout each time',
      );
      expect(client.lastWritePath, FileWritePath.staged);
      expect(client.lastWriteNote, contains('refused Storage CID 3 earlier'));
    },
  );

  test(
    'an unreadable file table falls back to staging, after ONE probe',
    () async {
      device.files['SCR_007'] = List<int>.filled(16, 0x11);
      device.tablePartial = true;

      final ok = await client.writeFile('SCR_007', List<int>.filled(16, 0x22));

      expect(ok, isTrue);
      expect(client.lastWritePath, FileWritePath.staged);
      expect(client.lastWriteNote, contains('file table could not be read'));
      expect(
        device.tableReads,
        1,
        reason:
            'the table is a PROBE here: a failed probe means the staging path '
            'runs, so the full three-send budget would only add timeouts',
      );
    },
  );

  test(
    'a table read taken inside a write leaves the page\'s last read alone',
    () async {
      client.lastReadError = 'Could not read "LAY_1": ...';
      client.lastReadAttempts = 2;

      await client.writeFile('NEW', List<int>.filled(8, 0xAB));

      expect(client.lastReadError, 'Could not read "LAY_1": ...');
      expect(client.lastReadAttempts, 2);
    },
  );

  test('the path each write took is in the diagnostics ring', () async {
    device.files['SCR_101'] = List<int>.filled(101, 0x11);
    await client.writeFile('SCR_101', List<int>.filled(101, 0x22));
    await client.writeFile('SCR_101', List<int>.filled(131, 0x22));
    await client.writeFile('NEW101', List<int>.filled(101, 0xAB));

    final lines = [
      for (final event in AppDiagnostics.events)
        if (event.toString().contains('write "SCR_101"') ||
            event.toString().contains('write "NEW101"'))
          event.toString(),
    ];
    expect(lines, hasLength(3));
    expect(lines[0], contains('inPlace, no record'));
    expect(lines[1], contains('resize, 1 record'));
    expect(lines[2], contains('staged, 2 records'));
  });
}
