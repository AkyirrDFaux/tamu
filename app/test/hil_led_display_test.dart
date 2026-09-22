@Tags(['hil'])
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/subscription_client.dart';
import 'package:tamuapp/core/types.dart';

import 'hil_helpers.dart';

/// LED display probe (Tamu) on the FLAT dynamic model (Docs/Services/Register.md):
/// a block is a sorted table of (field, key) entries; a Geometry/Texture dictionary is
/// a field whose (field, 0) entry is a type marker (no value) followed by value keys.
///
/// Geometry keys: Shape=1, Operation=2, Position=3 (Matrix 2x3), Size=4, Fade=5,
/// Alpha=6, Rounding=7, Angles=8, PointNumber=9, PointCoordinates=10, NoiseSeed=11.
/// Texture keys: Type=1, Position=2, Size=3, Colour1=4, Colour2=5, Colour3=6, Amount=7.
Future<void> runTests() async {
  final reg = RegisterClient(deviceId: 1);

  // Clean slate: clear any persisted requester subscriptions, then drop every dynamic
  // block (tombstones keep their slots, so re-create the render block at index 0).
  final subClient = SubscriptionClient(deviceId: 1);
  var subs = await subClient.getRequesterSubscriptions();
  while (subs.isNotEmpty) {
    await subClient.setRequesterSubscription(subs.first.index);
    subs = await subClient.getRequesterSubscriptions();
  }
  for (;;) {
    final current = await reg.readDynamicBlocks() ?? <DynBlock>[];
    if (current.isEmpty) break;
    await reg.deleteDynamic(block: current.first.index);
  }
  await reg.saveDynamic();

  final created = await reg.createDynamicBlock(BlockType.dynamic, 'RENDER', index: 0);
  if (created == null) fail('createDynamicBlock failed');
  final b = (await reg.readDynamicBlockMeta(0))!;

  // Entry write helpers.
  Future<bool> setEntry(int field, int key, int type, List<int> value) async =>
      await reg.writeDynamicEntry(
              b, field, key, BlockMeta(flagsAndType: type, key: key), value) !=
          null;

  // Geometry dictionary at field 0: marker + values.
  if (!await setEntry(0, 0, 0x101, [])) fail('geometry marker failed'); // Geometry
  if (!await setEntry(0, 1, DataType.enum_.value, [3])) fail('Shape failed'); // Square
  if (!await setEntry(0, 2, DataType.enum_.value, [0])) fail('Operation failed'); // Replace
  final identity = [
    2, 0, 3, 0,
    ...numberToBytes(1.0), ...numberToBytes(0.0), ...numberToBytes(0.0),
    ...numberToBytes(0.0), ...numberToBytes(1.0), ...numberToBytes(0.0),
  ]; // identity 2x3
  if (!await setEntry(0, 3, DataType.matrix.value, identity)) fail('Position failed');
  if (!await setEntry(0, 4, DataType.number.value, numberToBytes(4.0))) fail('Size failed');
  if (!await setEntry(0, 5, DataType.number.value, numberToBytes(1.0))) fail('Fade failed');
  if (!await setEntry(0, 6, DataType.number.value, numberToBytes(1.0))) fail('Alpha failed');

  // Texture dictionary at field 1: marker + Fill red.
  if (!await setEntry(1, 0, 0x102, [])) fail('texture marker failed'); // Texture
  if (!await setEntry(1, 1, DataType.enum_.value, [1])) fail('Type failed'); // Fill
  if (!await setEntry(1, 4, DataType.colour.value, [255, 0, 0, 255])) fail('Colour1 failed');

  // Configure Display2 (GPIO0): render block 0, brightness 20, LAY_1 layout.
  final writeBlock = await reg.writeBlockField(0x06, 1, 2, 0,
      BlockMeta(flagsAndType: DataType.integer.value, size: 4), intToBytes(0, 4));
  if (writeBlock == null) fail('set Display2.RenderBlock failed');
  final park1 = await reg.writeBlockField(0x06, 0, 2, 0,
      BlockMeta(flagsAndType: DataType.integer.value, size: 4), intToBytes(-1, 4));
  if (park1 == null) fail('park Display1 failed');
  final brightness = await reg.writeBlockField(0x06, 1, 0, 0,
      BlockMeta(flagsAndType: DataType.number.value, size: 4), numberToBytes(20.0));
  if (brightness == null) fail('set Display2.Brightness failed');
  final layout = await reg.writeBlockField(0x06, 1, 3, 0,
      BlockMeta(flagsAndType: DataType.filename.value, size: 5), 'LAY_1'.codeUnits);
  if (layout == null) fail('set Display2.LayoutFile failed');
  // ignore: avoid_print
  print('[D] Display2.RenderBlock=0 Brightness=20 LayoutFile=LAY_1');

  // Let the renderer run a few frames, then read back the achieved FPS.
  double fps = 0;
  for (var attempt = 0; attempt < 8; attempt++) {
    await Future<void>.delayed(const Duration(milliseconds: 400));
    final rate = await reg.readBlockField(0x06, 1, 4, 0);
    if (rate != null && rate.value.length >= 4) {
      fps = numberFromBytes(rate.value);
      // ignore: avoid_print
      print('[D] attempt $attempt RefreshRate=${fps.toStringAsFixed(1)} FPS');
      if (fps > 200) break;
    }
  }
  if (fps < 200) fail('RefreshRate $fps < 200 FPS');

  // Readback of a few entries.
  final shape = await reg.readDynamicField(b, 0, 1);
  final texType = await reg.readDynamicField(b, 1, 1);
  final colour = await reg.readDynamicField(b, 1, 4);
  // ignore: avoid_print
  print('[D] readback Shape=${shape?.value ?? []} Type=${texType?.value ?? []} Colour=${colour?.value ?? []}');
  if (shape == null || shape.value.first != 3) fail('Shape readback mismatch');
  if (texType == null || texType.value.first != 1) fail('Type readback mismatch');
  if (colour == null || colour.value.length != 4) fail('Colour readback mismatch');

  // Shape cycle: rewrite the geometry value keys every 1.6s.
  Future<void> geometry({
    required int shape,
    List<double>? sizeVec,
    double? sizeNum,
    double? angles,
    int? points,
    int? seed,
  }) async {
    if (!await setEntry(0, 1, DataType.enum_.value, [shape])) fail('shape write failed');
    if (sizeVec != null) {
      final v = <int>[];
      for (final x in sizeVec) v.addAll(numberToBytes(x));
      if (!await setEntry(0, 4, DataType.vector.value, v)) fail('size vec write failed');
    } else if (sizeNum != null) {
      if (!await setEntry(0, 4, DataType.number.value, numberToBytes(sizeNum))) fail('size write failed');
    }
    if (angles != null) {
      if (!await setEntry(0, 8, DataType.number.value, numberToBytes(angles))) fail('angles write failed');
    }
    if (points != null) {
      if (!await setEntry(0, 9, DataType.integer.value, intToBytes(points, 4))) fail('points write failed');
    }
    if (seed != null) {
      if (!await setEntry(0, 11, DataType.integer.value, intToBytes(seed, 4))) fail('seed write failed');
    }
  }

  final shapes = <(String, Future<void> Function())>[
    ('Square', () => geometry(shape: 3, sizeNum: 4.0)),
    ('Rectangle', () => geometry(shape: 4, sizeVec: [6.0, 3.0])),
    ('Circle', () => geometry(shape: 6, sizeNum: 4.0)),
    ('Ellipse', () => geometry(shape: 7, sizeVec: [6.0, 4.0])),
    ('Trapezoid', () => geometry(shape: 5, sizeVec: [6.0, 4.0], angles: 20.0)),
    ('Triangle', () => geometry(shape: 9, sizeNum: 4.0)),
    ('Triangle iso', () => geometry(shape: 9, sizeNum: 4.0, angles: 40.0)),
    ('Polygon', () => geometry(shape: 10, sizeNum: 3.0, points: 6)),
    ('Star', () => geometry(shape: 11, sizeNum: 3.0, points: 5)),
    ('HalfFill', () => geometry(shape: 2)),
    ('Fill', () => geometry(shape: 1)),
    ('Noise', () => geometry(shape: 13, sizeNum: 1.0, seed: 123)),
    ('DoubleParabola', () => geometry(shape: 8, sizeVec: [5.0, 4.0])),
  ];
  for (final s in shapes) {
    final (name, apply) = s;
    await apply();
    // ignore: avoid_print
    print('[D] shape -> $name');
    await Future<void>.delayed(const Duration(milliseconds: 1600));
  }
  await geometry(shape: 3, sizeNum: 4.0);
  // ignore: avoid_print
  print('[D] shapes done - restored Square');

  // Gradient textures (rewrite field 1 value keys).
  for (final g in <(String, int, int, int)>[
    ('Gradient linear', 2, 4, 5),
    ('Gradient circular', 3, 4, 5),
  ]) {
    final (name, type, c1k, c2k) = g;
    if (!await setEntry(1, 1, DataType.enum_.value, [type])) fail('gradient type failed');
    if (!await setEntry(1, c1k, DataType.colour.value, [255, 0, 0, 255])) fail('gradient c1 failed');
    if (!await setEntry(1, c2k, DataType.colour.value, [0, 0, 255, 255])) fail('gradient c2 failed');
    if (!await setEntry(1, 3, DataType.number.value, numberToBytes(6.0))) fail('gradient extent failed');
    // ignore: avoid_print
    print('[D] texture -> $name');
    await Future<void>.delayed(const Duration(milliseconds: 2000));
  }

  // Effects: green Fill (field 1) then an effect dictionary (field 2) after it.
  await setEntry(1, 4, DataType.colour.value, [0, 255, 0, 255]);
  await setEntry(1, 1, DataType.enum_.value, [1]);
  if (!await setEntry(2, 0, 0x102, [])) fail('effect marker failed'); // Texture
  for (final e in <(String, int, double?)>[
    ('Invert', 4, null),
    ('HueShift 120deg', 5, 120.0),
    ('Contrast 1.5', 6, 1.5),
    ('Brightness 1.5', 7, 1.5),
  ]) {
    final (name, type, amount) = e;
    if (!await setEntry(2, 1, DataType.enum_.value, [type])) fail('effect type failed');
    if (amount != null) {
      if (!await setEntry(2, 7, DataType.number.value, numberToBytes(amount))) fail('effect amount failed');
    }
    // ignore: avoid_print
    print('[D] effect -> $name');
    await Future<void>.delayed(const Duration(milliseconds: 1800));
  }

  // Restore: red Fill, remove the effect field.
  await setEntry(1, 4, DataType.colour.value, [255, 0, 0, 255]);
  await reg.deleteDynamic(block: b.index, field: 2);
  // ignore: avoid_print
  print('[D] textures done - restored red Square');
}

void main() async {
  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  setUpAll(() async => await connectHil());
  tearDownAll(disconnectHil);

  test('LED display: flat dynamic model', () async {
    final link = ConnectionManager.instance;
    final payload = [0, 0, 0, 1]; // field 0, key 1 (capabilities)
    final capReply = await link.request(1, ServiceType.register, 1, payload: payload);
    if (capReply != null && capReply.length >= 12) {
      final caps = capReply[8] | (capReply[9] << 8) | (capReply[10] << 16) | (capReply[11] << 24);
      if ((caps & Capability.dynamicMemory) == 0) {
        print('Skipping: device does not have dynamic memory capability (caps=0x${caps.toRadixString(16)})');
        return;
      }
    }
    await runTests();
  }, timeout: const Timeout(Duration(minutes: 3)), skip: skipReason);
}