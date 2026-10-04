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

  test('state labels', () {
    expect(ScriptState.label(ScriptState.stopped), 'Stopped');
    expect(ScriptState.label(ScriptState.error), 'Error');
  });
}
