import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/types.dart';

/// Regression: the System block reports meta type 0x00, which is the same numeric value as
/// the dynamic "None" tombstone. The Register view must not hide the System block when it
/// filters out empty dynamic slots (the slot type distinguishes them).
void main() {
  ValueInfo meta(int type, {int flags = 0}) => ValueInfo(type: type, flags: flags);

  test('System block slot is never hidden as a tombstone', () {
    // The System block reports meta type 0x00 - the same value as the tombstone - with the
    // ReadOnly flag in the separate flags byte; the slot type keeps it visible.
    expect(
        isHiddenRegisterSlot(systemBlockTypeValue,
            meta(systemBlockTypeValue, flags: ValueFlags.readOnly)),
        isFalse);
  });

  test('dynamic tombstone slots are hidden', () {
    expect(isHiddenRegisterSlot(dynamicTypeBase, meta(0x0000)), isTrue);
  });

  test('live dynamic and static blocks are shown', () {
    // Live dynamic block: its meta type is the owning bank type (0x3F0-0x3F3).
    expect(isHiddenRegisterSlot(dynamicTypeBase, meta(dynamicTypeBase)), isFalse);
    // Static block: meta type LEDButton (0x03).
    expect(isHiddenRegisterSlot(BlockType.ledButton.value, meta(0x0003)), isFalse);
  });
}
