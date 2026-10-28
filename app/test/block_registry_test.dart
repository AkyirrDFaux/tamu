import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/ble_security.dart';
import 'package:tamuapp/core/block_registry.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/core/wifi_settings.dart';

/// Guards the app's block/field metadata against the documented schemas
/// (Docs/Modules and blocks/*.md and Docs/Modules and blocks/Generic system blocks.md). The firmware
/// schemas must match these field counts/order.
void main() {
  test('block registry matches the documented schemas', () {
    // Fan output: Frequency, Duty.
    expect(blockInfoFor(BlockType.pwm)!.fields.map((f) => f.name), [
      'Frequency',
      'Duty',
    ]);

    // Button: Button raw state.
    expect(blockInfoFor(BlockType.button)!.fields.length, 1);
    expect(blockInfoFor(BlockType.button)!.fields[0].name, 'Button Raw State');

    // LED: LEDState.
    expect(blockInfoFor(BlockType.led)!.fields.map((f) => f.name), [
      'LED State',
    ]);

    // LED-Button: Button (0), LEDState (3) with 1-2 reserved.
    final ledButton = blockInfoFor(BlockType.ledButton)!;
    expect(ledButton.fields.length, 4);
    expect(ledButton.fields[0].name, 'Button Raw State');
    expect(ledButton.fields[3].name, 'LED State');

    // Acc&Gyr: fields 0-4 settings, 5 Acceleration, 6 Angular Velocity (no deadzones).
    final accGyr = blockInfoFor(BlockType.accGyr)!;
    expect(accGyr.fields.length, 7);
    expect(accGyr.fields[3].name, 'Acceleration Filter');
    expect(accGyr.fields[4].name, 'Angular Filter');
    expect(accGyr.fields[5].name, 'Acceleration');
    expect(accGyr.fields[6].name, 'Angular Velocity');

    // LED Display: Brightness, Offset, Render index, Layout file, Refresh rate.
    final display = blockInfoFor(BlockType.vysiDisplay)!;
    expect(display.fields.length, 5);
    expect(display.fields[2].name, 'Render Block Index');
    expect(display.fields[3].name, 'Layout File Name');
    expect(display.fields[4].name, 'Refresh Rate');

    // Resistive measurement: Sampling, Sensor type, Filter, Measured value, Current range,
    // then the two rail flags (firmware Core/Devices/Measuring.h fields 5/6).
    final meas = blockInfoFor(BlockType.resistiveMeasure)!;
    expect(meas.fields.map((f) => f.name), [
      'Sampling Rate',
      'Sensor Type',
      'Filter Coefficient',
      'Measured Value',
      'Current Range',
      'Open Circuit',
      'Short Circuit',
    ]);
    expect(meas.fields[2].name, 'Filter Coefficient');
  });

  // The rail flags are named at the indexes the firmware's ResistiveMeasEntries table uses,
  // so the register page, the backup view and the subscription picker stop rendering
  // "Field 5"/"Field 6". A shift here would silently mislabel both rails.
  test('resistive-measure rail flags resolve at field indices 5 and 6', () {
    final meas = blockInfoFor(BlockType.resistiveMeasure)!;
    expect(meas.field(5)?.name, 'Open Circuit');
    expect(meas.field(6)?.name, 'Short Circuit');
    // The two flags are plain Bools on the wire: no unit, no edit range, no enum labels.
    for (final index in const [5, 6]) {
      final info = meas.field(index)!;
      expect(info.unit, isNull);
      expect(info.min, isNull);
      expect(info.max, isNull);
      expect(info.enumValues, isNull);
      expect(info.transform, isFalse);
    }
  });

  // The WiFi Settings block (firmware BlockType 0x0C, core/wifi_settings.dart). It is a
  // compile-in option, off by default, so the names still have to line up with the block's
  // fixed field order for the builds that do carry it.
  test('the WiFi Settings block names its five fields in table order', () {
    final wifi = blockInfoFor(BlockType.wifiSettings)!;
    expect(wifi.typeName, 'WiFi Settings');
    expect(wifi.fields.map((f) => f.name), [
      'Enabled',
      'SSID',
      'Password',
      'State',
      'IP address',
    ]);
    // Password is the app's one secret field: masked on every read-out, never pre-filled.
    expect(wifi.field(wifiFieldPassword)!.secret, isTrue);
    for (final index in const [
      wifiFieldEnabled,
      wifiFieldSsid,
      wifiFieldState,
      wifiFieldIp,
    ]) {
      expect(wifi.field(index)!.secret, isFalse);
    }
    // State carries the block's own enum labels, so the register row prints "Connected" and
    // not "Enum 2".
    expect(wifi.field(wifiFieldState)!.enumValues, wifiStateLabels);
  });

  // The BLE Settings block (firmware BlockType 0x0B, core/ble_security.dart). The panel reads the
  // fields by number and the register page names them from here, so both the order and the two
  // enum label sets matter.
  test('the BLE Settings block names its nine fields in table order', () {
    final ble = blockInfoFor(BlockType.bleSettings)!;
    expect(ble.typeName, 'BLE Settings');
    expect(ble.fields.map((f) => f.name), [
      'Passkey',
      'Mode',
      'Bond count',
      'Bond address',
      'Window',
      'Action',
      'Enabled',
      'Display name',
      'Advertised name',
    ]);
    // The passkey is a secret in the app's sense: masked on every read-out, never pre-filled.
    expect(ble.field(bleFieldPasskey)!.secret, isTrue);
    expect(ble.field(bleFieldPasskey)!.maxChars, blePasskeyLength);
    // The two enums print the block's own words, and the window carries its unit.
    expect(ble.field(bleFieldMode)!.enumValues, bleModeLabels);
    expect(ble.field(bleFieldAction)!.enumValues, bleActionLabels);
    expect(ble.field(bleFieldWindow)!.unit, 'ms');
    // Address only: the device's bond store keeps no peer name (Issues.md E60).
    expect(ble.field(bleFieldBond)!.name, 'Bond address');
  });

  // Every call site bounds-checks before naming a field (register_page_tiles.dart,
  // register_backup_view.dart, subscriptions_dialog.dart all fall back to "Field N"), so an
  // unknown index must resolve to null rather than throw - a device with a newer firmware
  // must not crash the page.
  test('an unknown field index resolves to null instead of throwing', () {
    final meas = blockInfoFor(BlockType.resistiveMeasure)!;
    expect(meas.field(7), isNull);
    expect(meas.field(99), isNull);
    expect(meas.field(-1), isNull);
  });
}
