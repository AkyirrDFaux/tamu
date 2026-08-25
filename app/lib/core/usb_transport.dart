/// USB serial transport (Docs/Services/App Interface.md framing over RSBus
/// 115200 8N1, Docs/RSBus.md).
library;

import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_libserialport/flutter_libserialport.dart';

import 'diagnostics.dart';
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
  String get id => portName;

  @override
  Stream<Uint8List> get linkBytes => _linkController.stream;

  @override
  Stream<Uint8List> get packetStream => _streamController.stream;

  Future<void> connect() async {
    final port = SerialPort(portName);
    if (!port.openReadWrite()) {
      final error = SerialPort.lastError;
      port.dispose();
      throw TransportException('Cannot open $portName: $error');
    }

    // Opening the port asserts DTR/RTS, which resets the ESP32-C3 core; its USB
    // CDC rejects line-coding updates until it has re-enumerated, so applying
    // the configuration can fail transiently right after open. Retry a few
    // times - this also gives the core time to boot before the first packet -
    // and never leak the opened handle on failure.
    Object? configError;
    for (var attempt = 0; attempt < 5; attempt++) {
      try {
        // Control-line ownership policy (Docs/Services/App Interface.md): while
        // the app session is up, the app owns DTR/RTS and holds BOTH asserted -
        // the state the ESP32-C3 runs stably in (esptool only resets the chip
        // on line TRANSITIONS, e.g. when a closing process drops the lines).
        // They stay latched until the port closes.
        //
        // xonXoff MUST be set explicitly: upstream sp_new_config() leaves that
        // field as uninitialized malloc garbage (every other field is -1 =
        // "unchanged"), so an unset config randomly fails sp_set_config with
        // SP_ERR_ARG ("Invalid XON/XOFF setting") depending on heap contents.
        port.config = SerialPortConfig()
          ..baudRate = busBaudRate
          ..bits = 8
          ..stopBits = 1
          ..parity = SerialPortParity.none
          ..xonXoff = SerialPortXonXoff.disabled
          ..rts = SerialPortRts.on
          ..dtr = SerialPortDtr.on;
        configError = null;
        break;
      } catch (error) {
        configError = error;
        AppDiagnostics.log(
            'usb', 'config attempt ${attempt + 1} failed on $portName: $error');
        await Future<void>.delayed(const Duration(milliseconds: 300));
      }
    }
    if (configError != null) {
      try {
        port.close();
      } catch (_) {}
      port.dispose();
      throw TransportException('Cannot configure $portName: $configError');
    }
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
      // A clean port close/unplug completes the stream; completing packetStream
      // lets ConnectionManager's onDone tear the session down (like a BLE drop).
      onDone: () {
        if (!_streamController.isClosed) _streamController.close();
      },
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
    if (!_linkController.isClosed) await _linkController.close();
    if (!_streamController.isClosed) await _streamController.close();
  }
}
