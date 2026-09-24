import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/types.dart';

/// Regression: the System block reports meta type 0x00, which is the same numeric value as
/// the dynamic "None" tombstone. The Register view must not hide the System block when it
/// filters out empty dynamic slots (the slot type distinguishes them).
void main() {
  BlockMeta meta(int flagsAndType) => BlockMeta(flagsAndType: flagsAndType);

  test('System block slot is never hidden as a tombstone', () {
    // Firmware sends BlockType::System (0x00) | FieldFlags::ReadOnly (0x0400).
    expect(isHiddenRegisterSlot(systemBlockTypeValue, meta(0x0400)), isFalse);
  });

  test('dynamic tombstone slots are hidden', () {
    expect(isHiddenRegisterSlot(BlockType.dynamic.value, meta(0x0000)), isTrue);
  });

  test('live dynamic and static blocks are shown', () {
    // Live dynamic block: meta type PWM (0x04).
    expect(isHiddenRegisterSlot(BlockType.dynamic.value, meta(0x0004)), isFalse);
    // Static block: meta type LEDButton (0x03).
    expect(isHiddenRegisterSlot(BlockType.ledButton.value, meta(0x0003)), isFalse);
  });
}
