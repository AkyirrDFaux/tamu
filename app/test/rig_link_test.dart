/// The command-line link's receive path, and the upload CLI's shutdown on a refusal.
///
/// Both tests exist because the CLI was deaf AND hung against a healthy rig (2026-10-21): the
/// channel fed wire bytes straight into the packet parser without unwrapping the USB link frames
/// first, so every reply was dropped, and the "no live target" refusal returned with the port
/// still open, so the process never exited.
library;

import 'dart:convert';
import 'dart:io';

import 'package:archive/archive.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/types.dart';

import '../tool/rig_link.dart';
import '../tool/upload_setup.dart';

/// One `Device/Ping` reply, captured off the rig on 2026-10-21 over `/dev/ttyACM0`:
/// `0xFA | crc8 0x50 | length 0x0C | packet (12 bytes) | 0xBF`.
///
/// These are the bytes the core actually answers with, and they are a LINK frame - the packet
/// starts three bytes in. Feeding them to the packet parser alone yields nothing at all.
const List<int> _pingReply = [
  0xfa,
  0x50,
  0x0c,
  0xf4,
  0x0e,
  0x08,
  0x00,
  0x01,
  0x9c,
  0x01,
  0x9c,
  0x01,
  0x00,
  0x00,
  0xf0,
  0xbf,
];

void main() {
  group('RigReceivePath', () {
    test('unwraps the USB link frame: one captured reply is one response', () {
      final frames = RigReceivePath().feed(_pingReply);

      expect(
        frames,
        hasLength(1),
        reason:
            'the link frame is not a packet: without the UsbFrameParser stage the packet '
            'parser resyncs through it and yields no frame at all',
      );
      final frame = frames.single;
      expect(
        frame.isResponse,
        isTrue,
        reason: 'this is a reply, not a request',
      );
      expect(
        frame.srvSource,
        0xf000,
        reason: 'TRID 0xF000 identifies the transaction',
      );
      expect(serviceTypeOf(frame.srvTarget), ServiceType.device);
      expect(
        serviceCidOf(frame.srvTarget),
        1,
        reason: 'Device CID 1 is the ping',
      );
      expect(
        frame.payload,
        isEmpty,
        reason: 'the captured ping reply carries no payload',
      );
    });

    test('assembles the same reply when the reader splits the bytes', () {
      final path = RigReceivePath();
      final frames = <PacketFrame>[];
      // A serial reader delivers arbitrary chunk boundaries; one byte at a time is the worst.
      for (final byte in _pingReply) {
        frames.addAll(path.feed([byte]));
      }

      expect(frames, hasLength(1));
      expect(frames.single.isResponse, isTrue);
      expect(frames.single.srvSource, 0xf000);
    });
  });

  group('upload_setup', () {
    late Directory dir;

    setUp(() => dir = Directory.systemTemp.createTempSync('rig_link_test'));
    tearDown(() {
      installRequestChannel(null);
      if (dir.existsSync()) dir.deleteSync(recursive: true);
    });

    test('a refusal to write closes the link, so the run exits', () async {
      final archive = File('${dir.path}/setup.zip')
        ..writeAsBytesSync(_oneDeviceArchive());
      final link = _FakeRigLink();

      final stopwatch = Stopwatch()..start();
      final code = await runUpload([
        archive.path,
        '--yes',
        '--port',
        '/dev/null-test',
      ], openRig: (port) async => link);
      stopwatch.stop();

      expect(
        link.requests,
        isNotEmpty,
        reason: 'the CLI reached the rig first',
      );
      expect(
        code,
        3,
        reason: 'no live target is the "no rig / refused" exit code',
      );
      expect(
        link.closeCount,
        1,
        reason:
            'an open port keeps the isolate alive: a refusal that does not close the link '
            'hangs instead of exiting',
      );
      expect(stopwatch.elapsed, lessThan(const Duration(seconds: 30)));
    });
  });
}

/// A one-device setup archive: the smallest thing the CLI accepts, enough to build a plan for.
List<int> _oneDeviceArchive() {
  final json = jsonEncode(<String, Object?>{
    'app': 'tamuapp',
    'format': 2,
    'type': 'Tamu v2.0A',
    'typeId': 0x01,
    'id': coreId,
    'name': 'Tamu v2.0A',
    'serial': 'E4B063C820700000000000000000',
    'blocks': <Object?>[],
    'scripts': <Object?>[],
    'files': <Object?>[],
  });
  final bytes = utf8.encode(json);
  final archive = Archive()
    ..addFile(ArchiveFile('1_Tamu_v2.0A.json', bytes.length, bytes));
  return ZipEncoder().encode(archive);
}

/// A link that answers the ping and nothing else, so the CLI reaches the plan (with no live
/// device to build one against) and then has to refuse the write.
class _FakeRigLink implements RigLink {
  final List<String> requests = [];
  int closeCount = 0;
  bool _closed = false;

  @override
  bool get isConnected => !_closed;

  @override
  Future<void> close() async {
    closeCount++;
    _closed = true;
  }

  @override
  int takeTxId() => tridAppBase;

  @override
  void setSubscriptionListener(Function(List<int>)? listener) {}

  @override
  Future<List<int>> request(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
    bool requestFrag = false,
    int? transactionId,
  }) async => (await requestWithFlags(
    targetId,
    service,
    functionCid,
    payload: payload,
    timeout: timeout,
    requestFrag: requestFrag,
    transactionId: transactionId,
  )).payload;

  @override
  Future<PacketResponse> requestWithFlags(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
    bool requestFrag = false,
    int? transactionId,
  }) async {
    requests.add('${idToString(targetId)} $service/$functionCid');
    if (service == ServiceType.device && functionCid == 1) {
      return const PacketResponse(payload: [0], success: true, fail: false);
    }
    throw const LinkException('no answer from a fake link');
  }
}
