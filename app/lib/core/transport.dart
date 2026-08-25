/// Link transports for the App Interface (Docs/Services/App Interface.md).
///
/// Every transport delivers a continuous serialized packet stream to the
/// [stream] and accepts raw packet-stream bytes via [send].
library;

import 'dart:async';
import 'dart:typed_data';

import 'protocol.dart';

/// USB link frame (Docs/Services/App Interface.md):
/// 0xFA | CRC8 | Length | Payload (max 60 bytes) | 0xBF
const int usbFrameStart = 0xFA;
const int usbFrameStop = 0xBF;
const int usbMaxPayload = 60;

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

/// BLE link framing (Docs/Services/App Interface.md):
/// uint16 LE length prefix followed by that many stream bytes.
class BleLengthParser {
  final _out = BytesBuilder(copy: false);
  final List<int> _pending = [];

  Uint8List feed(List<int> chunk) {
    _pending.addAll(chunk);
    while (_pending.length >= 2) {
      final len = _pending[0] | (_pending[1] << 8);
      if (_pending.length < 2 + len) break;
      _out.add(Uint8List.fromList(_pending.sublist(2, 2 + len)));
      _pending.removeRange(0, 2 + len);
    }
    return _out.takeBytes();
  }

  /// Splits an outgoing stream into BLE writes with a length prefix each, sized so
  /// the whole write (prefix + payload) fits the negotiated MTU minus the 3-byte
  /// ATT header.
  static List<Uint8List> chunkOutgoing(List<int> streamBytes, {int mtu = 247}) {
    const headerSize = 2;
    // Total per write: headerSize + part <= mtu - 3.
    var maxChunk = mtu - 3 - headerSize;
    if (maxChunk < 1) maxChunk = 1; // degenerate MTUs: minimal chunks
    final chunks = <Uint8List>[];
    for (var offset = 0; offset < streamBytes.length; offset += maxChunk) {
      final end = (offset + maxChunk).clamp(offset, streamBytes.length);
      final part = streamBytes.sublist(offset, end);
      final out = Uint8List(part.length + headerSize);
      out[0] = part.length & 0xFF;
      out[1] = (part.length >> 8) & 0xFF;
      out.setAll(headerSize, part);
      chunks.add(out);
    }
    if (chunks.isEmpty) chunks.add(Uint8List.fromList([0, 0]));
    return chunks;
  }
}

/// A connected link to one device.
abstract class Transport {
  String get displayName;

  /// Stable identity used for autoconnect targeting (USB: port path,
  /// BLE: MAC address).
  String get id;

  /// Raw link bytes (USB frames / BLE notifications before de-framing is done
  /// internally by the implementation).
  Stream<Uint8List> get linkBytes;

  /// De-framed continuous packet stream bytes.
  Stream<Uint8List> get packetStream;

  Future<void> send(List<int> streamBytes);

  Future<void> close();
}

class TransportException implements Exception {
  final String message;
  const TransportException(this.message);

  @override
  String toString() => message;
}
