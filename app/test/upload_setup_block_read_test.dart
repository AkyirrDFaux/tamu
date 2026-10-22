/// A lost block read is not a device of the wrong shape.
///
/// On 2026-10-22 a setup upload onto a healthy rig was refused with `REFUSED: the target carries
/// no Button block`: the register block walk lost ONE reply, `readBlocks` skipped that instance,
/// and the plan read the short block list as a device that does not carry the block. The device
/// answered every block moments later, so the diagnosis - not the refusal - was wrong.
///
/// These tests drive the real CLI (`runUpload`) through a fake link, so the plan, the messages
/// and the exit code are the ones a run prints. They cover both halves of the fix: the read is
/// retried (bounded), and a read that still fails is reported AS a read failure.
library;

import 'dart:convert';
import 'dart:io';

import 'package:archive/archive.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/types.dart';

import '../tool/rig_link.dart';
import '../tool/upload_setup.dart';

/// The block type of the archive device this file restores: the rig's own failure.
const int _button = 0x09;

/// A static block the fake device also carries, so the "genuinely absent" case has a live block
/// list that is short for a real reason.
const int _pwm = 0x04;

void main() {
  group('upload_setup: a lost block read', () {
    late Directory dir;

    setUp(() => dir = Directory.systemTemp.createTempSync('block_read_test'));
    tearDown(() {
      installRequestChannel(null);
      if (dir.existsSync()) dir.deleteSync(recursive: true);
    });

    /// Runs the CLI against [rig] with the archive the rig's own setup would carry.
    Future<({int code, String out, String err})> run(_FakeRig rig) async {
      final archive = File('${dir.path}/setup.zip')
        ..writeAsBytesSync(_setupZip());
      final out = _Captured();
      final err = _Captured();
      final code = await IOOverrides.runZoned(
        () => runUpload([
          archive.path,
          '--port',
          '/dev/null-test',
        ], openRig: (_) async => rig),
        stdout: () => out,
        stderr: () => err,
      );
      return (code: code, out: out.text, err: err.text);
    }

    test('an enumeration that loses one reply is retried, and the plan matches', () async {
      final rig = _FakeRig(types: [_button], enumerateFailures: 1);

      final result = await run(rig);

      expect(
        rig.enumerateCalls,
        greaterThanOrEqualTo(2),
        reason:
            'the enumeration that went unanswered was re-sent (the plan walk, before the '
            'read-back that follows a matched plan): one lost reply must not read as an '
            'empty block list',
      );
      expect(result.out, contains('items=1/1 to write'));
      expect(
        result.out,
        isNot(contains('carries no')),
        reason: 'the device does carry the block - the read was lost',
      );
      expect(result.out, isNot(contains('unread')));
      expect(result.code, 0, reason: 'the plan matches, so a dry run is clean');
    });

    test('a block instance read that loses one reply is retried', () async {
      final rig = _FakeRig(types: [_button], buttonMetaFailures: 1);

      final result = await run(rig);

      expect(rig.buttonMetaCalls, 2);
      expect(result.out, contains('items=1/1 to write'));
      expect(result.out, isNot(contains('carries no')));
      expect(result.code, 0);
    });

    test('a Device Type read that loses one reply is retried', () async {
      // The identity's Device Type decides the per-device TYPE refusal, so a lost reply there
      // used to read as a device of the wrong type ("reports Unknown (0)").
      final rig = _FakeRig(types: [_button], typeFieldFailures: 1);

      final result = await run(rig);

      expect(
        rig.typeFieldCalls,
        greaterThanOrEqualTo(2),
        reason: 'the identity read that went unanswered was re-sent',
      );
      expect(result.out, isNot(contains('reports Unknown')));
      expect(result.out, contains('items=1/1 to write'));
      expect(result.code, 0);
    });

    test('an enumeration that never answers names the read failure, not the shape', () async {
      final rig = _FakeRig(
        types: [_button],
        enumerateFailures: RegisterClient.snapshotReadAttempts,
      );

      final result = await run(rig);

      expect(rig.enumerateCalls, RegisterClient.snapshotReadAttempts);
      expect(
        result.out,
        contains(
          'REFUSED: the target 0.1 did not answer its block list read '
          '(${RegisterClient.snapshotReadAttempts} attempts) - unread, not absent',
        ),
      );
      expect(
        result.out,
        isNot(contains('carries no')),
        reason: 'nothing was read, so nothing can be called absent',
      );
      expect(
        result.code,
        3,
        reason: 'an incomplete snapshot is still refused to write',
      );
    });

    test('the rig\'s own case: a Button instance read that never answers', () async {
      final rig = _FakeRig(
        types: [_button],
        buttonMetaFailures: RegisterClient.snapshotReadAttempts,
      );

      final result = await run(rig);

      expect(rig.buttonMetaCalls, RegisterClient.snapshotReadAttempts);
      expect(
        result.out,
        contains(
          'REFUSED: the target did not answer the Button block read '
          '(${RegisterClient.snapshotReadAttempts} attempts) - unread, not absent',
        ),
      );
      expect(
        result.out,
        isNot(contains('carries no Button block')),
        reason: 'this is the message the 2026-10-22 run printed for a healthy rig',
      );
      expect(result.code, 3);
    });

    test('a device that really lacks the block type is refused as before', () async {
      // The device answers everything; it simply carries PWM and no Button block.
      final rig = _FakeRig(types: [_pwm]);

      final result = await run(rig);

      expect(
        result.out,
        contains('REFUSED: the target carries no Button block'),
        reason: 'a wrong-device-type refusal must keep today\'s message',
      );
      expect(
        result.out,
        isNot(contains('not ready:')),
        reason:
            'a refused device is not reported as one about to be written: the refusal ends '
            'the per-device report, as it always did',
      );
      expect(result.out, isNot(contains('unread')));
      expect(result.code, 3, reason: 'and today\'s exit code');
    });
  });
}

/// A one-device setup archive whose device carries a persistent Button entry.
List<int> _setupZip() {
  final json = jsonEncode(<String, Object?>{
    'app': 'tamuapp',
    'format': backupFormatVersion,
    'type': 'Tamu v2.0A',
    'typeId': DeviceType.tamuV20A.value,
    'id': coreId,
    'name': 'Tamu v2.0A',
    'blocks': <Object?>[
      {
        'type': blockTypeDisplayName(_button),
        'typeIndex': _button,
        'instance': 0,
        'name': blockTypeDisplayName(_button),
        'entries': <Object?>[
          {
            'field': 'Field 0',
            'fieldIndex': 0,
            'key': 'Key 0',
            'keyIndex': 0,
            'type': dataTypeWord(DataType.number),
            'flags': ['Persistent'],
            'value': 25000,
            'size': 4,
          },
        ],
      },
    ],
    'scripts': <Object?>[],
    'files': <Object?>[],
  });
  final bytes = utf8.encode(json);
  final archive = Archive()
    ..addFile(ArchiveFile('1_Tamu_v2.0A.json', bytes.length, bytes));
  return ZipEncoder().encode(archive);
}

/// A rig whose Register service answers a fixed block list, and can lose a chosen number of
/// replies: the first [enumerateFailures] block-type enumerations and the first
/// [buttonMetaFailures] Button instance reads go unanswered, exactly as a lost reply does.
class _FakeRig implements RigLink {
  _FakeRig({
    required this.types,
    this.enumerateFailures = 0,
    this.buttonMetaFailures = 0,
    this.typeFieldFailures = 0,
  });

  /// The block types the device reports, one instance each.
  final List<int> types;
  final int enumerateFailures;
  final int buttonMetaFailures;
  final int typeFieldFailures;

  int enumerateCalls = 0;
  int buttonMetaCalls = 0;
  int typeFieldCalls = 0;
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
    final reply = _answer(service, functionCid, payload);
    if (reply == null) throw const LinkException('no answer from the fake rig');
    return PacketResponse(payload: reply, success: true, fail: false);
  }

  List<int>? _answer(ServiceType service, int cid, List<int> payload) {
    // The ping every identity read starts with.
    if (service == ServiceType.device && cid == 1) return const [0];
    // Storage / Scripts / Subscriptions are not this fake's business: unanswered.
    if (service != ServiceType.register) return null;
    switch (cid) {
      case RegisterCid.enumerateBlocks:
        enumerateCalls++;
        if (enumerateCalls <= enumerateFailures) return null;
        return _words([for (final type in types) type << 6]);
      case RegisterCid.enumerateFields:
        final packed = payload[0] | (payload[1] << 8);
        if (!types.contains((packed >> 6) & 0x3FF)) return null;
        return _words([0x0000]); // field 0, key 0
      case RegisterCid.read:
        final bi =
            payload[0] |
            (payload[1] << 8) |
            (payload[2] << 16) |
            (payload[3] << 24);
        final type = blockInfoType(bi);
        final field = blockInfoField(bi);
        final key = blockInfoKey(bi);
        if (type == systemBlockTypeValue && field != 0xFF) {
          return _systemField(field, key);
        }
        if (field == 0xFF) {
          if (type == systemBlockTypeValue) {
            return _reply(DataType.none.value, 9, _padded('System'));
          }
          if (type == _button) {
            buttonMetaCalls++;
            if (buttonMetaCalls <= buttonMetaFailures) return null;
          }
          if (!types.contains(type)) return null;
          return _reply(
            DataType.none.value,
            2,
            _padded(blockTypeDisplayName(type)),
          );
        }
        // The Button's one persistent entry.
        if (type == _button && field == 0 && key == 0) {
          return _reply(
            DataType.number.value,
            4,
            numberToBytes(25000),
            flags: ValueFlags.persistent,
          );
        }
        return null;
      default:
        return null;
    }
  }

  /// The System fields `readDeviceIdentity` reads: Device Type (1 = Tamu v2.0A), the serial and
  /// the name. Everything else reads as zero, which is a value, not a lost read.
  List<int>? _systemField(int field, int key) {
    final value = List<int>.filled(32, 0);
    if (field == 0 && key == 0) {
      typeFieldCalls++;
      if (typeFieldCalls <= typeFieldFailures) return null;
      value[0] = DeviceType.tamuV20A.value;
    }
    if (field == 6) {
      const name = 'Tamu v2.0A';
      for (var i = 0; i < name.length; i++) {
        value[i] = name.codeUnitAt(i);
      }
    }
    return _reply(DataType.number.value, value.length, value);
  }

  /// A reply is a 4-byte echoed BlockInfo, the 4-byte ValueInfo, then the payload.
  static List<int> _reply(
    int type,
    int size,
    List<int> payload, {
    int flags = 0,
  }) => [
    0,
    0,
    0,
    0,
    type & 0xFF,
    (type >> 8) & 0xFF,
    size,
    flags,
    ...payload,
  ];

  static List<int> _words(List<int> words) => [
    for (final w in words) ...[w & 0xFF, (w >> 8) & 0xFF],
  ];

  static List<int> _padded(String s) {
    final out = s.codeUnits.take(16).toList();
    while (out.length < 16) {
      out.add(0x00);
    }
    return out;
  }
}

/// Captures the CLI's stdout/stderr, so a test asserts on the text a run really prints.
class _Captured implements Stdout {
  final StringBuffer _buffer = StringBuffer();

  String get text => _buffer.toString();

  @override
  void write(Object? object) => _buffer.write(object ?? '');

  @override
  void writeln([Object? object = '']) => _buffer.writeln(object ?? '');

  // Only write/writeln are used by the CLI; the rest of the IOSink surface is not.
  @override
  dynamic noSuchMethod(Invocation invocation) => null;
}
