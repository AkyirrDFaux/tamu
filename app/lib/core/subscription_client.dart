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
  // The requester table is 0x10/0x11; the provider table 0x20/0x21.
  static const int _cidGetRequester = 0x10;
  static const int _cidSetRequester = 0x11;
  static const int _cidGetProvider = 0x20;

  /// Registers the client as the receiver of firmware-pushed subscription value
  /// updates (service Subscriptions CID 0). Must be called while connected; call
  /// [stopListening] when the page closes.
  ///
  /// NOTE: no UI consumer subscribes to [valueUpdates] yet, so pushed values are
  /// currently dropped. Wiring it into the Subscriptions page is a UI follow-up.
  void startListening() {
    ConnectionManager.instance.setSubscriptionListener((payload) {
      _valueUpdateController.add(payload);
    });
  }

  void stopListening() {
    ConnectionManager.instance.setSubscriptionListener(null);
  }

  // Use normal request with ConnectionManager's transaction ID. The reply carries the
  // packet SUCCESS/FAIL flags, so callers check [PacketResponse.ok] rather than just
  // "a payload came back".
  Future<PacketResponse?> _request(int cid,
      {List<int> payload = const [], Duration? timeout, int? toDevice, int? transactionId}) async {
    try {
      return await ConnectionManager.instance.requestWithFlags(toDevice ?? deviceId, service, cid,
          payload: payload, timeout: timeout ?? _requestTimeout, transactionId: transactionId);
    } catch (error) {
      return null;
    }
  }

  /// Decodes a fixed-size-entry table reply: the provider table is 32-byte entries and the
  /// requester table 28-byte ones (CIDs 0x20 and 0x10 respectively). Entries fill the stream.
  Future<List<T>> _getSubscriptionList<T>(
      int cid, int entrySize, T Function(int index, List<int> bytes) decode) async {
    final reply = await _request(cid, payload: []);
    final data = reply?.payload;
    if (data == null || data.isEmpty) return [];

    final result = <T>[];
    int offset = 0;
    for (int i = 0; offset + entrySize <= data.length; i++) {
      result.add(decode(i, data.sublist(offset, offset + entrySize)));
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
    final reply = await _request(_cidSetRequester,
        payload: entry.toCreatePayload(), transactionId: entry.trid);
    return reply?.ok ?? false;
  }

  /// CID 0x11: Cancel the requester subscription with `trid` (trigger None = delete).
  Future<bool> cancelRequesterSubscription(int trid) async {
    final reply = await _request(_cidSetRequester,
        payload: RequesterSubscription.cancel(trid).toCreatePayload(), transactionId: trid);
    return reply?.ok ?? false;
  }

  /// UI-position facade over the TRID-keyed protocol: [index] is the row's position in the
  /// current list. `entry == null` cancels the row; otherwise it creates/updates it,
  /// allocating a fresh TRID when the entry does not carry one.
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

  // The persisting recall/save CIDs (0x12/0x13) and the direct provider-set CID
  // (0x21) are firmware-management commands the app does not currently expose:
  // the Subscriptions page only edits the requester table. Add them here if a UI
  // needs table persistence or direct provider management.

  void dispose() {
    _valueUpdateController.close();
  }
}
