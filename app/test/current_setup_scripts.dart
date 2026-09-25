part of 'current_setup.dart';

ScriptSymbol _c(int i) => ScriptSymbol.constant(i);
ScriptSymbol _v(int i) => ScriptSymbol.variable(i);
ScriptSymbol _in(int i) => ScriptSymbol.input(i); // a script input read as an operand
ScriptSymbol _out(int i) => ScriptSymbol.output(i); // a script output (assignment destination)
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
ScriptDraftValue _cEnum(String name, int v) =>
    ScriptDraftValue(name: name, type: DataType.enum_, size: 1, value: Uint8List.fromList([v & 0xFF]));
ScriptDraftValue _cBool(String name, bool v) =>
    ScriptDraftValue(name: name, type: DataType.bool_, size: 1, value: Uint8List.fromList([v ? 1 : 0]));
ScriptDraftValue _cVector2(String name, double x, double y) => ScriptDraftValue(
    name: name, type: DataType.vector, value: Uint8List.fromList([...num(x), ...num(y)]));
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

/// Enum input with named choices ("custom enum"): the value stays the integer index and the
/// labels travel in the script's UI info, so the app renders a dropdown.
ScriptDraftValue _inEnum(String name, int v, List<String> options) => ScriptDraftValue(
    name: name,
    type: DataType.enum_,
    size: 1,
    value: Uint8List.fromList([v & 0xFF]),
    spec: ScriptInputSpec(uiType: ScriptUiType.dropdown, options: options));

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
    _cNum('LUX_MIN', luxMin), // 26: below this the brightness stays at the floor
    _cNum('LUX_SPAN_ABOVE_MIN', luxSpan - luxMin), // 27
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
    _var('luxT', DataType.number, 4), // 8: clamped lux, reused by both eyes
  ]);

  // Brightness = MIN + RANGE * ((Max(lux, LUX_MIN) - LUX_MIN) / SPAN_ABOVE_MIN)^EXP, clamped
  // to [MIN, MAX]. Flat at the floor below LUX_MIN, then a power law through the documented
  // 100 lux -> 10 % and 3000 lux -> 40 % anchors (cap from LUX_SPAN up).
  List<ScriptLine> brightness(int luxVar, int outVar, int regConst) => [
        // luxT = Max(lux, LUX_MIN)
        _line([_v(brightLuxVar)], _ins(catMath, 7), [_v(luxVar), _c(26)]),
        _line([_v(outVar)], _ins(catMath, 0), [
          _c(5), _op(0), _c(7), _op(2), // MIN + RANGE *
          _op(mathOpOpenParen),
          _op(mathOpOpenParen),
          _op(mathOpOpenParen), _v(brightLuxVar), _op(1), _c(26), _op(mathOpCloseParen), // (luxT - LUX_MIN)
          _op(3), _c(27), // / SPAN_ABOVE_MIN
          _op(mathOpCloseParen), // ... so the power applies to the quotient, not the divisor
          _op(5), _preNum(brightExpA), // ^ EXP
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

/// Script 2: gyro angular velocity -> pupil offset, published as outputs.
///
/// Input 0 = base offset (Vector2; x is flipped for the right eye),
/// Input 1 = sensitivity (Matrix 2x3, a plain XYZ -> XY linear map: the movement is
/// (m0,m1,m2)·gyro and (m3,m4,m5)·gyro, "not a transformation").
///
/// Output 0/1 = the pupil transform (2x3, translation only) for the left/right eye. The emote
/// script owns the eye render block and consumes these in the SAME main-loop pass (it is
/// loaded into a higher slot), so the pupil follows the calculation without a loop of delay.
ScriptDraft scriptEyeMovement() {
  final d = ScriptDraft(functionName: 'Eye movement', properties: _props());
  d.inputs.addAll([
    _inVector2('Offset', eyeOffsetX, eyeOffsetY),
    _inMatrix23('Sensitivity', identity23()), // default [1 0 0; 0 1 0]: 1 px per rad/s
  ]);
  d.outputs.addAll([
    ScriptDraftValue(name: 'offset L', type: DataType.matrix, size: 28), // 0
    ScriptDraftValue(name: 'offset R', type: DataType.matrix, size: 28), // 1
  ]);
  d.constants.addAll([
    _cBlockInfo('GYRO', bi(BlockType.accGyr.value, 0, 6, 0)), // Angular Velocity (Vector3)
    _cNum('LIMIT', eyeLimit), // 1
    _cNum('NEG_LIMIT', -eyeLimit), // 2
    _cIx('PERIOD', eyeMovePeriod), // 3
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
  ]);

  /// out[idx] = Transform(0, px, py, 1, 1): the transform the eye geometry needs, written
  /// straight into the output (a matrix-typed destination).
  List<ScriptLine> emit(int idx) => [
        _line([_out(idx)], _ins(catMath, 11), [_idx(0), _v(14), _v(15), _idx(1), _idx(1)]),
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
    _line([_v(10)], _ins(catMath, 10), [_v(10), _c(2), _c(1)]), // mx = Limit(mx, -LIMIT, LIMIT)
    _line([_v(11)], _ins(catMath, 10), [_v(11), _c(2), _c(1)]), // my = Limit(my, -LIMIT, LIMIT)
    _line([_v(12)], _ins(catCompose, 1), [_in(0), _idx(0)]), // offX = offset[0]
    _line([_v(13)], _ins(catCompose, 1), [_in(0), _idx(1)]), // offY = offset[1]
    ...at(10, 11), ...emit(0), // left
    ...at(10, 11, flipX: true), ...emit(1), // right (x mirrored)
    _line([], _ins(catTime, 0), [_c(3)]), // Delay PERIOD
    _line([], _ins(catFlow, 2)), // EndBlock
  ]);
  return d;
}

/// Script 3: blink. While blinking the lid is updated every loop tick (no artificial delay);
/// once the movement finishes (close + open over Input 1 each) the script waits Input 0 for
/// the next blink.
///
/// Script 3: blink. The lid closes, holds while a forced close is requested (the emote script
/// swaps the pupil then), opens to its max-opening position and waits for the next blink.
///
/// Input 0 = delay between blinks (ms, default 10 s), Input 1 = movement time each way (ms),
/// Input 2 = force close (hold the lid shut while set), Input 3 = max opening (0..1).
ScriptDraft scriptLidTimer() {
  final d = ScriptDraft(functionName: 'Lid timer', properties: _props());
  d.inputs.addAll([
    _inIx('Blink delay', lidWaitMs, min: 1000, max: 60000, step: 1000, ui: ScriptUiType.slider),
    _inIx('Movement time', lidMoveMs, min: 50, max: 1000, step: 50, ui: ScriptUiType.slider),
    _inBool('Force close', false),
    _inNum('Max opening', 1, min: 0, max: 1, step: 0.05, ui: ScriptUiType.slider),
  ]);
  d.constants.addAll([
    _cBlockInfo('LID_L', bi(BlockType.dynamic.value, dynLeftEye, eyeLidGeo, gkPosition)),
    _cBlockInfo('LID_R', bi(BlockType.dynamic.value, dynRightEye, eyeLidGeo, gkPosition)),
    _cNum('OPEN_TY', lidOpenTy), // fully open
    _cNum('DELTA', lidClosedTy - lidOpenTy),
    _cNum('CLOSED_TY', lidClosedTy), // fully closed
  ]);
  d.variables.addAll([
    // Get time yields a whole millisecond count, so its destinations are integers (Index):
    // a Q16.16 Number would overflow the absolute count past ~32767 ms.
    _var('t0', DataType.integer, 4), // 0
    _var('now', DataType.integer, 4), // 1
    _var('elapsed', DataType.number, 4), // 2
    _var('p', DataType.number, 4), // 3 ramp 0..1
    _var('restTy', DataType.number, 4), // 4 open position for the current max opening
    _var('ty', DataType.number, 4), // 5
    _var('mat', DataType.matrix, 28), // 6
    _var('newRest', DataType.number, 4), // 7 resting position recomputed while waiting
  ]);

  /// mat = Transform(0, 0, ty, 1, 1) written to the lid geometry field.
  List<ScriptLine> applyLid(int regConst) => [
        _line([_v(6)], _ins(catMath, 11), [_idx(0), _idx(0), _v(5), _idx(1), _idx(1)]), // Transform mat = 0, 0, ty, 1, 1
        _line([], _ins(catService, 2), [_c(regConst), _v(6)]), // write[reg] = mat
      ];
  List<ScriptLine> applyBoth() => [...applyLid(0), ...applyLid(1)];

  d.lines.addAll([
    _line([], _ins(catFlow, 1), [_true()]), // While true
    // Open position for the current max opening: restTy = OPEN_TY + (1 - MAX_OPEN) * DELTA.
    _line([_v(4)], _ins(catMath, 0), [
      _c(2), _op(0), _op(mathOpOpenParen), _idx(1), _op(1), _in(3), _op(mathOpCloseParen),
      _op(2), _c(3),
    ]),
    // 1) Wait the blink delay, ending early when a forced close appears.
    _line([_v(0)], _ins(catTime, 2)), // t0 = Get time
    _line([_v(1)], _ins(catTime, 2)), // now = Get time
    _line([], _ins(catFlow, 1), [
      _op(mathOpOpenParen), _v(1), _op(1), _v(0), _op(mathOpCloseParen), _op(14), _in(0),
      _op(6), _in(2), _op(12), _idx(0),
    ]), // While (now - t0) < DELAY and FORCE == 0
    _line([], _ins(catTime, 0), [_idx(1)]), // Delay 1 (yield; keeps the other scripts serviced)
    _line([_v(1)], _ins(catTime, 2)), // now = Get time
    // The resting position may change while the lid waits (emote switch that only moves the
    // lid, e.g. Normal <-> Annoyed): follow it live instead of waiting for the next blink.
    _line([_v(7)], _ins(catMath, 0), [
      _c(2), _op(0), _op(mathOpOpenParen), _idx(1), _op(1), _in(3), _op(mathOpCloseParen),
      _op(2), _c(3),
    ]), // newRest = OPEN_TY + (1 - MAX_OPEN) * DELTA
    _line([], _ins(catFlow, 0), [_v(7), _op(13), _v(5)]), // If newRest != ty
    _line([_v(5)], _ins(catMath, 0), [_v(7)]), // ty = newRest
    _line([_v(4)], _ins(catMath, 0), [_v(7)]), // restTy = newRest
    ...applyBoth(),
    _line([], _ins(catFlow, 2)), // EndBlock
    _line([], _ins(catFlow, 2)), // EndBlock
    // 2) Close ramp.
    _line([_v(0)], _ins(catTime, 2)), // t0 = Get time
    _line([_v(1)], _ins(catTime, 2)), // now = Get time
    _line([], _ins(catFlow, 1), [
      _op(mathOpOpenParen), _v(1), _op(1), _v(0), _op(mathOpCloseParen), _op(14), _in(1),
    ]), // While (now - t0) < MOVE
    _line([_v(2)], _ins(catMath, 0), [_v(1), _op(1), _v(0)]), // elapsed = now - t0
    _line([_v(3)], _ins(catMath, 0), [_v(2), _op(3), _in(1)]), // p = elapsed / MOVE
    _line([_v(3)], _ins(catMath, 10), [_v(3), _idx(0), _idx(1)]), // p = Limit(p, 0, 1)
    _line([_v(5)], _ins(catMath, 0), [
      _v(4), _op(0), _v(3), _op(2), _op(mathOpOpenParen), _c(4), _op(1), _v(4),
      _op(mathOpCloseParen),
    ]), // ty = restTy + p * (CLOSED_TY - restTy)
    ...applyBoth(),
    _line([_v(1)], _ins(catTime, 2)), // now = Get time (the While re-reads the condition)
    _line([], _ins(catFlow, 2)), // EndBlock
    // 3) Hold shut while Force close is set (the emote script swaps the pupil now).
    _line([_v(5)], _ins(catMath, 0), [_c(4)]), // ty = CLOSED_TY
    ...applyBoth(),
    _line([], _ins(catFlow, 1), [_in(2), _op(13), _idx(0)]), // While FORCE != 0
    _line([], _ins(catTime, 0), [_idx(1)]), // Delay 1
    _line([], _ins(catFlow, 2)), // EndBlock
    // The emote script may have changed Max opening while the lid was held shut: re-read it so
    // the open ramp targets the new resting position immediately.
    _line([_v(4)], _ins(catMath, 0), [
      _c(2), _op(0), _op(mathOpOpenParen), _idx(1), _op(1), _in(3), _op(mathOpCloseParen),
      _op(2), _c(3),
    ]), // restTy = OPEN_TY + (1 - MAX_OPEN) * DELTA
    // 4) Open ramp to the (possibly new) resting position.
    _line([_v(0)], _ins(catTime, 2)), // t0 = Get time
    _line([_v(1)], _ins(catTime, 2)), // now = Get time
    _line([], _ins(catFlow, 1), [
      _op(mathOpOpenParen), _v(1), _op(1), _v(0), _op(mathOpCloseParen), _op(14), _in(1),
    ]), // While (now - t0) < MOVE
    _line([_v(2)], _ins(catMath, 0), [_v(1), _op(1), _v(0)]), // elapsed = now - t0
    _line([_v(3)], _ins(catMath, 0), [_v(2), _op(3), _in(1)]), // p = elapsed / MOVE
    _line([_v(3)], _ins(catMath, 10), [_v(3), _idx(0), _idx(1)]), // p = Limit(p, 0, 1)
    _line([_v(5)], _ins(catMath, 0), [
      _c(4), _op(0), _v(3), _op(2), _op(mathOpOpenParen), _v(4), _op(1), _c(4),
      _op(mathOpCloseParen),
    ]), // ty = CLOSED_TY + p * (restTy - CLOSED_TY)
    ...applyBoth(),
    _line([_v(1)], _ins(catTime, 2)), // now = Get time (the While re-reads the condition)
    _line([], _ins(catFlow, 2)), // EndBlock
    // Park at the resting position so the lid is unambiguously open between blinks.
    _line([_v(5)], _ins(catMath, 0), [_v(4)]), // ty = restTy
    ...applyBoth(),
    _line([], _ins(catFlow, 2)), // EndBlock (While true)
  ]);
  return d;
}

/// Script 5: emote selector and eye renderer.
///
/// Owns the eye geometry: it consumes the pupil offsets published by script 2 and writes the
/// Script 5: emote selector and eye renderer.
///
/// Owns the eye geometry: it consumes the pupil offsets published by script 2 and writes the
/// iris, iris fade and both pupil positions, plus the per-emote pupil shapes. It is loaded
/// into a HIGHER slot than script 2, so it reads that pass's fresh offset and the displays
/// render in the same pass (no cross-script delay).
///
/// A change that alters the pupil SHAPE (Normal/Annoyed <-> Happy/Dead) is applied behind a
/// forced blink: set the lid's Force close, wait until it is shut (or a timeout), swap,
/// release. A change that only moves the lid (Normal <-> Annoyed) is applied at once.
ScriptDraft scriptEmoteSelector() {
  final d = ScriptDraft(functionName: 'Emote selector', properties: _props());
  d.inputs.addAll([
    _inEnum('Emote', emoteNormal, emoteNames),
  ]);

  final consts = <ScriptDraftValue>[];
  int add(ScriptDraftValue v) {
    consts.add(v);
    return consts.length - 1;
  }

  // Script 2's outputs (the pupil transforms it publishes).
  final offL = add(_cBlockInfo('OFF_L', bi(BlockType.script.value, scrEyeMovement, ScriptField.output, 0)));
  final offR = add(_cBlockInfo('OFF_R', bi(BlockType.script.value, scrEyeMovement, ScriptField.output, 1)));
  // Position keys (written whenever the offset or an emote offset changes).
  final lIris = add(_cBlockInfo('L_IRIS', bi(BlockType.dynamic.value, dynLeftEye, eyeIrisGeo, gkPosition)));
  final lIrisTex = add(_cBlockInfo('L_IRIS_TEX', bi(BlockType.dynamic.value, dynLeftEye, eyeIrisTex, tkPosition)));
  final lPupaPos = add(_cBlockInfo('L_PUPA_POS', bi(BlockType.dynamic.value, dynLeftEye, eyePupilGeoA, gkPosition)));
  final lPupbPos = add(_cBlockInfo('L_PUPB_POS', bi(BlockType.dynamic.value, dynLeftEye, eyePupilGeoB, gkPosition)));
  final rIris = add(_cBlockInfo('R_IRIS', bi(BlockType.dynamic.value, dynRightEye, eyeIrisGeo, gkPosition)));
  final rIrisTex = add(_cBlockInfo('R_IRIS_TEX', bi(BlockType.dynamic.value, dynRightEye, eyeIrisTex, tkPosition)));
  final rPupaPos = add(_cBlockInfo('R_PUPA_POS', bi(BlockType.dynamic.value, dynRightEye, eyePupilGeoA, gkPosition)));
  final rPupbPos = add(_cBlockInfo('R_PUPB_POS', bi(BlockType.dynamic.value, dynRightEye, eyePupilGeoB, gkPosition)));

  /// The 5 control keys of one pupil geometry (eye block, field).
  List<int> pupilKeys(int block, int field) => [
        add(_cBlockInfo('SHAPE', bi(BlockType.dynamic.value, block, field, gkShape))),
        add(_cBlockInfo('OP', bi(BlockType.dynamic.value, block, field, gkOperation))),
        add(_cBlockInfo('SIZE', bi(BlockType.dynamic.value, block, field, gkSize))),
        add(_cBlockInfo('FADE', bi(BlockType.dynamic.value, block, field, gkFade))),
        add(_cBlockInfo('ANG', bi(BlockType.dynamic.value, block, field, gkAngles))),
      ];
  final lA = pupilKeys(dynLeftEye, eyePupilGeoA);
  final lB = pupilKeys(dynLeftEye, eyePupilGeoB);
  final rA = pupilKeys(dynRightEye, eyePupilGeoA);
  final rB = pupilKeys(dynRightEye, eyePupilGeoB);
  // Lid handshake (script 3 inputs + the lid position, to know when it is shut).
  final lidForce = add(_cBlockInfo('LID_FORCE', bi(BlockType.script.value, scrLidTimer, ScriptField.input, 2)));
  final lidMaxOpen = add(_cBlockInfo('LID_MAXOPEN', bi(BlockType.script.value, scrLidTimer, ScriptField.input, 3)));
  final lidPos = add(_cBlockInfo('LID_L', bi(BlockType.dynamic.value, dynLeftEye, eyeLidGeo, gkPosition)));
  // Values.
  final vZero = add(_cNum('ZERO', 0));
  final vOne = add(_cNum('ONE', 1));
  final vTilt = add(_cNum('TILT', emoteTiltDead)); // +45 deg (radians)
  final vClosed = add(_cNum('CLOSED_TY', lidClosedTy));
  final vClosedSlack = add(_cNum('CLOSED_SLACK', 1.0));
  final vOpenFull = add(_cNum('OPEN_FULL', 1));
  final vOpenAnnoyed = add(_cNum('OPEN_ANNOYED', lidAnnoyedOpen));
  final vTimeout = add(_cIx('TIMEOUT', 2000));
  final vShapeNone = add(_cEnum('SHAPE_NONE', shapeNone));
  final vShapeParabola = add(_cEnum('SHAPE_PARABOLA', shapeDoubleParabola));
  final vShapeTriangle = add(_cEnum('SHAPE_TRIANGLE', shapeTriangle));
  final vShapeRect = add(_cEnum('SHAPE_RECT', shapeRectangle));
  final vOpReplace = add(_cEnum('OP_REPLACE', opReplace));
  final vOpAdd = add(_cEnum('OP_ADD', opAdd));
  final vOpCut = add(_cEnum('OP_CUT', opCut));
  final vSizeNormal = add(_cVector2('SIZE_NORMAL', pupilHalfW, pupilHalfH));
  final vSizeHappyA = add(_cVector2('SIZE_HAPPY_A', happyOuterSide, happyOuterSide));
  final vSizeHappyB = add(_cVector2('SIZE_HAPPY_B', happyInnerSide, happyInnerSide));
  final vSizeDead = add(_cVector2('SIZE_DEAD', deadBarWidth, deadBarLength));
  final vFadePupil = add(_cNum('FADE_PUPIL', pupilFade));
  final vFadeEmote = add(_cNum('FADE_EMOTE', emotePupilFade));
  final vAngleZero = add(_cNum('ANGLE_ZERO', 0));
  final vAngleHappy = add(_cNum('ANGLE_HAPPY', happyApexAngle));
  final vOffHappy = add(_cNum('OFFSET_HAPPY', happyOffsetY)); // the whole happy pupil sits higher
  final vCutHappy = add(_cNum('CUT_OFFSET_HAPPY', happyCutOffsetY)); // the happy cut slides down
  final vTrue = add(_cBool('TRUE', true));
  final vFalse = add(_cBool('FALSE', false));
  final vEps = add(_cNum('EPS', 0.05)); // px movement deadzone for the write-on-change check
  d.constants.addAll(consts);

  d.variables.addAll([
    _var('matL', DataType.matrix, 28), // 0
    _var('matR', DataType.matrix, 28), // 1
    _var('txL', DataType.number, 4), // 2
    _var('tyL', DataType.number, 4), // 3
    _var('txR', DataType.number, 4), // 4
    _var('tyR', DataType.number, 4), // 5
    _var('lastXL', DataType.number, 4), // 6
    _var('lastYL', DataType.number, 4), // 7
    _var('lastXR', DataType.number, 4), // 8
    _var('lastYR', DataType.number, 4), // 9
    _var('tilt', DataType.number, 4), // 10 pupil rotation (radians)
    _var('current', DataType.number, 4), // 11 applied emote
    _var('state', DataType.number, 4), // 12 0 idle, 1 applying
    _var('t0', DataType.integer, 4), // 13
    _var('now', DataType.integer, 4), // 14
    _var('lidMat', DataType.matrix, 28), // 15
    _var('lidTy', DataType.number, 4), // 16
    _var('e', DataType.number, 4), // 17 requested emote
    _var('mat', DataType.matrix, 28), // 18 scratch transform
    _var('negTilt', DataType.number, 4), // 19 -tilt for pupil part B
    _var('movedL', DataType.number, 4), // 20
    _var('movedR', DataType.number, 4), // 21
    _var('emoteOff', DataType.number, 4), // 22 per-emote pupil Y offset
    _var('cutY', DataType.number, 4), // 23 per-emote pupil B Y offset
    _var('tyP', DataType.number, 4), // 24 pupil Y (ty + emoteOff)
    _var('tyB', DataType.number, 4), // 25 pupil B Y (tyP + cutY)
    _var('blinkWanted', DataType.number, 4), // 26 a shape change needs the blink handshake
    _var('newClass', DataType.number, 4), // 27 shape class of the requested emote
    _var('currentClass', DataType.number, 4), // 28 shape class of the applied emote
  ]);

  /// transform = Transform(rot, tx, ty, 1, 1) into the scratch matrix.
  ScriptLine transform(ScriptSymbol rot, int txVar, int tyVar) => _line(
      [_v(18)], _ins(catMath, 11), [rot, _v(txVar), _v(tyVar), _idx(1), _idx(1)]);

  /// Writes the 5 pupil keys of one geometry.
  List<ScriptLine> setPupil(List<int> k, int shapeV, int opV, int sizeV, int fadeV, int angV) => [
        for (final (key, value) in [
          (k[0], shapeV),
          (k[1], opV),
          (k[2], sizeV),
          (k[3], fadeV),
          (k[4], angV),
        ])
          _line([], _ins(catService, 2), [_c(key), _c(value)]),
      ];

  /// Writes the scratch matrix into the given position key.
  ScriptLine writePos(int key) => _line([], _ins(catService, 2), [_c(key), _v(18)]);

  /// Places one eye: the iris + fade at (tx, ty), pupil A at (tx, ty + emoteOff) tilted +tilt
  /// and pupil B at (tx, ty + emoteOff + cutY) tilted -tilt.
  List<ScriptLine> placeEye(int txVar, int tyVar, int irisK, int irisTexK, int pupaK, int pupbK) => [
        transform(_c(vZero), txVar, tyVar), writePos(irisK),
        writePos(irisTexK), // the same matrix, no rebuild
        _line([_v(24)], _ins(catMath, 0), [_v(tyVar), _op(0), _v(22)]), // tyP = ty + emoteOff
        transform(_v(10), txVar, 24), writePos(pupaK),
        _line([_v(25)], _ins(catMath, 0), [_v(24), _op(0), _v(23)]), // tyB = tyP + cutY
        transform(_v(19), txVar, 25), writePos(pupbK),
      ];

  /// movedX = |value - last| > EPS for both components of one eye (manual absolute value).
  ScriptLine moved(int dst, int txVar, int tyVar, int lastX, int lastY) => _line([_v(dst)], _ins(catMath, 0), [
        _op(mathOpOpenParen), _v(txVar), _op(1), _v(lastX), _op(mathOpCloseParen), _op(16), _c(vEps),
        _op(7),
        _op(mathOpOpenParen), _v(lastX), _op(1), _v(txVar), _op(mathOpCloseParen), _op(16), _c(vEps),
        _op(7),
        _op(mathOpOpenParen), _v(tyVar), _op(1), _v(lastY), _op(mathOpCloseParen), _op(16), _c(vEps),
        _op(7),
        _op(mathOpOpenParen), _v(lastY), _op(1), _v(tyVar), _op(mathOpCloseParen), _op(16), _c(vEps),
      ]);

  /// Applies the pupil geometry for the requested emote in both eyes, plus the per-emote pupil
  /// offset (emoteOff), the cut shift (cutY) and the tilt.
  List<ScriptLine> applyEmote() => [
        _line([_v(22)], _ins(catMath, 0), [_c(vZero)]), // emoteOff = 0
        _line([_v(23)], _ins(catMath, 0), [_c(vZero)]), // cutY = 0
        _line([_v(10)], _ins(catMath, 0), [_c(vZero)]), // tilt = 0
        // Happy: a triangle with a smaller triangle cut from it, the cut pushed down so only
        // the two upper edges remain (a caret). The whole pupil sits a little higher.
        _line([], _ins(catFlow, 0), [_v(17), _op(12), _idx(emoteHappy)]),
        _line([_v(22)], _ins(catMath, 0), [_c(vOffHappy)]),
        _line([_v(23)], _ins(catMath, 0), [_c(vCutHappy)]),
        ...setPupil(lA, vShapeTriangle, vOpReplace, vSizeHappyA, vFadeEmote, vAngleHappy),
        ...setPupil(lB, vShapeTriangle, vOpCut, vSizeHappyB, vFadeEmote, vAngleHappy),
        ...setPupil(rA, vShapeTriangle, vOpReplace, vSizeHappyA, vFadeEmote, vAngleHappy),
        ...setPupil(rB, vShapeTriangle, vOpCut, vSizeHappyB, vFadeEmote, vAngleHappy),
        _line([], _ins(catFlow, 2)),
        // Dead: two bars added together, tilted opposite ways about their common centre.
        _line([], _ins(catFlow, 0), [_v(17), _op(12), _idx(emoteDead)]),
        _line([_v(10)], _ins(catMath, 0), [_c(vTilt)]), // tilt = +45 deg
        ...setPupil(lA, vShapeRect, vOpReplace, vSizeDead, vFadeEmote, vAngleZero),
        ...setPupil(lB, vShapeRect, vOpAdd, vSizeDead, vFadeEmote, vAngleZero),
        ...setPupil(rA, vShapeRect, vOpReplace, vSizeDead, vFadeEmote, vAngleZero),
        ...setPupil(rB, vShapeRect, vOpAdd, vSizeDead, vFadeEmote, vAngleZero),
        _line([], _ins(catFlow, 2)),
        // Normal / Annoyed: the tuned parabola, modifier unused.
        _line([], _ins(catFlow, 0), [
          _v(17), _op(13), _idx(emoteHappy), _op(6), _v(17), _op(13), _idx(emoteDead),
        ]),
        ...setPupil(lA, vShapeParabola, vOpReplace, vSizeNormal, vFadePupil, vAngleZero),
        ...setPupil(lB, vShapeNone, vOpAdd, vSizeNormal, vFadePupil, vAngleZero),
        ...setPupil(rA, vShapeParabola, vOpReplace, vSizeNormal, vFadePupil, vAngleZero),
        ...setPupil(rB, vShapeNone, vOpAdd, vSizeNormal, vFadePupil, vAngleZero),
        _line([], _ins(catFlow, 2)),
        _line([_v(19)], _ins(catMath, 0), [_preNum(0), _op(1), _v(10)]), // negTilt = 0 - tilt
        // The lid opening for the new emote.
        _line([], _ins(catService, 2), [_c(lidMaxOpen), _c(vOpenFull)]),
        _line([], _ins(catFlow, 0), [_v(17), _op(12), _idx(emoteAnnoyed)]),
        _line([], _ins(catService, 2), [_c(lidMaxOpen), _c(vOpenAnnoyed)]),
        _line([], _ins(catFlow, 2)),
        // Re-place the pupils so the new offsets and tilt take effect now.
        ...placeEye(2, 3, lIris, lIrisTex, lPupaPos, lPupbPos),
        ...placeEye(4, 5, rIris, rIrisTex, rPupaPos, rPupbPos),
      ];

  d.lines.addAll([
    _line([], _ins(catFlow, 1), [_true()]), // While true
    _line([_v(19)], _ins(catMath, 0), [_preNum(0), _op(1), _v(10)]), // negTilt = 0 - tilt
    // --- follow the offsets published by script 2 (this pass, script 2 runs first) ---
    _line([_v(0)], _ins(catService, 1), [_c(offL)]), // matL = register[OFF_L]
    _line([_v(1)], _ins(catService, 1), [_c(offR)]), // matR = register[OFF_R]
    _line([_v(2)], _ins(catCompose, 1), [_v(0), _idx(2)]), // txL = matL[2]
    _line([_v(3)], _ins(catCompose, 1), [_v(0), _idx(5)]), // tyL = matL[5]
    _line([_v(4)], _ins(catCompose, 1), [_v(1), _idx(2)]), // txR = matR[2]
    _line([_v(5)], _ins(catCompose, 1), [_v(1), _idx(5)]), // tyR = matR[5]
    moved(20, 2, 3, 6, 7), // movedL
    moved(21, 4, 5, 8, 9), // movedR
    _line([], _ins(catFlow, 0), [_v(20), _op(7), _v(21)]), // If either eye moved
    ...placeEye(2, 3, lIris, lIrisTex, lPupaPos, lPupbPos),
    ...placeEye(4, 5, rIris, rIrisTex, rPupaPos, rPupbPos),
    _line([_v(6)], _ins(catMath, 0), [_v(2)]), // lastXL = txL
    _line([_v(7)], _ins(catMath, 0), [_v(3)]), // lastYL = tyL
    _line([_v(8)], _ins(catMath, 0), [_v(4)]), // lastXR = txR
    _line([_v(9)], _ins(catMath, 0), [_v(5)]), // lastYR = tyR
    _line([], _ins(catFlow, 2)), // EndBlock
    // --- emote change ---
    _line([_v(17)], _ins(catMath, 0), [_in(0)]), // e = input[Emote]
    _line([_v(27)], _ins(catMath, 0), [_c(vZero)]), // newClass = 0 (Normal/Annoyed)
    _line([], _ins(catFlow, 0), [_v(17), _op(12), _idx(emoteHappy)]),
    _line([_v(27)], _ins(catMath, 0), [_c(vOne)]), // newClass = 1
    _line([], _ins(catFlow, 2)),
    _line([], _ins(catFlow, 0), [_v(17), _op(12), _idx(emoteDead)]),
    _line([_v(27)], _ins(catMath, 0), [_c(vOne), _op(0), _c(vOne)]), // newClass = 2
    _line([], _ins(catFlow, 2)),
    _line([], _ins(catFlow, 0), [_v(12), _op(12), _c(vZero)]), // If state == 0
    _line([], _ins(catFlow, 0), [_v(17), _op(13), _v(11)]), // If e != current
    _line([_v(26)], _ins(catMath, 0), [_c(vZero)]), // blinkWanted = 0
    // Only a pupil shape change needs the blink; Normal <-> Annoyed is a lid-only change.
    _line([], _ins(catFlow, 0), [_v(27), _op(13), _v(28)]), // If newClass != currentClass
    _line([], _ins(catService, 2), [_c(lidForce), _c(vTrue)]), // lid Force close = 1
    _line([_v(26)], _ins(catMath, 0), [_c(vOne)]), // blinkWanted = 1
    _line([], _ins(catFlow, 2)),
    _line([_v(12)], _ins(catMath, 0), [_c(vOne)]), // state = 1
    _line([_v(13)], _ins(catTime, 2)), // t0 = Get time
    _line([], _ins(catFlow, 2)), // EndBlock (e != current)
    _line([], _ins(catFlow, 2)), // EndBlock (state == 0)
    _line([], _ins(catFlow, 0), [_v(12), _op(12), _c(vOne)]), // If state == 1
    _line([_v(14)], _ins(catTime, 2)), // now = Get time
    _line([_v(15)], _ins(catService, 1), [_c(lidPos)]), // lidMat = register[LID_L]
    _line([_v(16)], _ins(catCompose, 1), [_v(15), _idx(5)]), // lidTy = lidMat[5]
    _line([], _ins(catFlow, 0), [
      // No blink wanted, or the lid is shut, or the wait timed out.
      _v(26), _op(12), _c(vZero), _op(7),
      _v(16), _op(17), _c(vClosed), _op(1), _c(vClosedSlack), _op(7),
      _op(mathOpOpenParen), _v(14), _op(1), _v(13), _op(mathOpCloseParen), _op(16), _c(vTimeout),
    ]),
    ...applyEmote(),
    _line([], _ins(catFlow, 0), [_v(26), _op(13), _c(vZero)]), // If a blink was wanted
    _line([], _ins(catService, 2), [_c(lidForce), _c(vFalse)]), // release the lid
    _line([], _ins(catFlow, 2)), // EndBlock (the blink If)
    _line([_v(26)], _ins(catMath, 0), [_c(vZero)]), // blinkWanted = 0
    _line([_v(28)], _ins(catMath, 0), [_v(27)]), // currentClass = newClass
    _line([_v(11)], _ins(catMath, 0), [_v(17)]), // current = e
    _line([_v(12)], _ins(catMath, 0), [_c(vZero)]), // state = 0
    _line([], _ins(catFlow, 2)), // EndBlock (the apply condition)
    _line([], _ins(catFlow, 2)), // EndBlock (state == 1)
    _line([], _ins(catTime, 0), [_idx(emotePeriod)]), // Delay PERIOD
    _line([], _ins(catFlow, 2)), // EndBlock (While true)
  ]);
  return d;
}
/// Uploads, loads and runs the five scripts.
