/// BLE transport (Docs/Services/App Interface.md).
library;

import 'dart:async';
import 'dart:typed_data';

import 'package:universal_ble/universal_ble.dart';

import 'transport.dart';

/// The device's App Interface GATT service. The firmware side is not
/// implemented yet (see Docs/Issues.md); these UUIDs are placeholders that the
/// firmware must match.
const String appServiceUuid = '8e7c1a10-9d36-4b5a-b0c2-2f5f4a9d0001';
const String appWriteCharUuid = '8e7c1a10-9d36-4b5a-b0c2-2f5f4a9d0002';
const String appNotifyCharUuid = '8e7c1a10-9d36-4b5a-b0c2-2f5f4a9d0003';

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
      if (service.uuid != appServiceUuid) continue;
      for (final characteristic in service.characteristics) {
        if (characteristic.uuid == appNotifyCharUuid) notifyChar = characteristic;
        if (characteristic.uuid == appWriteCharUuid) _mtu = _mtu; // keep default
      }
    }
    if (notifyChar == null) {
      await UniversalBle.disconnect(deviceId);
      throw const TransportException('App Interface service not found');
    }
    await UniversalBle.subscribeNotifications(
        deviceId, appServiceUuid, appNotifyCharUuid);
    _notifySub = UniversalBle.characteristicValueStream(deviceId, appNotifyCharUuid)
        .listen((value) {
      if (!_closed) _linkController.add(value);
    });
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
