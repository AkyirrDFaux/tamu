/// Programmatic setup upload (not part of the routine HIL suite): parses the akyirr setup
/// zip, matches each source device to a live one, applies the restore plan and reports what
/// landed.
///
///   TAMU_HIL=/dev/ttyACM1 TAMU_SETUP_ZIP=/path/to/Tamu_example_setup.zip \
///     flutter test --no-pub --concurrency=1 test/hil_setup_upload_test.dart
///
/// Destructive: it writes the setup's blocks, scripts, files, subscriptions and addresses.
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/script_client.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/subscription_client.dart';
import 'package:tamuapp/core/types.dart';

import 'hil/hil_env.dart';

void main() {
  final env = HilEnv();
  setUpAll(env.connectAndScan);
  tearDownAll(env.finish);

  test('upload the akyirr setup', () async {
    if (!env.hilRequested || env.rig.isEmpty) {
      markTestSkipped('no rig');
      return;
    }
    final path = Platform.environment['TAMU_SETUP_ZIP'] ??
        '/home/akyirr/Project/tamu-setup/akyirr/Tamu_example_setup.zip';
    final zip = File(path);
    expect(zip.existsSync(), isTrue, reason: 'setup zip at $path');
    final devices = parseBackupZip(zip.readAsBytesSync());
    // ignore: avoid_print
    print('SETUP ${devices.length} device(s): '
        '${devices.map((d) => '${d.name}/${d.serial}').join(', ')}');

    // Clean slate: the restore creates a dynamic block only when the target lacks it, so drop
    // the setup's own dynamic blocks before planning, and let the restore rebuild them.
    for (final source in devices) {
      final match = matchLiveDevice(source, <LiveDevice>[]);
      final targetId = match?.id ?? source.id;
      final reg = RegisterClient(deviceId: targetId);
      for (final b in source.blocks) {
        if (b.typeIndex < 0x3F0 || b.typeIndex > 0x3F3) continue;
        final global = ((b.typeIndex - 0x3F0) << 6) | b.instance;
        final gone = await reg.deleteDynamic(block: global);
        // ignore: avoid_print
        print('CLEAN device $targetId dynamic ${b.name}($global) -> $gone');
      }
      // The requester table is TRID-keyed and persisted, so clear it as well: a leftover entry
      // stays alive and holds a TRID the restore's deterministic allocation would reuse.
      final subs = SubscriptionClient(deviceId: targetId);
      for (final r in await subs.getRequesterSubscriptions()) {
        final gone = await subs.cancelRequesterSubscription(r.trid);
        // ignore: avoid_print
        print('CLEAN device $targetId requester trid=0x${r.trid.toRadixString(16)} -> $gone');
      }
    }

    final plan = await buildRestorePlan(devices);
    for (final source in devices) {
      final target = plan.deviceTargets[source.id];
      final items = plan.items.where((i) => i.device.id == source.id).toList();
      final ready = items.where((i) => i.ready).length;
      // ignore: avoid_print
      print('MATCH ${source.name} (${source.serial}) -> '
          '${target == null ? "NO TARGET" : "${target.name}#${target.id} "
              "(live serial ${target.serial})"}'
          '  items=$ready/${items.length} ready');
      for (final i in items.where((i) => !i.ready && i.issue != 'Read Only')) {
        // ignore: avoid_print
        print('   not ready: ${i.kind.name} '
            '${i.block?.name ?? ""}${i.entry == null ? "" : "/${i.entry!.field}"
                "[${i.entry!.key}] = ${i.entry!.value}"}'
            ' -> ${i.issue}');
      }
    }

    final dryRun = Platform.environment['TAMU_SETUP_DRYRUN'] == '1';
    if (dryRun) {
      for (final d in plan.liveDevices) {
        // ignore: avoid_print
        print('LIVE-DEV ${d.name}#${d.id} serial=${d.serial} caps=0x${d.capabilities.toRadixString(16)} '
            'blocks=${d.blocks.map((b) => "${b.type}#${b.instance}/${b.name}").join(", ")}');
      }
      for (final i in plan.items.where((i) => !i.ready && i.kind == RestoreKind.subscription)) {
        final s = i.subscription!;
        // ignore: avoid_print
        print('SUB-NOTREADY from ${s.provider} -> ${i.issue}'
            ' src=${s.source.block}#${s.source.typeIndex}/${s.source.instance}'
            '/${s.source.field}[${s.source.key}]'
            ' tgt=${s.target.block}#${s.target.typeIndex}/${s.target.instance}'
            '/${s.target.field}[${s.target.key}]');
      }
      for (final i in plan.items.where((i) => i.ready && i.selected)) {
        // ignore: avoid_print
        print('READY ${i.kind.name} ${i.device.name} '
            '${i.block?.name ?? ""}${i.entry == null ? "" : "/${i.entry!.field}"
                "[${i.entry!.key}]"}${i.script == null ? "" : " slot ${i.script!.slot}"}'
            '${i.subscription == null ? "" : " from ${i.subscription!.provider}"}'
            '${i.sndbEntry == null ? "" : " ${i.sndbEntry!.serial}@${i.sndbEntry!.address}"}'
            '${i.file == null ? "" : " ${i.file!.name}"}');
      }
      // ignore: avoid_print
      print('DRY RUN: plan only, nothing written');
      return;
    }

    final result = await applyRestorePlan(plan);
    // ignore: avoid_print
    print('APPLIED written=${result.written} failed=${result.failed}');
    for (final f in result.failures) {
      // ignore: avoid_print
      print('FAILED-ITEM $f');
    }
    // The device owns its persistence files (.DT_/.DV_/.SUBREQ/.SNREG are written by the
    // firmware in the current format): ask it to save the restored state, so a later boot
    // recalls the dynamic blocks and the tables instead of the stale captured files.
    for (final d in env.rig.devices) {
      final saved = await RegisterClient(deviceId: d.id).saveAll();
      // ignore: avoid_print
      print('SAVE-ALL ${d.name}#${d.id} -> $saved');
    }

    // Verify what landed on each device.
    for (final d in env.rig.devices) {
      final reg = RegisterClient(deviceId: d.id);
      final st = StorageClient(deviceId: d.id);
      final sc = ScriptClient(deviceId: d.id);
      final subs = SubscriptionClient(deviceId: d.id);
      final files = await st.readFileTable();
      final blocks = await reg.readDynamicBlocks();
      final req = await subs.getRequesterSubscriptions();
      final prov = await subs.getProviderSubscriptions();
      final states = <String>[];
      for (var slot = 0; slot < 12; slot++) {
        final r = await sc.readState(slot);
        if (r != null) {
          states.add('$slot:state=${r.state}${r.hasError ? " ERR" : ""}');
        }
      }
      for (final r in req) {
        // ignore: avoid_print
        print('REQ ${d.name}#${d.id} idx=${r.index} provider=${idToString(r.providerAddr)} '
            'trgType=${(r.targetReg >> 22) & 0x3FF} trgInst=${(r.targetReg >> 16) & 0x3F} '
            'trgField=${(r.targetReg >> 8) & 0xFF} srcType=${(r.sourceReg >> 22) & 0x3FF} '
            'srcInst=${(r.sourceReg >> 16) & 0x3F} srcField=${(r.sourceReg >> 8) & 0xFF}');
      }
      for (final p in prov) {
        // ignore: avoid_print
        print('PROV ${d.name}#${d.id} period=${p.periodMs} '
            'srcType=${(p.sourceReg >> 22) & 0x3FF} srcInst=${(p.sourceReg >> 16) & 0x3F} '
            'srcField=${(p.sourceReg >> 8) & 0xFF}');
      }
      // ignore: avoid_print
      print('LIVE ${d.name}#${d.id}: files=${files?.map((f) => "${f.name}(${f.size})").join(",")} '
          'dynamic=${blocks?.map((b) => "${b.index}:${b.name}(f${b.fieldCount})").join(",")} '
          'req=${req.length} prov=${prov.length} '
          'scripts=[${states.join(", ")}]');
    }
    expect(result.failed, 0, reason: 'every selected item must apply');
  }, timeout: const Timeout(Duration(minutes: 6)));
}
