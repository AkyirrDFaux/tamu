@Tags(['hil'])
library;

import 'dart:io';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/types.dart';
import 'hil_helpers.dart';

void main() async {
  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  setUpAll(() async => await connectHil());
  tearDownAll(disconnectHil);

  test('Debug: Register client readField for system block field 0 keys', () async {
    final client = RegisterClient(deviceId: 1);
    
    for (int key = 0; key <= 2; key++) {
      final field = await client.readField(0, key);
      print('Field 0, Key $key:');
      if (field != null) {
        print('  meta.dataType: ${field.meta.dataType}');
        print('  meta.flags: ${field.meta.flags}');
        print('  meta.size: ${field.meta.size}');
        print('  value: ${field.value}');
        // Parse
        final dt = DataType.values.firstWhere((e) => e.value == field.meta.dataType, orElse: () => DataType.none);
        if (field.meta.dataType == DataType.enum_.value && field.value.length >= 4) {
          final val = field.value[0] | (field.value[1] << 8) | (field.value[2] << 16) | (field.value[3] << 24);
          print('  Parsed DeviceType: $val (${DeviceType.fromValue(val).label})');
        } else if (field.meta.dataType == DataType.integer.value && field.value.length >= 4) {
          final val = field.value[0] | (field.value[1] << 8) | (field.value[2] << 16) | (field.value[3] << 24);
          print('  Parsed Capabilities: $val');
        } else if (field.meta.dataType == DataType.string.value && field.value.length >= 4) {
          print('  Parsed Version: ${field.value[0]}.${field.value[1]}.${field.value[2]}.${field.value[3]}');
        }
      } else {
        print('  NULL');
      }
    }
  }, timeout: const Timeout(Duration(seconds: 30)), skip: skipReason is String ? skipReason : false);
}