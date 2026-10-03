/// Subscriptions service client (Docs/Services/Subscriptions.md).
library;

import 'dart:async';

import 'connection.dart';
import 'protocol.dart';
import 'types.dart';

class SubscriptionClient {
  final int deviceId;
  // Value updates now carry the RAW value bytes (no TLFV header).
  final _valueUpdateController = StreamController<List<int>>.broadcast();

  SubscriptionClient({required this.deviceId});

  ServiceType get service => ServiceType.subscriptions;

  Duration get _requestTimeout => const Duration(seconds: 3);

  Stream<List<int>> get valueUpdates => _valueUpdateController.stream;

  // --- CIDs (Docs "Requester commands (041x)" / "Provider commands (042x)") ---
  static const int _cidGetRequester = 0x10;
  static const int _cidSetRequester = 0x11;
  static const int _cidRecallAll = 0x12;
  static const int _cidSaveAll = 0x13;
  static const int _cidGetProvider = 0x20;
  static const int _cidSetProvider = 0x21;

  /// Registers the client as the receiver of firmware-pushed subscription value
  /// updates (service Subscriptions CID 0). Must be called while connected; call
  /// [stopListening] when the page closes.
  void startListening() {
    ConnectionManager.instance.setSubscriptionListener((payload) {
      _valueUpdateController.add(payload);
    });
  }

  void stopListening() {
    ConnectionManager.instance.setSubscriptionListener(null);
  }

  // Use normal request with ConnectionManager's transaction ID
  Future<List<int>?> _request(int cid,
      {List<int> payload = const [], Duration? timeout, int? toDevice, int? transactionId}) async {
    try {
      return await ConnectionManager.instance.request(toDevice ?? deviceId, service, cid,
          payload: payload, timeout: timeout ?? _requestTimeout, transactionId: transactionId);
    } catch (error) {
      return null;
    }
  }

  /// Decodes a "count + fixed-size entries" reply: the provider table is 32-byte entries and
  /// the requester table 28-byte ones (CIDs 0x20 and 0x10 respectively).
  Future<List<T>> _getSubscriptionList<T>(
      int cid, int entrySize, T Function(int index, List<int> bytes) decode) async {
    final reply = await _request(cid, payload: []);
    if (reply == null || reply.isEmpty) return [];

    final count = reply[0];
    final result = <T>[];
    int offset = 1;
    for (int i = 0; i < count && offset + entrySize <= reply.length; i++) {
      result.add(decode(i, reply.sublist(offset, offset + entrySize)));
      offset += entrySize;
    }
    return result;
  }

  /// CID 0x20: Get provider subscriptions (device as provider)
  Future<List<ProviderSubscription>> getProviderSubscriptions() =>
      _getSubscriptionList(_cidGetProvider, 32, ProviderSubscription.fromBytes);

  /// CID 0x10: Get requester subscriptions (device as requester - Tamu only)
  Future<List<RequesterSubscription>> getRequesterSubscriptions() =>
      _getSubscriptionList(_cidGetRequester, 28, RequesterSubscription.fromBytes);

  /// Finds the lowest free subscription TRID in the reserved 0x1000-0x1FFF range.
  Future<int?> _allocateTrid() async {
    final used = (await getRequesterSubscriptions()).map((s) => s.trid).toSet();
    for (var t = tridSubBase; t <= tridSubMax; t++) {
      if (!used.contains(t)) return t;
    }
    return null;
  }

  /// CID 0x11: Set (create/update) a requester subscription. The entry's TRID is the
  /// subscription identity; allocate one from 0x1000-0x1FFF for a new entry.
  Future<bool> setRequesterSubscriptionEntry(RequesterSubscription entry) async {
    final reply =
        await _request(_cidSetRequester, payload: entry.toCreatePayload(), transactionId: entry.trid);
    return reply != null;
  }

  /// CID 0x11: Cancel the requester subscription with `trid` (trigger None = delete).
  Future<bool> cancelRequesterSubscription(int trid) async {
    final reply = await _request(_cidSetRequester,
        payload: RequesterSubscription.cancel(trid).toCreatePayload(), transactionId: trid);
    return reply != null;
  }

  /// Index-based facade over the TRID protocol (the UI addresses rows by position).
  /// `entry == null` cancels the row at [index]; otherwise it creates/updates, allocating a
  /// fresh TRID when the entry does not carry one.
  Future<bool> setRequesterSubscription(int index, {RequesterSubscription? entry}) async {
    if (entry == null) {
      final subs = await getRequesterSubscriptions();
      if (index < 0 || index >= subs.length) return false;
      return cancelRequesterSubscription(subs[index].trid);
    }
    var e = entry;
    if (e.trid < tridSubBase || e.trid > tridSubMax) {
      final trid = await _allocateTrid();
      if (trid == null) return false;
      e = RequesterSubscription(
        index: index,
        providerAddr: e.providerAddr,
        trid: trid,
        sourceReg: e.sourceReg,
        targetReg: e.targetReg,
        trigger: e.trigger,
        periodMs: e.periodMs,
        minTimeMs: e.minTimeMs,
        deadzone: e.deadzone,
      );
    }
    return setRequesterSubscriptionEntry(e);
  }

  /// CID 0x12: Recall all requester subscriptions from the persisted file.
  Future<bool> recallAll() async => (await _request(_cidRecallAll, payload: [])) != null;

  /// CID 0x13: Save all requester subscriptions to the persisted file.
  Future<bool> saveAll() async => (await _request(_cidSaveAll, payload: [])) != null;

  /// CID 0x21: Set a provider subscription directly (management; trigger None cancels).
  Future<bool> setProviderSubscription(ProviderSubscription entry) async {
    final reply = await _request(_cidSetProvider,
        payload: _providerPayload(entry), transactionId: entry.trid, toDevice: deviceId);
    return reply != null;
  }

  /// Provider entry wire (Docs "Provider table entry", 32 B): requesterAddr, trid, the shared
  /// subscription table, lastSent, hash, timeout (ignored by Set).
  List<int> _providerPayload(ProviderSubscription entry) {
    final buf = <int>[];
    buf.addAll([entry.requesterAddr & 0xFF, (entry.requesterAddr >> 8) & 0xFF]);
    buf.addAll([entry.trid & 0xFF, (entry.trid >> 8) & 0xFF]);
    buf.addAll(entry.table.toBytes());
    buf.addAll(uint32ToBytes(entry.lastSentMs));
    buf.addAll(uint32ToBytes(entry.hash));
    buf.addAll(uint32ToBytes(entry.timeout));
    return buf;
  }

  void dispose() {
    _valueUpdateController.close();
  }
}
