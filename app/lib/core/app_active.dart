/// System block field 8 `App Active` is a **bit field** (a uint8), not an enum index: one bit
/// per app interface. The firmware's `Enums.h` names the bits USB (0), BLE (1) and WiFi (2) —
/// several may be true at once, so "a phone is paired while the USB host is attached" is
/// `USB | BLE` = 3, a value the old enum could not express at all (it had to pick one winner).
/// Bit 2 is reserved for the WiFi app-link and is never set by the current firmware; the app
/// still names it, so a device that starts reporting it is shown rather than shown as "unknown".
/// Everything in the app that used to read that field as an index (the register row's text, the
/// device view's connection badge) decodes it here, so the bit numbering exists in exactly one
/// place.
library;

/// The interfaces field 8 can report, one bit each.
enum AppInterface {
  usb(0, 'USB'),
  ble(1, 'BLE'),

  /// Reserved: the firmware names the bit but the WiFi app-link is not implemented yet, so it
  /// is never reported. The app still decodes it - showing an interface the device claims is
  /// better than hiding it as "unknown".
  wifi(2, 'WiFi');

  const AppInterface(this.bit, this.label);

  /// The bit this interface occupies in the field.
  final int bit;

  /// The name the app shows for it.
  final String label;

  /// The field mask for this interface.
  int get mask => 1 << bit;
}

/// Every bit this app has a name for. A bit outside it belongs to an interface a newer
/// firmware reports and this build does not know - called out by name instead of being
/// dropped from the read-out.
const int knownAppInterfaceMask = (1 << 0) | (1 << 1) | (1 << 2);

/// The interfaces [bits] reports active, in bit order (USB, BLE, WiFi).
List<AppInterface> activeAppInterfaces(int bits) => [
  for (final interface in AppInterface.values)
    if (bits & interface.mask != 0) interface,
];

/// The bits set in [bits] that name no interface this build knows.
int unknownAppInterfaceBits(int bits) => bits & ~knownAppInterfaceMask;

/// "USB, BLE" - the interfaces [bits] reports active, "none" when it reports nothing, and
/// an explicit `unknown 0x20` for a bit this build cannot name.
String describeAppInterfaces(int bits) {
  final names = [
    for (final interface in activeAppInterfaces(bits)) interface.label,
  ];
  final unknown = unknownAppInterfaceBits(bits);
  if (unknown != 0) names.add('unknown 0x${unknown.toRadixString(16)}');
  return names.isEmpty ? 'none' : names.join(', ');
}

/// The field's value from its wire bytes (little-endian, 1 to 4 bytes wide). The field is
/// one byte today; reading it as a little-endian integer keeps a widened field working.
int appActiveBitsFromBytes(List<int> value) {
  var bits = 0;
  final width = value.length < 4 ? value.length : 4;
  for (var i = 0; i < width; i++) {
    bits |= (value[i] & 0xFF) << (8 * i);
  }
  return bits;
}
