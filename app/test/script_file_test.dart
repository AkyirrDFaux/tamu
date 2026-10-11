import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/types.dart';

void main() {
  test('script file build/parse round-trip', () {
    final image = ScriptFileBuilder(
      properties: ScriptProperties.loadOnBoot | ScriptProperties.runOnLoad,
      inputs: [ValueInfo(type: DataType.number.value, size: 4)],
      outputs: [ValueInfo(type: DataType.number.value, size: 4)],
      variables: [
        ValueInfo(type: DataType.number.value, size: 4),
        ValueInfo(type: DataType.number.value, size: 4),
      ],
      constants: [ValueInfo(type: DataType.number.value, size: 4)],
      inputDefaults: [numberToBytes(3.5)],
      constantValues: [numberToBytes(9.0)],
      instructions: const [0x01, 0, 0, 0, 0x07, 0, 0, 0],
      functionName: 'Blink',
    ).build();

    final parsed = ScriptFileData.parse(image);
    expect(parsed.properties,
        ScriptProperties.loadOnBoot | ScriptProperties.runOnLoad);
    expect(parsed.inputs.length, 1);
    expect(parsed.outputs.length, 1);
    expect(parsed.variables.length, 2);
    expect(parsed.constants.length, 1);
    expect(parsed.inputs.first.dataType, DataType.number);
    expect(parsed.inputs.first.size, 4);
    expect(parsed.instructions.length, 8);
    expect(parsed.functionName, 'Blink');
    expect(numberFromBytes(parsed.inputDefaults), closeTo(3.5, 1e-6));
    expect(numberFromBytes(parsed.constantValues), closeTo(9.0, 1e-6));
  });

  test('script value info flags/type packing', () {
    final info = ValueInfo(
        type: DataType.number.value, size: 4, flags: ValueFlags.readOnly);
    final bytes = info.toBytes();
    final back = ValueInfo.fromBytes(bytes);
    expect(back.dataType, DataType.number);
    expect(back.size, 4);
    expect(back.flags & ValueFlags.readOnly, ValueFlags.readOnly);
  });

  test('truncated script file is rejected', () {
    final image = ScriptFileBuilder(
      inputs: [ValueInfo(type: DataType.number.value, size: 4)],
      inputDefaults: [numberToBytes(1.0)],
    ).build();
    expect(
      () => ScriptFileData.parse(image.sublist(0, image.length - 2)),
      throwsA(isA<FormatException>()),
    );
  });

  test('header is a 16-bit Properties word + five counts + the four lengths', () {
    final image = ScriptFileBuilder(
      properties: scriptPropertiesWithSlot(
          ScriptProperties.loadOnBoot | ScriptProperties.functionLibrary, 7),
      inputs: [ValueInfo(type: DataType.number.value, size: 4)],
      outputs: [ValueInfo(type: DataType.bool_.value, size: 1)],
      variables: [ValueInfo(type: DataType.number.value, size: 4)],
      constants: [ValueInfo(type: DataType.number.value, size: 4)],
      inputDefaults: [numberToBytes(1.0)],
      constantValues: [numberToBytes(2.0)],
      functionReservations: [
        [for (var i = 0; i < scriptFunctionReservationSize; i++) i],
      ],
      instructions: const [0x01, 0, 0, 0],
    ).build();

    // Properties is a 16-bit little-endian word at bytes 0-1.
    expect(image[0] | (image[1] << 8),
        ScriptProperties.loadOnBoot | ScriptProperties.functionLibrary | (7 << 8));
    // Five uint8 counts (Input, Output, Variable, Constant, Function), then a padding byte.
    expect(image[2], 1);
    expect(image[3], 1);
    expect(image[4], 1);
    expect(image[5], 1);
    expect(image[6], 1); // function count
    expect(image[7], 0); // reserved padding

    final parsed = ScriptFileData.parse(image);
    expect(parsed.properties & ScriptProperties.functionLibrary,
        ScriptProperties.functionLibrary);
    expect(scriptPropertiesSlot(parsed.properties), 7);
    expect(parsed.functionCount, 1);
    expect(parsed.functionReservations, hasLength(scriptFunctionReservationSize));
    expect(parsed.functionReservations.first, 0);
    expect(parsed.functionReservations.last, scriptFunctionReservationSize - 1);
  });

  test('the boot slot round-trips independently of the flag bits', () {
    var p = ScriptProperties.loadOnBoot;
    p = scriptPropertiesWithSlot(p, 200);
    expect(scriptPropertiesSlot(p), 200);
    expect(p & ScriptProperties.loadOnBoot, ScriptProperties.loadOnBoot);
    p = scriptPropertiesWithSlot(p, 3); // re-slotting leaves the flags alone
    expect(scriptPropertiesSlot(p), 3);
    expect(p & ScriptProperties.loadOnBoot, ScriptProperties.loadOnBoot);
  });

  test('state labels', () {
    expect(ScriptState.label(ScriptState.stopped), 'Stopped');
    expect(ScriptState.label(ScriptState.error), 'Error');
  });
}
