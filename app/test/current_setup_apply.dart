part of 'current_setup.dart';

Future<void> buildScripts(StorageClient storage, ScriptClient scripts) async {
  final drafts = <int, ScriptDraft>{
    scrTemperature: scriptTemperature(),
    scrEyeMovement: scriptEyeMovement(),
    scrLidTimer: scriptLidTimer(),
    scrBrightness: scriptBrightness(),
    // Loaded last: the emote script must sit in a higher slot than the eye movement script
    // (the main loop runs the slots in ascending order within one pass).
    scrEmote: scriptEmoteSelector(),
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
  for (final id in [scrTemperature, scrEyeMovement, scrLidTimer, scrBrightness, scrEmote]) {
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
