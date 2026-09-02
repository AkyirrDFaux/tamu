@Tags(['hil', 'ble'])
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/device_db.dart';

import 'hil_helpers.dart';

void main() {
  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  setUpAll(() async {
    await connectHil();
  });

  tearDownAll(disconnectHil);

  test('ping RTT probe', () async {
    final db = DeviceDatabase.instance;
    final rtts = <int>[];
    for (var i = 0; i < 25; i++) {
      final sw = Stopwatch()..start();
      final ok = await db.pingCore();
      sw.stop();
      expect(ok, isTrue);
      if (i >= 5) rtts.add(sw.elapsedMilliseconds);
      await Future.delayed(const Duration(milliseconds: 30));
    }
    rtts.sort();
    // ignore: avoid_print
    print('[RTT] min=${rtts.first} median=${rtts[rtts.length ~/ 2]} '
        'max=${rtts.last} '
        'avg=${rtts.reduce((a, b) => a + b) ~/ rtts.length} ms');
  }, timeout: const Timeout(Duration(minutes: 3)), skip: skipReason);
}
