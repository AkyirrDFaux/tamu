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
      // ignore: avoid_print
      print('DRY RUN: plan only, nothing written');
      return;
    }

    final result = await applyRestorePlan(plan);
    // ignore: avoid_print
    print('APPLIED written=${result.written} failed=${result.failed}');

    // Verify what landed on each device.
    for (final d in env.rig.devices) {
      final reg = RegisterClient(deviceId: d.id);
      final st = StorageClient(deviceId: d.id);
      final sc = ScriptClient(deviceId: d.id);
      final files = await st.readFileTable();
      final blocks = await reg.readDynamicBlocks();
      final states = <String>[];
      for (var slot = 0; slot < 12; slot++) {
        final r = await sc.readState(slot);
        if (r != null) {
          states.add('$slot:state=${r.state}${r.hasError ? " ERR" : ""}');
        }
      }
      // ignore: avoid_print
      print('LIVE ${d.name}#${d.id}: files=${files?.length} '
          'dynamic=${blocks?.map((b) => "${b.index}:${b.name}").join(",")} '
          'scripts=[${states.join(", ")}]');
    }
    expect(result.failed, 0, reason: 'every selected item must apply');
  });
}
