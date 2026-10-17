/// USB link framing for the App Interface (Docs/Services/App Interface.md).
///
/// Pure Dart: the app's [UsbTransport] and a plain `dart run` tool both put the packet stream
/// on the wire through this framing, so a frame built by either is byte-identical.
library;

import 'dart:typed_data';

import 'protocol.dart';

/// USB link frame: 0xFA | CRC8 | Length | Payload (max 60 bytes) | 0xBF
const int usbFrameStart = 0xFA;
const int usbFrameStop = 0xBF;
const int usbMaxPayload = 60;

/// The ESP32-C3 USB Serial/JTAG appears as a CDC device; the baud is advisory
/// (matches the console speed; the RSBus itself runs at 460.8 kbaud).
const int busBaudRate = 115200;

Uint8List buildUsbFrame(List<int> payloadStream) {
  assert(payloadStream.length <= usbMaxPayload);
  final frame = Uint8List(payloadStream.length + 4);
  frame[0] = usbFrameStart;
  // CRC covers length + payload.
  frame[1] = crc8([payloadStream.length, ...payloadStream]);
  frame[2] = payloadStream.length;
  frame.setAll(3, payloadStream);
  frame[frame.length - 1] = usbFrameStop;
  return frame;
}

/// Incremental parser recovering the packet stream from USB link frames.
/// Wire order per the docs: START, CRC8 (over length + payload), Length,
/// Payload, STOP.
class UsbFrameParser {
  final _out = BytesBuilder(copy: false);
  final List<int> _current = [];
  bool _inFrame = false;

  /// Returns the accumulated packet-stream bytes carried by complete frames.
  Uint8List feed(List<int> chunk) {
    for (final byte in chunk) {
      if (!_inFrame) {
        if (byte == usbFrameStart) {
          _inFrame = true;
          _current.clear();
        }
        continue;
      }
      _current.add(byte);
      if (_current.length < 2) continue; // CRC + length not complete yet
      final length = _current[1];
      if (length > usbMaxPayload) {
        _inFrame = false; // corrupt, resync
        continue;
      }
      if (_current.length < 2 + length + 1) continue; // payload + stop pending
      final expectedCrc = crc8([length, ..._current.sublist(2, 2 + length)]);
      final stop = _current.last;
      if (stop == usbFrameStop && _current[0] == expectedCrc) {
        _out.add(Uint8List.fromList(_current.sublist(2, 2 + length)));
      }
      _inFrame = false;
      _current.clear();
    }
    return _out.takeBytes();
  }
}
