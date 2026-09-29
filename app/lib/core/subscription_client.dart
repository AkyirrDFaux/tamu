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
  Future<List<int>?> _request(int cid, {List<int> payload = const [], Duration? timeout, int? toDevice, int? transactionId}) async {
    try {
      return await ConnectionManager.instance.request(toDevice ?? deviceId, service, cid,
          payload: payload, timeout: timeout ?? _requestTimeout, transactionId: transactionId);
    } catch (error) {
      return null;
    }
  }

  /// Decodes a "count + fixed-size entries" reply: the provider table is 32-byte entries and
  /// the requester table 28-byte ones (CID 2 and 3 respectively).
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

  /// CID 2: Get provider subscriptions (device as provider)
  Future<List<ProviderSubscription>> getProviderSubscriptions() =>
      _getSubscriptionList(2, 32, ProviderSubscription.fromBytes);

  /// CID 3: Get requester subscriptions (device as requester - Tamu only)
  Future<List<RequesterSubscription>> getRequesterSubscriptions() =>
      _getSubscriptionList(3, 28, RequesterSubscription.fromBytes);

  /// CID 4: Set requester subscription (create/update/delete)
  /// Index only = delete
  /// Index + entry = create/update
  ///
  /// Per Docs/Services/Subscriptions.md ("A subscription service, initiated by the
  /// requester"), setting a requester subscription ALSO registers the provider side
  /// (CID 1 "Change subscription") so the provider knows to push value updates to this
  /// requester. The provider entry carries the REQUESTER's address (our device), not
  /// the provider's.
  Future<bool> setRequesterSubscription(int index, {RequesterSubscription? entry}) async {
    if (entry == null) {
      // Delete: the firmware clears the requester entry and cancels the provider side
      // (CID 1) using the shared TRID, so only the index-based CID 4 is sent here.
      final reply = await _request(4, payload: [index]);
      return reply != null;
    } else {
      // Create/update: register BOTH sides under one shared TRID so the provider's value
      // updates (which carry that TRID) are routed to this requester entry.
      final txId = ConnectionManager.instance.takeTxId();
      final buf = <int>[index];
      buf.addAll(entry.toCreatePayload());
      final reply = await _request(4, payload: buf, transactionId: txId);
      if (reply == null) return false;
      final prov = _providerPayload(entry);
      await _request(1, payload: prov, timeout: const Duration(seconds: 1),
          toDevice: entry.providerAddr, transactionId: txId);
      return true;
    }
  }

  /// Builds the provider-side subscription payload (CID 1). The provider's entry stores
  /// the REQUESTER's address so the provider knows where to send value updates.
  /// Wire: targetReg, sourceReg, requesterAddr, trigger + 24 pad, period, min, deadzone.
  List<int> _providerPayload(RequesterSubscription entry) {
    final buf = <int>[];
    buf.addAll(uint32ToBytes(entry.targetReg));
    buf.addAll(uint32ToBytes(entry.sourceReg));
    buf.addAll([deviceId & 0xFF, (deviceId >> 8) & 0xFF]); // requester address = us
    buf.add(entry.trigger.value);
    buf.addAll([0, 0, 0]); // 24-bit padding
    buf.addAll(uint32ToBytes(entry.periodMs));
    buf.addAll(uint32ToBytes(entry.minTimeMs));
    buf.addAll(numberToBytes(entry.deadzone));
    return buf;
  }

  void dispose() {
    _valueUpdateController.close();
  }
}