@Tags(['hil'])
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'hil_helpers.dart';
import 'hardware_register_test.dart' as register_test;
import 'hardware_storage_test.dart' as storage_test;
import 'dyn_flow_test.dart' as dyn_flow_test;

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

  // The tamu_hardware_verification_test.dart tests run from their own file (see
  // run_hil_tests.sh); they are not aggregated here.

  test('Dynamic Memory Flow Tests', () async {
    await dyn_flow_test.runTests();
  }, timeout: const Timeout(Duration(minutes: 2)), skip: skipReason);
}