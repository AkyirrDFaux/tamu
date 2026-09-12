@Tags(['hil'])
library;

import 'dart:io';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter/foundation.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/storage_client.dart';
import 'package:tamuapp/core/types.dart';
import 'hil_helpers.dart';

Future<void> runTests() async {
  const t = Timeout(Duration(seconds: 60));

  final storage = StorageClient(deviceId: 1);

  // First, read the file table to see what's there
  final table1 = await storage.readFileTable();
  print('[TEST] Initial table: ${table1?.map((f) => '${f.name}@${f.offset}(${f.size}B)').toList()}');

  // Create a test file
  const name = 'TESTFIL1 ';
  await storage.deleteFile(name);
  expect(await storage.createFile(name, 250), isTrue);

  // Read the file table again
  final table2 = await storage.readFileTable();
  print('[TEST] Table after create: ${table2?.map((f) => '${f.name}@${f.offset}(${f.size}B)').toList()}');

  // Write data
  final data = List<int>.generate(250, (i) => i & 0xFF);
  expect(await storage.writeFile(name, data), isTrue);

  // Read back
  final read = await storage.readFile(name, size: 250);
  expect(read, equals(data));

  // Delete
  expect(await storage.deleteFile(name), isTrue);

  // Final table
  final table3 = await storage.readFileTable();
  print('[TEST] Final table: ${table3?.map((f) => '${f.name}@${f.offset}(${f.size}B)').toList()}');
}

void main() async {
  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  setUpAll(() async => await connectHil());
  tearDownAll(disconnectHil);

  test('HIL: storage create/read/write/delete file', skip: skipReason, () async {
    // First, read the file table to see what's there
    final storage = StorageClient(deviceId: 1);
    final table1 = await storage.readFileTable();
    print('[TEST] Initial table: ${table1?.map((f) => '${f.name}@${f.offset}(${f.size}B)').toList()}');

    // Create a test file
    const name = 'TESTFIL1 ';
    await storage.deleteFile(name);
    expect(await storage.createFile(name, 250), isTrue);

    // Read the file table again
    final table2 = await storage.readFileTable();
    print('[TEST] Table after create: ${table2?.map((f) => '${f.name}@${f.offset}(${f.size}B)').toList()}');

    // Write data
    final data = List<int>.generate(250, (i) => i & 0xFF);
    expect(await storage.writeFile(name, data), isTrue);

    // Read back
    final read = await storage.readFile(name, size: 250);
    expect(read, equals(data));

    // Delete
    expect(await storage.deleteFile(name), isTrue);

    // Final table
    final table3 = await storage.readFileTable();
    print('[TEST] Final table: ${table3?.map((f) => '${f.name}@${f.offset}(${f.size}B)').toList()}');
  }, timeout: const Timeout(Duration(seconds: 60)));
}