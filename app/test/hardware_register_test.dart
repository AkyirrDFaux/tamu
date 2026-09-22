@Tags(['hil'])
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/types.dart';

import 'hil_helpers.dart';

/// Register service smoke checks, shared by `hil_test_suite.dart` and runnable
/// standalone via `main` (Tamu only).
Future<void> runTests() async {
  final link = ConnectionManager.instance;

  // Enumerate block types (01.00 Enum 0: enum_level byte + BlockInfo, min 5 bytes).
  final reply = await link.request(1, ServiceType.register, 0,
      payload: [0, 0, 0, 0, 0]);
  expect(reply.length, greaterThanOrEqualTo(5),
      reason: 'enumerate should echo BlockInfo + block types');
  final count = reply[4];
  expect(count, greaterThan(0), reason: 'Tamu has static blocks');
  expect(reply.length, greaterThanOrEqualTo(4 + count));

  // Read System block DeviceType (type 0, inst 0, field 0).
  final reply2 = await link.request(1, ServiceType.register, 1,
      payload: blockInfoBytes(0, 0, 0, 0));
  expect(reply2.length, greaterThanOrEqualTo(8));

  // Read System SN (field 1, key 0xFF).
  final reply3 = await link.request(1, ServiceType.register, 1,
      payload: blockInfoBytes(0, 0, 1, 0xFF));
  expect(reply3.length, greaterThanOrEqualTo(8 + 14));
}

void main() {
  final skipReason =
      Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  setUpAll(() async {
    if (skipReason is String) return;
    await connectHil();
  });
  tearDownAll(disconnectHil);

  test('HIL: Register service', skip: skipReason, () => runTests(),
      timeout: const Timeout(Duration(seconds: 30)));
}
