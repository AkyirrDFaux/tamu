/// Builds the evaluation scenario described in `Docs/Current setup.md`:
/// 2x DAS (NTC on ch1, LDR on ch2), 2x LED display eyes, 1x fan, the
/// "Subscriptions" scratch block, and the four scripts.
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
// Scenario constants (Docs/Current setup.md)
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

/// Display indices on the core.
const int dispLeft = 0;
const int dispRight = 1;

/// Fan PWM instance.
const int fanInst = 0;

/// Subscription block field indexes.
const int fTempA = 0;
const int fLuxA = 1;
const int fTempB = 2;
const int fLuxB = 3;

/// Eye render-block field indexes (interleaved geometry/texture parts).
const int eyeBgGeo = 0; // Fill (white background)
const int eyeBgTex = 1; // Texture Fill white
const int eyeIrisGeo = 2; // Circle
const int eyeIrisTex = 3; // Texture GradientLinear (green, horizontal fade)
const int eyePupilGeo = 4; // DoubleParabola
const int eyePupilTex = 5; // Texture Fill black
const int eyeLidGeo = 6; // HalfFill (closes from the top)
const int eyeLidTex = 7; // Texture Fill black

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
const int shapeCircle = 6;
const int shapeDoubleParabola = 8;
const int shapeHalfFill = 2;
const int opReplace = 0;
const int texFill = 1;
const int texGradientLinear = 2;

/// Sensor type values (Measurement.md).
const int sensorNtc100k = 5; // the DAS ch1 NTC is a 100 kohm part (firmware default)
const int sensorLdr10k = 3;

/// Eye look (values tuned on the device; applied to both eyes).
const double irisDiameter = 9; // px
const double irisFade = 0.6;
// Iris texture: a light horizontal fade, left brighter -> right darker, centred on the
// pupil (the eye script writes the texture Position to the pupil position).
const int irisR1 = 0, irisG1 = 150, irisB1 = 0; // brighter (left)
const int irisR2 = 0, irisG2 = 45, irisB2 = 0; // darker (right)
const double pupilHalfW = 2.4; // DoubleParabola half-width
const double pupilHalfH = 5.0; // DoubleParabola half-height
const double pupilFade = 0.6;
const double lidFade = 4.0;

/// Base eye position offset (iris+pupil) in display space: move the eye inward and
/// slightly up. The right eye mirrors the horizontal component. The vertical sign is
/// negative because +y in the render space points down on these mounted displays.
const double eyeBaseIn = 1.0; // px toward the face centre
const double eyeBaseUp = -0.5; // px up (negative y)

/// Display Offset matrices (2x3 affine, raw wire bytes) - the mounting rotations fixed
/// on the device. Left is mounted ~180 deg, right ~5 deg.
const List<int> dispLeftOffset = [
  2, 0, 3, 0, 6, 1, 255, 255, 70, 22, 0, 0, 0, 0, 0, 0, 186, 233, 255, 255, 6, 1, 255, 255, 0, 0, 0, 0,
];
const List<int> dispRightOffset = [
  2, 0, 3, 0, 250, 254, 0, 0, 70, 22, 0, 0, 0, 0, 0, 0, 186, 233, 255, 255, 250, 254, 0, 0, 0, 0, 0, 0,
];

/// Script constants (tunable).
const double tempMin = 20; // degC -> 0%
const double tempMax = 40; // degC -> 100%
const double luxBrightMin = 5; // % in the dark
const double luxBrightMax = 20; // % in bright ambient (kept low: high LED current browns out the board)
const double luxSpan = 500; // lux for the full brightness swing
const double eyeScale = 1.0; // px per rad/s
const double eyeLimit = 3.0; // px clamp
const double lidOpenTy = -5.5; // half-fill line below the screen (open)
const double lidClosedTy = 5.5; // half-fill line above the screen (closed)
const int lidWaitMs = 10000; // open time between blinks
const int lidMoveMs = 100; // close/open movement time

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
Future<void> setDynEntry(
    RegisterClient reg, int block, int field, int key, DataType type, List<int> value) async {
  final meta = BlockMeta(flagsAndType: type.value, key: key, size: value.length);
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
    await setDynEntry(reg, dynSubscriptions, f, 0, DataType.number, num(0));
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

  // 2: Circle (iris) + 3: solid green fill.
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

  // 4: DoubleParabola pupil + 5: black fill.
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

  // 6: HalfFill lid (closes from the top) + 7: black fill.
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
  // Preserve the mounting rotations fixed on the device.
  await setStatic(reg, BlockType.vysiDisplay.value, dispLeft, 1, dispLeftOffset);
  await setStatic(reg, BlockType.vysiDisplay.value, dispRight, 1, dispRightOffset);
  final layout = 'LAY_1'.padRight(8).codeUnits; // 8-char space-padded storage name
  await setStatic(reg, BlockType.vysiDisplay.value, dispLeft, 3, layout);
  await setStatic(reg, BlockType.vysiDisplay.value, dispRight, 3, layout);
  // Keep the brightness low: the LED strips draw enough current at high brightness to
  // brown out the board (the default 30% is already near the limit on USB power).
  await setStatic(reg, BlockType.vysiDisplay.value, dispLeft, 0, num(5));
  await setStatic(reg, BlockType.vysiDisplay.value, dispRight, 0, num(5));
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
  // The lux reading is noisy, so it needs a larger deadzone than the temperature, but the
  // period is short so the brightness reacts quickly to a real light change.
  final sources = [
    (das[0].id, 0, fTempA, 0.2, 500, 200),
    (das[0].id, 1, fLuxA, 2.0, 500, 200),
    (das[1].id, 0, fTempB, 0.2, 500, 200),
    (das[1].id, 1, fLuxB, 2.0, 500, 200),
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
ScriptSymbol _idx(int n) => ScriptSymbol.predefine(preIndex, n); // index / integer literal
ScriptSymbol _true() => ScriptSymbol.predefine(preBool, 1);
ScriptSymbol _ins(int cat, int op) => ScriptSymbol.instruction(cat, op);

ScriptLine _line(List<ScriptSymbol> dest, ScriptSymbol instr, [List<ScriptSymbol> ops = const []]) =>
    ScriptLine(destinations: dest, instruction: instr, operands: ops);

ScriptDraftValue _cNum(String name, double v) =>
    ScriptDraftValue(name: name, type: DataType.number, value: num(v));
ScriptDraftValue _cBlockInfo(String name, int v) =>
    ScriptDraftValue(name: name, type: DataType.blockInfo, value: u32(v));
ScriptDraftValue _cIx(String name, int v) =>
    ScriptDraftValue(name: name, type: DataType.integer, size: 4, value: u32(v));
ScriptDraftValue _cMatrix(String name, List<int> bytes) =>
    ScriptDraftValue(name: name, type: DataType.matrix, value: Uint8List.fromList(bytes));
ScriptDraftValue _var(String name, DataType type, int size) =>
    ScriptDraftValue(name: name, type: type, size: size);

int _props() => ScriptProperties.loadOnBoot | ScriptProperties.runOnLoad;

/// Script 1: temperature -> fan duty (average of both DAS NTCs).
ScriptDraft scriptTemperature() {
  final d = ScriptDraft(functionName: 'Temperature regulation', properties: _props());
  d.constants.addAll([
    _cBlockInfo('TEMP_A', bi(BlockType.dynamic.value, dynSubscriptions, fTempA, 0)),
    _cBlockInfo('TEMP_B', bi(BlockType.dynamic.value, dynSubscriptions, fTempB, 0)),
    _cBlockInfo('FAN_DUTY', bi(BlockType.pwm.value, fanInst, 1, 0)),
    _cNum('T_MIN', tempMin),
    _cNum('DUTY_MAX', 100),
    _cNum('T_SPAN', tempMax - tempMin),
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
    _line([_v(2)], _ins(catMath, 1), [_v(0), _v(1)]), // avg = tempA + tempB
    _line([_v(2)], _ins(catMath, 4), [_v(2), _idx(2)]), // avg /= 2
    _line([_v(3)], _ins(catMath, 2), [_v(2), _c(3)]), // duty = avg - T_MIN
    _line([_v(3)], _ins(catMath, 3), [_v(3), _c(4)]), // duty *= DUTY_MAX
    _line([_v(3)], _ins(catMath, 4), [_v(3), _c(5)]), // duty /= T_SPAN
    _line([_v(3)], _ins(catMath, 7), [_v(3), _idx(0)]), // duty = Max(duty, 0)
    _line([_v(3)], _ins(catMath, 6), [_v(3), _c(4)]), // duty = Min(duty, DUTY_MAX)
    _line([], _ins(catService, 2), [_c(2), _v(3)]), // write[FAN_DUTY] = duty
    _line([], _ins(catTime, 0), [_c(6)]), // Delay PERIOD
    _line([], _ins(catFlow, 2)), // EndBlock
  ]);
  return d;
}

/// Script 4: LDR -> display brightness (each display uses its own DAS LDR).
ScriptDraft scriptBrightness() {
  final d = ScriptDraft(functionName: 'Brightness regulation', properties: _props());
  d.constants.addAll([
    _cBlockInfo('SUB_LUX_A', bi(BlockType.dynamic.value, dynSubscriptions, fLuxA, 0)),
    _cBlockInfo('SUB_LUX_B', bi(BlockType.dynamic.value, dynSubscriptions, fLuxB, 0)),
    _cBlockInfo('DISP_L_BRIGHT', bi(BlockType.vysiDisplay.value, dispLeft, 0, 0)),
    _cBlockInfo('DISP_R_BRIGHT', bi(BlockType.vysiDisplay.value, dispRight, 0, 0)),
    _cNum('BRIGHT_MAX', luxBrightMax),
    _cNum('BRIGHT_MIN', luxBrightMin),
    _cNum('LUX_SPAN', luxSpan),
    _cNum('RANGE', luxBrightMax - luxBrightMin),
    _cIx('PERIOD', 100),
  ]);
  d.variables.addAll([
    _var('luxA', DataType.number, 4),
    _var('luxB', DataType.number, 4),
    _var('bL', DataType.number, 4),
    _var('bR', DataType.number, 4),
  ]);
  // brightness = clamp(BRIGHT_MIN + lux * RANGE / LUX_SPAN, MIN, MAX): a brighter ambient
  // makes the display brighter.
  List<ScriptLine> calc(int luxVar, int outVar, int regConst) => [
        _line([_v(outVar)], _ins(catMath, 0), [_v(luxVar)]), // out = lux
        _line([_v(outVar)], _ins(catMath, 3), [_v(outVar), _c(7)]), // out *= RANGE
        _line([_v(outVar)], _ins(catMath, 4), [_v(outVar), _c(6)]), // out /= LUX_SPAN
        _line([_v(outVar)], _ins(catMath, 1), [_v(outVar), _c(5)]), // out += BRIGHT_MIN
        _line([_v(outVar)], _ins(catMath, 7), [_v(outVar), _c(5)]), // out = Max(out, MIN)
        _line([_v(outVar)], _ins(catMath, 6), [_v(outVar), _c(4)]), // out = Min(out, MAX)
        _line([], _ins(catService, 2), [_c(regConst), _v(outVar)]), // reg[bright] = out
      ];
  d.lines.addAll([
    _line([], _ins(catFlow, 1), [_true()]), // While true
    _line([_v(0)], _ins(catService, 1), [_c(0)]), // luxA = reg[SUB_LUX_A]
    _line([_v(1)], _ins(catService, 1), [_c(1)]), // luxB = reg[SUB_LUX_B]
    ...calc(1, 2, 2), // left display uses the RIGHT DAS LDR (LuxB)
    ...calc(0, 3, 3), // right display uses the LEFT DAS LDR (LuxA)
    _line([], _ins(catTime, 0), [_c(8)]), // Delay PERIOD
    _line([], _ins(catFlow, 2)), // EndBlock
  ]);
  return d;
}

/// Script 2: gyro XY -> eye iris + pupil position (both displays).
ScriptDraft scriptEyeMovement() {
  final d = ScriptDraft(functionName: 'Eye movement', properties: _props());
  d.constants.addAll([
    _cBlockInfo('GYRO', bi(BlockType.accGyr.value, 0, 6, 0)), // Angular Velocity (Vector3)
    _cBlockInfo('LEFT_IRIS', bi(BlockType.dynamic.value, dynLeftEye, eyeIrisGeo, gkPosition)),
    _cBlockInfo('LEFT_PUPIL', bi(BlockType.dynamic.value, dynLeftEye, eyePupilGeo, gkPosition)),
    _cBlockInfo('RIGHT_IRIS', bi(BlockType.dynamic.value, dynRightEye, eyeIrisGeo, gkPosition)),
    _cBlockInfo('RIGHT_PUPIL', bi(BlockType.dynamic.value, dynRightEye, eyePupilGeo, gkPosition)),
    _cMatrix('IDENT', identity23()),
    _cNum('SCALE', eyeScale),
    _cNum('LIMIT', eyeLimit),
    _cNum('NEG_LIMIT', -eyeLimit),
    _cIx('PERIOD', 30),
    // Base eye position: inward (+x on the left eye, -x on the right) and slightly up.
    _cNum('BASE_L_X', eyeBaseIn),
    _cNum('BASE_L_Y', eyeBaseUp),
    _cNum('BASE_R_X', -eyeBaseIn),
    _cNum('BASE_R_Y', eyeBaseUp),
    // Iris texture Position: keeps the fade centred on the pupil.
    _cBlockInfo('LEFT_IRIS_TEX', bi(BlockType.dynamic.value, dynLeftEye, eyeIrisTex, tkPosition)),
    _cBlockInfo('RIGHT_IRIS_TEX', bi(BlockType.dynamic.value, dynRightEye, eyeIrisTex, tkPosition)),
  ]);
  d.variables.addAll([
    _var('offset', DataType.vector, 12), // gyro scaled + clamped as one vector op
    _var('gx', DataType.number, 4),
    _var('gy', DataType.number, 4),
    _var('halfX', DataType.number, 4),
    _var('halfY', DataType.number, 4),
    _var('mat', DataType.matrix, 28), // reused 2x3 position matrix
    _var('posX', DataType.number, 4),
    _var('posY', DataType.number, 4),
  ]);

  /// mat = IDENT with translation (xVar, yVar); written to the geometry Position and, for
  /// the iris, also to the texture Position (so the fade stays centred on the pupil).
  List<ScriptLine> place(int xVar, int yVar, int regConst, {int? texConst}) => [
        _line([_v(5)], _ins(catMath, 0), [_c(5)]), // mat = IDENT
        _line([_v(5)], _ins(catCompose, 0), [_idx(2), _v(xVar)]), // mat[0,2] = x
        _line([_v(5)], _ins(catCompose, 0), [_idx(5), _v(yVar)]), // mat[1,2] = y
        _line([], _ins(catService, 2), [_c(regConst), _v(5)]), // write[reg] = mat
        if (texConst != null)
          _line([], _ins(catService, 2), [_c(texConst), _v(5)]),
      ];

  /// posX/posY = source (srcX, srcY) + the base offset constants.
  List<ScriptLine> at(int srcX, int srcY, int baseXConst, int baseYConst) => [
        _line([_v(6)], _ins(catMath, 0), [_v(srcX)]), // posX = srcX
        _line([_v(6)], _ins(catMath, 1), [_v(6), _c(baseXConst)]), // posX += baseX
        _line([_v(7)], _ins(catMath, 0), [_v(srcY)]), // posY = srcY
        _line([_v(7)], _ins(catMath, 1), [_v(7), _c(baseYConst)]), // posY += baseY
      ];

  d.lines.addAll([
    _line([], _ins(catFlow, 1), [_true()]), // While true
    _line([_v(0)], _ins(catService, 1), [_c(0)]), // offset = read[GYRO] (vector)
    _line([_v(0)], _ins(catMath, 3), [_v(0), _c(6)]), // offset *= SCALE   (element-wise)
    _line([_v(0)], _ins(catMath, 7), [_v(0), _c(8)]), // offset = Max(offset, -LIMIT)
    _line([_v(0)], _ins(catMath, 6), [_v(0), _c(7)]), // offset = Min(offset, LIMIT)
    _line([_v(1)], _ins(catCompose, 1), [_v(0), _idx(0)]), // gx = offset[0]
    _line([_v(2)], _ins(catCompose, 1), [_v(0), _idx(1)]), // gy = offset[1]
    _line([_v(3)], _ins(catMath, 0), [_v(1)]), // halfX = gx
    _line([_v(3)], _ins(catMath, 4), [_v(3), _idx(2)]), // halfX /= 2
    _line([_v(4)], _ins(catMath, 0), [_v(2)]), // halfY = gy
    _line([_v(4)], _ins(catMath, 4), [_v(4), _idx(2)]), // halfY /= 2
    ...at(3, 4, 10, 11), ...place(6, 7, 1), // left iris (half offset)
    ...at(1, 2, 10, 11), ...place(6, 7, 2, texConst: 14), // left pupil + iris fade
    ...at(3, 4, 12, 13), ...place(6, 7, 3), // right iris
    ...at(1, 2, 12, 13), ...place(6, 7, 4, texConst: 15), // right pupil + iris fade
    _line([], _ins(catTime, 0), [_c(9)]), // Delay PERIOD
    _line([], _ins(catFlow, 2)), // EndBlock
  ]);
  return d;
}

/// Script 3: blink. While blinking the lid is updated every loop tick (no artificial delay);
/// once the movement finishes (close + open over [lidMoveMs] each) the script waits exactly
/// [lidWaitMs] for the next blink.
ScriptDraft scriptLidTimer() {
  final d = ScriptDraft(functionName: 'Lid timer', properties: _props());
  final blinkMs = 2 * lidMoveMs; // close + open
  d.constants.addAll([
    _cBlockInfo('LID_L', bi(BlockType.dynamic.value, dynLeftEye, eyeLidGeo, gkPosition)),
    _cBlockInfo('LID_R', bi(BlockType.dynamic.value, dynRightEye, eyeLidGeo, gkPosition)),
    _cMatrix('IDENT', identity23()),
    _cNum('OPEN_TY', lidOpenTy),
    _cNum('DELTA', lidClosedTy - lidOpenTy),
    _cIx('MOVE_MS', lidMoveMs),
    _cIx('BLINK_MS', blinkMs),
    _cIx('WAIT_MS', lidWaitMs),
  ]);
  d.variables.addAll([
    _var('t0', DataType.number, 4),
    _var('now', DataType.number, 4),
    _var('elapsed', DataType.number, 4),
    _var('closeP', DataType.number, 4),
    _var('openP', DataType.number, 4),
    _var('ty', DataType.number, 4),
    _var('mat', DataType.matrix, 28),
    _var('cond', DataType.bool_, 1),
  ]);

  /// mat = IDENT with translation ty, written to the lid geometry field.
  List<ScriptLine> applyLid(int regConst) => [
        _line([_v(6)], _ins(catMath, 0), [_c(2)]), // mat = IDENT
        _line([_v(6)], _ins(catCompose, 0), [_idx(5), _v(5)]), // mat[1,2] = ty
        _line([], _ins(catService, 2), [_c(regConst), _v(6)]), // write[reg] = mat
      ];

  d.lines.addAll([
    _line([], _ins(catFlow, 1), [_true()]), // While true
    _line([_v(0)], _ins(catTime, 2)), // t0 = Get time
    _line([_v(7)], _ins(catMath, 0), [_true()]), // cond = true
    _line([], _ins(catFlow, 1), [_v(7)]), // While cond  (blink: no delay -> fastest update)
    _line([_v(1)], _ins(catTime, 2)), // now = Get time
    _line([_v(2)], _ins(catMath, 2), [_v(1), _v(0)]), // elapsed = now - t0
    // closeP = Limit(elapsed / MOVE_MS, 0, 1)
    _line([_v(3)], _ins(catMath, 4), [_v(2), _c(5)]), // closeP = elapsed / MOVE_MS
    _line([_v(3)], _ins(catMath, 10), [_v(3), _idx(0), _idx(1)]), // closeP = Limit(closeP,0,1)
    // openP = Limit((elapsed - MOVE_MS) / MOVE_MS, 0, 1)
    _line([_v(4)], _ins(catMath, 2), [_v(2), _c(5)]), // openP = elapsed - MOVE_MS
    _line([_v(4)], _ins(catMath, 4), [_v(4), _c(5)]), // openP /= MOVE_MS
    _line([_v(4)], _ins(catMath, 10), [_v(4), _idx(0), _idx(1)]), // openP = Limit(openP,0,1)
    // ty = OPEN_TY + DELTA * (closeP - openP)
    _line([_v(5)], _ins(catMath, 2), [_v(3), _v(4)]), // ty = closeP - openP
    _line([_v(5)], _ins(catMath, 3), [_v(5), _c(4)]), // ty *= DELTA
    _line([_v(5)], _ins(catMath, 1), [_v(5), _c(3)]), // ty += OPEN_TY
    ...applyLid(0),
    ...applyLid(1),
    // Recompute `cond` INSIDE the loop (the While re-reads its operand).
    _line([_v(7)], _ins(catLogic, 8), [_v(2), _c(6)]), // cond = elapsed < BLINK_MS
    _line([], _ins(catFlow, 2)), // EndBlock
    // Movement finished: park the lid open and wait exactly WAIT_MS for the next blink.
    _line([_v(5)], _ins(catMath, 0), [_c(3)]), // ty = OPEN_TY
    ...applyLid(0),
    ...applyLid(1),
    _line([], _ins(catTime, 0), [_c(7)]), // Delay WAIT_MS
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
  await clampBrightness(reg, 5);

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
  return found;
}
