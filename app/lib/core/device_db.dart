/// In-RAM device database (Docs/App/General info.md): "App keeps local database
/// in RAM, updates upon arrival of new data."
///
/// The app always reaches the directly connected device via ID 1 (Net 0) and
/// learns about the rest of the network from it (Device service / SNDB).
library;

import 'package:flutter/foundation.dart';

import 'app_active.dart';
import 'connection.dart';
import 'device_identity.dart';
import 'diagnostics.dart';
import 'notifications.dart';
import 'protocol.dart';
import 'register_client.dart';
import 'request_channel.dart';
import 'service_request.dart';
import 'sndb.dart' as sndb;
import 'types.dart';

// `coreId` (the core's address on the local net) is a wire constant and lives in protocol.dart
// with the rest of them; re-exported here because the pages that reach the core import it from
// the device database.
export 'protocol.dart' show coreId;

/// Everything known about one network device.
class DeviceEntry {
  final int id;
  String name;
  DeviceType type;
  String? serialNumber; // 28-char hex
  String? softwareVersion;
  int capabilities;
  int? uptimeMs;
  double? avgLoopTimeMs;
  double? maxLoopTimeMs;
  int? timeOffsetMs;
  DateTime lastSeen;

  /// System field 8 `App Active`, as the BITS the device reports (core/app_active.dart):
  /// USB (0), BLE (1), WiFi (2). Several can be live at once, which the old enum could not
  /// say. Zero means the device reports no app interface.
  int appActiveBits = 0;

  /// The interfaces [appActiveBits] names, in bit order.
  List<AppInterface> get activeInterfaces => activeAppInterfaces(appActiveBits);

  DeviceEntry({required this.id})
    : name = 'Device ${idToString(id)}',
      type = DeviceType.unknown,
      capabilities = 0,
      lastSeen = DateTime.now();

  bool get isCore => id == coreId || capabilities & Capability.core != 0;
  int get net => idNet(id);

  /// The name shown in the UI (the reported System Name, with a per-id fallback).
  String get displayName => name;

  /// How many replies in a row this device failed to give: a lost frame, a refused ping, or a
  /// reply that arrived with a fragment hole or without its STOP frame.
  ///
  /// Cleared by any whole answer. Only [deviceMissThreshold] misses in a row make a known node
  /// read stale, so one lost frame cannot demote a device that is on the bus and answering.
  int consecutiveMisses = 0;

  /// Marks a device unreachable this sweep.
  bool stale = false;
}

/// How many consecutive missed replies a KNOWN device may have before it reads as stale.
///
/// One lost frame must never demote a live node: on this rig a few percent of one-shot reads are
/// still lost while the deployed scenario keeps the RS485 bus busy with subscription traffic
/// (2026-11-05), and the owner's report was exactly that - a DAS dropped out of the list for a
/// moment while its subscriptions and buttons kept working, because it had never been gone.
/// A sweep therefore re-sends a ping that did not come back whole ([devicePingAttempts] sends),
/// and only [deviceMissThreshold] unanswered sends in a row mark the node stale. Any whole answer
/// clears the count.
const int deviceMissThreshold = 3;

/// The pause before each ping re-send; [devicePingAttempts] - 1 entries.
///
/// The roster read's own backoff (`sndb.sndbReadRetryBackoff`), so discovery re-tries a missed
/// reply on one schedule rather than two: a ping and a roster fragment fail for the same reason.
const List<Duration> devicePingBackoff = [
  Duration(milliseconds: 200),
  Duration(milliseconds: 600),
];

/// Sends one known device's ping per sweep: the first, plus one re-send per
/// [devicePingBackoff] entry. A device that answers costs ONE send, so a healthy sweep is exactly
/// as fast as before; only a device that missed pays for the re-sends, inside the same sweep.
const int devicePingAttempts = 3;

class DeviceDatabase extends ChangeNotifier {
  DeviceDatabase._();

  static final DeviceDatabase instance = DeviceDatabase._();

  /// A database that is not the app's singleton, so a test can drive a whole sweep through a
  /// fake channel without touching the app's state.
  @visibleForTesting
  factory DeviceDatabase.forTest() => DeviceDatabase._();

  /// The link gate a sweep checks, when a test drives one without a radio or a serial port.
  /// Null (the app's case) means "ask the connection".
  @visibleForTesting
  bool? linkIsUpOverride;

  bool get _linkIsUp => linkIsUpOverride ?? _link.isConnected;

  final Map<int, DeviceEntry> _devices = {};
  // One Register client per target, reused across sweeps (no per-read allocation).
  final Map<int, RegisterClient> _regClients = {};
  bool _refreshing = false;
  String? lastError;

  bool get isRefreshing => _refreshing;

  List<DeviceEntry> get all =>
      List.unmodifiable(_devices.values.sortedBy((d) => d.id));

  DeviceEntry? byId(int id) => _devices[id];

  // ===========================================================================
  // Queries (Docs/Services/System Block and Device Commands.md)
  // ===========================================================================

  ConnectionManager get _link => ConnectionManager.instance;

  Future<List<int>?> _request(
    int targetId,
    ServiceType service,
    int cid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
  }) => serviceRequest(
    targetId,
    service,
    cid,
    payload: payload,
    timeout: timeout,
    tag: 'db',
  );

  /// Pings the connected device (ID 1).
  Future<bool> pingCore() async {
    final reply = await _request(
      coreId,
      ServiceType.device,
      1,
      timeout: const Duration(milliseconds: 800),
    );
    return reply != null;
  }

  /// Reads one System-block field through the shared Register client.
  Future<List<int>?> _registerRead(int targetId, int field, int key) async {
    final client = _regClients.putIfAbsent(
      targetId,
      () => RegisterClient(deviceId: targetId),
    );
    final read = await client.readField(field, key);
    return read?.value;
  }

  /// Refreshes one device's identity fields into the database via Register System block.
  Future<DeviceEntry?> refreshDevice(int id) async {
    if (!_linkIsUp) return null;

    final entry = _devices.putIfAbsent(id, () => DeviceEntry(id: id));

    // One reader for those fields (`readDeviceIdentity`), shared with the restore's live
    // snapshot: ping first, then the System block. Two attempts per read, because a single lost
    // reply would otherwise leave the node on screen with no serial or the wrong type - and the
    // deployed scenario keeps the bus busy enough for that to happen (2026-11-05).
    final identity = await readDeviceIdentity(id, readAttempts: 2);
    if (identity == null) {
      entry.stale = true;
      notifyListeners();
      return null;
    }
    entry.stale = false;
    entry.consecutiveMisses = 0;
    entry.lastSeen = DateTime.now();
    entry.serialNumber = identity.serial;
    entry.type = identity.type;
    entry.softwareVersion = identity.softwareVersion;
    entry.capabilities = identity.capabilities;
    entry.name = identity.name;

    notifyListeners();
    return entry;
  }

  /// Reads live runtime values (uptime + loop times) via Register System block.
  ///
  /// The four live fields are independent reads of the same block, so they are issued together
  /// and awaited once: over BLE each carries a ~22 ms round trip, and the app's start would
  /// otherwise spend four of them in a row before the device list can show uptime.
  Future<void> refreshRuntime(int id) async {
    final entry = _devices[id];
    if (entry == null) return;

    final uptimeF = _registerRead(id, 3, 0);
    final loopAvgF = _registerRead(id, 3, 3);
    final loopMaxF = _registerRead(id, 3, 4);
    final activeF = _registerRead(id, 8, 0);
    final uptimeReply = await uptimeF;
    final loopAvg = await loopAvgF;
    final loopMax = await loopMaxF;
    final active = await activeF;
    if (uptimeReply != null && uptimeReply.length >= 4) {
      entry.uptimeMs = uint32FromBytes(uptimeReply);
    }
    if (loopAvg != null && loopAvg.length >= 4) {
      entry.avgLoopTimeMs = numberFromBytes(loopAvg, 0);
    }
    if (loopMax != null && loopMax.length >= 4) {
      entry.maxLoopTimeMs = numberFromBytes(loopMax, 0);
    }
    // System field 8 App Active: the interfaces the DEVICE reports live, as bits (a phone
    // can be paired while the USB host is attached, so this is not one enum value). A device
    // that answers with nothing keeps what it last reported.
    if (active != null && active.isNotEmpty) {
      entry.appActiveBits = appActiveBitsFromBytes(active);
    }

    if (id == coreId) {
      entry.timeOffsetMs = 0;
    } else {
      // The core's synchronized clock is System field 3 key 1 (Current time); key 0 is
      // the raw Uptime, which would mix two clocks in the offset below.
      final coreBefore = await _registerRead(coreId, 3, 1);
      if (coreBefore != null && coreBefore.length >= 4) {
        final t0 = uint32FromBytes(coreBefore);
        final syncReply = await _request(
          id,
          ServiceType.device,
          3,
          payload: uint32ToBytes(t0),
        );
        if (syncReply != null && syncReply.length >= 12) {
          final t1 = uint32FromBytes(syncReply, 4);
          final t2 = uint32FromBytes(syncReply, 8);
          final coreAfter = await _registerRead(coreId, 3, 1);
          final t3 = (coreAfter != null && coreAfter.length >= 4)
              ? uint32FromBytes(coreAfter)
              : t0 + (t2 - t1);
          entry.timeOffsetMs = (((t1 - t0) + (t2 - t3)) / 2).round();
        }
      }
    }

    entry.lastSeen = DateTime.now();
    notifyListeners();
  }

  /// Renames a device via Register System Name (Block 0 Field 6)
  Future<bool> setName(int id, String name) async {
    final bytes = name.codeUnits.take(16).toList();
    final meta = ValueInfo(
      type: DataType.name.value,
      flags: ValueFlags.persistent,
      size: bytes.length,
    );
    // System block (type 0), field 6 (Name). The Register service exposes the System struct as
    // a whole at key 0, not per member (Docs/Services/System Block and Device Commands.md).
    final payload = [
      ...blockInfoBytes(0, 0, 6, 0),
      ...meta.toBytes(),
      ...bytes,
    ];
    final reply = await _request(
      id,
      ServiceType.register,
      RegisterCid.write,
      payload: payload,
    );
    if (reply == null) return false;
    final entry = _devices[id];
    if (entry != null) entry.name = name;
    notifyListeners();
    return true;
  }

  /// Asks a device to identify itself (Device service CID 2): blink its red LED
  /// fast for ~10 s (docs). Returns true when the device acknowledged.
  Future<bool> identify(int id, {bool on = true}) async {
    final reply = await _request(
      id,
      ServiceType.device,
      2,
      payload: [on ? 1 : 0],
    );
    return reply != null;
  }

  // ===========================================================================
  // Network discovery
  // ===========================================================================

  /// Full sweep: query the core, then walk every registered device from the
  /// SNDB (Device service CID 13). Safe to call repeatedly.
  Future<void>? _refreshInFlight;

  Future<void> refreshNetwork() async {
    if (!_linkIsUp) return;
    // Coalesce concurrent refreshes: `connectTo` kicks one off and page code may
    // trigger another while it is still running. Returning the in-flight future
    // means callers actually wait for the full refresh instead of racing ahead
    // on half-populated entries.
    final inFlight = _refreshInFlight;
    if (inFlight != null) {
      return inFlight;
    }
    final run = _doRefresh();
    _refreshInFlight = run;
    try {
      await run;
    } finally {
      if (_refreshInFlight == run) _refreshInFlight = null;
    }
  }

  Future<void> _doRefresh() async {
    _refreshing = true;
    lastError = null;
    notifyListeners();

    // Reachable devices at the start of this sweep (for lost/discovered events).
    final reachableBefore = _devices.entries
        .where((e) => !e.value.stale)
        .map((e) => e.key)
        .toSet();

    try {
      // No blanket "everything is stale until it answers" here: a known node's state is the
      // sweep's own verdict (see [_pingDevice] and [deviceMissThreshold]), and a device the
      // roster no longer names is marked below. Pre-marking stale is what made one lost ping
      // render as a node that had vanished.

      // The core's identity and the SNDB roster are independent requests, so they go out
      // together: the roster read alone costs ~315 ms on the rig, which used to be spent after
      // the identity round trips rather than beside them.
      final sndbFuture = sndb.readSndbRoster();
      await refreshDevice(coreId);
      final roster = await sndbFuture;
      if (!roster.ok) {
        // A roster that could not be read whole is NOT an empty network: say why instead of
        // reporting every child device as gone (see sndb.readSndbRoster's re-request policy).
        throw Exception(
          'SNDB read failed after ${roster.attempts} attempt(s): ${roster.fault}',
        );
      }
      final parsed = roster.entries!;
      // ID 0 marks an unassigned entry; skip it so no phantom "device" is created
      // and no requests are fired at an invalid target.
      final ids = <int>{
        coreId,
        for (final e in parsed)
          if (e.id != 0) e.id,
      };
      // A device the roster no longer names is gone from the registry, not merely quiet: a
      // whole roster read is the one place an absence is real, so it reads stale at once.
      for (final entry in _devices.values) {
        if (!ids.contains(entry.id)) entry.stale = true;
      }
      // Every reachable id is refreshed beside the others, not one after another: a device
      // that is already known costs one ping, an unknown one its whole identity, and either
      // way the round trips of one device overlap the next device's.
      await Future.wait([
        for (final id in ids)
          () async {
            final known = _devices[id];
            if (known == null || known.serialNumber == null) {
              await refreshDevice(id);
            } else {
              await _pingKnownDevice(known);
            }
          }(),
      ]);

      // In-app notifications (Docs/App/Settings.md events):
      //   - a device that was unknown becomes a "discovered" event;
      //   - a device that was reachable but failed this sweep is "lost".
      for (final id in _devices.keys) {
        final entry = _devices[id]!;
        if (!reachableBefore.contains(id)) {
          if (!entry.stale) {
            notifyAppEvent(
              'Device discovered',
              '${entry.displayName} discovered',
            );
          }
        } else if (entry.stale) {
          notifyAppEvent('Device lost', '${entry.displayName} lost');
        }
      }
    } catch (error) {
      lastError = error.toString();
    } finally {
      _refreshing = false;
      notifyListeners();
    }
  }

  /// Records one whole answer: the node is present and its miss count starts over.
  void _markAnswered(DeviceEntry entry) {
    entry.consecutiveMisses = 0;
    entry.stale = false;
    entry.lastSeen = DateTime.now();
  }

  /// Records one missed reply; the node reads stale only once [deviceMissThreshold] run together.
  void _markMissed(DeviceEntry entry) {
    entry.consecutiveMisses++;
    if (entry.consecutiveMisses >= deviceMissThreshold) entry.stale = true;
  }

  /// One known device's ping for this sweep, re-sent until a whole reply arrives.
  ///
  /// A refused reply (FAIL), or one that lost a fragment or stopped before its STOP frame, is NOT
  /// an answer: `PacketResponse.gap` is the same test `sndb.readSndbRoster` applies to the roster,
  /// so discovery judges a truncated reply one way rather than two. Every lost reply is one
  /// consecutive miss and every whole answer clears the count ([_markAnswered]), so a single lost
  /// frame leaves the node PRESENT while a device that is really gone still reaches
  /// [deviceMissThreshold] inside this same sweep - the stale bound stays one refresh, and a
  /// sweep in which nothing is lost still costs one send per device.
  Future<void> _pingKnownDevice(DeviceEntry entry) async {
    final id = entry.id;
    for (var attempt = 1; attempt <= devicePingAttempts; attempt++) {
      final reply = await _pingReply(id);
      if (reply != null && !reply.fail && reply.gap == null) {
        _markAnswered(entry);
        return;
      }
      final why = reply == null
          ? 'no reply'
          : reply.fail
          ? 'the device refused the ping (FAIL)'
          : reply.gap!.message;
      _markMissed(entry);
      AppDiagnostics.log(
        'db',
        'dev ${idToString(id)} ping $attempt/$devicePingAttempts lost - $why '
            '(miss ${entry.consecutiveMisses}/$deviceMissThreshold)',
      );
      if (attempt < devicePingAttempts) {
        await Future<void>.delayed(devicePingBackoff[attempt - 1]);
      }
    }
  }

  /// Device CID 1 (Ping) through whichever request the channel offers.
  ///
  /// The app's connection implements [TolerantRequestChannel], which is what lets a reply that
  /// lost a fragment arrive at all; the caller then rejects it by its [PacketResponse.gap]
  /// instead of reading a prefix as an answer. Any other channel gets the plain request.
  Future<PacketResponse?> _pingReply(int id) async {
    final channel = requestChannel;
    if (channel == null || !channel.isConnected) return null;
    try {
      if (channel is TolerantRequestChannel) {
        return await channel.requestTolerant(
          id,
          ServiceType.device,
          1,
          timeout: const Duration(milliseconds: 800),
        );
      }
      final payload = await channel.request(
        id,
        ServiceType.device,
        1,
        timeout: const Duration(milliseconds: 800),
      );
      return PacketResponse(payload: payload, success: true, fail: false);
    } catch (error) {
      AppDiagnostics.log('db', 'dev ${idToString(id)} ping failed: $error');
      return null;
    }
  }

  /// Plants a known device (one with a serial), so a test can drive the ping branch of a sweep
  /// without a bus.
  @visibleForTesting
  void seedDeviceForTest(DeviceEntry entry) => _devices[entry.id] = entry;

  /// SNDB Write per docs 00.12
  Future<bool> sndbWrite(List<int> sn, int id) => sndb.sndbWrite(sn, id);

  /// SNDB Delete per docs 00.12 with ID 0
  Future<bool> sndbDelete(List<int> sn) => sndb.sndbDelete(sn);

  /// Full SNDB dump for the SNDB viewer: [id, serial hex] pairs.
  Future<List<(int, String)>> sndbEntries() async {
    final entries = await sndb.sndbEntries();
    return [for (final e in entries) (e.id, e.serial)];
  }
}

extension _SortExt<T> on Iterable<T> {
  List<T> sortedBy(int Function(T) key) =>
      toList()..sort((a, b) => key(a).compareTo(key(b)));
}
