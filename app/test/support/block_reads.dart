/// The two fixtures every page test builds by hand: one field read, and a fixed-width text value.
///
/// The block-panel suites (BLE Settings, WiFi Settings) each declared their own copy - the same
/// record and the same NUL padding, under two different names (`read` in one, `field` in another).
/// A copy that drifted would have modelled a different reply than the page under test reads, so
/// the shapes live here once.
library;

import 'package:tamuapp/core/types.dart';

/// One block field as the device answered it: the record `RegisterClient.readBlockField` returns.
typedef BlockRead = ({ValueInfo meta, List<int> value});

/// A field of [type] holding [value].
///
/// [size] overrides the declared width (a device that reports a wider field than it sent), and
/// [flags] carries the field's `ValueInfo` flags (read-only, secret, persistent).
BlockRead fieldBytes(int type, List<int> value, {int flags = 0, int? size}) => (
  meta: ValueInfo(type: type, size: size ?? value.length, flags: flags),
  value: value,
);

/// [value] as a fixed-width, NUL-padded field: truncated at [width], zero-filled past it.
///
/// The firmware declares fixed-width `Text` fields and keeps the padding, so a fixture that sends
/// a short string without padding is a value the device would never answer with.
List<int> paddedText(String value, int width) {
  final bytes = value.codeUnits.toList();
  if (bytes.length > width) {
    bytes.length = width;
  } else {
    bytes.addAll(List<int>.filled(width - bytes.length, 0));
  }
  return bytes;
}

/// The reply `RegisterClient.readBlockField` parses: the 4-byte BlockInfo echo, the field's
/// `ValueInfo`, then its bytes.
///
/// Two fakes built this by hand (`backup_walk_test`, `upload_setup_block_read_test`) and a third
/// spelled the same bytes out as literals; a fixture that got the ValueInfo layout wrong would
/// model a reply the client decodes as another field's.
List<int> blockReply(int type, int size, List<int> value, {int flags = 0}) => [
  0,
  0,
  0,
  0,
  ...ValueInfo(type: type, size: size, flags: flags).toBytes(),
  ...value,
];

/// [words] as the little-endian u16 run the device packs them into.
List<int> u16Words(List<int> words) => [
  for (final w in words) ...[w & 0xFF, (w >> 8) & 0xFF],
];
