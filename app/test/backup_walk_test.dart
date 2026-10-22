import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/backup.dart';
import 'package:tamuapp/core/backup_format.dart';
import 'package:tamuapp/core/backup_value.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/types.dart';

/// A fake Register service that answers the CIDs used by [walkDeviceBlocks] with one
/// static PWM block and one dynamic block. Every high-level read routes through
/// `request`, so overriding it exercises the real walker end to end.
class _FakeRegister extends RegisterClient {
  _FakeRegister() : super(deviceId: 1);

  static const _pwm = 0x04;
  static const _dyn = 0x3F0; // dynamic global index 0 (banked type 0x3F0)

  @override
  Future<List<int>?> request(
    int cid, {
    List<int> payload = const [],
    Duration? timeout,
  }) async {
    switch (cid) {
      case RegisterCid.enumerateBlocks:
        // PWM (static, one instance, 10.6 split) + one dynamic bank (8.8 split).
        return _words([(_pwm << 6) | 0, (0xF0 << 8) | 0]);
      case RegisterCid.enumerateFields:
        final packed = payload[0] | (payload[1] << 8);
        final type = (packed >> 6) & 0x3FF;
        final inst = packed & 0x3F;
        if (type == _pwm && inst == 0) return _words([0x0000, 0x0100]);
        if (type == _dyn && inst == 0) return _words([0x0000]);
        return null;
      case RegisterCid.read:
        final bi =
            payload[0] |
            (payload[1] << 8) |
            (payload[2] << 16) |
            (payload[3] << 24);
        final type = blockInfoType(bi);
        final inst = blockInfoInstance(bi);
        final field = blockInfoField(bi);
        final key = blockInfoKey(bi);
        if (field == 0xFF) {
          // Block meta: ValueInfo(size = field count) + 16-char name.
          if (type == _pwm && inst == 0) {
            return _reply(DataType.none.value, 2, _padded('Fan'));
          }
          if (type == _dyn && inst == 0) {
            return _reply(_dyn, 1, _padded('Panel'));
          }
          return null; // no System block in this fixture
        }
        if (type == _pwm && inst == 0 && field == 0) {
          // Frequency is a Persistent setting (kept in flash), Duty is not - it is a live output
          // the firmware resets, so a capture archives the first and drops the second.
          return _reply(
            DataType.number.value,
            4,
            numberToBytes(25000),
            flags: ValueFlags.persistent,
          );
        }
        if (type == _pwm && inst == 0 && field == 1) {
          return _reply(DataType.number.value, 4, numberToBytes(50));
        }
        if (type == _dyn && inst == 0 && field == 0 && key == 0) {
          // A volatile row of a dynamic block: the row is device structure, the value is a
          // reading the firmware resets at boot and the archive must not carry.
          return _reply(DataType.number.value, 4, numberToBytes(7));
        }
        return null;
      case DynamicCid.getName:
        // The dynamic name (CID 0x12): a raw 16-char name, no header.
        final idx = payload[0] | (payload[1] << 8);
        if (idx == 0) return _padded('Panel');
        return null;
      default:
        return null;
    }
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

void main() {
  test(
    'the shared walk visits static and dynamic blocks in one order',
    () async {
      final walked = (await walkDeviceBlocks(_FakeRegister()))!.blocks;
      expect(walked, isNotNull);

      final pwm = walked.firstWhere((b) => b.type == 0x04);
      expect(pwm.name, 'Fan');
      expect(pwm.isDynamic, isFalse);
      expect(pwm.fields.map((f) => f.field), [0, 1]);
      expect(pwm.fields.first.fieldName, 'Frequency');
      expect(pwm.fields.first.info?.unit, 'Hz');
      expect(pwm.fields.first.value, numberToBytes(25000));
      expect(pwm.fields.last.fieldName, 'Duty');

      final dyn = walked.firstWhere((b) => b.isDynamic);
      expect(dyn.type, 0x3F0);
      expect(dyn.name, 'Panel');
      expect(dyn.fields.single.fieldName, 'Field 0');
      expect(dyn.fields.single.keyName, 'Key 0');
      expect(dyn.fields.single.value, numberToBytes(7));
    },
  );

  test('capture keeps persistent entries and a dynamic block\'s structure', () async {
    final walked = (await walkDeviceBlocks(_FakeRegister()))!.blocks;

    // A STATIC block's non-persistent field is dropped: its layout is compile-time, so there is
    // nothing structural to archive and the value is a live output.
    final pwm = walked.firstWhere((b) => b.type == 0x04);
    final backupPwm = backupBlockFromVisited(pwm);
    expect(backupPwm.entries.map((e) => e.field), ['Frequency']);
    expect(backupPwm.entries.single.persistent, isTrue);
    expect(backupPwm.entries.single.value, 25000.0);
    expect(backupPwm.entries.single.toJson()['value'], 25000.0);

    // A DYNAMIC block's non-persistent row stays, WITHOUT its value: the row (with its type, flags
    // and size) is the structure a restore rebuilds, and the value is reset at boot.
    final dyn = walked.firstWhere((b) => b.isDynamic);
    final backupDyn = backupBlockFromVisited(dyn);
    final liveDyn = liveBlockFromVisited(dyn);
    expect(backupDyn.entries, hasLength(liveDyn.entries.length));
    final e = backupDyn.entries.single;
    final l = liveDyn.entries.single;
    expect(e.structural, isTrue);
    expect(e.value, isNull);
    expect(e.toJson().containsKey('value'), isFalse);
    expect(e.type, dataTypeWord(DataType.number));
    expect(e.size, 4);
    expect(
      [e.field, e.fieldIndex, e.key, e.keyIndex],
      [l.fieldName, l.field, l.keyName, l.key],
    );
  });

  test(
    'a captured device round-trips through the archive back to live bytes',
    () async {
      final walked = (await walkDeviceBlocks(_FakeRegister()))!.blocks;
      final device = BackupDevice(
        type: 'Tamu v2.0A',
        typeId: 1,
        id: 1,
        name: 'Tamu',
        blocks: [for (final b in walked) backupBlockFromVisited(b)],
      );
      final liveBlocks = [for (final b in walked) liveBlockFromVisited(b)];

      final parsed = parseBackupZip(buildBackupZip([device])).single;
      expect(parsed.blocks, hasLength(device.blocks.length));

      var checked = 0;
      for (final block in parsed.blocks) {
        final live = liveBlocks.firstWhere(
          (l) => l.type == block.typeIndex && l.instance == block.instance,
        );
        for (final e in block.entries) {
          final target = live.entries.firstWhere(
            (l) => l.field == e.fieldIndex && l.key == e.keyIndex,
          );
          // A structural row still matches an address on the target, but plans no write.
          if (!restoresEntryValue(e)) {
            expect(e.structural, isTrue);
            expect(
              [block.isDynamic, e.field],
              [true, 'Field 0'],
              reason: 'only a dynamic block carries a structural row',
            );
            continue;
          }
          final bytes = resolveEntryBytes(e, target);
          expect(bytes, isNotNull, reason: '${block.name} ${e.field}');
          // The bytes restored to the live target re-encode to the archived value.
          expect(
            encodeSemantic(target.meta.dataType, bytes!, info: target.info),
            e.value,
          );
          checked++;
        }
      }
      expect(checked, 1, reason: 'one persistent PWM field');
    },
  );
}
