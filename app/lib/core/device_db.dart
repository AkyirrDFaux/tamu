/// In-RAM device database (Docs/App/General info.md): "App keeps local database
/// in RAM, updates upon arrival of new data."
///
/// The app always reaches the directly connected device via ID 1 (Net 0) and
/// learns about the rest of the network from it (Device service / SNDB).
library;

import 'package:flutter/foundation.dart';

import 'connection.dart';
import 'diagnostics.dart';
import 'notifications.dart';
import 'protocol.dart';
import 'types.dart';

const int coreId = 1;

/// Everything known about one network device.
class DeviceEntry {
  final int id;
  String name;
  /// Name the transport link was established under (e.g. the BLE advertisement
  /// name). When set it wins for display, keeping the identity the user picked
  /// in the connection list stable even though the reported device name (CID 6)
  /// may differ.
  String? linkName;
  String get displayName => name.isNotEmpty ? name : (linkName ?? 'Device ${idToString(id)}');
  DeviceType type;
  String? serialNumber; // 28-char hex
  String? softwareVersion;
  int capabilities;
  int? uptimeMs;
  double? avgLoopTimeMs;
  double? maxLoopTimeMs;
  int? timeOffsetMs;
  DateTime lastSeen;

  DeviceEntry({required this.id})
      : name = 'Device ${idToString(id)}',
        type = DeviceType.unknown,
        capabilities = 0,
        lastSeen = DateTime.now();

  bool get isCore => id == coreId || capabilities & Capability.core != 0;
  int get net => idNet(id);

  /// Marks a device unreachable this sweep.
  bool stale = false;
}

class DeviceDatabase extends ChangeNotifier {
  DeviceDatabase._();

  static final DeviceDatabase instance = DeviceDatabase._();

  final Map<int, DeviceEntry> _devices = {};
  bool _refreshing = false;
  String? lastError;

  bool get isRefreshing => _refreshing;

  List<DeviceEntry> get all =>
      List.unmodifiable(_devices.values.sortedBy((d) => d.id));

  DeviceEntry? byId(int id) => _devices[id];

  // ===========================================================================
  // Queries (Docs/Services/Device service.md)
  // ===========================================================================

  ConnectionManager get _link => ConnectionManager.instance;

  Future<List<int>?> _request(
    int targetId,
    ServiceType service,
    int cid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
  }) async {
    try {
      return await _link.request(targetId, service, cid,
          payload: payload, timeout: timeout);
    } catch (error) {
      AppDiagnostics.log('db',
          'dev ${idToString(targetId)} $service/$cid failed: $error');
      return null;
    }
  }

  /// Seeds the name the transport link was established under (connection-list
  /// identity) for the core device.
  void seedLinkName(String name) {
    final entry = _devices.putIfAbsent(coreId, () => DeviceEntry(id: coreId));
    if (entry.linkName != name) {
      entry.linkName = name;
      notifyListeners();
    }
  }

  /// Pings the connected device (ID 1).
  Future<bool> pingCore() async {
    final reply = await _request(coreId, ServiceType.device, 1, timeout: const Duration(milliseconds: 800));
    return reply != null;
  }

  /// Helper to read Register block type 0 (System) via Register 01.01
  Future<List<int>?> _registerRead(int targetId, int field, int key) async {
    final bi = (0 & 0x3FF) << 22 | (0 << 16) | (field << 8) | key;
    final payload = [bi & 0xFF, (bi>>8)&0xFF, (bi>>16)&0xFF, (bi>>24)&0xFF];
    final reply = await _request(targetId, ServiceType.register, 1, payload: payload);
    if (reply == null || reply.length < 8) return null;
    // reply is BlockInfo(4)+BlockMeta(4)+value
    return reply.sublist(8);
  }

  /// Refreshes one device's identity fields into the database via Register System block.
  Future<DeviceEntry?> refreshDevice(int id) async {
    if (!_link.isConnected) return null;

    final entry = _devices.putIfAbsent(id, () => DeviceEntry(id: id));

    // Check reachable via Ping 00.01
    final ping = await _request(id, ServiceType.device, 1, timeout: const Duration(milliseconds: 800));
    if (ping == null) {
      entry.stale = true;
      notifyListeners();
      return null;
    }
    entry.stale = false;
    entry.lastSeen = DateTime.now();

    final snReply = await _registerRead(id, 1, 0xFF);
    if (snReply != null && snReply.length >= 14) {
      entry.serialNumber = serialNumberToHex(snReply.sublist(0, 14));
    }
    final typeReply = await _registerRead(id, 0, 0);
    if (typeReply != null && typeReply.length >= 2) {
      entry.type = DeviceType.fromValue(typeReply[0] | (typeReply[1] << 8));
    }
    final versionReply = await _registerRead(id, 0, 2);
    if (versionReply != null && versionReply.length >= 4) {
      final year = versionReply[0];
      final month = versionReply[1];
      final day = versionReply[2];
      final iteration = versionReply[3];
      entry.softwareVersion = '$year.$month.$day.$iteration';
    }
    final capReply = await _registerRead(id, 0, 1);
    if (capReply != null && capReply.length >= 4) {
      entry.capabilities = uint32FromBytes(capReply);
    }
    final nameReply = await _registerRead(id, 6, 0xFF);
    if (nameReply != null && nameReply.isNotEmpty) {
      entry.name = String.fromCharCodes(nameReply).replaceAll('\x00', '').trim();
    }
    if (entry.name.isEmpty) entry.name = 'Device ${idToString(id)}';

    notifyListeners();
    return entry;
  }

  /// Reads live runtime values (uptime + loop times) via Register System block.
  Future<void> refreshRuntime(int id) async {
    final entry = _devices[id];
    if (entry == null) return;

    final uptimeReply = await _registerRead(id, 3, 0);
    if (uptimeReply != null && uptimeReply.length >= 4) {
      entry.uptimeMs = uint32FromBytes(uptimeReply);
    }
    final loopAvg = await _registerRead(id, 3, 3);
    final loopMax = await _registerRead(id, 3, 4);
    if (loopAvg != null && loopAvg.length >= 4) entry.avgLoopTimeMs = numberFromBytes(loopAvg, 0);
    if (loopMax != null && loopMax.length >= 4) entry.maxLoopTimeMs = numberFromBytes(loopMax, 0);

    if (id == coreId) {
      entry.timeOffsetMs = 0;
    } else {
      final coreBefore = await _registerRead(coreId, 3, 0);
      if (coreBefore != null && coreBefore.length >= 4) {
        final t0 = uint32FromBytes(coreBefore);
        final syncReply = await _request(id, ServiceType.device, 3,
            payload: uint32ToBytes(t0));
        if (syncReply != null && syncReply.length >= 12) {
          final t1 = uint32FromBytes(syncReply, 4);
          final t2 = uint32FromBytes(syncReply, 8);
          final coreAfter = await _registerRead(coreId, 3, 0);
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
    final bi = (0 & 0x3FF) << 22 | (0 << 16) | (6 << 8) | 0xFF;
    final meta = BlockMeta(flagsAndType: DataType.string.value | FieldFlags.persistent, size: bytes.length);
    final payload = [...[bi & 0xFF, (bi>>8)&0xFF, (bi>>16)&0xFF, (bi>>24)&0xFF], ...meta.toBytes(), ...bytes];
    final reply = await _request(id, ServiceType.register, 2, payload: payload);
    if (reply == null) return false;
    final entry = _devices[id];
    if (entry != null) entry.name = name;
    notifyListeners();
    return true;
  }

  /// Asks a device to identify itself (Device service CID 2): blink its red LED
  /// fast for ~10 s (docs). Returns true when the device acknowledged.
  Future<bool> identify(int id, {bool on = true}) async {
    final reply =
        await _request(id, ServiceType.device, 2, payload: [on ? 1 : 0]);
    return reply != null;
  }

  // ===========================================================================
  // Network discovery
  // ===========================================================================

  /// Full sweep: query the core, then walk every registered device from the
  /// SNDB (Device service CID 12). Safe to call repeatedly.
  Future<void>? _refreshInFlight;

  Future<void> refreshNetwork() async {
    if (!_link.isConnected) return;
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
    final reachableBefore =
        _devices.entries.where((e) => !e.value.stale).map((e) => e.key).toSet();

    try {
      for (final device in _devices.values) {
        device.stale = true;
      }

      await refreshDevice(coreId);

      final sndbReply = await _request(coreId, ServiceType.device, 13,
          timeout: const Duration(seconds: 5));
      if (sndbReply == null) {
        throw Exception('SNDB read failed');
      }
      final ids = <int>{coreId};
      for (var offset = 0; offset + 16 <= sndbReply.length; offset += 16) {
        final id = sndbReply[offset + 14] | (sndbReply[offset + 15] << 8);
        // ID 0 marks an unassigned entry; skip it so no phantom "device" is
        // created and no requests are fired at an invalid target.
        if (id == 0) continue;
        ids.add(id);
      }
      for (final id in ids) {
        if (_devices[id] == null || _devices[id]!.serialNumber == null) {
          await refreshDevice(id);
        } else {
          final entry = _devices[id]!;
          entry.stale = false;
          entry.lastSeen = DateTime.now();
        }
      }

      // In-app notifications (Docs/App/Settings.md events):
      //   - a device that was unknown becomes a "discovered" event;
      //   - a device that was reachable but failed this sweep is "lost".
      for (final id in _devices.keys) {
        final entry = _devices[id]!;
        if (!reachableBefore.contains(id)) {
          if (!entry.stale) {
            notifyAppEvent(
                'Device discovered', '${entry.displayName} discovered');
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

  /// SNDB Write per docs 00.12
  Future<bool> sndbWrite(List<int> sn, int id) async {
    final reply = await _request(coreId, ServiceType.device, 12,
        payload: [...sn, id & 0xFF, (id >> 8) & 0xFF],
        timeout: const Duration(seconds: 3));
    return reply != null && reply.length >= 16;
  }

  /// SNDB Delete per docs 00.12 with ID 0
  Future<bool> sndbDelete(List<int> sn) async {
    if (sn.length < 14) return false;
    final reply = await _request(coreId, ServiceType.device, 12,
        payload: [...sn.take(14), 0, 0],
        timeout: const Duration(seconds: 3));
    return reply != null && reply.length >= 16;
  }

  /// Full SNDB dump for the SNDB viewer: [id, serial hex] pairs.
  Future<List<(int, String)>> sndbEntries() async {
    final reply = await _request(coreId, ServiceType.device, 13,
        timeout: const Duration(seconds: 5));
    final entries = <(int, String)>[];
    if (reply == null) return entries;
    for (var offset = 0; offset + 16 <= reply.length; offset += 16) {
      final id = reply[offset + 14] | (reply[offset + 15] << 8);
      final sn = serialNumberToHex(reply.sublist(offset, offset + 14));
      entries.add((id, sn));
    }
    return entries;
  }
}

extension _SortExt<T> on Iterable<T> {
  List<T> sortedBy(int Function(T) key) => toList()..sort((a, b) => key(a).compareTo(key(b)));
}
