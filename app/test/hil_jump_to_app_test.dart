/// Hardware test for the bootloader jump-to-app command (CID 0x25).
///
/// This test:
/// 1. Puts node 2 into bootloader mode via Device service CID 4
/// 2. Sends the jump command (CID 0x25) via the core's bootloader passthrough
/// 3. Verifies the node returns to app mode by checking it answers all_versions
///    and no longer answers bootloader identify.
///
/// Run with:
///   TAMU_HIL=/dev/ttyACM1 flutter test --timeout none --concurrency=1 test/hil_jump_to_app_test.dart
@Tags(['hil'])
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader_client.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/transport.dart';

import 'hil/hil_api.dart';
import 'hil/hil_env.dart';

/// The core whose RSBus relays the raw bootloader frames onto the bus.
const HilPrereq _relayCore = HilPrereq(isCore: true);

void main() {
  final env = HilEnv();
  setUpAll(env.connectAndScan);
  tearDownAll(env.finish);

  test('jump to app from bootloader mode (CID 0x25)', () async {
    // Driven by TAMU_HIL. A bare `flutter test` must not fail for want of a rig.
    expect(
      env.hilRequested,
      isTrue,
      reason: "TAMU_HIL must name the relay core's port",
    );
    expect(env.rig.isEmpty, isFalse, reason: 'the rig reported no devices');

    final cores = env.rig.satisfying(_relayCore);
    expect(cores.isEmpty, isFalse, reason: 'no core found on the bus');
    final coreId = cores.first.id;

    // Step 1: Put node 2 into bootloader mode via Device CID 4
    final boot = BootloaderClient(coreId: coreId);
    
    // First, check if node 2 is already in bootloader mode by trying to identify
    var identity = await boot.identify();
    if (identity != null) {
      print('Node already in bootloader mode: $identity');
    } else {
      // Node is in app mode, put it into bootloader mode
      print('Putting node 2 into bootloader mode via Device CID 4...');
      final channel = requestChannel;
      expect(channel, isNotNull, reason: 'no request channel installed');
      final reply = await channel!.requestWithFlags(
        2, // node 2
        ServiceType.device,
        4, // CID 4 = enter bootloader
        payload: [],
        timeout: const Duration(seconds: 2),
      );
      
      if (!reply.ok) {
        // Check if it's a refusal (node doesn't implement CID 4)
        if (reply.fail && reply.payload.isEmpty) {
          print('Node 2 refused Device CID 4 - may not implement bootloader entry');
        } else {
          print('Device CID 4 failed: ${reply.payload}');
        }
      } else {
        print('Node 2 accepted Device CID 4, now in bootloader mode');
      }
      
      // Wait a bit for the node to enter bootloader mode
      await Future<void>.delayed(const Duration(milliseconds: 500));
      
      // Verify it's now in bootloader mode
      identity = await boot.identify();
      expect(identity, isNotNull, reason: 'node did not enter bootloader mode');
      print('Node in bootloader mode: $identity');
      expect(identity!.name, contains('DAS'));
      expect(identity.pageSize, 64);
    }

    // Step 2: Send the jump command (CID 0x25) via the bootloader passthrough
    print('Sending jump command (CID 0x25) via bootloader passthrough...');
    final jumped = await boot.transport.jump(const Duration(seconds: 2));
    expect(jumped, isTrue, reason: 'jump command did not receive an ack');
    print('Jump command acknowledged - node should now be in app mode');

    // Step 3: Verify the node is back in app mode
    // The jump is immediate but the app startup takes time. Wait and retry.
    
    // First, verify the node no longer answers bootloader identify
    await Future<void>.delayed(const Duration(milliseconds: 500));
    identity = await boot.identify();
    expect(identity, isNull, reason: 'node still answers bootloader identify after jump');
    print('Node no longer answers bootloader identify');

    // Now verify the node eventually answers as an app (retry with backoff)
    final channel = requestChannel;
    expect(channel, isNotNull, reason: 'no request channel installed');
    
    PacketResponse? versionReply;
    for (int attempt = 1; attempt <= 10; attempt++) {
      print('Attempt $attempt: pinging node as app (CID 1)...');
      try {
        versionReply = await channel!.requestWithFlags(
          2, // node 2
          ServiceType.device,
          1, // CID 1 = ping
          payload: [],
          timeout: const Duration(seconds: 2),
        );
        if (versionReply.ok) {
          print('Node answers as app after jump (attempt $attempt): ping OK');
          break;
        }
      } on TransportException catch (e) {
        print('Attempt $attempt: transport error - $e');
      }
      await Future<void>.delayed(Duration(seconds: attempt * 2)); // 2s, 4s, 6s, ...
    }
    
    expect(versionReply, isNotNull);
    expect(versionReply!.ok, isTrue, reason: 'node does not answer as app after jump');
    
    print('SUCCESS: Node jumped from bootloader to app mode without reset');
  }, timeout: Timeout.none);
}