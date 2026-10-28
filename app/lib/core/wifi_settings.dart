/// The `WiFi Settings` block (BlockType `0x0C`): the WiFi link's settings and state as
/// register fields, the same shape the BLE link's settings block has.
///
/// It is a **compile-in option in the firmware and off by default**, so on a stock core the
/// block does not exist at all. The app therefore treats it as optional: a caller shows the
/// panel only when the block came back in the device's own block enumeration
/// ([hasWifiSettingsBlock]), and renders nothing when it did not.
///
/// Field order is the block's table order, and it is fixed here because the panel and the block
/// registry both address the fields by number:
///
///   0 Enabled     Bool, persistent    false = the radio is not used at all
///   1 SSID        Text, persistent    the network to join
///   2 Password    Text, write-only    a read answers empty; the device never reads it back
///   3 State       Enum, read-only     0 Disconnected, 1 Connecting, 2 Connected, 3 No SSID,
///                                     4 No password, 5 Wrong password, 6 Error
///   4 IP address  Undefined, 16 bytes, read-only, network order. All-zero is "no address";
///                 IPv4 is reported IPv4-mapped (`::ffff:a.b.c.d`). This is the only side that
///                 formats it - see [wifiAddressText].
library;

import 'types.dart';

/// Field numbers of the `WiFi Settings` block, in table order.
const int wifiFieldEnabled = 0;
const int wifiFieldSsid = 1;
const int wifiFieldPassword = 2;
const int wifiFieldState = 3;
const int wifiFieldIp = 4;

/// How many fields the block carries (0..4).
const int wifiFieldCount = 5;

/// The block type the firmware assigns to `WiFi Settings` (`BlockType::WifiSettings`).
const int wifiSettingsBlockTypeValue = 0x0C;

/// `State` values, exactly as the firmware numbers them.
const int wifiStateDisconnected = 0;
const int wifiStateConnecting = 1;
const int wifiStateConnected = 2;
const int wifiStateNoSsid = 3;
const int wifiStateNoPassword = 4;
const int wifiStateWrongPassword = 5;
const int wifiStateError = 6;

const Map<int, String> wifiStateLabels = {
  wifiStateDisconnected: 'Disconnected',
  wifiStateConnecting: 'Connecting',
  wifiStateConnected: 'Connected',
  wifiStateNoSsid: 'No SSID',
  wifiStateNoPassword: 'No password',
  wifiStateWrongPassword: 'Wrong password',
  wifiStateError: 'Error',
};

/// The label for a `State` value, `State N` for one this build does not know.
String wifiStateLabel(int state) => wifiStateLabels[state] ?? 'State $state';

/// Whether the device's block enumeration carries the WiFi Settings block. The block is a
/// compile-in option, so its absence is the normal case and not an error.
bool hasWifiSettingsBlock(Iterable<({int type, int maxInstance})> types) =>
    types.any((t) => t.type == wifiSettingsBlockTypeValue);

/// One reading of the block's fields, as the device reported them.
///
/// Nothing here is inferred: every value is what the block holds. [password] is the device's
/// read-back, which the block always answers empty (the field is write-only) - the app must
/// never paper over that by showing what it once wrote.
class WifiSnapshot {
  const WifiSnapshot({
    this.enabled = false,
    this.ssid = '',
    this.password = '',
    this.state = wifiStateDisconnected,
    this.ip = '',
  });

  /// Builds the snapshot from the five field reads, ignoring fields the device did not
  /// answer for (they keep the empty default, which the panel shows as `-`).
  factory WifiSnapshot.fromFields(
    Map<int, ({ValueInfo meta, List<int> value})?> reads,
  ) {
    List<int> bytesOf(int field) => reads[field]?.value ?? const <int>[];
    final state = bytesOf(wifiFieldState);
    return WifiSnapshot(
      enabled: bytesOf(wifiFieldEnabled).any((b) => b != 0),
      ssid: textOfCharacters(bytesOf(wifiFieldSsid)),
      password: textOfCharacters(bytesOf(wifiFieldPassword)),
      // A missing State read stays Disconnected: the field's own "no state" value.
      state: state.isEmpty ? wifiStateDisconnected : state[0],
      ip: wifiAddressText(bytesOf(wifiFieldIp)),
    );
  }

  final bool enabled;
  final String ssid;

  /// What the device reads back for the password; the block answers empty (write-only).
  final String password;
  final int state;

  /// The address the device holds, already formatted (empty = no address).
  final String ip;

  /// The state the panel reports for this reading.
  WifiCondition get condition => wifiCondition(this);

  /// True when the block carries nothing at all: no network, no secret, no address.
  bool get isEmpty => ssid.isEmpty && password.isEmpty && ip.isEmpty;

  /// True when the device reads the password back, so the app may show it (masked, with a
  /// deliberate reveal). The block is write-only, so this stays false on real firmware; the
  /// app still asks the value instead of assuming, because a build may change that.
  bool get passwordReadsBack => password.isNotEmpty;

  @override
  String toString() =>
      'WifiSnapshot(enabled: $enabled, ssid: "$ssid", state: ${wifiStateLabel(state)}, '
      'ip: "$ip", password: ${passwordReadsBack ? 'read back' : 'not read back'})';
}

/// What the panel says about a reading.
enum WifiCondition {
  /// The block exists but carries nothing: State Disconnected and every other field empty.
  /// On a firmware build with the WiFi option compiled out of the radio path that is the
  /// honest answer, and the panel says so instead of drawing an empty network.
  notImplemented,

  /// The block holds a network (or an address) but the radio is switched off.
  disabled,

  /// The device says it has no SSID to join (State 3).
  noNetwork,

  /// The device says it has no password for the network (State 4).
  noPassword,

  /// The device rejected the password (State 5).
  wrongPassword,

  /// The device reports a WiFi error (State 6).
  error,

  /// Switched on and joining (State 1).
  connecting,

  /// Switched on and joined (State 2).
  connected,

  /// Switched on, idle, with a network configured (State 0).
  idle,

  /// A State value this build does not know; the panel prints the number instead of guessing.
  unknownState,
}

/// Classifies one reading.
///
/// The device's `State` is the answer wherever it says something: a block that reports
/// Connecting, Connected, No SSID, No password, Wrong password or Error is a working radio
/// whatever the `Enabled` flag says, and reporting it as "disabled" would be the panel lying
/// about the device.
///
/// "Not implemented" is decided last, and only on the shape a stub block has: State
/// Disconnected, the radio switched off, and nothing else in the block at all - no network, no
/// secret, no address. An enabled block with nothing to join is [WifiCondition.noNetwork]
/// instead, because that is a setting the user can act on rather than a missing feature.
WifiCondition wifiCondition(WifiSnapshot snapshot) {
  switch (snapshot.state) {
    case wifiStateConnecting:
      return WifiCondition.connecting;
    case wifiStateConnected:
      return WifiCondition.connected;
    case wifiStateNoSsid:
      return WifiCondition.noNetwork;
    case wifiStateNoPassword:
      return WifiCondition.noPassword;
    case wifiStateWrongPassword:
      return WifiCondition.wrongPassword;
    case wifiStateError:
      return WifiCondition.error;
  }
  if (snapshot.state != wifiStateDisconnected) {
    return WifiCondition.unknownState;
  }
  if (!snapshot.enabled) {
    return snapshot.isEmpty
        ? WifiCondition.notImplemented
        : WifiCondition.disabled;
  }
  if (snapshot.ssid.isEmpty) return WifiCondition.noNetwork;
  return WifiCondition.idle;
}

/// The headline the panel shows for [condition].
String wifiConditionHeadline(WifiCondition condition) => switch (condition) {
  WifiCondition.notImplemented => 'Not implemented on this device',
  WifiCondition.disabled => 'WiFi is disabled',
  WifiCondition.noNetwork => 'No network configured',
  WifiCondition.noPassword => 'No password set',
  WifiCondition.wrongPassword => 'Wrong password',
  WifiCondition.error => 'WiFi error',
  WifiCondition.connecting => 'Connecting',
  WifiCondition.connected => 'Connected',
  WifiCondition.idle => 'Enabled, not connected',
  WifiCondition.unknownState => 'Unrecognised state',
};

/// The line under the headline: why the device is in that state, in the device's own terms.
String wifiConditionDetail(
  WifiCondition condition,
  WifiSnapshot snapshot,
) => switch (condition) {
  WifiCondition.notImplemented =>
    'The WiFi block reports no network, no password and no address, and its State '
        'is Disconnected.',
  WifiCondition.disabled =>
    snapshot.ssid.isEmpty
        ? 'The radio is switched off.'
        : 'The radio is switched off; the saved network is "${snapshot.ssid}".',
  WifiCondition.noNetwork =>
    'The device reports no SSID to join. Set one here, or the network has not been set '
        'up on this device.',
  WifiCondition.noPassword =>
    'The device reports the network has no password set for it.',
  WifiCondition.wrongPassword =>
    'The device rejected the password for '
        '${snapshot.ssid.isEmpty ? 'the network' : '"${snapshot.ssid}"'}.',
  WifiCondition.error =>
    'The device reports a WiFi error. The block carries no reason beyond the state.',
  WifiCondition.connecting =>
    snapshot.ssid.isEmpty
        ? 'Joining the configured network.'
        : 'Joining "${snapshot.ssid}".',
  WifiCondition.connected =>
    snapshot.ip.isEmpty
        ? 'The device reports a connection but no address.'
        : 'The device holds the address ${snapshot.ip}.',
  WifiCondition.idle =>
    'The radio is on and idle; the device reports no address yet.',
  WifiCondition.unknownState =>
    'The device reports State ${snapshot.state}, which this build does not know.',
};

/// The state tone the panel paints the row with.
WifiTone wifiConditionTone(WifiCondition condition) => switch (condition) {
  WifiCondition.connected => WifiTone.success,
  WifiCondition.connecting || WifiCondition.idle => WifiTone.neutral,
  WifiCondition.wrongPassword || WifiCondition.error => WifiTone.error,
  WifiCondition.disabled ||
  WifiCondition.noNetwork ||
  WifiCondition.noPassword ||
  WifiCondition.notImplemented ||
  WifiCondition.unknownState => WifiTone.warning,
};

/// Coarse severity a widget maps onto the theme's status colours.
enum WifiTone { success, neutral, warning, error }

/// A fixed-width character field's text: NUL-padded on the wire, so the padding is dropped.
/// Public because the panel and the block registry both describe these fields.
String textOfCharacters(List<int> bytes) {
  if (bytes.isEmpty) return '';
  final end = bytes.indexOf(0);
  final body = end < 0 ? bytes : bytes.sublist(0, end);
  return String.fromCharCodes(body).trim();
}

/// Formats the block's `IP address`: 16 bytes in network order.
///
/// The field is `Undefined` - the device never says what the bytes mean - so the app is the
/// only side that formats it (`Docs/Services/App Interface.md`, "WiFi Settings"):
///   - all zero        -> `''`, which the panel draws as no address;
///   - IPv4-mapped     -> `::ffff:a.b.c.d`, the notation the document uses for an IPv4 address
///                        reported in this field (bytes 10 and 11 are 0xFF);
///   - anything else   -> the IPv6 text form, longest zero run compressed to `::`.
///
/// The mapped form keeps the family visible; printing the bare quad would throw away the one
/// thing the mapping exists to tell.
String wifiAddressText(List<int> bytes) {
  if (bytes.length != 16) {
    // Not the field's shape: print what arrived rather than invent an address.
    return bytes.isEmpty
        ? ''
        : bytes.map((b) => b.toRadixString(16).padLeft(2, '0')).join();
  }
  if (bytes.every((b) => b == 0)) return '';
  final ipv4Mapped =
      bytes.take(10).every((b) => b == 0) &&
      bytes[10] == 0xFF &&
      bytes[11] == 0xFF;
  if (ipv4Mapped) return '::ffff:${bytes.sublist(12, 16).join('.')}';
  return _ipv6Text(bytes);
}

/// Eight big-endian groups, with the longest run of two or more zero groups collapsed to `::`.
String _ipv6Text(List<int> bytes) {
  final groups = [
    for (var i = 0; i < 16; i += 2) (bytes[i] << 8) | bytes[i + 1],
  ];
  var bestStart = -1;
  var bestLength = 0;
  var runStart = -1;
  for (var i = 0; i <= groups.length; i++) {
    if (i < groups.length && groups[i] == 0) {
      if (runStart < 0) runStart = i;
      continue;
    }
    if (runStart >= 0) {
      final length = i - runStart;
      if (length > bestLength) {
        bestLength = length;
        bestStart = runStart;
      }
      runStart = -1;
    }
  }
  String hex(int group) => group.toRadixString(16);
  if (bestLength < 2) return groups.map(hex).join(':');
  final head = groups.sublist(0, bestStart).map(hex).join(':');
  final tail = groups.sublist(bestStart + bestLength).map(hex).join(':');
  return '$head::$tail';
}
