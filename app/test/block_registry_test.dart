import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/block_registry.dart';
import 'package:tamuapp/core/types.dart';

/// Guards the app's block/field metadata against the documented schemas
/// (Docs/Modules and blocks/*.md and Docs/Modules/Generic system blocks.md). The firmware
/// schemas must match these field counts/order.
void main() {
  test('block registry matches the documented schemas', () {
    // Fan output: Frequency, Duty.
    expect(blockInfoFor(BlockType.pwm)!.fields.map((f) => f.name),
        ['Frequency', 'Duty']);

    // Button: Button raw state.
    expect(blockInfoFor(BlockType.button)!.fields.length, 1);
    expect(blockInfoFor(BlockType.button)!.fields[0].name, 'Button Raw State');

    // LED: LEDState.
    expect(blockInfoFor(BlockType.led)!.fields.map((f) => f.name), ['LED State']);

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

    // Resistive measurement: Sampling, Sensor type, Filter, Measured value, Current range.
    final meas = blockInfoFor(BlockType.resistiveMeasure)!;
    expect(meas.fields.length, 5);
    expect(meas.fields[2].name, 'Filter Coefficient');
    expect(meas.fields[3].name, 'Measured Value');
    expect(meas.fields[4].name, 'Current Range');
  });
}
