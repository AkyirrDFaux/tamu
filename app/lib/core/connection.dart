/// Connection management: scanning, link establishment and the packet
/// transaction layer (Docs/App/Connection.md, Docs/Services/App Interface.md).
///
/// The app allocates a 16-bit transaction ID from its own App range
/// (0xF000-0xFFFF, Docs/RSBus and Packets.md "Transaction IDs"); the device echoes it, so
/// responses are matched on the full TRID.
library;

import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_libserialport/flutter_libserialport.dart'
    show SerialPort;
import 'package:universal_ble/universal_ble.dart';

import 'ble_permissions.dart';
import 'ble_security.dart';
import 'diagnostics.dart';
import 'device_db.dart';
import 'fragment_reassembler.dart';
import 'platform_caps.dart';
import 'protocol.dart';
import 'request_channel.dart';
import 'settings.dart';
import 'transport.dart';

enum LinkSource { all, ble, usb }

enum DeviceSort { signal, alphabetical }

enum LinkType { ble, usb }

/// How long one connect attempt may run before the app gives up on its own.
///
/// Long enough for a Bluetooth connect, service discovery and MTU negotiation, short enough that
/// a device that is off or out of range cannot hold the panel hostage: after this the attempt is
/// abandoned and the panel is idle again with a plain line instead of a spinner that never ends.
const Duration connectAttemptTimeout = Duration(seconds: 10);

/// What the panel says when an attempt ended without the device answering (the timeout).
const String connectNoAnswerMessage = 'No answer from the device';

/// What it says when the user abandoned the attempt: the panel's own action, not a failure.
const String connectCancelledMessage = 'Connection cancelled';

/// What it says when the device answered by turning this phone away.
///
/// The device answers every connect request - advertising does not depend on the window or on the
/// bond store - and disconnects an unbonded peer immediately when no pairing window is open
/// (`BleServerCallbacks::onConnect` in `Devices/Tamu_v2.0A/AppBLE.h`), so that refusal reaches the
/// phone as a fast failure rather than a hang. Saying "no answer" would send the user looking for
/// a device that is standing right there.
const String connectPairingWindowMessage =
    'The device only accepts paired phones (open a pairing window from USB)';

/// How a connect attempt ended when it produced no session.
enum ConnectEnd {
  /// The user pressed Cancel: the attempt was abandoned, not merely hidden.
  cancelled,

  /// The app waited [connectAttemptTimeout] and the device never answered.
  noAnswer,

  /// The device answered and refused this phone (see [connectPairingWindowMessage]).
  refused,

  /// A failure the app cannot place: an unopenable port, a platform error.
  failed,
}

/// The line the panel shows after an attempt that produced no session.
///
/// It exists so the pending state has ONE way out, whichever way it ended: cancel, timeout and a
/// refusal all leave the same field behind, and the panel reads it instead of each caller having
/// to word the failure itself.
class ConnectNotice {
  const ConnectNotice({
    required this.end,
    required this.message,
    this.detail,
    this.isBle = false,
  });

  final ConnectEnd end;

  /// The plain line for the user.
  final String message;

  /// The platform's own words, kept for the Log page rather than shown.
  final String? detail;

  /// Whether the attempt went over Bluetooth (the refusal classification is BLE-shaped).
  final bool isBle;

  @override
  String toString() =>
      'ConnectNotice(${end.name}, "$message"'
      '${detail == null ? '' : ', detail: "$detail"'})';
}

/// Places an attempt that ended without a session, from what the app can see.
///
/// Only called for an attempt that failed on its own: an attempt the app had to abandon is a
/// timeout (it reached [connectAttemptTimeout]) and never gets here. A Bluetooth attempt that
/// fails before that deadline therefore has an answer behind it - the device or the platform said
/// something - which is the refusal the copy names, unless the platform's words say nothing
/// answered at all (`not found`, a timeout of its own), in which case the timeout copy is right.
/// A USB attempt keeps its platform message: a port that will not open is the cable, not the
/// device's pairing policy.
ConnectNotice classifyConnectFailure({
  required DiscoveredLink link,
  required String errorText,
}) {
  if (link.type == LinkType.ble) {
    final noAnswer =
        !bleErrorLooksLikeSecurity(errorText) &&
        bleErrorLooksUnreachable(errorText);
    return ConnectNotice(
      end: noAnswer ? ConnectEnd.noAnswer : ConnectEnd.refused,
      message: noAnswer ? connectNoAnswerMessage : connectPairingWindowMessage,
      detail: errorText,
      isBle: true,
    );
  }
  return ConnectNotice(
    end: ConnectEnd.failed,
    message: 'Connect failed: $errorText',
    detail: errorText,
  );
}

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

/// A link that ended without the user asking, recorded so the UI can explain it.
///
/// A Bluetooth link that comes up and dies within seconds is the shape of a pairing the device
/// refused (it drops a link that never became secure), which is why [lastedMs] is part of the
/// record: the connection page uses it to tell "could not pair" from "the device went away".
class LinkTrouble {
  const LinkTrouble({
    required this.linkName,
    required this.isBle,
    required this.lastedMs,
    this.errorText,
  });

  final String linkName;
  final bool isBle;

  /// How long the session lived before it ended.
  final int lastedMs;

  /// The platform's own words, when it gave any.
  final String? errorText;

  /// Whether the session died before it could plausibly have become secure.
  ///
  /// The device drops a link that has not finished pairing by its deadline, and that deadline is
  /// policy-shaped: `bleSecureBudgetJustWorksMs` for Just Works, `bleSecureBudgetPasskeyMs` with a
  /// passkey (`firmware/src/Blocks/BleSecurityPolicy.h`). Which one was in force is unknowable
  /// here — the link is gone, so `Mode` cannot be read over it, and from the app's side the two
  /// look identical — so the LARGER budget is the test. Using the Just Works figure would drop
  /// every 5-30 s failure, which is exactly where a phone showing a passkey dialog lives.
  bool get droppedDuringSecuring =>
      isBle && lastedMs < bleSecureBudgetPasskeyMs;

  @override
  String toString() =>
      'LinkTrouble($linkName, ${isBle ? 'BLE' : 'USB'}, ${lastedMs}ms'
      '${errorText == null ? '' : ', "$errorText"'})';
}

class ConnectionManager extends ChangeNotifier implements RequestChannel {
  ConnectionManager._({bool installChannel = true}) {
    // The service clients send through the installed channel (`request_channel.dart`), and the
    // app's channel is this connection. Installing it here arms the clients for anything that
    // reaches the app's connection - the coupling they had when they called
    // `ConnectionManager.instance` directly. A tool installs its own channel instead and never
    // touches this class.
    if (installChannel) installRequestChannel(this);
  }

  static final ConnectionManager instance = ConnectionManager._();

  /// A manager that is not the app's singleton, for tests that drive the pending path against a
  /// fake transport. It does not install itself as the app's request channel.
  @visibleForTesting
  ConnectionManager.forTest() : this._(installChannel: false);

  /// Builds the transport for one attempt. Tests replace it with a fake so the pending path
  /// (cancel, timeout) can be exercised without a radio or a serial port.
  @visibleForTesting
  Transport Function(DiscoveredLink link) transportFactory = _defaultTransport;

  static Transport _defaultTransport(DiscoveredLink link) =>
      link.type == LinkType.ble ? BleTransport(link.id) : UsbTransport(link.id);

  /// How long one attempt may run before the app abandons it. A test shortens it.
  @visibleForTesting
  Duration connectTimeout = connectAttemptTimeout;

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

  /// The last link loss this session saw that the user did not ask for, or null. The connection
  /// page reads it once to explain a failed Bluetooth attempt and clears it.
  LinkTrouble? lastLinkTrouble;

  /// When the current session started, for [LinkTrouble.lastedMs].
  DateTime? _sessionStartedAt;

  /// Drops [lastLinkTrouble] once the UI has shown it.
  void clearLinkTrouble() {
    if (lastLinkTrouble == null) return;
    lastLinkTrouble = null;
    notifyListeners();
  }

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
        addIfActive(
          DiscoveredLink(
            id: e.deviceId,
            type: LinkType.ble,
            name: e.name,
            rssi: e.rssi,
          ),
        );
      }
    }
    if (source != LinkSource.ble) {
      for (final p in _usbEntries) {
        addIfActive(
          DiscoveredLink(
            id: p.portName,
            type: LinkType.usb,
            name: p.description?.isNotEmpty == true
                ? p.description!
                : p.portName,
          ),
        );
      }
    }
    switch (sort) {
      case DeviceSort.signal:
        links.sort((a, b) => (b.rssi ?? -999).compareTo(a.rssi ?? -999));
      case DeviceSort.alphabetical:
        links.sort(
          (a, b) => a.name.toLowerCase().compareTo(b.name.toLowerCase()),
        );
    }
    _linksCache = links;
    return links;
  }

  // --- Connection state -----------------------------------------------------
  Transport? _transport;
  DiscoveredLink? _activeLink;
  _ConnectAttempt? _attempt;
  bool _disposed = false;

  String? get connectedName => _transport?.displayName;
  @override
  bool get isConnected => _transport != null;
  Transport? get transport => _transport;

  /// True while a connect attempt is in flight (BLE connect + service discovery
  /// + MTU negotiation takes several seconds - the UI must show progress).
  bool get isConnecting => _attempt != null;
  String? get connectingTarget => _attempt?.link.name;

  /// How the last attempt that produced no session ended, or null when there is nothing to say.
  ///
  /// One field rather than a message per caller: the panel reads it, so a cancel, a timeout and a
  /// refusal all leave the pending state the same way - idle, with one plain line - no matter
  /// which code path (a tap or autoconnect) started the attempt.
  ConnectNotice? lastConnectNotice;

  final PacketStreamParser _parser = PacketStreamParser();
  StreamSubscription<Uint8List>? _streamSub;
  int _nextTxId = tridAppBase;
  final Map<int, Completer<PacketResponse>> _pending = {};
  // Reassembles multi-fragment replies ([FragmentReassembler]).
  final FragmentReassembler _reassembler = FragmentReassembler();

  // General listener for subscription value updates (service Subscriptions CID 0).
  // Only one listener is supported; SubscriptionClient uses it to feed its
  // valueUpdates stream.
  Function(List<int>)? _subscriptionListener;

  @override
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
    if (_transport != null || isConnecting) return;
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
    final target = discoveredLinks
        .where(
          (l) =>
              l.id == settings.autoConnectDeviceId ||
              l.name == settings.autoConnectDeviceId,
        )
        .firstOrNull;
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
            _bleEntries.add(
              BleScanEntry(
                deviceId: device.deviceId,
                name: name,
                rssi: device.rssi,
                lastSeen: DateTime.now(),
              ),
            );
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
  Stream<String?> get bluetoothWarning => UniversalBle.availabilityStream.map(
    (state) => switch (state) {
      AvailabilityState.poweredOff => 'Bluetooth is off',
      AvailabilityState.unauthorized => 'Bluetooth permission denied',
      AvailabilityState.unsupported => 'Bluetooth is not supported',
      _ => null,
    },
  );

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

  /// Returns null on success or the line the panel shows when the attempt produced no session.
  ///
  /// The attempt is abandonable: it ends by itself on success, by the device refusing it, by
  /// [connectTimeout] running out, or by [cancelConnect]. Whichever way it ends, the panel is left
  /// with no attempt in flight and (unless a session came up) one [ConnectNotice] saying why.
  Future<String?> connectTo(DiscoveredLink link) async {
    // Whatever the last attempt said is old news once a new one is asked for; clearing it here
    // also keeps a precondition refusal below from inheriting it.
    lastConnectNotice = null;
    if (_attempt != null) return 'Already connecting';
    if (_transport != null) return 'Already connected - disconnect first';
    // Android is BLE-only (Docs/App/General info.md).
    if (link.type == LinkType.usb && !supportsUsb) {
      return 'USB is not supported on this platform';
    }

    final attempt = _ConnectAttempt(link);
    _attempt = attempt;
    notifyListeners();

    ConnectNotice? notice;
    try {
      final transport = transportFactory(link);
      attempt.transport = transport;
      attempt.deadline = Timer(connectTimeout, () {
        // Nothing answered within the deadline: abandon the wait AND drop the platform's work,
        // so the panel cannot be left waiting on a connect nobody is listening to any more.
        _abandonAttempt(attempt, timedOut: true);
      });
      await Future.any<void>([
        transport.connect(),
        attempt.abandoned.then<void>((_) => throw const _ConnectAbandoned()),
      ]);
      // A connect that finished after the attempt was abandoned must not become a session: the
      // user (or the deadline) already gave up on it.
      if (attempt.isAbandoned) throw const _ConnectAbandoned();
      await _attach(transport);
      // Cancel can land while the attach runs (a few awaits); a session nobody waited for must
      // not stay up behind an idle panel.
      if (attempt.isAbandoned) {
        await _detach();
        throw const _ConnectAbandoned();
      }
      _activeLink = link;
      _invalidateLinks();
      // Pull the live network (core + SNDB devices) now that the link is up,
      // so the UI shows real data immediately after an autoconnect.
      unawaited(DeviceDatabase.instance.refreshNetwork());
      AppDiagnostics.log('link', 'connected: ${transport.displayName}');
    } on _ConnectAbandoned {
      await attempt.closeTransport();
      notice = attempt.timedOut
          ? const ConnectNotice(
              end: ConnectEnd.noAnswer,
              message: connectNoAnswerMessage,
            )
          : const ConnectNotice(
              end: ConnectEnd.cancelled,
              message: connectCancelledMessage,
            );
    } catch (error) {
      await attempt.closeTransport();
      AppDiagnostics.log('link', 'connect failed: $error');
      notice = classifyConnectFailure(link: link, errorText: error.toString());
    } finally {
      attempt.deadline?.cancel();
      if (identical(_attempt, attempt)) _attempt = null;
      if (notice != null) {
        lastConnectNotice = notice;
        final suffix = notice.message.isEmpty ? '' : ' (${notice.message})';
        AppDiagnostics.log(
          'link',
          'connect ${notice.end.name}: ${link.name}$suffix',
        );
      }
      // An attempt's own teardown can outlive the manager (the app closing mid-connect), and a
      // change notification after dispose is an error.
      if (!_disposed) notifyListeners();
    }
    return notice?.message;
  }

  /// Abandons the attempt in flight: the panel's way out of a connect that hangs.
  ///
  /// This is not "hide the spinner": the transport is closed (which disconnects the peer a
  /// Bluetooth connect is still negotiating with, and releases a serial port), scanning stops,
  /// and autoconnect is held off briefly so the abandoned attempt is not restarted on the next
  /// refresh. Returns false when nothing was pending.
  Future<bool> cancelConnect() async {
    final attempt = _attempt;
    if (attempt == null) return false;
    AppDiagnostics.log(
      'link',
      'connect cancelled by the user: ${attempt.link.name}',
    );
    _autoConnectSuppressedUntil = DateTime.now().add(
      const Duration(seconds: 30),
    );
    _abandonAttempt(attempt, timedOut: false);
    await stopScan();
    return true;
  }

  /// Tells the attempt to stop waiting and drops the work behind it. Idempotent: cancel, the
  /// deadline and a late platform answer can all reach the same attempt.
  void _abandonAttempt(_ConnectAttempt attempt, {required bool timedOut}) {
    if (attempt.isAbandoned) return;
    attempt.abandon(timedOut: timedOut);
    attempt.deadline?.cancel();
    unawaited(attempt.closeTransport());
  }

  Future<void> _attach(Transport transport) async {
    await _detach();
    _transport = transport;
    connectedId = transport.id;
    _sessionStartedAt = DateTime.now();
    _streamSub = transport.packetStream.listen(
      _onStreamBytes,
      onError: (Object error) {
        AppDiagnostics.log('link', 'packet stream error: $error -> disconnect');
        _noteLinkLoss('$error');
        unawaited(disconnect());
      },
      onDone: () {
        AppDiagnostics.log('link', 'packet stream done -> disconnect');
        _noteLinkLoss(null);
        unawaited(disconnect());
      },
    );
    notifyListeners();
  }

  /// Records a link that ended by itself, so the connection page can explain it rather than
  /// leaving the user with a session that silently vanished.
  void _noteLinkLoss(String? errorText) {
    final transport = _transport;
    final started = _sessionStartedAt;
    _sessionStartedAt = null;
    if (transport == null) return;
    lastLinkTrouble = LinkTrouble(
      linkName: transport.displayName,
      isBle: transport is BleTransport,
      lastedMs: started == null
          ? 0
          : DateTime.now().difference(started).inMilliseconds,
      errorText: errorText,
    );
  }

  Future<void> disconnect({bool manual = false}) async {
    // A manual disconnect (disconnect button / switching to another device)
    // must not be immediately undone by autoconnect: suppress it briefly so
    // the user stays disconnected (Docs/App/Settings.md).
    if (manual) {
      _autoConnectSuppressedUntil = DateTime.now().add(
        const Duration(seconds: 30),
      );
    }
    await _detach();
    notifyListeners();
  }

  Future<void> _detach() async {
    if (_transport != null) {
      AppDiagnostics.log(
        'link',
        'disconnected from ${_transport!.displayName}',
      );
    }
    await _streamSub?.cancel();
    _streamSub = null;
    _sessionStartedAt = null;
    _activeLink = null; // re-list the device once the session ends
    _invalidateLinks();
    connectedId = null;
    for (final completer in _pending.values) {
      if (!completer.isCompleted) {
        completer.completeError(const TransportException('Disconnected'));
      }
    }
    _pending.clear();
    _reassembler.clear();
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
      // Multi-packet streams accumulate until the fragment with STOP set (see
      // [FragmentReassembler]): FRAG packets carry 4 bytes of fragmentation info
      // (u16 current + u16 total) at the start of the payload, stripped here so streams
      // reassemble into the raw service data. Non-FRAG single packets are exactly
      // PayloadLen bytes; clients slice the real lengths via the format fields
      // (ValueInfo.Size etc.), so any service-level 4-byte padding is simply ignored.
      switch (_reassembler.feed(txId, frame)) {
        case FragmentPending():
          _pending[txId] = completer;
        case FragmentComplete(:final payload, :final success, :final fail):
          completer.complete(
            PacketResponse(payload: payload, success: success, fail: fail),
          );
        case FragmentAbort(:final reason):
          AppDiagnostics.log('link', 'fragment stream aborted: $reason');
          if (!completer.isCompleted) {
            completer.completeError(
              TransportException('Fragment stream aborted: $reason'),
            );
          }
      }
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
  @override
  int takeTxId() => _takeTxId();

  /// Sends a single-packet request and waits for its response payload (REQACK is
  /// always set). Throws [TransportException] on timeout.
  @override
  Future<List<int>> request(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
    bool requestFrag = false,
    int? transactionId,
  }) async => (await requestWithFlags(
    targetId,
    service,
    functionCid,
    payload: payload,
    timeout: timeout,
    requestFrag: requestFrag,
    transactionId: transactionId,
  )).payload;

  /// Like [request], but returns the packet-level SUCCESS/FAIL flags alongside the
  /// payload so callers can tell an accepted write from a FAIL reply.
  @override
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
    _reassembler.begin(txId);
    try {
      await transport.send(frame.toBytes());
      return await completer.future.timeout(
        timeout,
        onTimeout: () {
          _pending.remove(txId);
          _reassembler.discard(txId);
          throw TransportException(
            '${service.name} CID $functionCid request timed out',
          );
        },
      );
    } catch (error) {
      _pending.remove(txId);
      _reassembler.discard(txId);
      rethrow;
    }
  }

  @override
  void dispose() {
    _disposed = true;
    _autoTimer?.cancel();
    final attempt = _attempt;
    if (attempt != null) _abandonAttempt(attempt, timedOut: false);
    stopScan();
    _detach();
    super.dispose();
  }
}

/// One connect attempt, and what it takes to abandon it.
///
/// It exists because a connect is a long platform wait with no handle on it: the attempt owns the
/// transport (so the link can be closed from outside the waiting code), a deadline, and a future
/// that completes when anyone gives up on it. [ConnectionManager.connectTo] races the platform's
/// connect against [abandoned], which is what makes Cancel and the timeout real rather than a
/// hidden spinner.
class _ConnectAttempt {
  _ConnectAttempt(this.link);

  final DiscoveredLink link;

  /// The transport this attempt is building; set before the platform call starts.
  Transport? transport;

  /// The attempt's own timeout, cancelled however the attempt ends.
  Timer? deadline;

  /// True when the attempt ended on its deadline rather than on the user's Cancel.
  bool timedOut = false;

  final _abandoned = Completer<void>();
  bool _transportClosed = false;

  Future<void> get abandoned => _abandoned.future;
  bool get isAbandoned => _abandoned.isCompleted;

  void abandon({required bool timedOut}) {
    if (isAbandoned) return;
    this.timedOut = timedOut;
    _abandoned.complete();
  }

  /// Drops the transport, which is what stops the platform's work: closing a Bluetooth transport
  /// disconnects the peer it may still be connecting to, and closing a serial transport releases
  /// the port. Safe to call from the abandon path and the failing path alike.
  Future<void> closeTransport() async {
    if (_transportClosed) return;
    _transportClosed = true;
    final t = transport;
    if (t == null) return;
    try {
      await t.close();
    } catch (_) {}
  }
}

/// Control flow marker for "this attempt was given up on", never a message.
class _ConnectAbandoned implements Exception {
  const _ConnectAbandoned();
}
