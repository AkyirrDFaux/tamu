/// Connection management: scanning, link establishment and the packet
/// transaction layer (Docs/App/Connection.md, Docs/Services/App Interface.md).
///
/// The app allocates a 16-bit transaction ID from its own App range
/// (0xF000-0xFFFF, Docs/RSBus and Packets.md "Transaction IDs"); the device echoes it, so
/// responses are matched on the full TRID.
library;

import 'dart:async';

import 'settings.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter_libserialport/flutter_libserialport.dart'
    show SerialPort;
import 'package:universal_ble/universal_ble.dart';

import 'ble_permissions.dart';
import 'diagnostics.dart';
import 'device_db.dart';
import 'platform_caps.dart';
import 'protocol.dart';
import 'transport.dart';

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

/// A completed service reply: its reassembled payload plus the packet-level
/// SUCCESS/FAIL flags. Most callers only need the payload ([ConnectionManager.request]);
/// the subscription client checks the flags to tell an accepted write from a refusal.
class PacketResponse {
  final List<int> payload;
  final bool success;
  final bool fail;

  const PacketResponse({
    required this.payload,
    required this.success,
    required this.fail,
  });

  /// A reply is accepted only when it is explicitly successful. A FAIL response (or a
  /// response with neither flag) is not a success.
  bool get ok => success && !fail;
}

class ConnectionManager extends ChangeNotifier {
  ConnectionManager._();

  static final ConnectionManager instance = ConnectionManager._();

  // --- Scan state -----------------------------------------------------------
  // Android is BLE-only, so the source menu must not offer USB there.
  LinkSource source = supportsUsb ? LinkSource.all : LinkSource.ble;
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

  /// BLE scan entries older than this are pruned: the scan backend only reports a
  /// device when it is (re)seen, so this is what eventually drops an out-of-range one.
  /// Kept generous because the BlueZ backend is silent for a stationary device whose
  /// RSSI has not changed.
  static const Duration _bleEntryTtl = Duration(minutes: 2);

  // `discoveredLinks` is rebuilt only when the underlying scan state changes.
  List<DiscoveredLink>? _linksCache;
  void _invalidateLinks() => _linksCache = null;

  // Coalesces the burst of per-scan-event UI rebuilds into one notification.
  bool _notifyScheduled = false;
  void _notifySoon() {
    if (_notifyScheduled) return;
    _notifyScheduled = true;
    scheduleMicrotask(() {
      _notifyScheduled = false;
      notifyListeners();
    });
  }

  // Android runtime BLE permission state (see _refreshBle). The request is made
  // once; afterwards the user must fix it from the system settings page.
  bool _blePermissionRequested = false;
  bool _blePermissionGranted = false;
  bool _blePermissionBlocked = false;

  /// True when scanning is blocked because the BLE runtime permission was
  /// permanently denied (only the system settings page can re-enable it).
  bool get blePermissionBlocked => _blePermissionBlocked;

  /// The visible list: remaining devices, sorted as selected. Memoized until the
  /// scan state, source, sort or active link changes.
  List<DiscoveredLink> get discoveredLinks {
    final cached = _linksCache;
    if (cached != null) return cached;
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
    _linksCache = links;
    return links;
  }

  // --- Connection state -----------------------------------------------------
  Transport? _transport;
  DiscoveredLink? _activeLink;
  bool _connecting = false;

  String? get connectedName => _transport?.displayName;
  bool get isConnected => _transport != null;
  Transport? get transport => _transport;

  /// True while a connect attempt is in flight (BLE connect + service discovery
  /// + MTU negotiation takes several seconds - the UI must show progress).
  bool get isConnecting => _connecting;
  String? _connectingTarget;
  String? get connectingTarget => _connectingTarget;

  final PacketStreamParser _parser = PacketStreamParser();
  StreamSubscription<Uint8List>? _streamSub;
  int _nextTxId = tridAppBase;
  final Map<int, Completer<PacketResponse>> _pending = {};
  final Map<int, List<int>> _rxBuffers = {};
  // Expected next fragment index + total for each in-flight fragmented reply.
  final Map<int, ({int next, int total})> _rxFrag = {};

  // General listener for subscription value updates (service Subscriptions CID 0).
  // Only one listener is supported; SubscriptionClient uses it to feed its
  // valueUpdates stream.
  Function(List<int>)? _subscriptionListener;

  void setSubscriptionListener(Function(List<int>)? listener) {
    _subscriptionListener = listener;
  }

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
    if (source != LinkSource.ble && supportsUsb) await _refreshUsb();
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
    _invalidateLinks();
    await _syncAutoTimer();
    notifyListeners();
  }

  void setSort(DeviceSort newSort) {
    sort = newSort;
    _invalidateLinks();
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
    // Android needs runtime BLE permissions before the first scan. Request them
    // once; if denied, scanning is blocked until the user grants them (the
    // Connection page shows the settings shortcut and retries on resume).
    if (isAndroid && !_blePermissionGranted) {
      if (_blePermissionRequested) {
        refreshError = true;
        return;
      }
      _blePermissionRequested = true;
      final result = await ensureBlePermissions();
      _blePermissionGranted = result == BlePermissionResult.granted;
      // A plain denial also blocks scanning: surface the banner and let the
      // resume path re-request, instead of failing silently forever.
      _blePermissionBlocked = !_blePermissionGranted;
      if (!_blePermissionGranted) {
        refreshError = true;
        notifyListeners();
        return;
      }
    }
    _pruneBleEntries();
    try {
      if (!_bleScanActive) {
        await _scanSub?.cancel();
        await UniversalBle.stopScan();
        // NOTE: deliberately NOT clearing _bleEntries here. The BlueZ backend
        // only emits a scan event when a device's RSSI property CHANGES, so a
        // stationary close-range device stays silent in later scans - clearing
        // would make previously-found devices vanish mid-session. Old entries are
        // instead aged out by _pruneBleEntries().
        _scanSub = UniversalBle.scanStream.listen((device) {
          // Identify OUR devices solely by the advertised App Interface GATT
          // service, never by name: BlueZ caches stale names (or none) and the
          // name is not a reliable, unique marker. Unrelated BLE devices are
          // therefore hidden from the scan list.
          if (!advertisesAppInterface(device.services)) return;
          final name = (device.name == null || device.name!.isEmpty)
              ? 'BLE device ${device.deviceId}'
              : device.name!;
          final existing = _bleEntries
              .where((e) => e.deviceId == device.deviceId)
              .firstOrNull;
          if (existing != null) {
            existing
              ..rssi = device.rssi
              ..name = name
              ..lastSeen = DateTime.now();
          } else {
            _bleEntries.add(BleScanEntry(
                deviceId: device.deviceId,
                name: name,
                rssi: device.rssi,
                lastSeen: DateTime.now()));
          }
          _invalidateLinks();
          _notifySoon();
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

  /// Drops scan entries not seen for [_bleEntryTtl] (out-of-range devices).
  void _pruneBleEntries() {
    final cutoff = DateTime.now().subtract(_bleEntryTtl);
    final before = _bleEntries.length;
    _bleEntries.removeWhere((e) => e.lastSeen.isBefore(cutoff));
    if (_bleEntries.length != before) _invalidateLinks();
  }

  Future<void> _refreshUsb() async {
    if (!supportsUsb) {
      if (_usbEntries.isNotEmpty) {
        _usbEntries = [];
        _invalidateLinks();
      }
      return;
    }
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
      _invalidateLinks();
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

  /// A human-readable Bluetooth warning, or null while Bluetooth is usable
  /// (Android surfaces "Bluetooth is off" and permission problems).
  Stream<String?> get bluetoothWarning =>
      UniversalBle.availabilityStream.map((state) => switch (state) {
            AvailabilityState.poweredOff => 'Bluetooth is off',
            AvailabilityState.unauthorized => 'Bluetooth permission denied',
            AvailabilityState.unsupported => 'Bluetooth is not supported',
            _ => null,
          });

  /// Re-requests the BLE runtime permissions, e.g. after the user returned from
  /// the system settings page.
  Future<void> retryBlePermissions() async {
    _blePermissionRequested = false;
    _blePermissionGranted = false;
    _blePermissionBlocked = false;
    await refresh();
  }

  /// Opens this app's OS settings page (used to fix a denied permission).
  Future<void> openPermissionSettings() => openBlePermissionSettings();

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
          // Android is BLE-only (Docs/App/General info.md).
          if (!supportsUsb) return 'USB is not supported on this platform';
          final usb = UsbTransport(link.id);
          await usb.connect();
          transport = usb;
      }
      await _attach(transport);
      _activeLink = link;
      _invalidateLinks();
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
    _invalidateLinks();
    connectedId = null;
    for (final completer in _pending.values) {
      if (!completer.isCompleted) {
        completer.completeError(const TransportException('Disconnected'));
      }
    }
    _pending.clear();
    _rxBuffers.clear();
    _rxFrag.clear();
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
      // Firmware-pushed subscription value updates (service Subscriptions CID 0) are
      // routed to the registered listener (SubscriptionClient) - they are addressed to
      // us by subscription TRID, not by an app transaction ID.
      if (serviceTypeOf(frame.srvTarget) == ServiceType.subscriptions &&
          serviceCidOf(frame.srvTarget) == 0) {
        final listener = _subscriptionListener;
        if (listener != null) {
          final data = frame.isFrag && frame.payload.length >= 4
              ? frame.payload.sublist(4)
              : frame.payload;
          listener(data);
          continue;
        }
      }

      if (!frame.isResponse) continue; // unsolicited requests ignored for now
      
      // Responses echo the request's TRID (Docs "Transaction IDs"), so match on the full
      // 16-bit TRID field.
      final txId = frame.srvSource;
      final completer = _pending.remove(txId);
      if (completer == null || completer.isCompleted) continue;
      // Multi-packet streams accumulate until the fragment with STOP set. FRAG packets
      // carry 4 bytes of fragmentation info (u16 current + u16 total) at the start of the
      // payload; it is stripped here so streams reassemble into the raw service data.
      // Non-FRAG single packets are exactly PayloadLen bytes; clients slice the real
      // lengths via the format fields (ValueInfo.Size etc.), so any service-level
      // 4-byte padding is simply ignored.
      final buffer = _rxBuffers.putIfAbsent(txId, () => <int>[]);
      if (frame.isFrag) {
        if (frame.payload.length < 4) {
          _abortFragment(completer, txId, 'FRAG packet without fragment info');
          continue;
        }
        final info = fragInfoOf(frame.payload);
        final state = _rxFrag[txId];
        if (state == null) {
          if (info.current != 0) {
            _abortFragment(completer, txId, 'stream starts at fragment ${info.current}');
            continue;
          }
        } else if (info.current != state.next || info.total != state.total) {
          _abortFragment(
              completer,
              txId,
              'fragment ${info.current}/${info.total} out of order '
              '(expected ${state.next}/${state.total})');
          continue;
        }
        _rxFrag[txId] = (next: info.current + 1, total: info.total);
        buffer.addAll(frame.payload.sublist(4));
      } else {
        buffer.addAll(frame.payload);
      }
      if (frame.isStop) {
        _rxBuffers.remove(txId);
        _rxFrag.remove(txId);
        completer.complete(PacketResponse(
          payload: buffer,
          success: frame.isSuccess,
          fail: frame.isFail,
        ));
      } else {
        _pending[txId] = completer;
      }
    }
  }

  /// Drops a reply whose fragment sequence is inconsistent (a lost or duplicated
  /// fragment would otherwise silently corrupt the reassembled stream).
  void _abortFragment(Completer<PacketResponse> completer, int txId, String reason) {
    _rxBuffers.remove(txId);
    _rxFrag.remove(txId);
    _pending.remove(txId);
    AppDiagnostics.log('link', 'fragment stream aborted: $reason');
    if (!completer.isCompleted) {
      completer.completeError(TransportException('Fragment stream aborted: $reason'));
    }
  }

  int _takeTxId() {
    // The app owns the 0xF000-0xFFFF TRID range (Docs "Transaction IDs"). Skip any id still
    // pending so a slow request can never have its slot silently re-used by a later one.
    for (var guard = 0; guard <= tridAppMax - tridAppBase; guard++) {
      final txId = _nextTxId;
      _nextTxId = _nextTxId >= tridAppMax ? tridAppBase : _nextTxId + 1;
      if (!_pending.containsKey(txId)) return txId;
    }
    throw const TransportException('No free transaction IDs');
  }

  /// Allocates a free transaction ID for manual use (e.g. sharing one TRID across a
  /// paired pair of requests). The caller owns the slot until it is used by [request].
  int takeTxId() => _takeTxId();

  /// Sends a single-packet request and waits for its response payload (REQACK is
  /// always set). Throws [TransportException] on timeout.
  Future<List<int>> request(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
    bool requestFrag = false,
    int? transactionId,
  }) async =>
      (await requestWithFlags(targetId, service, functionCid,
              payload: payload,
              timeout: timeout,
              requestFrag: requestFrag,
              transactionId: transactionId))
          .payload;

  /// Like [request], but returns the packet-level SUCCESS/FAIL flags alongside the
  /// payload so callers can tell an accepted write from a FAIL reply.
  Future<PacketResponse> requestWithFlags(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
    bool requestFrag = false,
    int? transactionId,
  }) async {
    final transport = _transport;
    if (transport == null) throw const TransportException('Not connected');

    // A caller-supplied transaction ID lets several requests share one TRID (used by
    // the Subscriptions client to register both the requester and provider side of a
    // subscription under the same ID, so value updates route correctly).
    final txId = transactionId ?? _takeTxId();
    if (transactionId != null && _pending.containsKey(txId)) {
      throw TransportException('Transaction ID already in use');
    }
    final frame = PacketFrame.single(
      targetId: targetId,
      srvTarget: makeService(service, functionCid),
      // The TRID field carries our transaction ID from the reserved App range (0xF000-0xFFFF).
      // The device echoes it, so replies route back by TRID and are matched on the full value.
      srvSource: txId,
      response: false,
      payload: payload,
      requestFrag: requestFrag,
    );

    final completer = Completer<PacketResponse>();
    _pending[txId] = completer;
    _rxBuffers[txId] = <int>[];
    try {
      await transport.send(frame.toBytes());
      return await completer.future.timeout(timeout, onTimeout: () {
        _pending.remove(txId);
        _rxBuffers.remove(txId);
        _rxFrag.remove(txId);
        throw TransportException(
            '${service.name} CID $functionCid request timed out');
      });
    } catch (error) {
      _pending.remove(txId);
      _rxBuffers.remove(txId);
      _rxFrag.remove(txId);
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
