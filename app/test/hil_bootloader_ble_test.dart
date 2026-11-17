/// Bootloader v2 upload over BLE transport.
///
/// Tests the full bootloader v2 protocol (identify, read, erase, write, CRC, jump)
/// over the BLE transport path: app -> BLE -> core -> RSBus -> node bootloader.
///
/// Requirements:
/// - Node 2 (or target node) must be in bootloader mode
/// - Core must be reachable over BLE
/// - Run with: TAMU_BLE_DEVICE=<MAC> TAMU_DAS_IMAGE=... flutter test ...
///
/// The test uses the BLE transport to send bootloader passthrough commands
/// (CIDs 0x20-0x25) to the core over BLE, which relays them over RSBus.
@Tags(['hil'])
library;

import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader_client.dart';
import 'package:tamuapp/core/bootloader.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/transport.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/core/request_channel.dart';

import 'hil/hil_env.dart';

/// The BLE transport bootloader wrapper.
class BleBootloaderTransport implements BootloaderTransport {
  final String deviceId;
  final int coreId;

  BleBootloaderTransport({required this.deviceId, this.coreId = 1});

  ConnectionManager get _link => ConnectionManager.instance;

  @override
  Future<BootloaderIdentity?> identify(Duration timeout) async {
    final reply = await _ask(
      'identify',
      0x20,
      payload: [],
      timeout: timeout,
    );
    if (!reply.ok) throw _classify('identify', reply);
    if (reply.payload.length < 4 + 4) {
      throw const PassthroughFailure(
        PassthroughReason.unreported,
        'identify: the reply carried no page size',
      );
    }
    final appSize = uint32FromBytes(reply.payload, 0);
    final pageSize = uint32FromBytes(reply.payload, 4);
    final nameBytes = reply.payload.sublist(8);
    var n = nameBytes.length;
    while (n > 0 && nameBytes[n - 1] == 0) n--;
    return BootloaderIdentity(
      appSize: appSize,
      pageSize: pageSize,
      name: String.fromCharCodes(reply.payload.sublist(8, 8 + n)),
    );
  }

  @override
  Future<Uint8List> readChunk(int offset, Duration timeout) async {
    final reply = await _ask(
      'read 0x${offset.toRadixString(16)}',
      0x21,
      payload: uint32ToBytes(offset),
      timeout: timeout,
    );
    if (reply.payload.length < 4 + Bootloader.payloadSize) {
      throw PassthroughFailure(
        PassthroughReason.unreported,
        'read 0x${offset.toRadixString(16)}: the reply was '
        '${reply.payload.length} bytes, not 36',
      );
    }
    return Uint8List.fromList(
      reply.payload.sublist(4, 4 + Bootloader.payloadSize),
    );
  }

  @override
  Future<int> erasePage(int offset, Duration timeout) async {
    final reply = await _ask(
      'erase 0x${offset.toRadixString(16)}',
      0x22,
      payload: uint32ToBytes(offset),
      timeout: timeout,
    );
    if (reply.payload.length < 4) {
      throw PassthroughFailure(
        PassthroughReason.unreported,
        'erase 0x${offset.toRadixString(16)}: the reply carried no page',
      );
    }
    final page = uint32FromBytes(reply.payload, 0);
    if (Bootloader.ackFailed(page)) {
      throw PassthroughFailure(
        PassthroughReason.nodeRefused,
        'erase 0x${offset.toRadixString(16)}',
        code: PassthroughReason.nodeRefused.code,
      );
    }
    return page;
  }

  @override
  Future<WriteAck> writeChunk(
    int offset,
    List<int> data,
    Duration timeout,
  ) async {
    final reply = await _ask(
      'write 0x${offset.toRadixString(16)}',
      0x23,
      payload: [...uint32ToBytes(offset), ...data],
      timeout: timeout,
    );
    final acked = reply.payload.length >= 4
        ? uint32FromBytes(reply.payload, 0)
        : null;
    return WriteAck(true, acked: acked);
  }

  @override
  Future<int> crc(Duration timeout) async {
    final reply = await _ask('CRC', 0x24, timeout: timeout);
    if (reply.payload.length < 4) {
      throw const PassthroughFailure(
        PassthroughReason.unreported,
        'CRC: the reply carried no value',
      );
    }
    return uint32FromBytes(reply.payload, 0);
  }

  @override
  Future<bool> jump(Duration timeout) async {
    final reply = await _ask('jump', 0x25, timeout: timeout);
    return reply.payload.length >= 4;
  }

  @override
  Future<bool> enterBootloader(Duration timeout) async {
    final reply = await _ask('enter bootloader', 0x04, timeout: timeout);
    // The node answers with ack and then enters bootloader mode.
    return reply.ok;
  }

  Future<PacketResponse> _ask(
    String what,
    int cid, {
    List<int> payload = const [],
    required Duration timeout,
  }) async {
    final reply;
    try {
      reply = await _link.requestWithFlags(
        1,
        ServiceType.device,
        cid,
        payload: payload,
        timeout: timeout,
      );
    } on TransportException {
      throw PassthroughFailure(PassthroughReason.linkLost, what);
    }
    if (!reply.ok) throw _classify(what, reply);
    return reply;
  }

  static PassthroughFailure _classify(String what, PacketResponse reply) {
    final payload = reply.payload;
    if (payload.length == 1) {
      return PassthroughFailure(
        PassthroughReason.fromCode(payload[0]),
        what,
        code: payload[0],
      );
    }
    if (payload.length == 4) {
      return PassthroughFailure(PassthroughReason.nodeRefused, what);
    }
      return PassthroughFailure(PassthroughReason.unreported, what);
      }
    }

@Tags(['hil'])
void main() {
  final env = HilEnv();
  setUpAll(env.connectAndScan);
  tearDownAll(env.finish);

  test('bootloader v2 upload over BLE transport', () async {
    final imagePath = Platform.environment['TAMU_DAS_IMAGE'];
    final bleDeviceId = Platform.environment['TAMU_BLE_DEVICE'];

    if (imagePath == null || bleDeviceId == null) {
      markTestSkipped('requires TAMU_DAS_IMAGE and TAMU_BLE_DEVICE');
      return;
    }

    final image = File(imagePath);
    expect(image.existsSync(), isTrue, reason: 'no image at $imagePath');

    final boot = BootloaderClient(
      transport: BleBootloaderTransport(deviceId: bleDeviceId),
      requestTimeout: const Duration(seconds: 15), // BLE needs more time
    );

    final identity = await boot.identify();
    expect(identity, isNotNull, reason: 'no node answered identify');
    final appSize = identity!.appSize;

    final bytes = Uint8List.fromList(image.readAsBytesSync());
    print('UPLOAD ${image.path}: ${bytes.length} B into a $appSize B app space');

    final result = await boot.flash(
      bytes,
      maxImageSize: appSize,
      onProgress: (p) {
        print('  ${p.phase.name} ${p.completed}/${p.total}');
      },
    );

    print(
      'RESULT: ${result.outcome.label} (crc=${result.verify.crc}) '
      'chunksWritten=${result.chunksWritten} reads=${result.reads}',
    );
  }, timeout: Timeout(Duration(minutes: 15)));
}