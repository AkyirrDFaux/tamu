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

/// LED display rewrite probe (Tamu): creates a dynamic render block whose field 0 is a
/// Geometry dictionary (Replace/Square) and field 1 a Texture dictionary (Fill colour),
/// points Display2 (static block type 6, inst 1 - the strip on GPIO0) at it, checks the
/// achieved Refresh Rate, then cycles the square's colour red -> green -> blue -> black.
/// The render block + Display2 config are left in place so the square (red) stays visible
/// on GPIO0 after the test.
///
/// Docs/Modules and blocks/LED display.md dict keys:
///   Geometry: Operation=0, Shape=1, Position=2 (Matrix 2x3), Size=3, Fade=4, Alpha=5
///   Texture:  Type=0, Colour1=3
Future<void> runTests() async {
  final reg = RegisterClient(deviceId: 1);

  // Geometry dict entry (field value = concatenated keyed entries). Each entry must be
  // padded to a multiple of 4 bytes: the firmware's keyed-entry walker aligns entries
  // (AlignTo4(BlockMeta + value)) and would misread unaligned entries.
  List<int> entry(int flagsAndType, int key, List<int> value) {
    final pad = (4 - ((4 + value.length) % 4)) % 4;
    return [
          ...BlockMeta(flagsAndType: flagsAndType, key: key, size: value.length)
              .toBytes(),
          ...value,
          ...List<int>.filled(pad, 0),
        ];
  }

  // Matrix 2x3 wire: u16 height, u16 width, then h*w Numbers (Q16.16).
  List<int> affine23(List<double> a, List<double> b) => [
        2, 0, 3, 0,
        ...a.expand((v) => numberToBytes(v)),
        ...b.expand((v) => numberToBytes(v)),
      ];

  final identity = affine23([1, 0, 0], [0, 1, 0]);

  final geometry = [
    ...entry(DataType.enum_.value, 0, [0]), // Operation: Replace
    ...entry(DataType.enum_.value, 1, [3]), // Shape: Square
    ...entry(DataType.matrix.value, 2, identity), // Position: identity 2x3 affine
    ...entry(DataType.number.value, 3, numberToBytes(4.0)), // Size: side (px)
    ...entry(DataType.number.value, 4, numberToBytes(0.0)), // Fade (px)
    ...entry(DataType.number.value, 5, numberToBytes(1.0)), // Alpha
  ];

  final texture = [
    ...entry(DataType.enum_.value, 0, [1]), // Type: Fill
    ...entry(DataType.colour.value, 3, [255, 0, 0, 255]), // Colour1: red RGBA
  ];

  // Texture dict with a different Colour1 (RGBA).
  List<int> textureWith(List<int> colour) => [
        ...entry(DataType.enum_.value, 0, [1]), // Type: Fill
        ...entry(DataType.colour.value, 3, colour),
      ];

  // Clean slate: clear any persisted requester subscriptions (their provider side
  // re-registers at boot and can write into dynamic block 0 field 0, clobbering the
  // render dict), then drop every dynamic block.
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

  final created = await reg.createDynamicBlock(BlockType.dynamic, 'RENDER');
  if (created == null) fail('createDynamicBlock failed');
  var b = (await reg.readDynamicBlocks())!.first;
  // ignore: avoid_print
  print('[D] created block index=${b.index} fieldCount=${b.fieldCount}');

  final geoField = await reg.appendDynamicEntry(
      b, BlockMeta(flagsAndType: 0x101 /* Geometry */, size: geometry.length), geometry);
  if (geoField == null) fail('append geometry dict failed');
  b = (await reg.readDynamicBlocks())!.first;
  // ignore: avoid_print
  print('[D] geometry field -> ok fieldCount=${b.fieldCount}');

  final texField = await reg.appendDynamicEntry(
      b, BlockMeta(flagsAndType: 0x102 /* Texture */, size: texture.length), texture);
  if (texField == null) fail('append texture dict failed');
  b = (await reg.readDynamicBlocks())!.first;
  // ignore: avoid_print
  print('[D] texture field -> ok fieldCount=${b.fieldCount}');

  // Point Display2 (Vysi1, type 0x06, inst 1 - the strip wired on GPIO0) at the render
  // block with brightness 20/255; park Display1 (inst 0) so only GPIO0 lights.
  final blockIndex = b.index;
  final setBlock = await reg.writeBlockField(0x06, 1, 2, 0,
      BlockMeta(flagsAndType: DataType.integer.value, size: 4), intToBytes(blockIndex, 4));
  if (setBlock == null) fail('set Display2.RenderBlock failed');
  final park1 = await reg.writeBlockField(0x06, 0, 2, 0,
      BlockMeta(flagsAndType: DataType.integer.value, size: 4), intToBytes(-1, 4));
  if (park1 == null) fail('park Display1.RenderBlock failed');
  final setBrightness = await reg.writeBlockField(0x06, 1, 0, 0,
      BlockMeta(flagsAndType: DataType.number.value, size: 4), numberToBytes(20.0));
  if (setBrightness == null) fail('set Display2.Brightness failed');
  // Apply the preloaded VYSIV1 layout (11x10) so the static block shows it used.
  final setLayout = await reg.writeBlockField(0x06, 1, 3, 0,
      BlockMeta(flagsAndType: DataType.string.value, size: 6), 'VYSIV1'.codeUnits);
  if (setLayout == null) fail('set Display2.LayoutFile failed');
  // ignore: avoid_print
  print('[D] Display2.RenderBlock=$blockIndex Brightness=20 LayoutFile=VYSIV1 (Display1 parked)');

  // Let the renderer run a few frames, then read back the achieved FPS.
  final link = ConnectionManager.instance;
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
  if (fps < 200) fail('RefreshRate $fps < 200 FPS (parallel bit-bang expected ~300)');

  // Sanity: read the render block back (whole dict blobs). Retries guard against a
  // requester subscription firing mid-run and clobbering the geometry field (the probe
  // clears requester subs first, but a provider could still push between cleanup and here).
  DynField? checkGeo;
  DynField? checkTex;
  for (var attempt = 0; attempt < 4; attempt++) {
    checkGeo = await reg.readDynamicField(b, 0);
    checkTex = await reg.readDynamicField(b, 1);
    if (checkGeo != null && checkGeo.value.length == geometry.length) break;
    await Future<void>.delayed(const Duration(milliseconds: 150));
  }
  // ignore: avoid_print
  print('[D] readback geometry=${checkGeo?.value.length ?? -1}B (expected ${geometry.length}B) '
      'texture=${checkTex?.value.length ?? -1}B (expected ${texture.length}B)');
  if (checkGeo == null || checkGeo.value.length != geometry.length) fail('geometry readback mismatch');
  if (checkTex == null || checkTex.value.length != texture.length) fail('texture readback mismatch');

  // RGB-black colour cycle on the square: rewrite the texture dict's Colour1 every 1.5s.
  const colours = [
    'red',
    'green',
    'blue',
    'black',
  ];
  final rgbas = <List<int>>[
    [255, 0, 0, 255],
    [0, 255, 0, 255],
    [0, 0, 255, 255],
    [0, 0, 0, 255],
  ];
  final dynField1 = (await reg.readDynamicField(b, 1))!;
  for (var c = 0; c < colours.length; c++) {
    final set = await reg.writeDynamicField(b, dynField1, textureWith(rgbas[c]));
    if (set == null) fail('cycle colour ${colours[c]} failed');
    // ignore: avoid_print
    print('[D] cycle -> ${colours[c]}');
    await Future<void>.delayed(const Duration(milliseconds: 1500));
  }
  // Leave the square red.
  await reg.writeDynamicField(b, dynField1, textureWith([255, 0, 0, 255]));
  // ignore: avoid_print
  print('[D] done - square cycled RGB-black, left red on Display2/GPIO0');
}

void main() async {
  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  setUpAll(() async => await connectHil());
  tearDownAll(disconnectHil);

  test('LED display: square + RefreshRate', () async {
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