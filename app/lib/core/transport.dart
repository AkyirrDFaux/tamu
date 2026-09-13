/// Link transports for the App Interface (Docs/Services/App Interface.md).
///
/// Every transport delivers a continuous serialized packet stream to the
/// [stream] and accepts raw packet-stream bytes via [send].
library;

import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_libserialport/flutter_libserialport.dart';
import 'package:universal_ble/universal_ble.dart';

import 'diagnostics.dart';
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

// ---------------------------------------------------------------------------
// USB serial transport
// ---------------------------------------------------------------------------

/// The ESP32-C3 USB Serial/JTAG appears as a CDC device; the baud is advisory
/// (matches the console speed; the RSBus itself runs at 460.8 kbaud).
const int busBaudRate = 115200;

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

// ---------------------------------------------------------------------------
// BLE transport
// ---------------------------------------------------------------------------

/// The device's App Interface GATT service: Nordic UART style UUIDs, matching the
/// firmware implementation (tamu/src/Devices/Tamu_v2.0A/AppBLE.h).
const String appServiceUuid = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
const String appWriteCharUuid = '6e400002-b5a3-f393-e0a9-e50e24dcca9e';
const String appNotifyCharUuid = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';

class BleScanEntry {
  final String deviceId;
  String name;
  int? rssi;

  BleScanEntry({required this.deviceId, required this.name, this.rssi});
}

class BleTransport implements Transport {
  final String deviceId;
  final _linkController = StreamController<Uint8List>.broadcast();
  final BleLengthParser _parser = BleLengthParser();
  StreamSubscription<Uint8List>? _notifySub;
  StreamSubscription<bool>? _connSub;
  int _mtu = 247;
  bool _closed = false;

  BleTransport(this.deviceId);

  @override
  String get displayName => 'BLE $deviceId';

  @override
  String get id => deviceId;

  @override
  Stream<Uint8List> get linkBytes => _linkController.stream;

  @override
  Stream<Uint8List> get packetStream =>
      linkBytes.map(_parser.feed).where((bytes) => bytes.isNotEmpty);

  /// Connects, discovers the App service and subscribes to notifications.
  Future<void> connect() async {
    // BLE connections can fail transiently (ATT error 0x0e) when the device is
    // mid-advertising-restart right after a disconnect; retry a couple of times.
    Object? lastError;
    for (var attempt = 0; attempt < 3; attempt++) {
      if (attempt > 0) {
        await Future<void>.delayed(Duration(milliseconds: 600 * attempt));
      }
      try {
        await _connectOnce();
        return;
      } catch (error) {
        lastError = error;
        try {
          await UniversalBle.disconnect(deviceId);
        } catch (_) {}
      }
    }
    throw TransportException('BLE connect failed: $lastError');
  }

  Future<void> _connectOnce() async {
    await UniversalBle.connect(deviceId);
    final services = await UniversalBle.discoverServices(deviceId);
    BleCharacteristic? notifyChar;
    for (final service in services) {
      // Platform-reported UUID casing varies; compare normalized.
      if (service.uuid.toLowerCase() != appServiceUuid) continue;
      for (final characteristic in service.characteristics) {
        if (characteristic.uuid.toLowerCase() == appNotifyCharUuid) {
          notifyChar = characteristic;
        }
      }
    }
    if (notifyChar == null) {
      await UniversalBle.disconnect(deviceId);
      throw const TransportException('App Interface service not found');
    }
    await UniversalBle.subscribeNotifications(
        deviceId, appServiceUuid, appNotifyCharUuid);
    // The value stream filters by CHARACTERISTIC id - passing the service uuid
    // here would silently drop every notification.
    _notifySub = UniversalBle.characteristicValueStream(deviceId, appNotifyCharUuid)
        .listen((value) {
      if (!_closed) _linkController.add(value);
    });
    // Surface a REMOTE link loss (device moved out of range, powered off, ...).
    // connectionStream emits false on disconnect; our own close() sets _closed
    // first so the teardown is never mistaken for a drop. The error propagates
    // through packetStream to ConnectionManager's onError -> session teardown.
    _connSub = UniversalBle.connectionStream(deviceId).listen((connected) {
      if (!connected && !_closed) {
        _linkController.addError(const TransportException('BLE link lost'));
      }
    });
    // Negotiate a large MTU so stream chunks stay big; fall back silently on
    // platforms that ignore the request (iOS negotiates automatically).
    try {
      final negotiated = await UniversalBle.requestMtu(deviceId, 512);
      if (negotiated >= 23) _mtu = negotiated;
    } catch (_) {}
  }

  @override
  Future<void> send(List<int> streamBytes) async {
    for (final chunk in BleLengthParser.chunkOutgoing(streamBytes, mtu: _mtu)) {
      await UniversalBle.write(deviceId, appServiceUuid, appWriteCharUuid, chunk,
          withoutResponse: chunk.length < _mtu - 8);
    }
  }

  @override
  Future<void> close() async {
    _closed = true;
    await _notifySub?.cancel();
    await _connSub?.cancel();
    try {
      await UniversalBle.disconnect(deviceId);
    } catch (_) {}
    if (!_linkController.isClosed) await _linkController.close();
  }
}
