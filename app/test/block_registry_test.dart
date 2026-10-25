import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/block_registry.dart';
import 'package:tamuapp/core/types.dart';

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
