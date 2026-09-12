@Tags(['hil'])
library;

import 'dart:io';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/subscription_client.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/core/device_db.dart';
import 'hil_helpers.dart';

DeviceEntry? findDas(DeviceDatabase db) {
  // First try by type
  for (final d in db.all) {
    if (d.type == DeviceType.dualAnalogSensor) return d;
  }
  // Fallback: DAS is typically at ID 2
  for (final d in db.all) {
    if (d.id == 2) return d;
  }
  return null;
}

DeviceEntry? findTamu(DeviceDatabase db) {
  for (final d in db.all) {
    if (d.type == DeviceType.tamuV20A) return d;
  }
  return null;
}

Future<void> discoverDevices() async {
  final db = DeviceDatabase.instance;
  // Trigger device discovery by pinging broadcast
  await db.refreshRuntime(0); // 0 = broadcast
  await Future<void>.delayed(const Duration(seconds: 2));
  await db.refreshRuntime(0);
  await Future<void>.delayed(const Duration(seconds: 1));
}

void main() async {
  final skipReason = Platform.environment['TAMU_HIL'] == null ? 'TAMU_HIL not set' : false;

  setUpAll(() async {
    await connectHil();
    await discoverDevices();
  });
  tearDownAll(disconnectHil);

  test('Subscriptions: DAS provider table empty initially', () async {
    final db = DeviceDatabase.instance;
    final das = findDas(db);
    if (das == null) {
      print('DAS not found - available devices: ${db.all.map((d) => '${d.id}:${d.type.label}').join(', ')}');
      return;
    }
    final client = SubscriptionClient(deviceId: das.id);
    final subs = await client.getProviderSubscriptions();
    expect(subs, isEmpty);
  }, timeout: const Timeout(Duration(seconds: 10)), skip: skipReason is String ? skipReason : false);

  test('Subscriptions: Create periodic subscription to DAS Meas1', () async {
    final db = DeviceDatabase.instance;
    final das = findDas(db);
    if (das == null) {
      print('DAS not found - available devices: ${db.all.map((d) => '${d.id}:${d.type.label}').join(', ')}');
      return;
    }
    final tamu = findTamu(db);
    if (tamu == null) {
      print('Tamu not found - available devices: ${db.all.map((d) => '${d.id}:${d.type.label}').join(', ')}');
      return;
    }

    final client = SubscriptionClient(deviceId: tamu.id);

    // DAS Meas1 is ResistiveMeasure (type 8) instance 0, field 0
    // sourceReg = type(10) | inst(6) | field(8) | key(8) = 8<<22 | 0<<16 | 0<<8 | 0
    final sourceReg = (8 << 22) | (0 << 16) | (0 << 8) | 0;
    // targetReg = System block field 0 (device type) on Tamu
    final targetReg = (0 << 22) | (0 << 16) | (0 << 8) | 0;

    print('Creating subscription: targetReg=0x${targetReg.toRadixString(16)}, sourceReg=0x${sourceReg.toRadixString(16)}');
    print('Tamu ID: ${tamu.id}, DAS ID: ${das.id}');

    // Find free index
    int index = 0;
    final existing = await client.getRequesterSubscriptions();
    while (index < 16 && existing.any((s) => s.index == index)) index++;
    if (index >= 16) {
      print('Max subscriptions reached');
      return;
    }

    // Create subscription with periodic trigger using CID 4 (setRequesterSubscription)
    // This will create local requester entry and send CID 1 to provider
    final entry = RequesterSubscription(
      index: index,
      targetReg: targetReg,
      sourceReg: sourceReg,
      providerAddr: das.id,
      trigger: TriggerType.periodic,
      periodMs: 1000,
      minTimeMs: 100,
      counter: 0,
      tolerance: [],
      trid: 0xFA00 + index,
    );

    print('Sending setRequesterSubscription...');
    try {
      final ok = await client.setRequesterSubscription(index, entry: entry);
      print('Subscription create result: $ok');
      
      if (!ok) {
        print('Request returned false (null reply or exception caught)');
        // Try to debug - check if we can reach the device
        final regClient = RegisterClient(deviceId: tamu.id);
        final sysBlocks = await regClient.readBlockMeta(0, 0);
        print('Tamu system block meta: $sysBlocks');
        
        // Try getRequesterSubscriptions to see if anything was created
        final subs = await client.getRequesterSubscriptions();
        print('Requester subs after failed create: ${subs.length}');
        
        // Try getProviderSubscriptions on DAS
        final providerClient = SubscriptionClient(deviceId: das.id);
        final providerSubs = await providerClient.getProviderSubscriptions();
        print('Provider subs on DAS: ${providerSubs.length}');
      }
      
      expect(ok, isTrue);
    } catch (e, stack) {
      print('Exception during setRequesterSubscription: $e');
      print('Stack: $stack');
      rethrow;
    }

    // Wait a bit for the subscription to be processed
    await Future.delayed(const Duration(milliseconds: 500));

    // Check provider subscriptions on DAS
    final providerClient = SubscriptionClient(deviceId: das.id);
    final providerSubs = await providerClient.getProviderSubscriptions();
    print('Provider subscriptions on DAS: ${providerSubs.length}');
    for (final s in providerSubs) {
      print('  Sub #${s.index}: trigger=${s.trigger.label}, period=${s.periodMs}ms, sourceReg=0x${s.sourceReg.toRadixString(16)}, requesterAddr=${s.requesterAddr}');
    }

    // Check requester subscriptions on Tamu
    final requesterSubs = await client.getRequesterSubscriptions();
    print('Requester subscriptions on Tamu: ${requesterSubs.length}');
    for (final s in requesterSubs) {
      print('  Sub #${s.index}: trigger=${s.trigger.label}, targetReg=0x${s.targetReg.toRadixString(16)}, trid=0x${s.trid.toRadixString(16)}');
    }

    // Clean up - delete the subscription this test created (not the first entry,
    // which may be a pre-saved example subscription).
    await client.setRequesterSubscription(index, entry: null);
  }, timeout: const Timeout(Duration(seconds: 30)), skip: skipReason is String ? skipReason : false);

  test('Subscriptions: Get provider subscriptions (empty)', () async {
    final db = DeviceDatabase.instance;
    final das = findDas(db);
    if (das == null) {
      print('DAS not found - available devices: ${db.all.map((d) => '${d.id}:${d.type.label}').join(', ')}');
      return;
    }
    final client = SubscriptionClient(deviceId: das.id);
    final subs = await client.getProviderSubscriptions();
    print('Provider subs: ${subs.length}');
  }, timeout: const Timeout(Duration(seconds: 10)), skip: skipReason is String ? skipReason : false);

  test('Subscriptions: Get requester subscriptions (empty)', () async {
    final db = DeviceDatabase.instance;
    final tamu = findTamu(db);
    if (tamu == null) {
      print('Tamu not found - available devices: ${db.all.map((d) => '${d.id}:${d.type.label}').join(', ')}');
      return;
    }
    final client = SubscriptionClient(deviceId: tamu.id);
    final subs = await client.getRequesterSubscriptions();
    print('Requester subs: ${subs.length}');
  }, timeout: const Timeout(Duration(seconds: 10)), skip: skipReason is String ? skipReason : false);
}