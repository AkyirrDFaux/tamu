/// Connects to the bus, scans it and keeps the bookkeeping the runner needs.
///
/// Mirrors the connection flow of the old `hil_helpers.dart`: `TAMU_HIL` names
/// the core port (`ble` selects the BLE path); when it is unset the rig stays
/// empty and every test skips.
library;

import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/types.dart';

import 'hil_api.dart';
import 'hil_log.dart';

enum HilOutcome { pass, fail, skip }

class HilEnv {
  final HilLog log = HilLog();
  HilRig rig = const HilRig([]);
  String? fatal;

  final List<({String label, HilOutcome outcome, String? detail})> results = [];

  bool get hilRequested => Platform.environment['TAMU_HIL'] != null;

  /// `--scan`: list the rig and run no test.
  bool get scanOnly => Platform.environment['TAMU_HIL_SCAN'] == '1';

  /// The `--category` filter, or null for every category.
  HilCategory? get categoryFilter {
    final raw = Platform.environment['TAMU_HIL_CATEGORY'];
    if (raw == null || raw.isEmpty) return null;
    for (final c in HilCategory.values) {
      if (c.name == raw) return c;
    }
    return null;
  }

  /// The `--device` filter, or null for every device.
  Set<int>? get deviceFilter {
    final raw = Platform.environment['TAMU_HIL_DEVICE'];
    if (raw == null || raw.isEmpty) return null;
    final ids = raw
        .split(',')
        .map((s) => s.trim())
        .map(int.tryParse)
        .whereType<int>()
        .toSet();
    return ids.isEmpty ? null : ids;
  }

  /// The skip reason for a test, or null when it may run.
  String? skipReason(HilTest test) {
    if (!hilRequested) return 'TAMU_HIL not set';
    if (fatal != null) return fatal;
    return rig.unmet(test.prereq);
  }

  void record(HilTest test, HilOutcome outcome, [String? detail]) =>
      recordLabeled(test.label, outcome, detail);

  void recordLabeled(String label, HilOutcome outcome, [String? detail]) {
    results.add((label: label, outcome: outcome, detail: detail));
    log.line('  ${_tick(outcome)}  $label'
        '${detail == null ? '' : '  -- $detail'}');
  }

  static String _tick(HilOutcome o) => switch (o) {
        HilOutcome.pass => 'PASS',
        HilOutcome.fail => 'FAIL',
        HilOutcome.skip => 'SKIP',
      };

  /// Runs from `setUpAll`: connect, settle, scan.
  Future<void> connectAndScan() async {
    if (!hilRequested) {
      log.line('TAMU_HIL not set - the whole suite skips.');
      stdout.writeln('[hil] TAMU_HIL not set - skipping the suite.');
      return;
    }
    final target = Platform.environment['TAMU_HIL']!;
    TestWidgetsFlutterBinding.ensureInitialized();
    debugDefaultTargetPlatformOverride = TargetPlatform.linux;

    final mgr = ConnectionManager.instance;
    await mgr.setAutoRefresh(false);

    if (target == 'ble') {
      await _connectBle(mgr);
    } else {
      final err = await mgr.connectTo(
          DiscoveredLink(id: target, type: LinkType.usb, name: 'Tamu core'));
      if (err != null) {
        fatal = 'connect failed: $err';
        fail(fatal!);
      }
    }

    // Wait for the core to answer pings (boot settle).
    final db = DeviceDatabase.instance;
    var live = false;
    for (var i = 0; i < 20; i++) {
      if (await db.pingCore()) {
        live = true;
        break;
      }
      await Future<void>.delayed(const Duration(milliseconds: 250));
    }
    if (!live) {
      fatal = 'core did not answer ping within the settle window';
      fail(fatal!);
    }

    await _scan();
  }

  Future<void> _connectBle(ConnectionManager mgr) async {
    await mgr.setSource(LinkSource.ble);
    DiscoveredLink? link;
    for (var round = 0; round < 3 && link == null; round++) {
      await mgr.refresh();
      final deadline = DateTime.now().add(const Duration(seconds: 12));
      while (link == null && DateTime.now().isBefore(deadline)) {
        await Future<void>.delayed(const Duration(milliseconds: 500));
        for (final l in mgr.discoveredLinks) {
          if (l.type == LinkType.ble) {
            link = l;
            break;
          }
        }
      }
      await mgr.stopScan();
    }
    if (link == null) {
      fatal = 'Tamu not found during BLE scan';
      fail(fatal!);
    }
    final err = await mgr.connectTo(link);
    if (err != null) {
      fatal = 'BLE connect failed: $err';
      fail(fatal!);
    }
  }

  Future<void> _scan() async {
    final db = DeviceDatabase.instance;
    await db.refreshNetwork();

    final declared = (Platform.environment['TAMU_HIL_BOOTLOADER'] ?? '')
        .split(',')
        .map((s) => s.trim())
        .where((s) => s.isNotEmpty)
        .map(int.tryParse)
        .whereType<int>()
        .toSet();

    final devices = <HilDevice>[];
    for (final entry in db.all) {
      if (entry.stale) continue; // unreachable this sweep
      final blocks = <BlockType>[];
      try {
        final types = await RegisterClient(deviceId: entry.id).enumerateBlockTypes();
        for (final t in types ?? const <({int type, int maxInstance})>[]) {
          final block = BlockType.fromValue(t.type);
          if (block != BlockType.none &&
              block != BlockType.undefined &&
              block != BlockType.deleted) {
            blocks.add(block);
          }
        }
      } catch (_) {
        // A device that does not answer enumeration contributes no blocks.
      }
      devices.add(HilDevice(
        id: entry.id,
        type: entry.type,
        capabilities: entry.capabilities,
        name: entry.name,
        blocks: blocks,
        bootloader: declared.contains(entry.id),
      ));
    }
    // A device in bootloader mode is off the bus - the bootloader ignores the standard
    // packets - so the scan cannot see it; the operator declares it with
    // TAMU_HIL_BOOTLOADER. Add the declared ids the scan did not find, so the bootloader
    // tests have a target. The core passthrough reaches it without addressing it.
    for (final id in declared) {
      if (devices.any((d) => d.id == id)) continue;
      devices.add(HilDevice(
        id: id,
        type: DeviceType.unknown,
        capabilities: 0,
        name: 'device $id (bootloader)',
        bootloader: true,
      ));
    }

    final wanted = deviceFilter;
    rig = HilRig(
        wanted == null ? devices : devices.where((d) => wanted.contains(d.id)).toList());
  }

  /// Runs from `tearDownAll`: write the log, disconnect.
  Future<void> finish() async {
    log.section('Rig');
    if (rig.isEmpty) {
      log.line('  (no devices scanned)');
    } else {
      rig.describe().forEach(log.line);
    }
    log.section('Results');
    var pass = 0, failed = 0, skipped = 0;
    for (final r in results) {
      switch (r.outcome) {
        case HilOutcome.pass:
          pass++;
        case HilOutcome.fail:
          failed++;
        case HilOutcome.skip:
          skipped++;
      }
      log.line('  ${_tick(r.outcome)}  ${r.label}'
          '${r.detail == null ? '' : '  -- ${r.detail}'}');
    }
    log.line('  total: $pass passed, $failed failed, $skipped skipped');
    log.write();
    if (hilRequested) {
      await ConnectionManager.instance.disconnect();
    }
  }
}
