@Tags(['hil'])
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/diagnostics.dart';

import 'hil_helpers.dart';

void main() {
  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  setUpAll(() async {
    await connectHil();
  });

  tearDownAll(disconnectHil);

  test('ping x10', () async {
    final db = DeviceDatabase.instance;
    for (var i = 0; i < 10; i++) {
      final started = DateTime.now();
      try {
        final ok = await db.pingCore()
            .timeout(const Duration(seconds: 5));
        // ignore: avoid_print
        print('[B] ping $i ok=$ok '
            '${DateTime.now().difference(started).inMilliseconds} ms');
      } catch (e) {
        // ignore: avoid_print
        print('[B] ping $i FAILED: $e '
            '${DateTime.now().difference(started).inMilliseconds} ms');
      }
    }
    // ignore: avoid_print
    print('[B] diagnostics:\n${AppDiagnostics.dump()}');

    // HIL-style: immediate refreshNetwork after connect (races the unawaited one)
    await db.refreshNetwork();
    final core = db.byId(1);
    // ignore: avoid_print
    print('[B] HIL-style immediate: core type=${core?.type} '
        'cap=${core?.capabilities.toRadixString(16)}');

    // wait a bit for the unawaited refresh to finish, then re-check
    await Future<void>.delayed(const Duration(seconds: 3));
    final core2 = db.byId(1);
    // ignore: avoid_print
    print('[B] after 3s settle: core type=${core2?.type} '
        'sn=${core2?.serialNumber} devices=${db.all.map((d) => d.id).toList()}');

    // concurrent pings
    final concurrent = await Future.wait(
        List.generate(8, (_) => db.pingCore().catchError((e) => false)));
    // ignore: avoid_print
    print('[B] concurrent 8 pings: ok=${concurrent.where((x) => x).length}/8');
  }, timeout: const Timeout(Duration(minutes: 2)), skip: skipReason);
}
