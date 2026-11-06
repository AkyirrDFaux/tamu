import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/types.dart';

/// The printed address is the archive's own form: every `net.device` string an archive carries
/// (`address`, `subscriptions.requester[].provider`, `sndb[].address`) was produced by
/// [idToString] and is read back by [idFromString]. The pair must therefore agree on ONE radix,
/// and that radix is decimal - `39.3` is net 39, not net `0x39` = 57.
void main() {
  group('address form: one radix, decimal on both sides', () {
    test('every archive address form round-trips unchanged', () {
      for (final address in [
        '39.3', // the archived requester provider of card t_82b4537e
        '39.2',
        '39.4',
        '0.1', // a local-net device row
        '0.4',
        '14.2', // a net the rig runs on
        '63.1023', // both fields at their bound
        '63.0',
        '0.0',
        '0.1023',
      ]) {
        final id = idFromString(address);
        expect(id, isNotNull, reason: '"$address" must parse');
        expect(idToString(id!), address, reason: '"$address" must print back');
      }
    });

    test('the net field is decimal, never hex', () {
      // The historical defect: printed decimal, parsed hex, so 39 became 0x39 = 57 on the device.
      expect(idFromString('39.3'), (39 << 10) | 3);
      expect(idFromString('39.3'), isNot((0x39 << 10) | 3));
      expect(idNet(idFromString('39.3')!), 39);
      expect(idToString((39 << 10) | 3), '39.3');
      // And the same for the device field: 10 is ten, not sixteen.
      expect(idDevice(idFromString('0.10')!), 10);
      expect(idToString(idFromString('0.10')!), '0.10');
    });

    test('every net 0..63 round-trips with the device field at its edges', () {
      for (var net = 0; net <= 0x3F; net++) {
        for (final dev in [0, 1, 2, 3, 4, 10, 39, 999, 1023]) {
          final text = '$net.$dev';
          final id = idFromString(text);
          expect(id, (net << 10) | dev, reason: text);
          expect(idNet(id!), net, reason: text);
          expect(idDevice(id), dev, reason: text);
          expect(idToString(id), text, reason: text);
        }
      }
    });

    test('a hex field is not silently re-read in this radix', () {
      expect(idFromString('1a.2'), isNull);
      expect(idFromString('e.1'), isNull);
      expect(idFromString('0x1.2'), isNull);
      expect(idFromString('1.2F'), isNull);
    });

    test('a field wider than its bits is refused, not truncated into range', () {
      expect(idFromString('64.1'), isNull, reason: 'net is 6 bit');
      expect(idFromString('0.1024'), isNull, reason: 'device is 10 bit');
      expect(idFromString('-1.2'), isNull);
      expect(idFromString('1.-2'), isNull);
      expect(idFromString('1.2.3'), isNull);
      expect(idFromString('1'), isNull);
      expect(idFromString('1.'), isNull);
      expect(idFromString(''), isNull);
    });
  });
}
