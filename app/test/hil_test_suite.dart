@Tags(['hil'])
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'hil_helpers.dart';
import 'hardware_register_test.dart' as register_test;
import 'hardware_storage_test.dart' as storage_test;
import 'tamu_hardware_verification_test.dart' as verification_test;
import 'dyn_flow_test.dart' as dyn_flow_test;
import 'keyed_refresh_test.dart' as keyed_refresh_test;
import 'keyed_crash_test.dart' as keyed_crash_test;
import 'mem_probe_test.dart' as mem_probe_test;

void main() {
  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  setUpAll(() async {
    await connectHil();
  });

  tearDownAll(disconnectHil);

  test('Register Service Tests', () async {
    await register_test.runTests();
  }, timeout: const Timeout(Duration(minutes: 2)), skip: skipReason);

  test('Storage Service Tests', () async {
    await storage_test.runTests();
  }, timeout: const Timeout(Duration(minutes: 2)), skip: skipReason);

  test('Hardware Verification Tests', () async {
    await verification_test.runTests();
  }, timeout: const Timeout(Duration(minutes: 3)), skip: skipReason);

  test('Dynamic Memory Flow Tests', () async {
    await dyn_flow_test.runTests();
  }, timeout: const Timeout(Duration(minutes: 2)), skip: skipReason);

  test('Keyed Memory Refresh Tests', () async {
    await keyed_refresh_test.runTests();
  }, timeout: const Timeout(Duration(minutes: 2)), skip: skipReason);

  test('Keyed Memory Crash Tests', () async {
    await keyed_crash_test.runTests();
  }, timeout: const Timeout(Duration(minutes: 5)), skip: skipReason);

  test('Memory Probe Tests', () async {
    await mem_probe_test.runTests();
  }, timeout: const Timeout(Duration(minutes: 3)), skip: skipReason);
}