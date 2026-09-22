@Tags(['hil'])
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/storage_client.dart';

import 'hil_helpers.dart';

/// Storage service create/read/write/delete round-trip, shared by
/// `hil_test_suite.dart` and runnable standalone via `main` (Tamu only).
Future<void> runTests() async {
  final storage = StorageClient(deviceId: 1);
  const name = 'TESTFIL1 ';

  await storage.deleteFile(name);
  expect(await storage.createFile(name, 250), isTrue);

  final data = List<int>.generate(250, (i) => i & 0xFF);
  expect(await storage.writeFile(name, data), isTrue);
  expect(await storage.readFile(name, size: 250), equals(data));

  expect(await storage.deleteFile(name), isTrue);
}

void main() {
  final skipReason =
      Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  setUpAll(() async {
    if (skipReason is String) return;
    await connectHil();
  });
  tearDownAll(disconnectHil);

  test('HIL: storage create/read/write/delete file', skip: skipReason, runTests,
      timeout: const Timeout(Duration(seconds: 60)));
}
