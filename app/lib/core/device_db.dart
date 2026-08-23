/// In-RAM device database (Docs/App/General info.md): "App keeps local database
/// in RAM, updates upon arrival of new data."
///
/// The app always reaches the directly connected device via ID 1 (Net 0) and
/// learns about the rest of the network from it (Device service / SNDB).
library;

import 'package:flutter/foundation.dart';

import 'connection.dart';
import 'diagnostics.dart';
import 'protocol.dart';
import 'types.dart';

const int coreId = 1;

/// App session start (wall clock): reference for the relative time-offset probe.
final int _sessionStartMs = DateTime.now().millisecondsSinceEpoch;

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

  /// Pings the connected device (ID 1).
  Future<bool> pingCore() async {
    final reply = await _request(coreId, ServiceType.device, 1, timeout: const Duration(milliseconds: 800));
    return reply != null;
  }

  /// Refreshes one device's identity fields into the database.
  Future<DeviceEntry?> refreshDevice(int id) async {
    if (!_link.isConnected) return null;

    final entry = _devices.putIfAbsent(id, () => DeviceEntry(id: id));

    final snReply =
        await _request(id, ServiceType.device, 3, timeout: const Duration(seconds: 2));
    if (snReply == null) {
      entry.stale = true;
      notifyListeners();
      return null; // unreachable
    }
    entry.stale = false;
    entry.lastSeen = DateTime.now();
    if (snReply.length >= 14) {
      entry.serialNumber = serialNumberToHex(snReply.sublist(0, 14));
    }

    final typeReply = await _request(id, ServiceType.device, 2);
    if (typeReply != null && typeReply.length >= 2) {
      entry.type = DeviceType.fromValue(typeReply[0] | (typeReply[1] << 8));
    }

    final versionReply = await _request(id, ServiceType.device, 4);
    if (versionReply != null && versionReply.isNotEmpty) {
      entry.softwareVersion = String.fromCharCodes(versionReply);
    }

    final capReply = await _request(id, ServiceType.device, 5);
    if (capReply != null && capReply.length >= 4) {
      entry.capabilities = uint32FromBytes(capReply);
    }

    final nameReply = await _request(id, ServiceType.device, 6);
    if (nameReply != null && nameReply.isNotEmpty) {
      entry.name = String.fromCharCodes(nameReply);
    }

    notifyListeners();
    return entry;
  }

  /// Reads live runtime values (uptime + loop times) for the device view.
  Future<void> refreshRuntime(int id) async {
    final entry = _devices[id];
    if (entry == null) return;

    final uptimeReply = await _request(id, ServiceType.device, 8);
    if (uptimeReply != null && uptimeReply.length >= 4) {
      entry.uptimeMs = uint32FromBytes(uptimeReply);
    }
    final loopReply = await _request(id, ServiceType.device, 9);
    if (loopReply != null && loopReply.length >= 8) {
      entry.avgLoopTimeMs = numberFromBytes(loopReply, 0);
      entry.maxLoopTimeMs = numberFromBytes(loopReply, 4);
    }

    // Time offset (Device view): NTP-style estimate from a Time sync probe
    // (CID 10). The reply carries {time sent, t1, t2} in DEVICE uptime ms;
    // theta = (t1 + t2)/2 - t_app tells how the device clock relates to the
    // app session clock.
    final tApp = DateTime.now().millisecondsSinceEpoch - _sessionStartMs;
    final syncReply = await _request(id, ServiceType.device, 10,
        payload: uint32ToBytes(tApp & 0xFFFFFFFF));
    if (syncReply != null && syncReply.length >= 12) {
      final t1 = uint32FromBytes(syncReply, 4);
      final t2 = uint32FromBytes(syncReply, 8);
      entry.timeOffsetMs = ((t1 + t2) >> 1) - tApp;
    }

    entry.lastSeen = DateTime.now();
    notifyListeners();
  }

  /// Renames a device (Device service CID 7) and updates the database.
  Future<bool> setName(int id, String name) async {
    final bytes = name.codeUnits.take(23).toList();
    final reply = await _request(id, ServiceType.device, 7, payload: bytes);
    if (reply == null) return false;
    final entry = _devices[id];
    if (entry != null) entry.name = String.fromCharCodes(reply);
    notifyListeners();
    return true;
  }

  // ===========================================================================
  // Network discovery
  // ===========================================================================

  /// Full sweep: query the core, then walk every registered device from the
  /// SNDB (Device service CID 12). Safe to call repeatedly.
  Future<void> refreshNetwork() async {
    if (_refreshing || !_link.isConnected) return;
    _refreshing = true;
    lastError = null;
    notifyListeners();

    try {
      for (final device in _devices.values) {
        device.stale = true;
      }

      await refreshDevice(coreId);

      final sndbReply = await _request(coreId, ServiceType.device, 12,
          timeout: const Duration(seconds: 5));
      if (sndbReply == null) {
        throw Exception('SNDB read failed');
      }
      final ids = <int>{coreId};
      for (var offset = 0; offset + 16 <= sndbReply.length; offset += 16) {
        ids.add(sndbReply[offset + 14] | (sndbReply[offset + 15] << 8));
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
    } catch (error) {
      lastError = error.toString();
    } finally {
      _refreshing = false;
      notifyListeners();
    }
  }

  /// Looks up which serial number belongs to an ID (SNDB Read CID 13).
  Future<String?> serialNumberOf(int id) async {
    final reply = await _request(coreId, ServiceType.device, 13,
        payload: [id & 0xFF, (id >> 8) & 0xFF]);
    if (reply == null || reply.length < 14) return null;
    return serialNumberToHex(reply.sublist(0, 14));
  }

  /// Full SNDB dump for the SNDB viewer: [id, serial hex] pairs.
  Future<List<(int, String)>> sndbEntries() async {
    final reply = await _request(coreId, ServiceType.device, 12,
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
