@Tags(['hil'])
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/script_client.dart';
import 'package:tamuapp/core/script_file.dart';
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
    await db.refreshRuntime(0);
    await Future<void>.delayed(const Duration(seconds: 2));
    await db.refreshRuntime(0);
    await Future<void>.delayed(const Duration(seconds: 1));
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
      expect(fields.length, 8, reason: 'eye block $eye parts');
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
    }
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('setup: displays point at the eye render blocks', skip: skipReason, () async {
    final reg = RegisterClient(deviceId: found.core.id);
    for (final (inst, eye) in [(dispLeft, dynLeftEye), (dispRight, dynRightEye)]) {
      final rb = await reg.readBlockField(BlockType.vysiDisplay.value, inst, 2, 0);
      expect(rb, isNotNull);
      expect(int32FromBytes(rb!.value), eye, reason: 'display $inst render block');
    }
  }, timeout: const Timeout(Duration(seconds: 30)));

  test('setup: four requester subscriptions exist', skip: skipReason, () async {
    final subs = await SubscriptionClient(deviceId: found.core.id).getRequesterSubscriptions();
    expect(subs.length, 4);
  }, timeout: const Timeout(Duration(seconds: 30)));

  test('setup: DAS values flow into the Subscriptions block', skip: skipReason, () async {
    final reg = RegisterClient(deviceId: found.core.id);
    final block = DynBlock(index: dynSubscriptions, meta: BlockMeta(flagsAndType: BlockType.dynamic.value), name: '');
    // Wait for the delta subscriptions to push a first value.
    var ok = false;
    for (var i = 0; i < 20 && !ok; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 250));
      final vals = <double>[];
      for (final f in [fTempA, fLuxA, fTempB, fLuxB]) {
        final e = await reg.readDynamicField(block, f, 0);
        vals.add(e == null ? 0 : numberFromBytes(e.value));
      }
      // ignore: avoid_print
      print('[SETUP] subscription targets = $vals');
      ok = vals.every((v) => v != 0.0);
    }
    expect(ok, isTrue, reason: 'all four DAS values should reach the core');
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('setup: scripts are loaded and running', skip: skipReason, () async {
    final scripts = ScriptClient(deviceId: found.core.id);
    for (final id in [scrTemperature, scrEyeMovement, scrLidTimer, scrBrightness]) {
      final state = await scripts.readState(id);
      // ignore: avoid_print
      print('[SETUP] script $id state=$state (${state == null ? '-' : ScriptState.label(state)})');
      expect(state, isNotNull, reason: 'script $id loaded');
      expect(state, isNot(ScriptState.error), reason: 'script $id not in error');
    }
  }, timeout: const Timeout(Duration(seconds: 30)));

  test('setup: temperature script drives the fan duty', skip: skipReason, () async {
    final reg = RegisterClient(deviceId: found.core.id);
    final duty = await reg.readBlockField(BlockType.pwm.value, fanInst, 1, 0);
    final pct = duty == null ? null : uint32FromBytes(duty.value); // PWM Duty is a uint32 (%)
    // ignore: avoid_print
    print('[SETUP] fan duty = $pct %');
    expect(pct, isNotNull);
    expect(pct, inInclusiveRange(0, 100));
  }, timeout: const Timeout(Duration(seconds: 30)));

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
  }, timeout: const Timeout(Duration(seconds: 30)));

  test('setup: capture the semantic backup zip to the project root', skip: skipReason, () async {
    final devices = <BackupDevice>[];
    for (final d in [found.core, ...found.das]) {
      final cap = await captureDevice(d.id, includeFiles: true, maxFileBytes: 65536);
      // ignore: avoid_print
      print('[SETUP] captured device ${d.id} (${d.displayName}) -> ${cap == null ? "null" : "${cap.blocks.length} blocks, ${cap.files.length} files, ${cap.scripts.length} scripts"}');
      expect(cap, isNotNull, reason: 'capture device ${d.id}');
      devices.add(cap!);
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
}
