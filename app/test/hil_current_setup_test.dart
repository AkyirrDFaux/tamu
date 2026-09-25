@Tags(['hil'])
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/script_client.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/subscription_client.dart';
import 'package:tamuapp/core/types.dart';

import 'current_setup.dart';
import 'hil_helpers.dart';

void main() {
  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  late ({DeviceEntry core, List<DeviceEntry> das}) found;

  setUpAll(() async {
    if (skipReason is String) return;
    await connectHil();
    final db = DeviceDatabase.instance;
    // Bounded discovery: the DAS re-register on their own schedule after a reset.
    for (var i = 0; i < 20; i++) {
      await db.refreshRuntime(0);
      await Future<void>.delayed(const Duration(seconds: 2));
      if (db.all.where((d) => d.type == DeviceType.dualAnalogSensor).length >= 2) break;
    }
    found = findDevices(db);
    await applyCurrentSetup(db);
  });
  tearDownAll(disconnectHil);

  test('setup: dynamic blocks exist with the expected fields', skip: skipReason, () async {
    final reg = RegisterClient(deviceId: found.core.id);
    final blocks = await reg.readDynamicBlocks() ?? <DynBlock>[];
    final names = {for (final b in blocks) b.index: b.name};
    expect(names[dynSubscriptions], 'Subscriptions');
    expect(names[dynLeftEye], 'Left Eye');
    expect(names[dynRightEye], 'Right Eye');

    // Subscription targets: four numbers.
    final subFields = await reg.getDynamicFields(dynSubscriptions) ?? <int>[];
    expect(subFields, containsAll([fTempA, fLuxA, fTempB, fLuxB]));

    // Eye render dictionaries: 8 parts, key-0 markers are Geometry/Texture.
    for (final eye in [dynLeftEye, dynRightEye]) {
      final fields = await reg.getDynamicFields(eye) ?? <int>[];
      expect(fields.length, eyePartCount, reason: 'eye block $eye parts');
      final block = DynBlock(index: eye, meta: BlockMeta(flagsAndType: BlockType.dynamic.value), name: '');
      for (final pair in [
        (eyeBgGeo, DataType.geometry),
        (eyeBgTex, DataType.texture),
        (eyeIrisGeo, DataType.geometry),
        (eyePupilGeo, DataType.geometry),
        (eyeLidGeo, DataType.geometry),
      ]) {
        final head = await reg.readDynamicField(block, pair.$1, 0);
        expect(head?.meta.dataType, pair.$2, reason: 'eye $eye field ${pair.$1} marker');
      }
      // The iris gradient is one of the two documented modes. Which one depends on the
      // ambient light (the scripts are running), so the mode test below pins the switching.
      final irisC1 = await reg.readDynamicField(block, eyeIrisTex, tkColour1);
      expect(
          irisC1?.value,
          anyOf(<List<int>>[
            [irisR1, irisG1, irisB1, 255],
            [darkIrisR1, darkIrisG1, darkIrisB1, 255],
          ]),
          reason: 'eye $eye iris gradient');
      final irisC2 = await reg.readDynamicField(block, eyeIrisTex, tkColour2);
      expect(
          irisC2?.value,
          anyOf(<List<int>>[
            [irisR2, irisG2, irisB2, 255],
            [darkIrisR2, darkIrisG2, darkIrisB2, 255],
          ]),
          reason: 'eye $eye iris gradient 2');
    }
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('setup: each display keeps its own mount rotation', skip: skipReason, () async {
    // The two panels are mounted differently (~5 deg and ~175 deg) and the rotations are keyed
    // by core instance. Getting these swapped makes one eye render upside down.
    final reg = RegisterClient(deviceId: found.core.id);
    for (final (inst, expected) in [(0, dispOffsetInst0), (1, dispOffsetInst1)]) {
      final off = await reg.readBlockField(BlockType.vysiDisplay.value, inst, 1, 0);
      expect(off?.value, expected, reason: 'display $inst mount rotation');
    }
  }, timeout: const Timeout(Duration(seconds: 30)));

  test('setup: scripts expose their documented inputs', skip: skipReason, () async {
    final storage = StorageClient(deviceId: found.core.id);
    Future<ScriptFileData> file(int slot) async {
      final name = 'SCR_${slot.toString().padLeft(2, '0')}';
      final bytes = await storage.readFile(name);
      expect(bytes, isNotNull, reason: '$name present on the device');
      return ScriptFileData.parse(bytes!);
    }

    List<String> names(ScriptFileData f) =>
        [for (var i = 0; i < f.inputs.length; i++) f.nameOf(f.inputNames, i, 'Input $i')];

    expect(names(await file(scrTemperature)), ['Target temperature', 'P constant']);
    expect(names(await file(scrEyeMovement)), ['Offset', 'Sensitivity']);
    expect(names(await file(scrLidTimer)), ['Blink delay', 'Movement time']);
    expect(names(await file(scrBrightness)),
        ['Manual mode', 'Manual left dark', 'Manual right dark']);

    // The documented defaults: 10 s between blinks, 200 ms each way (integer ms inputs).
    final lid = await file(scrLidTimer);
    expect(int32FromBytes(lid.inputDefault(0)), 10000);
    expect(int32FromBytes(lid.inputDefault(1)), 200);
    // The eye inputs carry a Vector2 offset and a 2x3 sensitivity matrix.
    final eye = await file(scrEyeMovement);
    expect(eye.inputs[0].type, DataType.vector);
    expect(eye.inputs[0].size, 8);
    expect(eye.inputs[1].type, DataType.matrix);
    expect(eye.inputs[1].size, 28);
  }, timeout: const Timeout(Duration(seconds: 30)));

  test('setup: displays point at the eye render blocks', skip: skipReason, () async {
    final reg = RegisterClient(deviceId: found.core.id);
    for (final (inst, eye) in [(dispLeft, dynLeftEye), (dispRight, dynRightEye)]) {
      final rb = await reg.readBlockField(BlockType.vysiDisplay.value, inst, 2, 0);
      expect(rb, isNotNull);
      expect(int32FromBytes(rb!.value), eye, reason: 'display $inst render block');
    }
    // Both displays render the 9-part eye scene in real time (Refresh Rate, averaged FPS).
    var minFps = double.infinity;
    for (var attempt = 0; attempt < 8; attempt++) {
      await Future<void>.delayed(const Duration(milliseconds: 400));
      minFps = double.infinity;
      for (final inst in [dispLeft, dispRight]) {
        final rate = await reg.readBlockField(BlockType.vysiDisplay.value, inst, 4, 0);
        final fps = rate == null ? 0.0 : numberFromBytes(rate.value);
        if (fps < minFps) minFps = fps;
      }
      if (minFps > 200) break;
    }
    // ignore: avoid_print
    print('[SETUP] eye render RefreshRate=${minFps.toStringAsFixed(1)} FPS');
    expect(minFps, greaterThan(100), reason: 'both eye scenes must render in real time');

    // Brightness is the LDR-driven LED duty, inside the documented band.
    for (final inst in [dispLeft, dispRight]) {
      final b = await reg.readBlockField(BlockType.vysiDisplay.value, inst, 0, 0);
      final pct = b == null ? null : numberFromBytes(b.value);
      // ignore: avoid_print
      print('[SETUP] display $inst brightness=$pct %');
      expect(pct, isNotNull);
      expect(pct, inInclusiveRange(luxBrightMin, luxBrightMax),
          reason: 'display $inst brightness within the LDR map');
    }
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('setup: four requester subscriptions exist', skip: skipReason, () async {
    final subs = await SubscriptionClient(deviceId: found.core.id).getRequesterSubscriptions();
    expect(subs.length, 4);
  }, timeout: const Timeout(Duration(seconds: 30)));

  test('setup: DAS values flow into the Subscriptions block', skip: skipReason, () async {
    final reg = RegisterClient(deviceId: found.core.id);
    final block = DynBlock(index: dynSubscriptions, meta: BlockMeta(flagsAndType: BlockType.dynamic.value), name: '');
    // Wait for the delta subscriptions to push a first value (a busy bus can delay it).
    var ok = false;
    for (var i = 0; i < 40 && !ok; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 250));
      final vals = <double>[];
      for (final f in [fTempA, fLuxA, fTempB, fLuxB]) {
        final e = await reg.readDynamicField(block, f, 0);
        vals.add(e == null ? 0 : numberFromBytes(e.value));
      }
      // ignore: avoid_print
      print('[SETUP] subscription targets = $vals');
      // The temperatures are non-zero; the recalibrated lux can legitimately read ~0 in a
      // dark room, so it only has to arrive as a finite non-negative value.
      ok = vals[0] != 0.0 && vals[2] != 0.0 && vals[1] >= 0 && vals[3] >= 0;
    }
    expect(ok, isTrue, reason: 'all four DAS values should reach the core');
  }, timeout: const Timeout(Duration(seconds: 120)));

  test('setup: scripts are loaded and running', skip: skipReason, () async {
    // Give a just-started script a moment (a Waiting/Delay state is fine; Error is not).
    final scripts = ScriptClient(deviceId: found.core.id);
    for (final id in [scrTemperature, scrEyeMovement, scrLidTimer, scrBrightness]) {
      int? state;
      for (var i = 0; i < 8; i++) {
        state = await scripts.readState(id);
        if (state != null && state != ScriptState.error) break;
        await Future<void>.delayed(const Duration(milliseconds: 250));
      }
      // ignore: avoid_print
      print('[SETUP] script $id state=$state (${state == null ? '-' : ScriptState.label(state)})');
      expect(state, isNotNull, reason: 'script $id loaded');
      expect(state, isNot(ScriptState.error), reason: 'script $id not in error');
    }
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('setup: temperature script drives the fan duty (P control)', skip: skipReason, () async {
    final reg = RegisterClient(deviceId: found.core.id);
    final scripts = ScriptClient(deviceId: found.core.id);

    Future<int?> duty() async {
      final r = await reg.readBlockField(BlockType.pwm.value, fanInst, 1, 0);
      return r == null ? null : uint32FromBytes(r.value); // PWM Duty is a uint32 (%)
    }

    final pct = await duty();
    // ignore: avoid_print
    print('[SETUP] fan duty = $pct %');
    expect(pct, isNotNull);
    expect(pct, inInclusiveRange(0, 100));

    // Drive the P controller through its input: a target below any plausible ambient makes
    // duty = P * (temp - target) positive.
    final target = await scripts.readEntry(scrTemperature, ScriptField.input, 0);
    expect(target, isNotNull);
    expect(target!.meta.dataType, DataType.number);
    final meta = BlockMeta(flagsAndType: target.meta.flagsAndType, key: 0, size: 4);
    expect(await scripts.writeEntry(scrTemperature, ScriptField.input, 0, meta, numberToBytes(5)),
        isTrue);
    var heated = 0;
    for (var i = 0; i < 40; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 250));
      heated = await duty() ?? 0;
      if (heated > 0) break;
    }
    // ignore: avoid_print
    print('[SETUP] fan duty with target 5 C = $heated %');
    expect(heated, greaterThan(0), reason: 'the P controller should ask for cooling');

    // Restore the documented default target.
    expect(
        await scripts.writeEntry(
            scrTemperature, ScriptField.input, 0, meta, numberToBytes(targetTemp)),
        isTrue);
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('setup: the eye script drives valid render matrices', skip: skipReason, () async {
    final reg = RegisterClient(deviceId: found.core.id);
    final block = DynBlock(index: dynLeftEye, meta: BlockMeta(flagsAndType: BlockType.dynamic.value), name: '');
    for (final (name, field) in [('iris', eyeIrisGeo), ('pupil', eyePupilGeo), ('lid', eyeLidGeo)]) {
      final pos = await reg.readDynamicField(block, field, gkPosition);
      expect(pos, isNotNull, reason: '$name position present');
      expect(pos!.meta.dataType, DataType.matrix, reason: '$name is a matrix');
      expect(pos.value.length, 28, reason: '$name 2x3 wire size');
      expect(pos.value[0], 2, reason: '$name matrix height');
      expect(pos.value[2], 3, reason: '$name matrix width');
      // ignore: avoid_print
      print('[SETUP] $name position = ${pos.value}');
    }
    // Iris, pupil and the iris fade all share one position: the pupil is smaller than the
    // iris and must stay inside it (moving them by different fractions let the pupil's tone
    // slide out past the iris edge). The script writes them from the same matrix; a read can
    // straddle a write, so retry.
    bool same(List<int> a, List<int> b) {
      if (a.length != b.length) return false;
      for (var k = 0; k < a.length; k++) {
        if (a[k] != b[k]) return false;
      }
      return true;
    }

    var centred = false;
    for (var i = 0; i < 10 && !centred; i++) {
      final iris = await reg.readDynamicField(block, eyeIrisGeo, gkPosition);
      final tex = await reg.readDynamicField(block, eyeIrisTex, tkPosition);
      final pupil = await reg.readDynamicField(block, eyePupilGeo, gkPosition);
      if (iris != null && tex != null && pupil != null) {
        centred = same(iris.value, pupil.value) && same(tex.value, pupil.value);
      }
      if (!centred) await Future<void>.delayed(const Duration(milliseconds: 30));
    }
    expect(centred, isTrue, reason: 'iris, pupil and fade share one position');
  }, timeout: const Timeout(Duration(seconds: 30)));

  test('setup: the lid script stays bounded and reaches the open position', skip: skipReason, () async {
    final reg = RegisterClient(deviceId: found.core.id);
    final block = DynBlock(index: dynLeftEye, meta: BlockMeta(flagsAndType: BlockType.dynamic.value), name: '');
    // Sample across a blink cycle: the lid must stay within the screen range (a runaway
    // loop condition used to drive `ty` to thousands, jamming the lid shut).
    var maxAbs = 0.0;
    var sawOpen = false;
    for (var i = 0; i < 16; i++) {
      final pos = await reg.readDynamicField(block, eyeLidGeo, gkPosition);
      final ty = numberFromBytes(pos!.value, 24);
      if (ty.abs() > maxAbs) maxAbs = ty.abs();
      if (ty < -4) sawOpen = true; // open position (~-5.5)
      await Future<void>.delayed(const Duration(milliseconds: 250));
    }
    // ignore: avoid_print
    print('[SETUP] lid ty max|.|=$maxAbs sawOpen=$sawOpen');
    expect(maxAbs, lessThan(8), reason: 'lid position must stay within the screen range');
    expect(sawOpen, isTrue, reason: 'the lid must reach its open position');
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('setup: brightness script switches each eye between light and dark', skip: skipReason, () async {
    final reg = RegisterClient(deviceId: found.core.id);
    final scripts = ScriptClient(deviceId: found.core.id);
    final right = DynBlock(
        index: dynRightEye, meta: BlockMeta(flagsAndType: BlockType.dynamic.value), name: 'Right Eye');
    final left = DynBlock(
        index: dynLeftEye, meta: BlockMeta(flagsAndType: BlockType.dynamic.value), name: 'Left Eye');

    Future<List<int>?> read(DynBlock b, int field, int key) async =>
        (await reg.readDynamicField(b, field, key))?.value;

    Future<void> writeInput(int index, List<int> value) async {
      final e = await scripts.readEntry(scrBrightness, ScriptField.input, index);
      expect(e, isNotNull, reason: 'brightness input $index');
      final ok = await scripts.writeEntry(scrBrightness, ScriptField.input, index,
          BlockMeta(flagsAndType: e!.meta.flagsAndType, key: 0, size: value.length), value);
      expect(ok, isTrue, reason: 'write brightness input $index');
    }

    // Manual mode: the right eye dark, the left light.
    await writeInput(0, [1]); // manual
    await writeInput(1, [0]); // left  = light
    await writeInput(2, [1]); // right = dark

    List<int> bg = const [], irisC1 = const [], pupil = const [], size = const [];
    for (var i = 0; i < 40; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 250));
      bg = await read(right, eyeBgTex, tkColour1) ?? const [];
      irisC1 = await read(right, eyeIrisTex, tkColour1) ?? const [];
      pupil = await read(right, eyePupilTex, tkColour1) ?? const [];
      size = await read(right, eyePupilGeo, gkSize) ?? const [];
      if (bg.isNotEmpty && bg.first == 0 && irisC1.isNotEmpty && irisC1[1] == darkIrisG1) break;
    }
    // ignore: avoid_print
    print('[SETUP] dark right eye bg=$bg iris1=$irisC1 pupil=$pupil size=$size');
    expect(bg, [0, 0, 0, 255], reason: 'dark background');
    expect(irisC1, [darkIrisR1, darkIrisG1, darkIrisB1, 255], reason: 'really dark green iris');
    expect(pupil, [darkPupilR, darkPupilG, darkPupilB, 255], reason: 'slightly lighter dark pupil');
    // The pupil keeps its size across modes.
    expect(size.length, 8);
    expect(numberFromBytes(size), closeTo(pupilHalfW, 0.01),
        reason: 'pupil size is mode-independent');
    // The left eye was not switched: each display changes independently.
    expect(await read(left, eyeBgTex, tkColour1), [255, 255, 255, 255]);
    expect(await read(left, eyeIrisTex, tkColour1), [irisR1, irisG1, irisB1, 255]);

    // Back to light on the right eye.
    await writeInput(2, [0]);
    for (var i = 0; i < 40; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 250));
      irisC1 = await read(right, eyeIrisTex, tkColour1) ?? const [];
      bg = await read(right, eyeBgTex, tkColour1) ?? const [];
      if (irisC1.isNotEmpty && irisC1[1] == irisG1) break;
    }
    // ignore: avoid_print
    print('[SETUP] light right eye bg=$bg iris1=$irisC1');
    expect(irisC1, [irisR1, irisG1, irisB1, 255], reason: 'bright green iris in light mode');
    expect(bg, [255, 255, 255, 255], reason: 'white background');

    // Restore the documented default (auto).
    await writeInput(0, [0]);
  }, timeout: const Timeout(Duration(minutes: 3)));

  test('setup: the lux -> brightness curve hits its anchors', skip: skipReason, () async {
    // Silence every feed so driven lux values stick (the DAS would otherwise overwrite them).
    final reg = RegisterClient(deviceId: found.core.id);
    final subs = SubscriptionClient(deviceId: found.core.id);
    for (final r in await subs.getRequesterSubscriptions()) {
      await subs.setRequesterSubscription(r.index);
    }
    for (final das in found.das) {
      final c = SubscriptionClient(deviceId: das.id);
      for (var i = 0; i < 4; i++) {
        await c.setRequesterSubscription(i);
      }
    }

    final block = DynBlock(
        index: dynSubscriptions,
        meta: BlockMeta(flagsAndType: BlockType.dynamic.value),
        name: 'Subscriptions');
    Future<void> drive(double lux) async {
      for (var i = 0; i < 5; i++) {
        await reg.writeDynamicEntry(block, fLuxA, 0,
            BlockMeta(flagsAndType: DataType.number.value), numberToBytes(lux));
        await reg.writeDynamicEntry(block, fLuxB, 0,
            BlockMeta(flagsAndType: DataType.number.value), numberToBytes(lux));
        await Future<void>.delayed(const Duration(milliseconds: 60));
      }
      await Future<void>.delayed(const Duration(milliseconds: 250));
    }

    Future<double> brightness(int inst) async =>
        numberFromBytes((await reg.readBlockField(BlockType.vysiDisplay.value, inst, 0, 0))!.value);

    await drive(200);
    final at200 = await brightness(dispLeft);
    await drive(10000);
    final at10k = await brightness(dispRight);
    // ignore: avoid_print
    print('[SETUP] curve: 200 lux -> $at200 %, 10000 lux -> $at10k %');
    expect(at200, closeTo(20, 2), reason: '~200 lux should give ~20 %');
    expect(at10k, closeTo(luxBrightMax, 1), reason: 'full brightness is reached around 10k lux');

    // Put the real feeds back (the later tests and the capture expect four subscriptions).
    await buildSubscriptions(subs, found.core, found.das);
  }, timeout: const Timeout(Duration(minutes: 3)));

  test('setup: capture the semantic backup zip to the project root', skip: skipReason, () async {
    // A busy device can drop the block enumeration; retry until each device reports blocks.
    Future<BackupDevice> captureWithRetry(DeviceEntry d) async {
      BackupDevice? cap;
      for (var i = 0; i < 4; i++) {
        cap = await captureDevice(d.id, includeFiles: true, maxFileBytes: 65536);
        if (cap != null && cap.blocks.isNotEmpty) break;
        await Future<void>.delayed(const Duration(milliseconds: 600));
      }
      expect(cap, isNotNull, reason: 'capture device ${d.id}');
      // ignore: avoid_print
      print('[SETUP] captured device ${d.id} (${d.displayName}) -> '
          '${cap!.blocks.length} blocks, ${cap.files.length} files, ${cap.scripts.length} scripts');
      expect(cap.blocks, isNotEmpty, reason: 'device ${d.id} blocks captured');
      return cap;
    }

    final devices = <BackupDevice>[];
    for (final d in [found.core, ...found.das]) {
      devices.add(await captureWithRetry(d));
    }
    final zip = buildBackupZip(devices);
    final out = File('${Directory.current.parent.path}/Tamu_current_setup.zip');
    await out.writeAsBytes(zip, flush: true);
    // ignore: avoid_print
    print('[SETUP] wrote ${out.path} (${zip.length} bytes)');
    expect(await out.exists(), isTrue);
    expect(zip.length, greaterThan(0));
  }, timeout: const Timeout(Duration(seconds: 180)));

  test('setup: the captured archive parses back', skip: skipReason, () async {
    final out = File('${Directory.current.parent.path}/Tamu_current_setup.zip');
    final devices = parseBackupZip(await out.readAsBytes());
    expect(devices.length, 3);
    final core = devices.firstWhere((d) => d.typeId == DeviceType.tamuV20A.value);
    expect(core.scripts.length, 4, reason: 'four scripts captured');
    expect(core.requesterSubscriptions.length, 4, reason: 'four subscriptions captured');
    final names = [for (final b in core.blocks) b.name];
    expect(names, containsAll(['Subscriptions', 'Left Eye', 'Right Eye']));
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('setup: static settings survive a reboot', skip: skipReason, () async {
    // The displays' Render Block is a *static* persistent field: it only reaches flash when
    // applyCurrentSetup issues a Save (Register.md). Without one, a reboot reverts it to
    // -1 and the eyes stop rendering the setup entirely - this was the reported "config did
    // not persist". Reboot and check the whole configuration comes back on its own.
    final db = DeviceDatabase.instance;
    final reg = RegisterClient(deviceId: found.core.id);
    final port = Platform.environment['TAMU_HIL']!;

    await ConnectionManager.instance.disconnect();
    await Process.run('/home/akyirr/.platformio/penv/bin/python', [
      '/home/akyirr/.platformio/packages/tool-esptoolpy/esptool.py',
      '--port', port, 'run'
    ]);
    await Future<void>.delayed(const Duration(seconds: 6));
    final err = await connectHil();
    expect(err, isNull, reason: 'reconnect after reboot');
    for (var i = 0; i < 20; i++) {
      await db.refreshRuntime(0);
      await Future<void>.delayed(const Duration(seconds: 2));
      if (db.all.where((d) => d.type == DeviceType.dualAnalogSensor).length >= 2) break;
    }

    final left = await reg.readBlockField(BlockType.vysiDisplay.value, dispLeft, 2, 0);
    final right = await reg.readBlockField(BlockType.vysiDisplay.value, dispRight, 2, 0);
    // ignore: avoid_print
    print('[SETUP] after reboot: left renders ${left?.value} right renders ${right?.value}');
    expect(uint32FromBytes(left!.value), dynLeftEye, reason: 'left display render block restored');
    expect(uint32FromBytes(right!.value), dynRightEye, reason: 'right display render block restored');

    // The rest of the configuration restores through its own paths (DT/DV, script files,
    // the requester table).
    final layout = await reg.readBlockField(BlockType.vysiDisplay.value, dispLeft, 3, 0);
    expect(layout?.value, 'LAY_1'.padRight(8).codeUnits, reason: 'layout restored');
    final names = {for (final b in await reg.readDynamicBlocks() ?? <DynBlock>[]) b.index: b.name};
    expect(names[dynLeftEye], 'Left Eye');
    expect(names[dynRightEye], 'Right Eye');

    // The render dictionary itself must survive too: its entries carry the Persistent flag,
    // so the shapes/texture types/sizes/fades come back instead of being zeroed (the scripts
    // only rewrite the positions and the mode colours, so a zeroed dictionary renders wrong).
    final leftBlock = DynBlock(
        index: dynLeftEye, meta: BlockMeta(flagsAndType: BlockType.dynamic.value), name: '');
    Future<List<int>?> val(int field, int key) async =>
        (await reg.readDynamicField(leftBlock, field, key))?.value;
    expect(await val(eyeBgGeo, gkShape), enumByte(shapeFill), reason: 'bg shape restored');
    expect(await val(eyeIrisTex, tkType), enumByte(texGradientLinear), reason: 'iris texture restored');
    expect(await val(eyePupilGeo, gkSize), [...num(pupilHalfW), ...num(pupilHalfH)],
        reason: 'pupil size restored');
    expect(await val(eyeLidGeo, gkFade), num(lidFade), reason: 'lid fade restored');
    expect(await val(eyeLidTex, tkType), enumByte(texFill), reason: 'lid texture restored');

    expect(await ScriptClient(deviceId: found.core.id).loadedScripts(), [0, 1, 2, 3]);
    expect(await SubscriptionClient(deviceId: found.core.id).getRequesterSubscriptions(), hasLength(4));
  }, timeout: const Timeout(Duration(minutes: 3)));
}
