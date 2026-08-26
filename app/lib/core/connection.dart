/// Connection management: scanning, link establishment and the packet
/// transaction layer (Docs/App/Connection.md, Docs/Services/App Interface.md).
///
/// The app uses the full SRV CID range of its own source service as transaction
/// IDs; responses are matched by the CID echoed back in SRV TGT
/// ("SRV CID are App defined transaction IDs, the device does not care").
library;

import 'dart:async';

import 'settings.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter_libserialport/flutter_libserialport.dart'
    show SerialPort;
import 'package:universal_ble/universal_ble.dart';

import 'ble_transport.dart';
import 'diagnostics.dart';
import 'device_db.dart';
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

  /// Identity of the current session's link (MAC or serial path), for
  /// autoconnect targeting.
  String? connectedId;
  Duration autoInterval = const Duration(seconds: 1); // docs default
  bool refreshError = false;

  final List<BleScanEntry> _bleEntries = [];
  List<UsbPortEntry> _usbEntries = [];
  StreamSubscription<BleDevice>? _scanSub;
  Timer? _autoTimer;
  Timer? _autoConnectRetry;
  DateTime? _autoConnectSuppressedUntil;
  bool _bleScanActive = false;

  /// The visible list: remaining devices, sorted as selected.
  List<DiscoveredLink> get discoveredLinks {
    final links = <DiscoveredLink>[];
    // The connected device is represented by the connected-session banner, not
    // as a tappable list row.
    void addIfActive(DiscoveredLink l) {
      final active = _activeLink;
      if (active != null && active.type == l.type && active.id == l.id) return;
      links.add(l);
    }
    if (source != LinkSource.usb) {
      for (final e in _bleEntries) {
        addIfActive(DiscoveredLink(
            id: e.deviceId, type: LinkType.ble, name: e.name, rssi: e.rssi));
      }
    }
    if (source != LinkSource.ble) {
      for (final p in _usbEntries) {
        addIfActive(DiscoveredLink(
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
  DiscoveredLink? _activeLink;
  bool _connecting = false;

  String? get connectedName => _transport?.displayName;
  bool get isConnected => _transport != null;

  /// True while a connect attempt is in flight (BLE connect + service discovery
  /// + MTU negotiation takes several seconds - the UI must show progress).
  bool get isConnecting => _connecting;
  String? _connectingTarget;
  String? get connectingTarget => _connectingTarget;

  final PacketStreamParser _parser = PacketStreamParser();
  StreamSubscription<Uint8List>? _streamSub;
  int _nextTxId = 1;
  final Map<int, Completer<List<int>>> _pending = {};
  final Map<int, List<int>> _rxBuffers = {};

  // ===========================================================================
  // Scanning
  // ===========================================================================

  Future<void> refresh() async {
    // A connected session owns the link: scanning (especially BlueZ discovery)
    // alongside live GATT traffic starves the connection and stalls requests,
    // so enumeration is paused until disconnect. Also pause while a connect is
    // still being established (_transport is only set after BLE connect +
    // service discovery + MTU, which can take seconds).
    if (_transport != null || _connecting) return;
    // NOTE: BLE entries are MERGED, not cleared, on every refresh: the BlueZ
    // backend only emits a device when its RSSI property CHANGES, so a
    // stationary close-range device stays silent in later scans and clearing
    // here would make previously-found devices vanish from the list.
    if (source != LinkSource.usb) await _refreshBle();
    if (source != LinkSource.ble) await _refreshUsb();
    notifyListeners();
    unawaited(_maybeAutoConnect());
  }

  Future<void> setAutoRefresh(bool enabled, {Duration? interval}) async {
    autoRefresh = enabled;
    if (interval != null) autoInterval = interval;
    await _syncAutoTimer();
    notifyListeners();
  }

  /// Autoconnect (Docs/App/Settings.md): when enabled with a target device,
  /// connect to it as soon as discovery sees it - at startup or whenever a
  /// refresh finds the link while disconnected. If the target is not in the
  /// current scan results yet, keep scanning on a short timer until it appears
  /// (the discovery stream may not have emitted it on the first pass).
  Future<void> _maybeAutoConnect() async {
    final settings = AppSettings.instance;
    if (!settings.autoConnect ||
        settings.autoConnectDeviceId.isEmpty ||
        isConnected ||
        isConnecting) {
      _autoConnectRetry?.cancel();
      _autoConnectRetry = null;
      return;
    }
    // After a manual disconnect, leave the device alone for a while; resume
    // autoconnect once the window expires.
    final suppressed = _autoConnectSuppressedUntil;
    if (suppressed != null && DateTime.now().isBefore(suppressed)) {
      final remaining = suppressed.difference(DateTime.now());
      _autoConnectRetry?.cancel();
      _autoConnectRetry = Timer(remaining, () {
        _autoConnectRetry = null;
        refresh();
      });
      return;
    }
    final target = discoveredLinks.where((l) =>
        l.id == settings.autoConnectDeviceId ||
        l.name == settings.autoConnectDeviceId).firstOrNull;
    if (target == null) {
      _autoConnectRetry?.cancel();
      _autoConnectRetry = Timer(const Duration(seconds: 3), () {
        _autoConnectRetry = null;
        refresh();
      });
      return;
    }
    _autoConnectRetry?.cancel();
    _autoConnectRetry = null;
    AppDiagnostics.log('link', 'autoconnecting to ${target.name}');
    await connectTo(target);
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
      _autoTimer?.cancel();
      _autoTimer = Timer.periodic(autoInterval, (_) => refresh());
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
        // NOTE: deliberately NOT clearing _bleEntries here. The BlueZ backend
        // only emits a scan event when a device's RSSI property CHANGES, so a
        // stationary close-range device stays silent in later scans - clearing
        // would make previously-found devices vanish mid-session.
        _scanSub = UniversalBle.scanStream.listen((device) {
          // A missing/unknown name must not disqualify a device: BlueZ caches
          // stale names (or none), so identify OUR devices by the advertised
          // App Interface service instead.
          final advertisesAppService = device.services.any(
              (s) => s.toLowerCase().contains('6e400001'));
          final name = (device.name == null || device.name!.isEmpty)
              ? 'BLE device ${device.deviceId}'
              : device.name!;
          if (device.name == null || device.name!.isEmpty) {
            if (!advertisesAppService) return;
          }
          final existing =
              _bleEntries.where((e) => e.deviceId == device.deviceId).toList();
          if (existing.isNotEmpty) {
            existing.first
              ..rssi = device.rssi
              ..name = name;
          } else {
            _bleEntries.add(BleScanEntry(
                deviceId: device.deviceId, name: name, rssi: device.rssi));
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
    if (_connecting) return 'Already connecting';
    if (_transport != null) return 'Already connected - disconnect first';

    _connecting = true;
    _connectingTarget = link.name;
    notifyListeners();
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
      _activeLink = link;
      // Keep the connection-list identity stable for the session (the reported
      // device name may differ from the advertised one).
      DeviceDatabase.instance.seedLinkName(link.name);
      // Pull the live network (core + SNDB devices) now that the link is up,
      // so the UI shows real data immediately after an autoconnect.
      unawaited(DeviceDatabase.instance.refreshNetwork());
      AppDiagnostics.log('link', 'connected: ${transport.displayName}');
      return null;
    } catch (error) {
      AppDiagnostics.log('link', 'connect failed: $error');
      return error.toString();
    } finally {
      _connecting = false;
      notifyListeners();
    }
  }

  Future<void> _attach(Transport transport) async {
    await _detach();
    _transport = transport;
    connectedId = transport.id;
    _streamSub = transport.packetStream.listen(
      _onStreamBytes,
      onError: (Object error) {
        AppDiagnostics.log('link', 'packet stream error: $error -> disconnect');
        unawaited(disconnect());
      },
      onDone: () {
        AppDiagnostics.log('link', 'packet stream done -> disconnect');
        unawaited(disconnect());
      },
    );
    notifyListeners();
  }

  Future<void> disconnect({bool manual = false}) async {
    // A manual disconnect (disconnect button / switching to another device)
    // must not be immediately undone by autoconnect: suppress it briefly so
    // the user stays disconnected (Docs/App/Settings.md).
    if (manual) {
      _autoConnectSuppressedUntil =
          DateTime.now().add(const Duration(seconds: 30));
    }
    await _detach();
    notifyListeners();
  }

  Future<void> _detach() async {
    if (_transport != null) {
      AppDiagnostics.log('link', 'disconnected from ${_transport!.displayName}');
    }
    await _streamSub?.cancel();
    _streamSub = null;
    _activeLink = null; // re-list the device once the session ends
    connectedId = null;
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
    // The CID byte wraps at 256; skip any id still pending so a slow request
    // can never have its slot silently re-used by a later one.
    for (var guard = 0; guard < 255; guard++) {
      final txId = _nextTxId;
      _nextTxId = (_nextTxId + 1) & 0xFF; // full CID range as transaction IDs
      if (_nextTxId == 0) _nextTxId = 1;
      if (!_pending.containsKey(txId)) return txId;
    }
    throw const TransportException('No free transaction IDs');
  }

  /// Sends a single-packet request and waits for its response payload (REQACK is
  /// always set). Throws [TransportException] on timeout.
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

  /// Sends a single-packet request without waiting for a reply (fire-and-forget).
  /// Used for write-stream chunks that the device acknowledges implicitly by
  /// completing the file; txId 0 is never used by [request], so no collision.
  Future<void> sendNoReply(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
  }) async {
    final transport = _transport;
    if (transport == null) throw const TransportException('Not connected');
    final frame = PacketFrame.single(
      targetId: targetId,
      srvTarget: makeService(service, functionCid),
      srvSource: makeService(ServiceType.app, 0),
      response: false,
      payload: payload,
    );
    await transport.send(frame.toBytes());
  }

  @override
  void dispose() {
    _autoTimer?.cancel();
    stopScan();
    _detach();
    super.dispose();
  }
}
