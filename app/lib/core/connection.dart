/// Connection management: scanning, link establishment and the packet
/// transaction layer (Docs/App/Connection.md, Docs/Services/App Interface.md).
///
/// The app uses the full SRV CID range of its own source service as transaction
/// IDs; responses are matched by the CID echoed back in SRV TGT
/// ("SRV CID are App defined transaction IDs, the device does not care").
library;

import 'dart:async';
import 'package:flutter/foundation.dart';
import 'package:flutter_libserialport/flutter_libserialport.dart'
    show SerialPort;
import 'package:universal_ble/universal_ble.dart';

import 'ble_transport.dart';
import 'diagnostics.dart';
import 'protocol.dart';
import 'transport.dart';
import 'usb_transport.dart';

enum LinkSource { all, ble, usb }

enum DeviceSort { signal, alphabetical }

enum LinkType { ble, usb }

/// One row of the connection list.
class DiscoveredLink {
  final String id;
  final LinkType type;
  final String name;
  final int? rssi; // BLE only

  const DiscoveredLink({
    required this.id,
    required this.type,
    required this.name,
    this.rssi,
  });
}

class ConnectionManager extends ChangeNotifier {
  ConnectionManager._();

  static final ConnectionManager instance = ConnectionManager._();

  // --- Scan state -----------------------------------------------------------
  LinkSource source = LinkSource.all;
  DeviceSort sort = DeviceSort.alphabetical;
  bool autoRefresh = true; // automatically on per the docs
  bool refreshError = false;

  final List<BleScanEntry> _bleEntries = [];
  List<UsbPortEntry> _usbEntries = [];
  StreamSubscription<BleDevice>? _scanSub;
  Timer? _autoTimer;
  bool _bleScanActive = false;

  /// True while a scan source is actively being refreshed (green dot).
  bool get isRefreshing =>
      _bleScanActive || (_autoTimer != null && source != LinkSource.ble);

  /// The visible list: remaining devices, sorted as selected.
  List<DiscoveredLink> get discoveredLinks {
    final links = <DiscoveredLink>[];
    if (source != LinkSource.usb) {
      for (final e in _bleEntries) {
        links.add(DiscoveredLink(
            id: e.deviceId, type: LinkType.ble, name: e.name, rssi: e.rssi));
      }
    }
    if (source != LinkSource.ble) {
      for (final p in _usbEntries) {
        links.add(DiscoveredLink(
            id: p.portName,
            type: LinkType.usb,
            name:
                p.description?.isNotEmpty == true ? p.description! : p.portName));
      }
    }
    switch (sort) {
      case DeviceSort.signal:
        links.sort((a, b) => (b.rssi ?? -999).compareTo(a.rssi ?? -999));
      case DeviceSort.alphabetical:
        links.sort(
            (a, b) => a.name.toLowerCase().compareTo(b.name.toLowerCase()));
    }
    return links;
  }

  // --- Connection state -----------------------------------------------------
  Transport? _transport;
  String? get connectedName => _transport?.displayName;
  bool get isConnected => _transport != null;

  final PacketStreamParser _parser = PacketStreamParser();
  StreamSubscription<Uint8List>? _streamSub;
  int _nextTxId = 1;
  final Map<int, Completer<List<int>>> _pending = {};
  final Map<int, List<int>> _rxBuffers = {};

  // ===========================================================================
  // Scanning
  // ===========================================================================

  Future<void> refresh() async {
    if (source != LinkSource.usb) await _refreshBle();
    if (source != LinkSource.ble) await _refreshUsb();
    notifyListeners();
  }

  Future<void> setAutoRefresh(bool enabled) async {
    autoRefresh = enabled;
    await _syncAutoTimer();
    notifyListeners();
  }

  Future<void> setSource(LinkSource newSource) async {
    source = newSource;
    await _syncAutoTimer();
    notifyListeners();
  }

  void setSort(DeviceSort newSort) {
    sort = newSort;
    notifyListeners();
  }

  /// Keeps the periodic refresh running whenever there is anything to refresh
  /// (1 s period per the docs).
  Future<void> _syncAutoTimer() async {
    if (autoRefresh) {
      _autoTimer ??= Timer.periodic(const Duration(seconds: 1), (_) => refresh());
      await refresh();
    } else {
      _autoTimer?.cancel();
      _autoTimer = null;
      await stopScan();
      _bleScanActive = false;
      notifyListeners();
    }
  }

  Future<void> _refreshBle() async {
    try {
      if (!_bleScanActive) {
        await _scanSub?.cancel();
        await UniversalBle.stopScan();
        _bleEntries.clear();
        _scanSub = UniversalBle.scanStream.listen((device) {
          if (device.name == null || device.name!.isEmpty) return;
          final existing =
              _bleEntries.where((e) => e.deviceId == device.deviceId).toList();
          if (existing.isNotEmpty) {
            existing.first
              ..rssi = device.rssi
              ..name = device.name!;
          } else {
            _bleEntries.add(BleScanEntry(
                deviceId: device.deviceId, name: device.name!, rssi: device.rssi));
          }
          notifyListeners();
        });
        await UniversalBle.startScan();
        _bleScanActive = true;
      }
      refreshError = false;
    } catch (error) {
      _bleScanActive = false;
      refreshError = true;
      debugPrint('BLE scan failed: $error');
    }
  }

  Future<void> _refreshUsb() async {
    try {
      _usbEntries = SerialPort.availablePorts.map((n) {
        String? description;
        try {
          final port = SerialPort(n);
          description = port.description;
          port.dispose();
        } catch (_) {}
        return UsbPortEntry(portName: n, description: description);
      }).toList();
      refreshError = false;
    } catch (error) {
      refreshError = true;
      debugPrint('USB enumeration failed: $error');
    }
  }

  Future<void> stopScan() async {
    await _scanSub?.cancel();
    _scanSub = null;
    try {
      await UniversalBle.stopScan();
    } catch (_) {}
    _bleScanActive = false;
  }

  // ===========================================================================
  // Connecting / disconnecting
  // ===========================================================================

  /// Returns null on success or an error message.
  Future<String?> connectTo(DiscoveredLink link) async {
    try {
      final Transport transport;
      switch (link.type) {
        case LinkType.ble:
          final ble = BleTransport(link.id);
          await ble.connect();
          transport = ble;
        case LinkType.usb:
          final usb = UsbTransport(link.id);
          await usb.connect();
          transport = usb;
      }
      await _attach(transport);
      AppDiagnostics.log('link', 'connected: ${transport.displayName}');
      return null;
    } catch (error) {
      AppDiagnostics.log('link', 'connect failed: $error');
      return error.toString();
    }
  }

  Future<void> _attach(Transport transport) async {
    await _detach();
    _transport = transport;
    _streamSub = transport.packetStream.listen(
      _onStreamBytes,
      onError: (Object error) => disconnect(),
      onDone: () => disconnect(),
    );
    notifyListeners();
  }

  Future<void> disconnect() async {
    await _detach();
    notifyListeners();
  }

  Future<void> _detach() async {
    if (_transport != null) {
      AppDiagnostics.log('link', 'disconnected from ${_transport!.displayName}');
    }
    await _streamSub?.cancel();
    _streamSub = null;
    for (final completer in _pending.values) {
      if (!completer.isCompleted) {
        completer.completeError(const TransportException('Disconnected'));
      }
    }
    _pending.clear();
    final transport = _transport;
    _transport = null;
    if (transport != null) {
      try {
        await transport.close();
      } catch (_) {}
    }
  }

  // ===========================================================================
  // Transaction layer
  // ===========================================================================

  void _onStreamBytes(Uint8List bytes) {
    final List<PacketFrame> frames;
    try {
      frames = _parser.feed(bytes);
    } catch (error) {
      AppDiagnostics.log('link', 'packet parse error: $error');
      return;
    }
    for (final frame in frames) {
      if (!frame.isResponse) continue; // unsolicited requests ignored for now
      final txId = frame.srvTarget & 0xFF;
      final completer = _pending.remove(txId);
      if (completer == null || completer.isCompleted) continue;
      // Multi-packet streams accumulate until the fragment with STOP set.
      final buffer = _rxBuffers.putIfAbsent(txId, () => <int>[]);
      buffer.addAll(frame.payload);
      if (frame.isStop) {
        _rxBuffers.remove(txId);
        completer.complete(buffer);
      } else {
        _pending[txId] = completer;
      }
    }
  }

  int _takeTxId() {
    final txId = _nextTxId;
    _nextTxId = (_nextTxId + 1) & 0xFF; // full CID range as transaction IDs
    if (_nextTxId == 0) _nextTxId = 1;
    return txId;
  }

  /// Sends a single-packet request to `targetId` and waits for its response
  /// payload (REQACK is always set). Throws [TransportException] on timeout.
  Future<List<int>> request(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
  }) async {
    final transport = _transport;
    if (transport == null) throw const TransportException('Not connected');

    final txId = _takeTxId();
    final frame = PacketFrame.single(
      targetId: targetId,
      srvTarget: makeService(service, functionCid),
      // The app's identity is the App Interface service type (0x08); the CID byte
      // carries our transaction ID. The device routes replies back purely by this
      // service type (it rewrites id_src as a proxy, so no app address is needed).
      srvSource: makeService(ServiceType.app, txId),
      response: false,
      payload: payload,
    );

    final completer = Completer<List<int>>();
    _pending[txId] = completer;
    _rxBuffers[txId] = <int>[];
    try {
      await transport.send(frame.toBytes());
      return await completer.future.timeout(timeout, onTimeout: () {
        _pending.remove(txId);
        _rxBuffers.remove(txId);
        AppDiagnostics.log('link',
            'timeout: ${service.name} CID $functionCid to dev $targetId '
            '(txId $txId, ${timeout.inMilliseconds} ms)');
        throw TransportException(
            '${service.name} CID $functionCid request timed out');
      });
    } catch (error) {
      _pending.remove(txId);
      _rxBuffers.remove(txId);
      rethrow;
    }
  }

  @override
  void dispose() {
    _autoTimer?.cancel();
    stopScan();
    _detach();
    super.dispose();
  }
}
