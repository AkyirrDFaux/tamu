/// Builds the evaluation scenario described in `Docs/Current setup v3.md`:
/// 2x DAS (NTC on ch1, LDR on ch2), 2x LED display eyes (light/dark mode), 1x fan, the
/// "Subscriptions" scratch block, and the four scripts with their documented inputs.
///
/// This is a test-side tool (not production UI): it drives the existing
/// [RegisterClient]/[SubscriptionClient]/[ScriptClient]/[StorageClient] to
/// create the whole configuration, so the scenario is reproducible and can be
/// captured to a backup zip. Run through `hil_current_setup_test.dart`.
library;


import 'dart:typed_data';

import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/script_client.dart';
import 'package:tamuapp/core/script_draft.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_instructions.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/subscription_client.dart';
import 'package:tamuapp/core/transform_23.dart';
import 'package:tamuapp/core/types.dart';

part 'current_setup_scripts.dart'; // script symbol helpers + the five script builders
part 'current_setup_apply.dart'; // build/stop/clamp and the whole-scenario apply

// ---------------------------------------------------------------------------
// Scenario constants (Docs/Current setup v3.md)
// ---------------------------------------------------------------------------

/// Core dynamic blocks.
const int dynSubscriptions = 0; // received DAS values (subscription targets)
const int dynLeftEye = 1; // left display render dictionary
const int dynRightEye = 2; // right display render dictionary

/// Script slots. The emote selector MUST sit in a higher slot than the eye movement script:
/// the main loop runs the loaded scripts in ascending slot order within one pass, so the
/// emote script reads that pass's freshly published pupil offset and the displays render in
/// the same pass (no cross-script loop delay).
const int scrTemperature = 0;
const int scrEyeMovement = 1;
const int scrLidTimer = 2;
const int scrBrightness = 3;
const int scrEmote = 4;

/// Display indices on the core. The two LED outputs are swapped on the rig: the physical
/// left display answers on instance 1 and the right one on instance 0.
const int dispLeft = 1;
const int dispRight = 0;

/// Fan PWM instance. The fan is wired to the second output (Fan2).
const int fanInst = 1;

/// Subscription block field indexes.
const int fTempA = 0;
const int fLuxA = 1;
const int fTempB = 2;
const int fLuxB = 3;

/// Eye render-block field indexes (interleaved geometry/texture parts). Part order is
/// significant: a geometry modifies the mask, the textures after it fill it. The pupil uses
/// two geometries: A is the base shape, B modifies it (Cut for the happy caret band, Add for
/// the dead cross's second bar). B is a no-op (Shape None + Add) for emotes needing one shape.
const int eyeBgGeo = 0; // Fill (background)
const int eyeBgTex = 1; // Texture Fill (light: white, dark: black)
const int eyeIrisGeo = 2; // Circle (iris)
const int eyeIrisTex = 3; // Texture GradientLinear (green, horizontal fade)
const int eyePupilGeoA = 4; // pupil base (DoubleParabola / Triangle / Rectangle)
const int eyePupilGeoB = 5; // pupil modifier (Cut / Add / None)
const int eyePupilTex = 6; // Texture Fill (light: black, dark: desaturated green)
const int eyeLidGeo = 7; // HalfFill (closes from the top)
const int eyeLidTex = 8; // Texture Fill black
const int eyePartCount = 9;

/// Eye render keys.
const int gkShape = 1;
const int gkOperation = 2;
const int gkPosition = 3;
const int gkSize = 4;
const int gkFade = 5;
const int gkAngles = 8;
const int tkType = 1;
const int tkPosition = 2;
const int tkSize = 3;
const int tkColour1 = 4;
const int tkColour2 = 5;

/// Geometry / texture enum values (Blocks/Render.h).
const int shapeNone = 0;
const int shapeFill = 1;
const int shapeHalfFill = 2;
const int shapeRectangle = 4;
const int shapeCircle = 6;
const int shapeDoubleParabola = 8;
const int shapeTriangle = 9;
const int opReplace = 0;
const int opAdd = 1;
const int opCut = 2;
const int texFill = 1;
const int texGradientLinear = 2;

/// Sensor type values (Measurement.md).
const int sensorNtc100k = 5; // the DAS ch1 NTC is a 100 kohm part (firmware default)
const int sensorLdr10k = 3;

/// Eye look. The blocks are built in light mode; the brightness script writes the dark-mode
/// values in on a mode change. Both modes use a filled iris (no ring).
const double irisDiameter = 9; // px (outer)
const double irisFade = 0.6;
// Iris texture: a light horizontal fade, left brighter -> right darker, centred on the
// pupil (the eye script writes the texture Position to the pupil position).
const int irisR1 = 0, irisG1 = 242, irisB1 = 0; // light mode: brighter (left)
const int irisR2 = 0, irisG2 = 153, irisB2 = 0; // light mode: darker (right)
const double pupilHalfW = 2.4; // DoubleParabola half-width (same in both modes)
const double pupilHalfH = 5.0; // DoubleParabola half-height
const double pupilFade = 0.6;
const double lidFade = 4.0;

/// Dark-mode look (written by script 4 on a mode change), as tuned on the device.
const int darkIrisR1 = 0, darkIrisG1 = 140, darkIrisB1 = 0; // 0x008C00, brighter (left)
const int darkIrisR2 = 0, darkIrisG2 = 102, darkIrisB2 = 0; // 0x006600, darker (right)
const int darkPupilR = 179, darkPupilG = 173, darkPupilB = 116; // 0xB3AD74

/// Eye movement defaults (script 2 inputs).
const double eyeOffsetX = 1.0; // px toward the face centre (Input 0.x; flipped for the right eye)
const double eyeOffsetY = -0.5; // px up (+y in the render space points down on the mounted displays)
const double eyeLimit = 3.0; // px clamp (safety, internal)
const int eyeMovePeriod = 30; // ms between eye-movement updates (script 2 PERIOD)
const int emotePeriod = 5; // ms between emote-script passes (script 5 PERIOD)

/// Emote selector (script 5). The input value is the emote's integer index (custom enum).
const int emoteNormal = 0;
const int emoteHappy = 1;
const int emoteDead = 2;
const int emoteAnnoyed = 3;
const List<String> emoteNames = ['Normal', 'Happy', 'Dead', 'Annoyed'];

// Emote pupil geometry (px; the iris radius is irisDiameter/2 = 4.5). "Normal"/"Annoyed" use
// the tuned DoubleParabola; "Happy" is a hollow triangle (base + a smaller cut), "Dead" is two
// crossed bars (base + an added bar).
const double happyOuterSide = 7.0; // outer triangle side (full)
const double happyInnerSide = 3.0; // cut triangle side (full): the band is (outer-inner)/2
const double happyApexAngle = 55; // isosceles apex (deg): pointier = carets better
// The cut triangle is shifted DOWN so it removes the base, leaving the two upper edges (a
// caret). Needs inner centre + inner half-height >= outer base.
const double happyCutOffsetY = 1.9; // px
const double happyOffsetY = -1.5; // px: the whole happy pupil sits this much higher
const double deadBarWidth = 1.9; // rectangle bar width (full)
const double deadBarLength = 9.0; // rectangle bar length (full)
const double emoteTiltDead = 0.7853981633974483; // 45 deg: part A +tilt, part B -tilt
const double emotePupilFade = 0.4; // pupil edge softness while an emote is applied
const double lidAnnoyedOpen = 0.58; // lid max opening while annoyed (1 = fully open)

/// Display Offset matrices (2x3 affine, raw wire bytes) - the mounting rotations fixed on
/// the device. A rotation is a property of the panel and its mount, so it is keyed by CORE
/// INSTANCE and stays with the output even though the two outputs are swapped.
/// Instance 0 is mounted ~5 deg, instance 1 ~175 deg.
const List<int> dispOffsetInst0 = [
  2, 0, 3, 0, 250, 254, 0, 0, 70, 22, 0, 0, 0, 0, 0, 0, 186, 233, 255, 255, 250, 254, 0, 0, 0, 0, 0, 0,
];
const List<int> dispOffsetInst1 = [
  2, 0, 3, 0, 6, 1, 255, 255, 70, 22, 0, 0, 0, 0, 0, 0, 186, 233, 255, 255, 6, 1, 255, 255, 0, 0, 0, 0,
];

/// Script tunables / input defaults (Docs/Current setup v3.md).
const double targetTemp = 30; // degC (script 1 Input 0)
const double pGain = 4; // %/degC (script 1 Input 1)
const double luxBrightMin = 5; // % at 0 lux
const double luxBrightMax = 70; // % cap
// Brightness (Docs/Current setup v3.md): <10 lux -> the 5 % floor, 100 lux -> 10 %,
// 3000 lux -> 40 %, 70 % from ~8.8k lux up. Below LUX_MIN the curve is flat at the floor, so
// it is a power law over (lux - LUX_MIN) with the exponent fitted to the 100/3000 anchors.
const double luxSpan = 8850; // lux at which the brightness cap is reached
const double luxMin = 10; // lux below which the brightness stays at the floor
const double brightExpA = 0.5721; // curve exponent (see above)
/// Brightness script variable holding the clamped lux (index into `ScriptDraft.variables`).
const int brightLuxVar = 8;
const double darkLux = 1; // below this lux the eye switches to dark mode in auto
const double luxDeadzone = 0.1; // lux: LDR subscription change deadzone
const double lidOpenTy = -5.5; // half-fill line below the screen (open)
// A little past the bottom edge: the last LED row's centre sits just inside the screen, so a
// line exactly on the edge would leave that row uncovered when fully closed.
const double lidClosedTy = 6.5; // half-fill line below the screen (closed)
const int lidWaitMs = 10000; // default blink interval between blinks (script 3 Input 0, ms)
const int lidMoveMs = 200; // default movement time each way (script 3 Input 1, ms)

// ---------------------------------------------------------------------------
// Small wire helpers
// ---------------------------------------------------------------------------

Uint8List num(double v) => numberToBytes(v);
Uint8List u32(int v) => uint32ToBytes(v);
List<int> colour(int r, int g, int b, [int a = 255]) => [r, g, b, a];
List<int> enumByte(int v) => [v];

/// A 2x3 affine with a translation (and optional scale/rotation).
List<int> affine({double tx = 0, double ty = 0, double sx = 1, double sy = 1, double rot = 0}) {
  final t = Transform23()
    ..offsetX = tx
    ..offsetY = ty
    ..scaleX = sx
    ..scaleY = sy
    ..rotation = rot;
  return t.toMatrix();
}

List<int> identity23() => affine();

int bi(int type, int inst, int field, int key) => makeBlockInfo(type, inst, field, key);

// ---------------------------------------------------------------------------
// Register writes
// ---------------------------------------------------------------------------

DynBlock _dyn(int index, [String name = '']) =>
    DynBlock(index: index, meta: BlockMeta(flagsAndType: BlockType.dynamic.value, size: 0), name: name);

/// Writes one (field, key) entry into a dynamic block, creating the field if needed.
/// Retries a few times: a busy device (rendering, subscriptions) can drop a reply.
///
/// [persistent] defaults to true: a render dictionary is configuration, and a dynamic entry
/// without the Persistent flag is zeroed when the block is reloaded from its DT/DV files at
/// boot - so the whole scene (shapes, texture types, sizes, fades) would be lost on a reboot.
Future<void> setDynEntry(RegisterClient reg, int block, int field, int key, DataType type,
    List<int> value, {bool persistent = true}) async {
  final flags = type.value | (persistent ? FieldFlags.persistent : 0);
  final meta = BlockMeta(flagsAndType: flags, key: key, size: value.length);
  for (var attempt = 0; attempt < 4; attempt++) {
    final ok = await reg.writeDynamicEntry(_dyn(block), field, key, meta, value);
    if (ok != null) return;
    await Future<void>.delayed(const Duration(milliseconds: 200));
  }
  throw StateError('dynamic write failed: block $block field $field key $key (${type.name})');
}

/// Writes a static block field, reusing the field's current type from the device.
/// Skips the write when the value is already equal (avoids re-running write triggers,
/// e.g. the LED-display layout reload). Retries a busy device.
Future<void> setStatic(
    RegisterClient reg, int type, int inst, int field, List<int> value) async {
  BlockMeta? meta;
  List<int>? cur;
  for (var attempt = 0; attempt < 5; attempt++) {
    final r = await reg.readBlockField(type, inst, field, 0);
    if (r != null) {
      meta = r.meta;
      cur = r.value;
      break;
    }
    await Future<void>.delayed(const Duration(milliseconds: 200));
  }
  if (meta == null || cur == null) {
    throw StateError('static read failed: type $type inst $inst field $field');
  }
  if (_bytesEqual(cur, value)) return;
  final wm = BlockMeta(flagsAndType: meta.flagsAndType, key: 0, size: value.length);
  for (var attempt = 0; attempt < 4; attempt++) {
    final ok = await reg.writeBlockField(type, inst, field, 0, wm, value);
    if (ok != null) return;
    await Future<void>.delayed(const Duration(milliseconds: 200));
  }
  throw StateError('static write failed: type $type inst $inst field $field');
}

/// Persists a static block's persistent fields to flash (Register CID 3, field 0xFF).
/// Retries a busy device; throws so a setup that would not survive a reboot fails loudly.
Future<void> saveStaticBlock(RegisterClient reg, int type, int inst) async {
  for (var attempt = 0; attempt < 4; attempt++) {
    if (await reg.saveStatic(type, inst)) return;
    await Future<void>.delayed(const Duration(milliseconds: 250));
  }
  throw StateError('static save failed: type $type inst $inst');
}

bool _bytesEqual(List<int> a, List<int> b) {
  if (a.length != b.length) return false;
  for (var i = 0; i < a.length; i++) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Structure
// ---------------------------------------------------------------------------

/// Finds the core and the DAS nodes (ordered by short address).
({DeviceEntry core, List<DeviceEntry> das}) findDevices(DeviceDatabase db) {
  DeviceEntry? core;
  final das = <DeviceEntry>[];
  for (final d in db.all) {
    if (d.type == DeviceType.tamuV20A) core ??= d;
    if (d.type == DeviceType.dualAnalogSensor) das.add(d);
  }
  das.sort((a, b) => a.id.compareTo(b.id));
  if (core == null) throw StateError('Tamu core not found');
  if (das.length < 2) throw StateError('expected 2 DAS nodes, found ${das.length}');
  return (core: core, das: das);
}

/// Removes every dynamic block and requester subscription (deterministic start).
Future<void> clearSetup(RegisterClient reg, SubscriptionClient subs) async {
  var list = await subs.getRequesterSubscriptions();
  while (list.isNotEmpty) {
    await subs.setRequesterSubscription(list.first.index);
    list = await subs.getRequesterSubscriptions();
  }
  for (final b in await reg.readDynamicBlocks() ?? <DynBlock>[]) {
    await reg.deleteDynamic(block: b.index);
  }
  await reg.saveDynamic();
}

/// Dynamic block 0 "Subscriptions": the four DAS value targets.
Future<void> buildSubscriptionBlock(RegisterClient reg) async {
  await reg.deleteDynamic(block: dynSubscriptions);
  await reg.createDynamicBlock(BlockType.dynamic, 'Subscriptions', index: dynSubscriptions);
  for (final f in [fTempA, fLuxA, fTempB, fLuxB]) {
    // These hold live values pushed by the DAS, so they are volatile (a stale reading is
    // useless after a reboot; the subscriptions refill them immediately).
    await setDynEntry(reg, dynSubscriptions, f, 0, DataType.number, num(0), persistent: false);
  }
}

/// One eye render dictionary (dynamic block).
///
/// Parts are processed in field order: a geometry builds the mask, the texture
/// after it fills that mask (`Vysi1Display::Render`).
Future<void> buildEyeBlock(RegisterClient reg, int block, String name) async {
  await reg.deleteDynamic(block: block);
  await reg.createDynamicBlock(BlockType.dynamic, name, index: block);

  // 0: Fill geometry (whole screen) + 1: white fill.
  await setDynEntry(reg, block, eyeBgGeo, 0, DataType.geometry, const []);
  await setDynEntry(reg, block, eyeBgGeo, gkShape, DataType.enum_, enumByte(shapeFill));
  await setDynEntry(reg, block, eyeBgGeo, gkOperation, DataType.enum_, enumByte(opReplace));
  await setDynEntry(reg, block, eyeBgTex, 0, DataType.texture, const []);
  await setDynEntry(reg, block, eyeBgTex, tkType, DataType.enum_, enumByte(texFill));
  await setDynEntry(reg, block, eyeBgTex, tkColour1, DataType.colour, colour(255, 255, 255));

  // 2: Circle (iris) + 3: green horizontal fade.
  await setDynEntry(reg, block, eyeIrisGeo, 0, DataType.geometry, const []);
  await setDynEntry(reg, block, eyeIrisGeo, gkShape, DataType.enum_, enumByte(shapeCircle));
  await setDynEntry(reg, block, eyeIrisGeo, gkOperation, DataType.enum_, enumByte(opReplace));
  await setDynEntry(reg, block, eyeIrisGeo, gkPosition, DataType.matrix, identity23());
  await setDynEntry(reg, block, eyeIrisGeo, gkSize, DataType.number, num(irisDiameter));
  await setDynEntry(reg, block, eyeIrisGeo, gkFade, DataType.number, num(irisFade));
  await setDynEntry(reg, block, eyeIrisTex, 0, DataType.texture, const []);
  await setDynEntry(reg, block, eyeIrisTex, tkType, DataType.enum_, enumByte(texGradientLinear));
  await setDynEntry(reg, block, eyeIrisTex, tkPosition, DataType.matrix, identity23());
  await setDynEntry(reg, block, eyeIrisTex, tkSize, DataType.number, num(irisDiameter));
  await setDynEntry(reg, block, eyeIrisTex, tkColour1, DataType.colour, colour(irisR1, irisG1, irisB1));
  await setDynEntry(reg, block, eyeIrisTex, tkColour2, DataType.colour, colour(irisR2, irisG2, irisB2));

  // 4/5: pupil base (DoubleParabola) + modifier (unused here: Shape None + Add = no-op),
  // 6: pupil fill. Both geometries and their keys are created up front with their final types
  // so the emote script only ever changes values, never a key's type or length.
  await setDynEntry(reg, block, eyePupilGeoA, 0, DataType.geometry, const []);
  await setDynEntry(reg, block, eyePupilGeoA, gkShape, DataType.enum_, enumByte(shapeDoubleParabola));
  await setDynEntry(reg, block, eyePupilGeoA, gkOperation, DataType.enum_, enumByte(opReplace));
  await setDynEntry(reg, block, eyePupilGeoA, gkPosition, DataType.matrix, identity23());
  // Size = [half-width, half-height] for the parabola; always a Vector2 across emotes.
  await setDynEntry(reg, block, eyePupilGeoA, gkSize, DataType.vector,
      [...num(pupilHalfW), ...num(pupilHalfH)]);
  await setDynEntry(reg, block, eyePupilGeoA, gkFade, DataType.number, num(pupilFade));
  await setDynEntry(reg, block, eyePupilGeoA, gkAngles, DataType.number, num(0));

  await setDynEntry(reg, block, eyePupilGeoB, 0, DataType.geometry, const []);
  await setDynEntry(reg, block, eyePupilGeoB, gkShape, DataType.enum_, enumByte(shapeNone));
  await setDynEntry(reg, block, eyePupilGeoB, gkOperation, DataType.enum_, enumByte(opAdd));
  await setDynEntry(reg, block, eyePupilGeoB, gkPosition, DataType.matrix, identity23());
  await setDynEntry(reg, block, eyePupilGeoB, gkSize, DataType.vector,
      [...num(pupilHalfW), ...num(pupilHalfH)]);
  await setDynEntry(reg, block, eyePupilGeoB, gkFade, DataType.number, num(pupilFade));
  await setDynEntry(reg, block, eyePupilGeoB, gkAngles, DataType.number, num(0));

  await setDynEntry(reg, block, eyePupilTex, 0, DataType.texture, const []);
  await setDynEntry(reg, block, eyePupilTex, tkType, DataType.enum_, enumByte(texFill));
  await setDynEntry(reg, block, eyePupilTex, tkColour1, DataType.colour, colour(0, 0, 0));

  // 7: HalfFill lid (closes from the top) + 8: black fill.
  await setDynEntry(reg, block, eyeLidGeo, 0, DataType.geometry, const []);
  await setDynEntry(reg, block, eyeLidGeo, gkShape, DataType.enum_, enumByte(shapeHalfFill));
  await setDynEntry(reg, block, eyeLidGeo, gkOperation, DataType.enum_, enumByte(opReplace));
  await setDynEntry(reg, block, eyeLidGeo, gkPosition, DataType.matrix, affine(ty: lidOpenTy));
  await setDynEntry(reg, block, eyeLidGeo, gkFade, DataType.number, num(lidFade));
  await setDynEntry(reg, block, eyeLidTex, 0, DataType.texture, const []);
  await setDynEntry(reg, block, eyeLidTex, tkType, DataType.enum_, enumByte(texFill));
  await setDynEntry(reg, block, eyeLidTex, tkColour1, DataType.colour, colour(0, 0, 0));
}

/// Points a display at its eye render block, applies the layout and the mounting rotation.
Future<void> configureDisplays(RegisterClient reg) async {
  await setStatic(reg, BlockType.vysiDisplay.value, dispLeft, 2, u32(dynLeftEye));
  await setStatic(reg, BlockType.vysiDisplay.value, dispRight, 2, u32(dynRightEye));
  // Preserve the mounting rotations fixed on the device, by instance (not by side).
  await setStatic(reg, BlockType.vysiDisplay.value, 0, 1, dispOffsetInst0);
  await setStatic(reg, BlockType.vysiDisplay.value, 1, 1, dispOffsetInst1);
  final layout = 'LAY_1'.padRight(8).codeUnits; // 8-char space-padded storage name
  await setStatic(reg, BlockType.vysiDisplay.value, dispLeft, 3, layout);
  await setStatic(reg, BlockType.vysiDisplay.value, dispRight, 3, layout);
  // Start at the low end of the range; script 4 then drives it from the LDR. After the
  // transfer-curve fix the brightness value IS the LED duty (current ∝ duty), so the floor
  // is the safe starting point.
  await setStatic(reg, BlockType.vysiDisplay.value, dispLeft, 0, num(luxBrightMin));
  await setStatic(reg, BlockType.vysiDisplay.value, dispRight, 0, num(luxBrightMin));
}

/// Fan PWM: the fan is not connected in this setup. Keep the default 25 kHz timer
/// frequency (reconfiguring the LEDC timer to a low frequency is rejected by the driver)
/// and start the duty at 0; the temperature script drives it afterwards.
Future<void> configureFan(RegisterClient reg) async {
  await setStatic(reg, BlockType.pwm.value, fanInst, 1, num(0));
}

/// Acc&Gyr: sampling + filters (range defaults are fine).
Future<void> configureGyro(RegisterClient reg) async {
  await setStatic(reg, BlockType.accGyr.value, 0, 0, enumByte(3)); // 104 Hz
  await setStatic(reg, BlockType.accGyr.value, 0, 3, num(0.2)); // accel filter
  await setStatic(reg, BlockType.accGyr.value, 0, 4, num(0.2)); // angular filter
}

/// DAS ch1 = NTC10K, ch2 = LDR 10K, both sampling.
Future<void> configureDas(RegisterClient reg) async {
  for (final inst in [0, 1]) {
    await setStatic(reg, BlockType.resistiveMeasure.value, inst, 0, num(10)); // 10 Hz
    await setStatic(reg, BlockType.resistiveMeasure.value, inst, 1,
        enumByte(inst == 0 ? sensorNtc100k : sensorLdr10k));
    await setStatic(reg, BlockType.resistiveMeasure.value, inst, 2, num(0.2)); // EMA
  }
}

/// Four subscriptions: DAS A ch1/ch2 -> f0/f1, DAS B ch1/ch2 -> f2/f3.
Future<void> buildSubscriptions(
    SubscriptionClient subs, DeviceEntry core, List<DeviceEntry> das) async {
  // (provider, measurement instance, target field, deadzone, period ms, min ms)
  // The lux reading is noisy but its subscription needs to track it closely (0.1 lux), and
  // the period is short so the brightness reacts quickly to a real light change.
  final sources = [
    (das[0].id, 0, fTempA, 0.2, 500, 200),
    (das[0].id, 1, fLuxA, luxDeadzone, 500, 200),
    (das[1].id, 0, fTempB, 0.2, 500, 200),
    (das[1].id, 1, fLuxB, luxDeadzone, 500, 200),
  ];
  for (var i = 0; i < sources.length; i++) {
    final (addr, measInst, field, deadzone, period, minTime) = sources[i];
    final entry = RequesterSubscription(
      index: i,
      providerAddr: addr,
      trid: 0xFB00 + i,
      targetReg: bi(BlockType.dynamic.value, dynSubscriptions, field, 0),
      sourceReg: bi(BlockType.resistiveMeasure.value, measInst, 3, 0), // Measured Value
      trigger: TriggerType.deltaPeriodic,
      periodMs: period,
      minTimeMs: minTime,
      deadzone: deadzone,
    );
    if (!await subs.setRequesterSubscription(i, entry: entry)) {
      throw StateError('subscription $i failed');
    }
  }
}

// ---------------------------------------------------------------------------
// Scripts
// ---------------------------------------------------------------------------
