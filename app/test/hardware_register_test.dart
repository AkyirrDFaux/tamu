@Tags(['hil'])
library;

import 'dart:io';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter/foundation.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/types.dart';
import 'hil_helpers.dart';

Future<void> runTests() async {
  const t = Timeout(Duration(seconds: 30));

  // HIL: Register Enumerate block types (01.00 Enum 0)
  final link = ConnectionManager.instance;
  final payload = [0]; // Enum 0 for block types
  final reply = await link.request(1, ServiceType.register, 0, payload: payload);
  expect(reply, isNotNull, reason: 'Register enumerate should return reply');
  if (reply != null && reply.length >= 5) {
    final count = reply[4];
    final types = reply.sublist(5, 5 + count);
    expect(types.length, count);
  }

  // HIL: Register Read System Block 0 field 0 (DeviceType)
  final bi = (0 << 22) | (0 << 16) | (0 << 8) | 0; // type0 inst0 field0 key0
  final payload2 = [bi & 0xFF, (bi >> 8) & 0xFF, (bi >> 16) & 0xFF, (bi >> 24) & 0xFF];
  final reply2 = await link.request(1, ServiceType.register, 1, payload: payload2);
  expect(reply2, isNotNull);
  expect(reply2!.length, greaterThanOrEqualTo(8));

  // HIL: Register Read System SN (field 1, key=0xFF)
  final bi2 = (0 << 22) | (0 << 16) | (1 << 8) | 0xFF;
  final payload3 = [bi2 & 0xFF, (bi2 >> 8) & 0xFF, (bi2 >> 16) & 0xFF, (bi2 >> 24) & 0xFF];
  final reply3 = await link.request(1, ServiceType.register, 1, payload: payload3);
  expect(reply3, isNotNull);
  expect(reply3!.length, greaterThanOrEqualTo(8 + 14));
}