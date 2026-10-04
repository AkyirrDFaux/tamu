@Tags(['hil'])
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/types.dart';

import 'hil_helpers.dart';
import 'package:tamuapp/core/register_client.dart';

/// Register service smoke checks, shared by `hil_test_suite.dart` and runnable
/// standalone via `main` (Tamu only).
Future<void> runTests() async {
  final link = ConnectionManager.instance;

  // Enumerate block types (CID 0, no payload): a FRAG stream of `(type << 6) | maxInstance`.
  final reg = RegisterClient(deviceId: 1);
  final types = await reg.enumerateBlockTypes();
  expect(types, isNotNull, reason: 'enumerate returned no list');
  expect(types!, isNotEmpty, reason: 'Tamu has static blocks');

  // Read System block DeviceType (type 0, inst 0, field 0).
  final reply2 = await link.request(1, ServiceType.register, RegisterCid.read,
      payload: blockInfoBytes(0, 0, 0, 0));
  expect(reply2.length, greaterThanOrEqualTo(8));

  // Read System SN (field 1, key 0xFF).
  final reply3 = await link.request(1, ServiceType.register, RegisterCid.read,
      payload: blockInfoBytes(0, 0, 1, 0xFF));
  expect(reply3.length, greaterThanOrEqualTo(8 + 14));
}

void main() {
  final skipReason = hilSetup();

  test('HIL: Register service', skip: skipReason, () => runTests(),
      timeout: const Timeout(Duration(seconds: 30)));
}
