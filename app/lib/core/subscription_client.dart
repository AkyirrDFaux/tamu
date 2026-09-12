/// Subscriptions service client (Docs/Services/Subscriptions.md).
library;

import 'dart:async';

import 'connection.dart';
import 'protocol.dart';
import 'types.dart';

class SubscriptionClient {
  final int deviceId;
  final _valueUpdateController = StreamController<Tlvf>.broadcast();

  SubscriptionClient({required this.deviceId});

  ServiceType get service => ServiceType.subscriptions;
  String get logTag => 'subscription';

  Duration get _requestTimeout => const Duration(seconds: 3);

  Stream<Tlvf> get valueUpdates => _valueUpdateController.stream;

  /// Registers the client as the receiver of firmware-pushed subscription value
  /// updates (service Subscriptions CID 0). Must be called while connected; call
  /// [stopListening] when the page closes.
  void startListening() {
    ConnectionManager.instance.setSubscriptionListener((payload) {
      final tlvf = Tlvf.fromBytes(payload);
      if (tlvf != null) {
        _valueUpdateController.add(tlvf);
      }
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

  /// CID 2: Get provider subscriptions (device as provider)
  Future<List<ProviderSubscription>> getProviderSubscriptions() async {
    final reply = await _request(2, payload: []);
    if (reply == null || reply.isEmpty) return [];

    final count = reply[0];
    final result = <ProviderSubscription>[];
    int offset = 1;
    for (int i = 0; i < count && offset < reply.length; i++) {
      if (offset + 4 > reply.length) break;
      final entryLen = 4 + 2 + 1 + 4 + 4 + 4 + 4 + 1;
      if (offset + entryLen > reply.length) break;
      final tolLen = reply[offset + entryLen - 1];
      if (offset + entryLen + tolLen > reply.length) break;
      final lastValLen = reply[offset + entryLen + tolLen];
      if (offset + entryLen + tolLen + 1 + lastValLen > reply.length) break;
      
      final totalLen = entryLen + tolLen + 1 + lastValLen;
      final entryBytes = reply.sublist(offset, offset + totalLen);
      result.add(ProviderSubscription.fromBytes(i, entryBytes));
      offset += totalLen;
    }
    return result;
  }

  /// CID 3: Get requester subscriptions (device as requester - Tamu only)
  Future<List<RequesterSubscription>> getRequesterSubscriptions() async {
    final reply = await _request(3, payload: []);
    if (reply == null || reply.isEmpty) return [];

    final count = reply[0];
    final result = <RequesterSubscription>[];
    int offset = 1;
    for (int i = 0; i < count && offset < reply.length; i++) {
      if (offset + 4 > reply.length) break;
      final entryLen = 4 + 4 + 2 + 1 + 4 + 4 + 4 + 1;
      if (offset + entryLen > reply.length) break;
      final tolLen = reply[offset + entryLen - 1];
      if (offset + entryLen + tolLen + 2 > reply.length) break;
      
      final totalLen = entryLen + tolLen + 2;
      final entryBytes = reply.sublist(offset, offset + totalLen);
      result.add(RequesterSubscription.fromBytes(i, entryBytes));
      offset += totalLen;
    }
    return result;
  }

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
      // Delete: clear the requester entry and cancel the provider subscription. Read
      // the stored entry first to learn the provider address + shared TRID (the
      // provider entry is found by TRID, and the requester entry is gone after delete).
      int? providerAddr;
      int? trid;
      final before = await getRequesterSubscriptions();
      for (final s in before) {
        if (s.index == index) {
          providerAddr = s.providerAddr;
          trid = s.trid;
          break;
        }
      }
      final reply = await _request(4, payload: [index], transactionId: trid == null ? null : trid & 0xFF);
      if (reply == null) return false;
      if (providerAddr != null) {
        await _request(1, payload: [], timeout: const Duration(seconds: 1),
            toDevice: providerAddr, transactionId: trid == null ? null : trid & 0xFF);
      }
      return true;
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
  List<int> _providerPayload(RequesterSubscription entry) {
    final buf = <int>[];
    buf.addAll(uint32ToBytes(entry.targetReg));
    buf.addAll(uint32ToBytes(entry.sourceReg));
    buf.addAll([deviceId & 0xFF, (deviceId >> 8) & 0xFF]); // requester address = us
    buf.add(entry.trigger.value);
    buf.addAll(uint32ToBytes(entry.periodMs));
    buf.addAll(uint32ToBytes(entry.minTimeMs));
    buf.addAll(uint32ToBytes(entry.counter));
    buf.add(entry.tolerance.length);
    buf.addAll(entry.tolerance);
    return buf;
  }

  /// CID 5: Persist the requester table to its file. The firmware already saves on every
  /// set/delete; this forces a save so the current state survives the next reboot.
  Future<bool> saveRequesterSubscriptions() async {
    final reply = await _request(5, payload: []);
    return reply != null && reply.isNotEmpty && reply[0] != 0xFF;
  }

  void dispose() {
    _valueUpdateController.close();
  }
}