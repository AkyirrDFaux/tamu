import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/app_active.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/ui/formatters.dart';

/// System block field 8 `App Active` is a bit field: USB (0), BLE (1), WiFi (2). The old
/// reading was an enum index, so a device that was on USB and BLE at once had to be reported
/// as one of them; these cases pin the bit numbering, the multi-interface read-out, and the
/// register row's text.
void main() {
  group('the bit numbering', () {
    test('the interfaces occupy bits 0, 1 and 2', () {
      expect(AppInterface.usb.bit, 0);
      expect(AppInterface.ble.bit, 1);
      expect(AppInterface.wifi.bit, 2);
      expect(AppInterface.usb.mask, 1);
      expect(AppInterface.ble.mask, 2);
      expect(AppInterface.wifi.mask, 4);
      expect(knownAppInterfaceMask, 0x07);
    });

    test('every interface is in bit order, which is the display order', () {
      expect(
        [for (final i in AppInterface.values) i.label],
        ['USB', 'BLE', 'WiFi'],
      );
      expect([for (final i in AppInterface.values) i.bit], [0, 1, 2]);
    });
  });

  group('decoding a field value', () {
    test('one bit names one interface', () {
      expect([for (final i in activeAppInterfaces(1)) i.label], ['USB']);
      expect([for (final i in activeAppInterfaces(2)) i.label], ['BLE']);
      expect([for (final i in activeAppInterfaces(4)) i.label], ['WiFi']);
    });

    test('two interfaces at once is the case the enum could not report', () {
      // USB host attached while a phone is paired.
      expect([for (final i in activeAppInterfaces(3)) i.label], ['USB', 'BLE']);
      // A device on its own WiFi link with the USB host attached.
      expect(
        [for (final i in activeAppInterfaces(5)) i.label],
        ['USB', 'WiFi'],
      );
      expect(
        [for (final i in activeAppInterfaces(7)) i.label],
        ['USB', 'BLE', 'WiFi'],
      );
    });

    test('no bit set means no interface', () {
      expect(activeAppInterfaces(0), isEmpty);
      expect(describeAppInterfaces(0), 'none');
    });

    test('a bit this build does not know is kept in the read-out', () {
      expect(unknownAppInterfaceBits(0x07), 0);
      expect(unknownAppInterfaceBits(0x20), 0x20);
      expect(describeAppInterfaces(0x20), 'unknown 0x20');
      expect(describeAppInterfaces(0x21), 'USB, unknown 0x20');
    });

    test(
      'the wire bytes are read little-endian and a widened field still decodes',
      () {
        expect(appActiveBitsFromBytes(const [1]), 1);
        expect(appActiveBitsFromBytes(const [3]), 3);
        // Two bytes: bit 8, i.e. a bit the app has no name for.
        expect(appActiveBitsFromBytes(const [0, 1]), 0x100);
        expect(appActiveBitsFromBytes(const []), 0);
      },
    );
  });

  group('the register row', () {
    String row(List<int> value) =>
        formatSystemValue(DataType.enum_, value, 8, 0);

    test('prints the interfaces, not an index', () {
      expect(row(const [1]), 'USB');
      expect(row(const [2]), 'BLE');
      expect(row(const [3]), 'USB, BLE');
      expect(row(const [5]), 'USB, WiFi');
      expect(row(const [0]), 'none');
    });

    test(
      'an older firmware that only ever sets one bit still reads correctly',
      () {
        // The pre-bit-field firmware answered 0/1/2; each is a valid bit pattern too, and the
        // row names it the same way it always did.
        expect(row(const [0]), 'none');
        expect(row(const [1]), 'USB');
        expect(row(const [2]), 'BLE');
      },
    );

    test('the field decodes as bits whatever type the device declares', () {
      // The firmware keeps DataType::Enum on the entry; a build that widened it to Uint32
      // must still print interfaces, not a number.
      expect(formatSystemValue(DataType.uint32, const [3], 8, 0), 'USB, BLE');
    });
  });
}
