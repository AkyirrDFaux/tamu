/// USB serial transport (Docs/Services/App Interface.md framing over RSBus
/// 115200 8N1, Docs/RSBus.md).
library;

import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_libserialport/flutter_libserialport.dart';

import 'transport.dart';

const int busBaudRate = 115200;

/// A serial port available on the system.
class UsbPortEntry {
  final String portName;
  final String? description;

  UsbPortEntry({required this.portName, this.description});
}

class UsbTransport implements Transport {
  final String portName;
  SerialPort? _port;
  SerialPortReader? _reader;
  StreamSubscription<Uint8List>? _sub;
  final _linkController = StreamController<Uint8List>.broadcast();
  final _streamController = StreamController<Uint8List>.broadcast();
  final UsbFrameParser _parser = UsbFrameParser();

  UsbTransport(this.portName);

  @override
  String get displayName => 'USB $portName';

  @override
  Stream<Uint8List> get linkBytes => _linkController.stream;

  @override
  Stream<Uint8List> get packetStream => _streamController.stream;

  Future<void> connect() async {
    final port = SerialPort(portName);
    if (!port.openReadWrite()) {
      port.dispose();
      throw TransportException('Cannot open $portName: ${SerialPort.lastError}');
    }
    port.config = SerialPortConfig()
      ..baudRate = busBaudRate
      ..bits = 8
      ..stopBits = 1
      ..parity = SerialPortParity.none;
    _port = port;
    final reader = SerialPortReader(port);
    _reader = reader;
    _sub = reader.stream.listen(
      (bytes) {
        _linkController.add(bytes);
        final streamBytes = _parser.feed(bytes);
        if (streamBytes.isNotEmpty && !_streamController.isClosed) {
          _streamController.add(streamBytes);
        }
      },
      onError: (Object error) => _streamController.addError(error),
    );
  }

  @override
  Future<void> send(List<int> streamBytes) async {
    final port = _port;
    if (port == null || !port.isOpen) throw const TransportException('Port closed');
    // Split into full USB link frames (max 60 bytes of stream per frame).
    for (var offset = 0; offset < streamBytes.length; offset += usbMaxPayload) {
      final end = (offset + usbMaxPayload).clamp(offset, streamBytes.length);
      final frame = buildUsbFrame(streamBytes.sublist(offset, end));
      port.write(Uint8List.fromList(frame), timeout: 100);
    }
  }

  @override
  Future<void> close() async {
    await _sub?.cancel();
    _reader?.close();
    _port?.close();
    _port?.dispose();
    _port = null;
    await _linkController.close();
    await _streamController.close();
  }
}
