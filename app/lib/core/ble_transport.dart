/// BLE transport (Docs/Services/App Interface.md).
library;

import 'dart:async';
import 'dart:typed_data';

import 'package:universal_ble/universal_ble.dart';

import 'transport.dart';

/// The device's App Interface GATT service: Nordic UART style UUIDs, matching the
/// firmware implementation (tamu/src/Devices/Tamu_v2.0A/AppBLE.h).
const String appServiceUuid = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
const String appWriteCharUuid = '6e400002-b5a3-f393-e0a9-e50e24dcca9e';
const String appNotifyCharUuid = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';

/// A device discovered during a BLE scan.
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
  int _mtu = 247;
  bool _closed = false;

  BleTransport(this.deviceId);

  @override
  String get displayName => 'BLE $deviceId';

  @override
  Stream<Uint8List> get linkBytes => _linkController.stream;

  @override
  Stream<Uint8List> get packetStream =>
      linkBytes.map(_parser.feed).where((bytes) => bytes.isNotEmpty);

  /// Connects, discovers the App service and subscribes to notifications.
  Future<void> connect() async {
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
    try {
      await UniversalBle.disconnect(deviceId);
    } catch (_) {}
    await _linkController.close();
  }
}
