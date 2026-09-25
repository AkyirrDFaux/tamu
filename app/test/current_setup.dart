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

// ---------------------------------------------------------------------------
// Scenario constants (Docs/Current setup v3.md)
// ---------------------------------------------------------------------------

/// Core dynamic blocks.
const int dynSubscriptions = 0; // received DAS values (subscription targets)
const int dynLeftEye = 1; // left display render dictionary
const int dynRightEye = 2; // right display render dictionary

/// Script slots (SCR_00..SCR_03).
const int scrTemperature = 0;
const int scrEyeMovement = 1;
const int scrLidTimer = 2;
const int scrBrightness = 3;

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
/// significant: a geometry sets the mask, the textures after it fill it.
const int eyeBgGeo = 0; // Fill (background)
const int eyeBgTex = 1; // Texture Fill (light: white, dark: black)
const int eyeIrisGeo = 2; // Circle (iris)
const int eyeIrisTex = 3; // Texture GradientLinear (green, horizontal fade)
const int eyePupilGeo = 4; // DoubleParabola
const int eyePupilTex = 5; // Texture Fill (light: black, dark: dark green)
const int eyeLidGeo = 6; // HalfFill (closes from the top)
const int eyeLidTex = 7; // Texture Fill black
const int eyePartCount = 8;

/// Eye render keys.
const int gkShape = 1;
const int gkOperation = 2;
const int gkPosition = 3;
const int gkSize = 4;
const int gkFade = 5;
const int tkType = 1;
const int tkPosition = 2;
const int tkSize = 3;
const int tkColour1 = 4;
const int tkColour2 = 5;

/// Geometry / texture enum values (Blocks/Render.h).
const int shapeFill = 1;
const int shapeHalfFill = 2;
const int shapeCircle = 6;
const int shapeDoubleParabola = 8;
const int opReplace = 0;
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
// Brightness = MIN + RANGE * ( (1-w)*t + w*t^4 ), t = (lux/luxSpan)^brightExpA, clamped. The
// two terms are x^0.2 and x^0.8 of the same x: the slow term keeps a dim room dim, the fast
// one steepens the top, and the cap is reached at luxSpan. Tuned on the device: ~200 lux ->
// ~20 %, 3000 lux -> ~42 %, 10k lux -> 70 %.
const double luxSpan = 10000; // lux at which the brightness cap is reached
const double brightWeightA = 0.45; // weight of the slow term (matches on 1-w = 0.55)
const double brightExpA = 0.2; // slow-term exponent (the fast term is 4x this)
/// Brightness script variable holding the slow curve term (index into `ScriptDraft.variables`).
const int brightCurveT = 8;
const double darkLux = 1; // below this lux the eye switches to dark mode in auto
const double luxDeadzone = 0.1; // lux: LDR subscription change deadzone
const double lidOpenTy = -5.5; // half-fill line below the screen (open)
const double lidClosedTy = 5.5; // half-fill line above the screen (closed)
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

  // 5: DoubleParabola pupil + 6: black fill.
  await setDynEntry(reg, block, eyePupilGeo, 0, DataType.geometry, const []);
  await setDynEntry(reg, block, eyePupilGeo, gkShape, DataType.enum_, enumByte(shapeDoubleParabola));
  await setDynEntry(reg, block, eyePupilGeo, gkOperation, DataType.enum_, enumByte(opReplace));
  await setDynEntry(reg, block, eyePupilGeo, gkPosition, DataType.matrix, identity23());
  // Size = [half-width, half-height]: a tall, clearly visible vertical pupil.
  await setDynEntry(reg, block, eyePupilGeo, gkSize, DataType.vector,
      [...num(pupilHalfW), ...num(pupilHalfH)]);
  await setDynEntry(reg, block, eyePupilGeo, gkFade, DataType.number, num(pupilFade));
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

ScriptSymbol _c(int i) => ScriptSymbol.constant(i);
ScriptSymbol _v(int i) => ScriptSymbol.variable(i);
ScriptSymbol _in(int i) => ScriptSymbol.input(i); // a script input read as an operand
ScriptSymbol _idx(int n) => ScriptSymbol.predefine(preIndex, n); // index / integer literal
ScriptSymbol _op(int op) => ScriptSymbol.predefine(preMathOp, op); // inline expression operator
ScriptSymbol _true() => ScriptSymbol.predefine(preBool, 1);
ScriptSymbol _ins(int cat, int op) => ScriptSymbol.instruction(cat, op);
ScriptSymbol _preNum(double v) => ScriptSymbol.predefine(preNumber, (v * 256).round() & 0xFFFF); // Q8.8 literal

ScriptLine _line(List<ScriptSymbol> dest, ScriptSymbol instr, [List<ScriptSymbol> ops = const []]) =>
    ScriptLine(destinations: dest, instruction: instr, operands: ops);

ScriptDraftValue _cNum(String name, double v) =>
    ScriptDraftValue(name: name, type: DataType.number, value: num(v));
ScriptDraftValue _cBlockInfo(String name, int v) =>
    ScriptDraftValue(name: name, type: DataType.blockInfo, value: u32(v));
ScriptDraftValue _cIx(String name, int v) =>
    ScriptDraftValue(name: name, type: DataType.integer, size: 4, value: u32(v));
ScriptDraftValue _var(String name, DataType type, int size) =>
    ScriptDraftValue(name: name, type: type, size: size);

// Script inputs (Docs/Current setup v3.md): live values the Register/editor can drive.
ScriptDraftValue _inNum(String name, double v,
        {double? min, double? max, double step = 0, int ui = ScriptUiType.auto}) =>
    ScriptDraftValue(
        name: name,
        type: DataType.number,
        value: num(v),
        spec: ScriptInputSpec(uiType: ui, min: min ?? 0, max: max ?? 0, step: step));

ScriptDraftValue _inIx(String name, int v,
        {double? min, double? max, double step = 0, int ui = ScriptUiType.auto}) =>
    ScriptDraftValue(
        name: name,
        type: DataType.integer,
        size: 4,
        value: u32(v),
        spec: ScriptInputSpec(uiType: ui, min: min ?? 0, max: max ?? 0, step: step));

ScriptDraftValue _inBool(String name, bool v) => ScriptDraftValue(
    name: name,
    type: DataType.bool_,
    size: 1,
    value: Uint8List.fromList([v ? 1 : 0]),
    spec: const ScriptInputSpec(uiType: ScriptUiType.toggle));

ScriptDraftValue _inVector2(String name, double x, double y) => ScriptDraftValue(
    name: name, type: DataType.vector, value: Uint8List.fromList([...num(x), ...num(y)]));

ScriptDraftValue _inMatrix23(String name, List<int> bytes) => ScriptDraftValue(
    name: name, type: DataType.matrix, value: Uint8List.fromList(bytes));

int _props() => ScriptProperties.loadOnBoot | ScriptProperties.runOnLoad;

/// Script 1: temperature -> fan duty (average of both DAS NTCs) with a proportional
/// controller: duty = clamp(P * (avg - target), 0, 100).
/// Input 0 = target temperature (degC), Input 1 = P constant (%/degC).
ScriptDraft scriptTemperature() {
  final d = ScriptDraft(functionName: 'Temperature regulation', properties: _props());
  d.inputs.addAll([
    _inNum('Target temperature', targetTemp, min: 0, max: 60, step: 1, ui: ScriptUiType.slider),
    _inNum('P constant', pGain, min: 0, max: 20, step: 0.5, ui: ScriptUiType.slider),
  ]);
  d.constants.addAll([
    _cBlockInfo('TEMP_A', bi(BlockType.dynamic.value, dynSubscriptions, fTempA, 0)),
    _cBlockInfo('TEMP_B', bi(BlockType.dynamic.value, dynSubscriptions, fTempB, 0)),
    _cBlockInfo('FAN_DUTY', bi(BlockType.pwm.value, fanInst, 1, 0)),
    _cNum('DUTY_MAX', 100),
    _cIx('PERIOD', 200),
  ]);
  d.variables.addAll([
    _var('tempA', DataType.number, 4),
    _var('tempB', DataType.number, 4),
    _var('avg', DataType.number, 4),
    _var('duty', DataType.number, 4),
  ]);
  d.lines.addAll([
    _line([], _ins(catFlow, 1), [_true()]), // While true
    _line([_v(0)], _ins(catService, 1), [_c(0)]), // tempA = read[TEMP_A]
    _line([_v(1)], _ins(catService, 1), [_c(1)]), // tempB = read[TEMP_B]
    // avg = (tempA + tempB) / 2
    _line([_v(2)], _ins(catMath, 0), [
      _op(mathOpOpenParen), _v(0), _op(0), _v(1), _op(mathOpCloseParen), _op(3), _idx(2),
    ]),
    // duty = P * (avg - target)
    _line([_v(3)], _ins(catMath, 0), [
      _in(1), _op(2), _op(mathOpOpenParen), _v(2), _op(1), _in(0), _op(mathOpCloseParen),
    ]),
    _line([_v(3)], _ins(catMath, 10), [_v(3), _idx(0), _c(3)]), // duty = Limit(duty, 0, DUTY_MAX)
    _line([], _ins(catService, 2), [_c(2), _v(3)]), // write[FAN_DUTY] = duty
    _line([], _ins(catTime, 0), [_c(4)]), // Delay PERIOD
    _line([], _ins(catFlow, 2)), // EndBlock
  ]);
  return d;
}

/// Script 4: LDR -> display brightness (each display uses its own DAS LDR) plus per-display
/// light/dark mode selection.
///
/// Auto mode derives the mode from the display's lux (below [darkLux] it goes dark); manual
/// mode takes Input 1 (left) / Input 2 (right). On a mode change the eye block's background
/// colour, ring shape, pupil colour and pupil size are rewritten - the eye render blocks are
/// built in light mode, so a fresh scene and a fresh script agree at boot.
///
/// Input 0 = manual mode (false = auto), Input 1 = manual left (dark), Input 2 = manual right.
ScriptDraft scriptBrightness() {
  final d = ScriptDraft(functionName: 'Brightness regulation', properties: _props());
  d.inputs.addAll([
    _inBool('Manual mode', false),
    _inBool('Manual left dark', false),
    _inBool('Manual right dark', false),
  ]);
  d.constants.addAll([
    _cBlockInfo('SUB_LUX_A', bi(BlockType.dynamic.value, dynSubscriptions, fLuxA, 0)), // 0
    _cBlockInfo('SUB_LUX_B', bi(BlockType.dynamic.value, dynSubscriptions, fLuxB, 0)), // 1
    _cBlockInfo('DISP_L_BRIGHT', bi(BlockType.vysiDisplay.value, dispLeft, 0, 0)), // 2
    _cBlockInfo('DISP_R_BRIGHT', bi(BlockType.vysiDisplay.value, dispRight, 0, 0)), // 3
    _cNum('BRIGHT_MAX', luxBrightMax), // 4
    _cNum('BRIGHT_MIN', luxBrightMin), // 5
    _cNum('LUX_SPAN', luxSpan), // 6
    _cNum('RANGE', luxBrightMax - luxBrightMin), // 7
    _cIx('PERIOD', 100), // 8
    _cNum('DARK_LUX', darkLux), // 9
    // Left eye mode keys.
    _cBlockInfo('L_IRIS_C1', bi(BlockType.dynamic.value, dynLeftEye, eyeIrisTex, tkColour1)), // 10
    _cBlockInfo('L_IRIS_C2', bi(BlockType.dynamic.value, dynLeftEye, eyeIrisTex, tkColour2)), // 11
    _cBlockInfo('L_BG_COL', bi(BlockType.dynamic.value, dynLeftEye, eyeBgTex, tkColour1)), // 12
    _cBlockInfo('L_PUPIL_COL', bi(BlockType.dynamic.value, dynLeftEye, eyePupilTex, tkColour1)), // 13
    // Right eye mode keys.
    _cBlockInfo('R_IRIS_C1', bi(BlockType.dynamic.value, dynRightEye, eyeIrisTex, tkColour1)), // 14
    _cBlockInfo('R_IRIS_C2', bi(BlockType.dynamic.value, dynRightEye, eyeIrisTex, tkColour2)), // 15
    _cBlockInfo('R_BG_COL', bi(BlockType.dynamic.value, dynRightEye, eyeBgTex, tkColour1)), // 16
    _cBlockInfo('R_PUPIL_COL', bi(BlockType.dynamic.value, dynRightEye, eyePupilTex, tkColour1)), // 17
    // Light-mode values (18..21).
    ScriptDraftValue(name: 'IRIS_C1_LIGHT', type: DataType.colour, size: 4, value: Uint8List.fromList(colour(irisR1, irisG1, irisB1))), // 18
    ScriptDraftValue(name: 'IRIS_C2_LIGHT', type: DataType.colour, size: 4, value: Uint8List.fromList(colour(irisR2, irisG2, irisB2))), // 19
    ScriptDraftValue(name: 'BG_LIGHT', type: DataType.colour, size: 4, value: Uint8List.fromList(colour(255, 255, 255))), // 20
    ScriptDraftValue(name: 'PUPIL_LIGHT', type: DataType.colour, size: 4, value: Uint8List.fromList(colour(0, 0, 0))), // 21
    // Dark-mode values (22..25): a really dark green iris and a slightly lighter pupil.
    ScriptDraftValue(name: 'IRIS_C1_DARK', type: DataType.colour, size: 4, value: Uint8List.fromList(colour(darkIrisR1, darkIrisG1, darkIrisB1))), // 22
    ScriptDraftValue(name: 'IRIS_C2_DARK', type: DataType.colour, size: 4, value: Uint8List.fromList(colour(darkIrisR2, darkIrisG2, darkIrisB2))), // 23
    ScriptDraftValue(name: 'BG_DARK', type: DataType.colour, size: 4, value: Uint8List.fromList(colour(0, 0, 0))), // 24
    ScriptDraftValue(name: 'PUPIL_DARK', type: DataType.colour, size: 4, value: Uint8List.fromList(colour(darkPupilR, darkPupilG, darkPupilB))), // 25
  ]);
  d.variables.addAll([
    _var('luxL', DataType.number, 4), // 0
    _var('luxR', DataType.number, 4), // 1
    _var('bL', DataType.number, 4), // 2
    _var('bR', DataType.number, 4), // 3
    _var('wantL', DataType.number, 4), // 4
    _var('modeL', DataType.number, 4), // 5
    _var('wantR', DataType.number, 4), // 6
    _var('modeR', DataType.number, 4), // 7
    _var('curveT', DataType.number, 4), // 8: slow curve term, reused by both eyes
  ]);

  // Brightness = MIN + RANGE * ( (1-w)*t + w*t^4 ), t = (lux/LUX_SPAN)^0.2, clamped to
  // [MIN, MAX]. The two terms are x^0.2 and x^0.8 of the same x = lux/LUX_SPAN: the slow term
  // keeps a dim room dim, the fast one steepens the top, and the cap lands at LUX_SPAN.
  List<ScriptLine> brightness(int luxVar, int outVar, int regConst) => [
        // t = (lux / LUX_SPAN) ^ 0.2
        _line([_v(brightCurveT)], _ins(catMath, 0), [
          _op(mathOpOpenParen),
          _op(mathOpOpenParen), _v(luxVar), _op(3), _c(6), _op(mathOpCloseParen), // (lux / SPAN)
          _op(5), _preNum(brightExpA), _op(mathOpCloseParen), // ^ 0.2
        ]),
        // out = MIN + RANGE * ( (1-w)*t + w*t*t*t*t )
        _line([_v(outVar)], _ins(catMath, 0), [
          _c(5), _op(0), _c(7), _op(2), // MIN + RANGE *
          _op(mathOpOpenParen),
          _preNum(brightWeightA), _op(2), _v(brightCurveT), _op(0), // w*t +
          _preNum(1 - brightWeightA), _op(2), _v(brightCurveT), _op(2), _v(brightCurveT), _op(2), _v(brightCurveT), _op(2), _v(brightCurveT), // (1-w)*t^4
          _op(mathOpCloseParen),
        ]),
        _line([_v(outVar)], _ins(catMath, 10), [_v(outVar), _c(5), _c(4)]), // Limit(out, MIN, MAX)
        _line([], _ins(catService, 2), [_c(regConst), _v(outVar)]), // reg[bright] = out
      ];

  ScriptLine set(int regConst, int valConst) =>
      _line([], _ins(catService, 2), [_c(regConst), _c(valConst)]);

  // If want != mode { <write that mode>; mode = want }. A mode change rewrites the iris
  // gradient, the background and the pupil colour for that eye.
  List<ScriptLine> modeCheck(int wantVar, int modeVar, int irisC1, int irisC2, int bgC, int pupilC) => [
        _line([], _ins(catFlow, 0), [_v(wantVar), _op(13), _v(modeVar)]), // If want != mode
        // Dark (want == 1)
        _line([], _ins(catFlow, 0), [_v(wantVar)]), // If want
        set(irisC1, 22), set(irisC2, 23), set(bgC, 24), set(pupilC, 25),
        _line([], _ins(catFlow, 2)), // EndBlock
        // Light (want == 0)
        _line([], _ins(catFlow, 0), [_v(wantVar), _op(12), _idx(0)]), // If want == 0
        set(irisC1, 18), set(irisC2, 19), set(bgC, 20), set(pupilC, 21),
        _line([], _ins(catFlow, 2)), // EndBlock
        _line([_v(modeVar)], _ins(catMath, 0), [_v(wantVar)]), // mode = want
        _line([], _ins(catFlow, 2)), // EndBlock
      ];

  d.lines.addAll([
    _line([], _ins(catFlow, 1), [_true()]), // While true
    _line([_v(0)], _ins(catService, 1), [_c(1)]), // luxL = reg[SUB_LUX_B]
    _line([_v(1)], _ins(catService, 1), [_c(0)]), // luxR = reg[SUB_LUX_A]
    ...brightness(0, 2, 2), // left display uses the RIGHT DAS LDR (LuxB)
    ...brightness(1, 3, 3), // right display uses the LEFT DAS LDR (LuxA)
    // Left mode: auto from the lux, or the manual input.
    _line([_v(4)], _ins(catMath, 0), [_v(0), _op(14), _c(9)]), // wantL = luxL < DARK_LUX
    _line([], _ins(catFlow, 0), [_in(0)]), // If manual
    _line([_v(4)], _ins(catMath, 0), [_in(1)]), // wantL = manual left
    _line([], _ins(catFlow, 2)), // EndBlock
    ...modeCheck(4, 5, 10, 11, 12, 13),
    // Right mode.
    _line([_v(6)], _ins(catMath, 0), [_v(1), _op(14), _c(9)]), // wantR = luxR < DARK_LUX
    _line([], _ins(catFlow, 0), [_in(0)]), // If manual
    _line([_v(6)], _ins(catMath, 0), [_in(2)]), // wantR = manual right
    _line([], _ins(catFlow, 2)), // EndBlock
    ...modeCheck(6, 7, 14, 15, 16, 17),
    _line([], _ins(catTime, 0), [_c(8)]), // Delay PERIOD
    _line([], _ins(catFlow, 2)), // EndBlock
  ]);
  return d;
}

/// Script 2: gyro angular velocity -> eye iris/pupil position (both displays).
///
/// Input 0 = base offset (Vector2; x is flipped for the right eye),
/// Input 1 = sensitivity (Matrix 2x3, a plain XYZ -> XY linear map: the movement is
/// (m0,m1,m2)·gyro and (m3,m4,m5)·gyro, "not a transformation").
ScriptDraft scriptEyeMovement() {
  final d = ScriptDraft(functionName: 'Eye movement', properties: _props());
  d.inputs.addAll([
    _inVector2('Offset', eyeOffsetX, eyeOffsetY),
    _inMatrix23('Sensitivity', identity23()), // default [1 0 0; 0 1 0]: 1 px per rad/s
  ]);
  d.constants.addAll([
    _cBlockInfo('GYRO', bi(BlockType.accGyr.value, 0, 6, 0)), // Angular Velocity (Vector3)
    _cBlockInfo('L_IRIS', bi(BlockType.dynamic.value, dynLeftEye, eyeIrisGeo, gkPosition)), // 1
    _cBlockInfo('L_PUPIL', bi(BlockType.dynamic.value, dynLeftEye, eyePupilGeo, gkPosition)), // 2
    _cBlockInfo('L_IRIS_TEX', bi(BlockType.dynamic.value, dynLeftEye, eyeIrisTex, tkPosition)), // 3
    _cBlockInfo('R_IRIS', bi(BlockType.dynamic.value, dynRightEye, eyeIrisGeo, gkPosition)), // 4
    _cBlockInfo('R_PUPIL', bi(BlockType.dynamic.value, dynRightEye, eyePupilGeo, gkPosition)), // 5
    _cBlockInfo('R_IRIS_TEX', bi(BlockType.dynamic.value, dynRightEye, eyeIrisTex, tkPosition)), // 6
    _cNum('LIMIT', eyeLimit), // 7
    _cNum('NEG_LIMIT', -eyeLimit), // 8
    _cIx('PERIOD', 30), // 9
  ]);
  d.variables.addAll([
    _var('g', DataType.vector, 12), // 0
    _var('gx', DataType.number, 4), // 1
    _var('gy', DataType.number, 4), // 2
    _var('gz', DataType.number, 4), // 3
    _var('s0', DataType.number, 4), // 4
    _var('s1', DataType.number, 4), // 5
    _var('s2', DataType.number, 4), // 6
    _var('s3', DataType.number, 4), // 7
    _var('s4', DataType.number, 4), // 8
    _var('s5', DataType.number, 4), // 9
    _var('mx', DataType.number, 4), // 10
    _var('my', DataType.number, 4), // 11
    _var('offX', DataType.number, 4), // 12
    _var('offY', DataType.number, 4), // 13
    _var('px', DataType.number, 4), // 14
    _var('py', DataType.number, 4), // 15
    _var('mat', DataType.matrix, 28), // 16
  ]);

  /// mat = Transform(0, px, py, 1, 1); written to the given field.
  List<ScriptLine> place(int regConst) => [
        _line([_v(16)], _ins(catMath, 11), [_idx(0), _v(14), _v(15), _idx(1), _idx(1)]),
        _line([], _ins(catService, 2), [_c(regConst), _v(16)]),
      ];

  /// px/py = movement (srcX/srcY) plus the base offset (mirrored when [flipX]).
  List<ScriptLine> at(int srcX, int srcY, {bool flipX = false}) => [
        _line([_v(14)], _ins(catMath, 0), [_v(srcX), _op(flipX ? 1 : 0), _v(12)]),
        _line([_v(15)], _ins(catMath, 0), [_v(srcY), _op(0), _v(13)]),
      ];

  d.lines.addAll([
    _line([], _ins(catFlow, 1), [_true()]), // While true
    _line([_v(0)], _ins(catService, 1), [_c(0)]), // g = read[GYRO]
    _line([_v(1)], _ins(catCompose, 1), [_v(0), _idx(0)]), // gx = g[0]
    _line([_v(2)], _ins(catCompose, 1), [_v(0), _idx(1)]), // gy = g[1]
    _line([_v(3)], _ins(catCompose, 1), [_v(0), _idx(2)]), // gz = g[2]
    // The gyro's in-plane sense is inverted relative to the render space (the panel is mounted
    // the other way round), so negate the two in-plane components: a tilt must move the pupils
    // the same way the rig leans. The axis mapping itself is correct.
    _line([_v(1)], _ins(catMath, 0), [_preNum(0), _op(1), _v(1)]), // gx = 0 - gx
    _line([_v(2)], _ins(catMath, 0), [_preNum(0), _op(1), _v(2)]), // gy = 0 - gy
    for (var i = 0; i < 6; i++)
      _line([_v(4 + i)], _ins(catCompose, 1), [_in(1), _idx(i)]), // s0..s5 = sensitivity[i]
    // movement = sensitivity * gyro (a plain 2x3 linear map, XYZ -> XY)
    _line([_v(10)], _ins(catMath, 0), [
      _v(4), _op(2), _v(1), _op(0), _v(5), _op(2), _v(2), _op(0), _v(6), _op(2), _v(3),
    ]),
    _line([_v(11)], _ins(catMath, 0), [
      _v(7), _op(2), _v(1), _op(0), _v(8), _op(2), _v(2), _op(0), _v(9), _op(2), _v(3),
    ]),
    _line([_v(10)], _ins(catMath, 10), [_v(10), _c(8), _c(7)]), // mx = Limit(mx, -LIMIT, LIMIT)
    _line([_v(11)], _ins(catMath, 10), [_v(11), _c(8), _c(7)]), // my = Limit(my, -LIMIT, LIMIT)
    _line([_v(12)], _ins(catCompose, 1), [_in(0), _idx(0)]), // offX = offset[0]
    _line([_v(13)], _ins(catCompose, 1), [_in(0), _idx(1)]), // offY = offset[1]
    // Iris, pupil and the iris fade all share one position: the pupil is smaller than the
    // iris and must stay inside it (moving them by different fractions let the pupil's tone
    // slide out past the iris edge).
    ...at(10, 11), ...place(1), ...place(2), ...place(3), // left iris + pupil + fade
    ...at(10, 11, flipX: true), ...place(4), ...place(5), ...place(6), // right (x mirrored)
    _line([], _ins(catTime, 0), [_c(9)]), // Delay PERIOD
    _line([], _ins(catFlow, 2)), // EndBlock
  ]);
  return d;
}

/// Script 3: blink. While blinking the lid is updated every loop tick (no artificial delay);
/// once the movement finishes (close + open over Input 1 each) the script waits Input 0 for
/// the next blink.
///
/// Input 0 = delay between blinks (ms, default 10 s), Input 1 = movement time each way (ms).
ScriptDraft scriptLidTimer() {
  final d = ScriptDraft(functionName: 'Lid timer', properties: _props());
  d.inputs.addAll([
    _inIx('Blink delay', lidWaitMs, min: 1000, max: 60000, step: 1000, ui: ScriptUiType.slider),
    _inIx('Movement time', lidMoveMs, min: 50, max: 1000, step: 50, ui: ScriptUiType.slider),
  ]);
  d.constants.addAll([
    _cBlockInfo('LID_L', bi(BlockType.dynamic.value, dynLeftEye, eyeLidGeo, gkPosition)),
    _cBlockInfo('LID_R', bi(BlockType.dynamic.value, dynRightEye, eyeLidGeo, gkPosition)),
    _cNum('OPEN_TY', lidOpenTy),
    _cNum('DELTA', lidClosedTy - lidOpenTy),
  ]);
  d.variables.addAll([
    // Get time yields a whole millisecond count, so its destinations are integers (Index):
    // a Q16.16 Number would overflow the absolute count past ~32767 ms.
    _var('t0', DataType.integer, 4), // 0
    _var('now', DataType.integer, 4), // 1
    _var('elapsed', DataType.number, 4), // 2
    _var('closeP', DataType.number, 4), // 3
    _var('openP', DataType.number, 4), // 4
    _var('ty', DataType.number, 4), // 5
    _var('mat', DataType.matrix, 28), // 6
  ]);

  /// mat = Transform(0, 0, ty, 1, 1) written to the lid geometry field.
  List<ScriptLine> applyLid(int regConst) => [
        _line([_v(6)], _ins(catMath, 11), [_idx(0), _idx(0), _v(5), _idx(1), _idx(1)]), // Transform mat = 0, 0, ty, 1, 1
        _line([], _ins(catService, 2), [_c(regConst), _v(6)]), // write[reg] = mat
      ];

  d.lines.addAll([
    _line([], _ins(catFlow, 1), [_true()]), // While true
    _line([_v(0)], _ins(catTime, 2)), // t0 = Get time
    _line([_v(1)], _ins(catTime, 2)), // now = Get time
    // While (now - t0) < MOVE_MS * 2  (the blink: no delay -> fastest update)
    _line([], _ins(catFlow, 1), [
      _op(mathOpOpenParen), _v(1), _op(1), _v(0), _op(mathOpCloseParen), _op(14),
      _in(1), _op(2), _idx(2),
    ]),
    _line([_v(2)], _ins(catMath, 0), [_v(1), _op(1), _v(0)]), // Set elapsed = now - t0
    _line([_v(3)], _ins(catMath, 0), [_v(2), _op(3), _in(1)]), // Set closeP = elapsed / MOVE_MS
    _line([_v(3)], _ins(catMath, 10), [_v(3), _idx(0), _idx(1)]), // closeP = Limit(closeP, 0, 1)
    _line([_v(4)], _ins(catMath, 0), [
      _op(mathOpOpenParen), _v(2), _op(1), _in(1), _op(mathOpCloseParen), _op(3), _in(1),
    ]), // Set openP = (elapsed - MOVE_MS) / MOVE_MS
    _line([_v(4)], _ins(catMath, 10), [_v(4), _idx(0), _idx(1)]), // openP = Limit(openP, 0, 1)
    _line([_v(5)], _ins(catMath, 0), [
      _op(mathOpOpenParen), _v(3), _op(1), _v(4), _op(mathOpCloseParen), _op(2), _c(3), _op(0), _c(2),
    ]), // Set ty = (closeP - openP) * DELTA + OPEN_TY
    ...applyLid(0),
    ...applyLid(1),
    _line([_v(1)], _ins(catTime, 2)), // now = Get time  (the While re-reads the condition)
    _line([], _ins(catFlow, 2)), // EndBlock
    // Movement finished: park the lid open and wait the blink delay for the next blink.
    _line([_v(5)], _ins(catMath, 0), [_c(2)]), // Set ty = OPEN_TY
    ...applyLid(0),
    ...applyLid(1),
    _line([], _ins(catTime, 0), [_in(0)]), // Delay blink delay
    _line([], _ins(catFlow, 2)), // EndBlock
  ]);
  return d;
}

/// Uploads, loads and runs the four scripts.
Future<void> buildScripts(StorageClient storage, ScriptClient scripts) async {
  final drafts = <int, ScriptDraft>{
    scrTemperature: scriptTemperature(),
    scrEyeMovement: scriptEyeMovement(),
    scrLidTimer: scriptLidTimer(),
    scrBrightness: scriptBrightness(),
  };
  for (final entry in drafts.entries) {
    final id = entry.key;
    final name = 'SCR_${id.toString().padLeft(2, '0')}';
    // Catch wrong constant indices / operand types before uploading.
    final errors = validateScriptLines(entry.value.lines, entry.value.validationContext);
    if (errors.isNotEmpty) {
      throw StateError('script $name invalid: ${errors.first}');
    }
    if (await scripts.readState(id) != null) await scripts.unload(id);
    await storage.deleteFile(name);
    if (!await storage.writeFile(name, entry.value.toImage())) {
      throw StateError('script upload failed: $name');
    }
    if (await scripts.load(id) != id) throw StateError('script load failed: $name');
    await scripts.setState(id, ScriptState.running);
  }
}

/// Unloads the scenario's scripts (they write into the dynamic blocks, so they must be
/// stopped before the blocks are rebuilt).
Future<void> stopScripts(ScriptClient scripts) async {
  for (final id in [scrTemperature, scrEyeMovement, scrLidTimer, scrBrightness]) {
    if (await scripts.readState(id) != null) await scripts.unload(id);
  }
}

/// Clamps both displays' brightness to a safe ceiling.
///
/// The LED strips draw enough current to brown out the board at high brightness, so this
/// runs first (a stale brightness script from a previous run can otherwise keep driving
/// the LEDs hard while the setup is being rebuilt).
Future<void> clampBrightness(RegisterClient reg, double pct) async {
  for (final inst in [dispLeft, dispRight]) {
    try {
      await setStatic(reg, BlockType.vysiDisplay.value, inst, 0, num(pct));
    } catch (_) {
      // Best-effort: the rebuild below reports a real failure if the device is unresponsive.
    }
  }
}

/// Applies the whole scenario. Returns the core + DAS for verification.
Future<({DeviceEntry core, List<DeviceEntry> das})> applyCurrentSetup(
    DeviceDatabase db, {
  bool clear = true,
  bool scripts = true,
}) async {
  final found = findDevices(db);
  final reg = RegisterClient(deviceId: found.core.id);
  final subs = SubscriptionClient(deviceId: found.core.id);
  final scriptClient = ScriptClient(deviceId: found.core.id);

  // Stop the scripts first: a running script keeps writing into the dynamic blocks
  // (eye/lid positions), which would race the rebuild below.
  await stopScripts(scriptClient);
  await clampBrightness(reg, luxBrightMin);

  // Clear stale provider entries on the DAS nodes. Their provider table is small (4) and
  // an earlier requester cancel does not always reach them, so a stale entry can block a
  // new subscription (see Issues.md).
  for (final das in found.das) {
    final c = SubscriptionClient(deviceId: das.id);
    for (var i = 0; i < 4; i++) {
      await c.setRequesterSubscription(i);
    }
  }

  if (clear) await clearSetup(reg, subs);
  await buildSubscriptionBlock(reg);
  await buildEyeBlock(reg, dynLeftEye, 'Left Eye');
  await buildEyeBlock(reg, dynRightEye, 'Right Eye');
  await configureDisplays(reg);
  await configureFan(reg);
  await configureGyro(reg);

  for (final das in found.das) {
    await configureDas(RegisterClient(deviceId: das.id));
  }

  await buildSubscriptions(subs, found.core, found.das);

  if (scripts) {
    await buildScripts(StorageClient(deviceId: found.core.id), scriptClient);
  }

  await reg.saveDynamic();

  // Flush the static settings to the STATLOG mirror. A static write only lands in RAM
  // until a Save (Register.md "the user selects what should be updated in the save"), so
  // without this the display render-block/layout, the fan, the gyro and the DAS
  // measurement settings all revert to their defaults on the next reboot.
  await saveStaticBlock(reg, BlockType.vysiDisplay.value, dispLeft);
  await saveStaticBlock(reg, BlockType.vysiDisplay.value, dispRight);
  await saveStaticBlock(reg, BlockType.pwm.value, fanInst);
  await saveStaticBlock(reg, BlockType.accGyr.value, 0);
  for (final das in found.das) {
    final dasReg = RegisterClient(deviceId: das.id);
    await saveStaticBlock(dasReg, BlockType.resistiveMeasure.value, 0);
    await saveStaticBlock(dasReg, BlockType.resistiveMeasure.value, 1);
  }

  return found;
}
