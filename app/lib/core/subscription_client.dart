/// Subscriptions service client (Docs/Services/Subscriptions.md).
library;

import 'dart:async';

import 'diagnostics.dart';
import 'protocol.dart';
import 'request_channel.dart';
import 'types.dart';

class SubscriptionClient {
  final int deviceId;
  // Value updates now carry the RAW value bytes (no TLFV header).
  final _valueUpdateController = StreamController<List<int>>.broadcast();

  SubscriptionClient({required this.deviceId});

  /// The first failure of this client's requests since the caller last cleared it, so a restore can
  /// report the code a refused requester write answered with (see [RequestTrace]).
  final RequestTrace _trace = RequestTrace();

  /// This client's request faults, for a caller that reports WHY a requester row did not land.
  RequestTrace get trace => _trace;

  ServiceType get service => ServiceType.subscriptions;

  Duration get _requestTimeout => const Duration(seconds: 3);

  Stream<List<int>> get valueUpdates => _valueUpdateController.stream;

  // --- CIDs (Docs "Requester commands (041x)" / "Provider commands (042x)") ---
  // The requester table is 0x10/0x11; the provider table 0x20/0x21.
  static const int _cidGetRequester = 0x10;
  static const int _cidSetRequester = 0x11;
  static const int _cidSaveRequester = 0x13;
  static const int _cidGetProvider = 0x20;

  /// SubCid::Change (0x01): the requester installs/cancels a provider entry on a node. This is
  /// the install command - `ProviderSet` (0x21) is a different one and is not it.
  static const int _cidSetProvider = 0x01;

  /// SubCid::ProviderSet (0x21): installs a FULL provider-table entry on a node, carrying the
  /// requester's address in the payload (the payload is the 32-byte entry, and the TRID rides the
  /// packet header). Change (0x01) is the requester's own install - its requester address is the
  /// packet source, so the app cannot name a requester that is not on the link; ProviderSet is
  /// what lets a restore rebuild a provider entry for a remote requester.
  static const int _cidProviderSet = 0x21;

  /// Registers the client as the receiver of firmware-pushed subscription value
  /// updates (service Subscriptions CID 0). Must be called while connected; call
  /// [stopListening] when the page closes.
  ///
  /// NOTE: no UI consumer subscribes to [valueUpdates] yet, so pushed values are
  /// currently dropped. Wiring it into the Subscriptions page is a UI follow-up.
  void startListening() {
    requestChannel?.setSubscriptionListener((payload) {
      _valueUpdateController.add(payload);
    });
  }

  void stopListening() {
    requestChannel?.setSubscriptionListener(null);
  }

  // Use a normal request with the channel's transaction ID. The reply carries the
  // packet SUCCESS/FAIL flags, so callers check [PacketResponse.ok] rather than just
  // "a payload came back".
  Future<PacketResponse?> _request(
    int cid, {
    List<int> payload = const [],
    Duration? timeout,
    int? toDevice,
    int? transactionId,
  }) async {
    final channel = requestChannel;
    if (channel == null) {
      AppDiagnostics.log('subscriptions', 'no link');
      _trace.record(
        RequestFault(
          kind: RequestFaultKind.noLink,
          deviceId: toDevice ?? deviceId,
          service: service,
          cid: cid,
        ),
      );
      return null;
    }
    try {
      final response = await channel.requestWithFlags(
        toDevice ?? deviceId,
        service,
        cid,
        payload: payload,
        timeout: timeout ?? _requestTimeout,
        transactionId: transactionId,
      );
      // A FAIL with the packet flags is the device refusing this entry; recording it is what lets
      // a restore report the code instead of only that the cancel did not land.
      if (response.fail) {
        _trace.refused(
          toDevice ?? deviceId,
          service: service,
          cid: cid,
          payload: response.payload,
        );
      }
      return response;
    } catch (error) {
      AppDiagnostics.log('subscriptions', 'request failed: $error');
      _trace.record(
        RequestFault(
          kind: RequestFaultKind.noReply,
          deviceId: toDevice ?? deviceId,
          service: service,
          cid: cid,
          detail: '$error',
        ),
      );
      return null;
    }
  }

  /// Reads a fixed-size-entry table, or null when it could NOT be read: no link, no reply within
  /// the timeout, or a FAIL answer. Both tables are 32-byte entries (provider CID 0x20, requester
  /// CID 0x10) - a requester row is providerAddr, TRID, the table, the target register, the
  /// timeout and its hash. Entries fill the stream.
  ///
  /// The null is what lets a caller that DELETES rows tell "the device holds nothing" from "I
  /// could not look". An empty table is a real answer - the firmware streams one empty fragment
  /// for a count of zero (`SubscriptionsControl.h`, `StreamSubscriptionTable`) - so `[]` here is
  /// never a stand-in for a failed read.
  ///
  /// Only the FAIL flag is checked, not [PacketResponse.ok]: a table reply is a FRAGMENT stream,
  /// and fragments carry neither SUCCESS nor FAIL (`SendFragFragment` sets FLAG_TYPE|FLAG_FRAG and
  /// the start/stop bits), so demanding SUCCESS would make every real table read look like a
  /// failure.
  Future<List<T>?> _readSubscriptionList<T>(
    int cid,
    int entrySize,
    T Function(int index, List<int> bytes) decode,
  ) async {
    final reply = await _request(cid, payload: []);
    if (reply == null || reply.fail) {
      AppDiagnostics.log(
        'subscriptions',
        'table read CID 0x${cid.toRadixString(16)} unreadable: '
            '${reply == null ? 'no reply' : 'device answered FAIL'}',
      );
      return null;
    }

    final result = <T>[];
    int offset = 0;
    final data = reply.payload;
    for (int i = 0; offset + entrySize <= data.length; i++) {
      result.add(decode(i, data.sublist(offset, offset + entrySize)));
      offset += entrySize;
    }
    return result;
  }

  /// CID 0x20: Get provider subscriptions (device as provider). Empty when the read fails.
  Future<List<ProviderSubscription>> getProviderSubscriptions() async =>
      await _readSubscriptionList(
        _cidGetProvider,
        32,
        ProviderSubscription.fromBytes,
      ) ??
      const [];

  /// CID 0x21: change a provider subscription on a node, telling it to send [entry]'s source
  /// register to the requester that asked. The entry is keyed by its TRID, and a table whose
  /// trigger is None cancels it. This is the half that makes a restored subscription live: the
  /// requester table alone leaves the node with nothing to send.
  /// Reads the reply as acceptance. The device answers a provider change with the source's
  /// CURRENT VALUE (so the requester starts from a known state), or with a short ack when the
  /// source does not resolve - either way a reply means the entry was accepted; only a timeout
  /// (null) is a failure. Checking `ok` instead reported false on every success.
  Future<bool> setProviderSubscriptionEntry(ProviderSubscription entry) async {
    final reply = await _request(
      _cidSetProvider,
      payload: entry.toChangePayload(),
      transactionId: entry.trid,
    );
    return reply != null;
  }

  /// CID 0x21 (ProviderSet): installs a full provider-table [entry] on the node, addressed by
  /// its TRID (in the packet header) and its requester's address (in the payload). Unlike
  /// [setProviderSubscriptionEntry], the requester's address is carried rather than taken from
  /// the packet source, so the app can rebuild the entry a remote requester installed. The node
  /// answers with the SUCCESS/FAIL flag, so `ok` is the acceptance signal.
  Future<bool> setProviderTableEntry(ProviderSubscription entry) async {
    final reply = await _request(
      _cidProviderSet,
      payload: entry.toBytes(),
      transactionId: entry.trid,
    );
    return reply?.ok ?? false;
  }

  /// CID 0x10: Get requester subscriptions (device as requester - Tamu only). Empty when the read
  /// fails, which keeps every pre-existing caller's behaviour; a caller that DELETES rows uses
  /// [readRequesterSubscriptions] instead, because it must never read a failed read as an empty
  /// table.
  Future<List<RequesterSubscription>> getRequesterSubscriptions() async =>
      await readRequesterSubscriptions() ?? const [];

  /// CID 0x10 as a read that can say "no answer": the target's requester table, or null when the
  /// device did not return it (no link, timeout, or a FAIL answer). `[]` is a real, empty table.
  /// Its entries are 32 B on the wire (the requester table's Hash/Hashlike at offset 28 included).
  Future<List<RequesterSubscription>?> readRequesterSubscriptions() =>
      _readSubscriptionList(
        _cidGetRequester,
        32,
        RequesterSubscription.fromBytes,
      );

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
  Future<bool> setRequesterSubscriptionEntry(
    RequesterSubscription entry,
  ) async {
    final reply = await _request(
      _cidSetRequester,
      payload: entry.toCreatePayload(),
      transactionId: entry.trid,
    );
    return reply?.ok ?? false;
  }

  /// CID 0x11: Cancel the requester subscription with `trid` (trigger None = delete).
  Future<bool> cancelRequesterSubscription(int trid) async {
    final reply = await _request(
      _cidSetRequester,
      payload: RequesterSubscription.cancel(trid).toCreatePayload(),
      transactionId: trid,
    );
    return reply?.ok ?? false;
  }

  /// CID 0x13: Save all subscriptions. Rewrites the requester table's persistence file (`.SUBREQ`)
  /// from the LIVE table, so the entries currently in RAM are exactly the ones a boot recalls.
  ///
  /// This is the durability half of a cancel: a Set with a None trigger removes the entry from RAM
  /// (and the firmware's cancel path rewrites the file itself - `SubscriptionsControl.h`,
  /// RequesterSet), but only a Save states, in one command, that the table as it now stands is the
  /// one to keep. Its answer is the device's own write result, so a failed save is visible instead
  /// of being read as "the table is what the app just sent".
  ///
  /// The counterpart, Recall all (CID 0x12), re-reads `.SUBREQ` over the live table; nothing here
  /// uses it - on a restore it would undo the cancels it had just made.
  Future<bool> saveRequesterSubscriptions() async {
    final reply = await _request(_cidSaveRequester, payload: const []);
    return reply?.ok ?? false;
  }

  /// UI-position facade over the TRID-keyed protocol: [index] is the row's position in the
  /// current list. `entry == null` cancels the row; otherwise it creates/updates it,
  /// allocating a fresh TRID when the entry does not carry one.
  Future<bool> setRequesterSubscription(
    int index, {
    RequesterSubscription? entry,
  }) async {
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

  // The persisting recall CID (0x12) and the direct provider-set CID (0x21) are firmware
  // management commands the app does not currently expose: the Subscriptions page only edits the
  // requester table, and only the restore persists it (see [saveRequesterSubscriptions]). Add
  // them here if a UI needs table persistence or direct provider management.

  void dispose() {
    _valueUpdateController.close();
  }
}
