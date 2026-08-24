import 'package:flutter/foundation.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:universal_ble/universal_ble.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  debugDefaultTargetPlatformOverride = TargetPlatform.linux;

  test('scan with services', () async {
    final seen = <String, List<String>>{};
    final sub = UniversalBle.scanStream.listen((d) {
      seen[d.deviceId] = d.services;
    });
    await UniversalBle.startScan();
    await Future<void>.delayed(const Duration(seconds: 15));
    await UniversalBle.stopScan();
    await sub.cancel();
    for (final e in seen.entries) {
      // ignore: avoid_print
      print('SCAN ${e.key} services=${e.value}');
    }
  }, timeout: const Timeout(Duration(seconds: 40)));
}
