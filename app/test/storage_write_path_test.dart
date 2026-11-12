/// The one path a file write takes, and what it costs a node's file table.
///
/// The owner's ruling (2026-11-12) was to change a file's LENGTH through `StorageClient.resizeFile`
/// (Storage CID 3) and write into the existing file, which costs one file-table record instead of
/// the staged path's two. That was implemented, measured and reverted, and this file pins the
/// revert so nobody "finishes" the optimisation: a resize only moves the record, while the CONTENT
/// still has to land in the file's own data area, and a CID-6 fragment stream writes with nothing
/// erased in front of it (flash only programs 1s to 0s). The byte-level evidence is the host probe
/// `app/.dart_tool/hermes/write_path_probe.cpp`, which runs the REAL `StorageBlockFS.h` over the
/// fake NOR flash: `resize answered success` then `content: CORRUPT ... on-flash ==
/// want&previous: YES`.
///
/// The device below is a model: it appends a record for the commands the firmware appends one for
/// (`CreateFile`, `ResizeFile`, `RenameFile`) and applies the firmware's rules for a CID-6 stream
/// (fragment 0 opens it, a fragment may not run past the file's end). That is what lets
/// [appends] stand in for the record count a rig session reads out of `.TABLE  `.
library;

import 'dart:math' as math;

import 'package:flutter_test/flutter_test.dart';
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

  /// Refuse this many fragment writes before accepting them (`-1` = never refuse).
  int refuseFragments = -1;

  /// How many times the `.TABLE  ` file was asked for: a write should not need it.
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
        if (!files.containsKey(name)) return busRefused;
        final size = _u32(payload, 8);
        files[name] = _resized(files[name]!, size);
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
          return _chunked(tableReply(names, (n) => files[n]!.length));
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
    expect(device.appends, 2, reason: 'create + rename');
    expect(device.ops, [
      'delete ~NEW',
      'create ~NEW 8',
      'write ~NEW 8@0',
      'rename ~NEW->NEW',
    ]);
    expect(device.files['NEW'], List<int>.filled(8, 0xAB));
  });

  test('an existing file is rewritten the staged way, never resized', () async {
    device.files['SCR_007'] = List<int>.filled(16, 0x11);
    device.appends = 0;

    final ok = await client.writeFile('SCR_007', List<int>.filled(16, 0x22));

    expect(ok, isTrue);
    // Two records rather than the one a resize would cost: a resize cannot carry the bytes (see
    // the library doc), so paying two is the price of a correct file.
    expect(device.appends, 2, reason: 'create + rename');
    expect(
      device.ops.where((op) => op.startsWith('resize')),
      isEmpty,
      reason: 'a resize leaves the content programmed over live flash',
    );
    expect(device.files['SCR_007'], List<int>.filled(16, 0x22));
  });

  test('a length change puts the NEW bytes on the device', () async {
    final first = [for (var i = 0; i < 16; i++) 0x11 + i];
    final second = [for (var i = 0; i < 92; i++) 0x60 + i];
    await client.writeFile('SCR_007', first);

    final ok = await client.writeFile('SCR_007', second);

    expect(ok, isTrue);
    expect(device.files['SCR_007'], second);
    expect(device.appends, 4, reason: 'two writes, two records each');
    // The file is a different length and holds bytes the first write never sent, so a masked
    // (new & old) write could not pass this.
    expect(device.files['SCR_007']!.length, 92);
  });

  test('a write that dies mid-stream leaves the original intact', () async {
    final original = [for (var i = 0; i < 16; i++) 0x40 + i];
    await client.writeFile('SCR_007', original);
    device.appends = 0;
    device.refuseFragments = 1;

    final ok = await client.writeFile('SCR_007', List<int>.filled(40, 0x7E));

    expect(ok, isFalse);
    expect(
      device.files['SCR_007'],
      original,
      reason: 'the target was never touched',
    );
    expect(
      device.files.containsKey('~SCR_007'),
      isFalse,
      reason: 'the temp is cleaned up',
    );
    expect(device.ops.last, 'delete ~SCR_007');
  });

  test('the write path reads no file table', () async {
    await client.writeFile('SCR_007', List<int>.filled(16, 0x11));
    device.tableReads = 0;

    await client.writeFile('SCR_007', List<int>.filled(32, 0x22));

    expect(device.tableReads, 0, reason: 'a write needs no probe of the table');
  });

  test(
    'resizeFile still speaks Storage CID 3, although nothing calls it',
    () async {
      device.files['SCR_007'] = List<int>.filled(16, 0x11);

      final ok = await client.resizeFile('SCR_007', 96);

      expect(ok, isTrue);
      expect(device.ops.last, 'resize SCR_007 96');
      expect(device.files['SCR_007']!.length, 96);
    },
  );
}
