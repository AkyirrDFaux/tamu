/// The HIL suite entry point (Docs/HIL Tests.md, "Entry Point").
///
/// Run through `test/run_hil_tests.sh`, which resolves the core port. One run
/// scans the bus, declares every test below, skips the unmet prerequisites and
/// writes `test/hil/logs/hil_<stamp>.log`.
///
/// Selection (Docs/HIL Tests.md, "Selecting What Runs"): `--scan` lists the rig
/// and stops; `--category <name>` and `--device <id>` narrow what runs.
@Tags(['hil'])
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'hil_api.dart';
import 'hil_env.dart';
import 'hil_tests_bootloader.dart';
import 'hil_tests_generic.dart';
import 'hil_tests_module.dart';
import 'hil_tests_partner.dart';
import 'hil_tests_service.dart';

/// Independent tests first, then the exclusive set, then the coupled set
/// (Docs/HIL Tests.md, "Independence and Ordering").
int _order(HilRunClass runClass) => switch (runClass) {
  HilRunClass.independent => 0,
  HilRunClass.exclusive => 1,
  HilRunClass.coupled => 2,
};

/// Every category the default run covers. The bootloader category is excluded:
/// it runs on its own with `--bootloader`.
const _defaultCategories = <HilCategory>[
  HilCategory.generic,
  HilCategory.service,
  HilCategory.module,
  HilCategory.partner,
];

void main() {
  final env = HilEnv();
  setUpAll(env.connectAndScan);
  tearDownAll(env.finish);

  // Scan-only: list the rig and run nothing else.
  if (env.scanOnly) {
    test('[scan] rig inventory', () async {
      if (!env.hilRequested) {
        env.recordLabeled(
          'scan rig inventory',
          HilOutcome.skip,
          'TAMU_HIL not set',
        );
        markTestSkipped('TAMU_HIL not set');
        return;
      }
      env.log.section('Scan');
      final lines = env.rig.describe();
      env.rig.describe().forEach(env.log.line);
      lines.forEach(stdout.writeln);
      env.recordLabeled(
        'scan rig inventory',
        HilOutcome.pass,
        '${env.rig.devices.length} device(s)',
      );
    });
    return;
  }

  final category = env.categoryFilter;
  // Bootloader tests run separately: they take their device off the bus, so the
  // default run leaves them out (Docs/HIL Tests.md, "Test Categories").
  final include = category == null
      ? _defaultCategories
      : <HilCategory>[category];
  final tests =
      <HilTest>[
          ...genericTests,
          ...serviceTests,
          ...moduleTests,
          ...bootloaderTests,
          ...partnerTests,
        ].where((t) => include.contains(t.category)).toList()
        ..sort((a, b) => _order(a.runClass).compareTo(_order(b.runClass)));

  env.log.section('Suite');
  for (final t in tests) {
    env.log.line('  ${t.label}');
  }
  stdout.writeln(
    '[hil] ${tests.length} test(s) declared; '
    'log -> ${env.log.path}',
  );

  for (final t in tests) {
    test(
      '[${t.category.name}] ${t.name}',
      () async {
        final reason = env.skipReason(t);
        if (reason != null) {
          env.record(t, HilOutcome.skip, reason);
          markTestSkipped(reason);
          return;
        }
        try {
          await t.body(HilContext(env.rig, env.log, t));
          env.record(t, HilOutcome.pass);
        } catch (e, st) {
          env.record(t, HilOutcome.fail, '$e');
          env.log.line('    $st');
          rethrow;
        }
      },
      timeout: const Timeout(Duration(minutes: 2)),
    );
  }
}
