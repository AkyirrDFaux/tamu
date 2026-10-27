import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/subscription_client.dart';
import 'package:tamuapp/core/types.dart';

/// A link that answers every request with one canned payload - enough for a table read, which is a
/// single 0x10 request whose reply carries the whole table.
class _TableChannel implements RequestChannel {
  _TableChannel(this.reply);

  /// The table the device streams back. Named distinctly from the request's `payload` parameter,
  /// which would otherwise shadow it inside the overrides.
  final List<int> reply;

  @override
  bool get isConnected => true;

  @override
  int takeTxId() => 1;

  @override
  Future<List<int>> request(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
    bool requestFrag = false,
    int? transactionId,
  }) async => reply;

  @override
  Future<PacketResponse> requestWithFlags(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
    bool requestFrag = false,
    int? transactionId,
  }) async => PacketResponse(payload: reply, success: true, fail: false);

  @override
  void setSubscriptionListener(Function(List<int>)? listener) {}
}

RequesterSubscription _row({
  required int index,
  required int providerAddr,
  required int trid,
  required int targetField,
  required TriggerType trigger,
  int hash = 0,
}) => RequesterSubscription(
  index: index,
  providerAddr: providerAddr,
  trid: trid,
  sourceReg: makeBlockInfo(8, 0, 3, 0),
  targetReg: makeBlockInfo(0x0A, 0, targetField, 0),
  trigger: trigger,
  periodMs: 1000,
  minTimeMs: 50,
  deadzone: 0.5,
  hash: hash,
);

/// Guards the subscription wire codecs (Docs/Services/Subscriptions.md): requester entries are
/// 32 bytes on the wire (Hash/Hashlike at offset 28) though the Set payload carries only their
/// 28-byte prefix; provider entries are 32 bytes. Both carry a trailing Deadzone (Number).
void main() {
  test('requester Set payload is the 28-byte prefix, the wire entry 32 bytes', () {
    final sub = RequesterSubscription(
      index: 0,
      providerAddr: 2,
      trid: 0xFA00,
      targetReg: makeBlockInfo(
        dynamicTypeForIndex(0),
        dynamicInstanceForIndex(0),
        0,
        0,
      ),
      sourceReg: makeBlockInfo(8, 0, 3, 0),
      trigger: TriggerType.deltaPeriodic,
      periodMs: 500,
      minTimeMs: 100,
      deadzone: 1.5,
    );
    final payload = sub.toCreatePayload();
    expect(payload.length, 28);
    // providerAddr(2) trid(2) table(16: sourceReg+trigger+minTime+period+deadzone)
    // targetReg(4) timeout(4). The hash is NOT sent: the device zeroes it on Set.
    final decoded = RequesterSubscription.fromBytes(0, payload);
    expect(decoded.providerAddr, 2);
    expect(decoded.trigger, TriggerType.deltaPeriodic);
    expect(decoded.periodMs, 500);
    expect(decoded.minTimeMs, 100);
    expect(decoded.deadzone, closeTo(1.5, 1e-6));
    expect(decoded.hash, 0, reason: 'a 28 B payload carries no hash');
  });

  test('requester wire entry is 32 bytes with the hash at offset 28', () {
    final sub = _row(
      index: 0,
      providerAddr: 0xE402,
      trid: 0x1234,
      targetField: 1,
      trigger: TriggerType.deltaPeriodic,
      hash: 0xDEADBEEF,
    );
    final wire = sub.toWireBytes();
    expect(wire.length, 32);
    expect(wire.sublist(0, 28), sub.toCreatePayload());
    expect(wire.sublist(28), [0xEF, 0xBE, 0xAD, 0xDE], reason: 'little-endian');
    final decoded = RequesterSubscription.fromBytes(5, wire);
    expect(decoded.index, 5);
    expect(decoded.providerAddr, 0xE402);
    expect(decoded.trid, 0x1234);
    expect(decoded.hash, 0xDEADBEEF);
  });

  test('a two-row 32 B table reads at its own stride', () async {
    // The rows differ in both identity fields, their target field and their hash, so a wrong
    // stride cannot pass by landing on zeroes: at 28 B the second row would start two bytes early
    // and decode the first row's timeout bytes as its own providerAddr and TRID.
    final rows = [
      _row(
        index: 0,
        providerAddr: idFromString('39.2')!,
        trid: 0x1100,
        targetField: 1,
        trigger: TriggerType.edgeRising,
        hash: 0x2A,
      ),
      _row(
        index: 1,
        providerAddr: idFromString('39.3')!,
        trid: 0x1101,
        targetField: 7,
        trigger: TriggerType.periodic,
        hash: 0xBEEF,
      ),
    ];
    final payload = [for (final r in rows) ...r.toWireBytes()];
    expect(payload.length, 64);

    final previous = requestChannel;
    installRequestChannel(_TableChannel(payload));
    final client = SubscriptionClient(deviceId: idFromString('39.1')!);
    addTearDown(() {
      client.dispose();
      installRequestChannel(previous);
    });

    final read = await client.readRequesterSubscriptions();
    expect(read, isNotNull);
    expect(read, hasLength(2));

    expect(read![0].trid, 0x1100);
    expect(read[0].providerAddr, idFromString('39.2'));
    expect(read[1].trid, 0x1101, reason: 'row 2 starts at byte 32, not 28');
    expect(read[1].providerAddr, idFromString('39.3'));
    expect(read[1].targetReg, makeBlockInfo(0x0A, 0, 7, 0));

    expect(read[0].trigger, TriggerType.edgeRising);
    expect(read[0].hash, 0x2A, reason: 'offset 28 of its own row');
    expect(read[1].trigger, TriggerType.periodic);
    expect(read[1].hash, 0xBEEF);
    expect(read[1].deadzone, closeTo(0.5, 1e-6));
  });

  test('provider entry decodes 32 bytes with deadzone', () {
    final bytes = <int>[
      2, 0, // requesterAddr
      0x34, 0x12, // trid 0x1234
      // subscription table: sourceReg(4) trigger(1) minTime(3) period(4) deadzone(4)
      0x55, 0x55, 0x55, 0x55, // sourceReg
      TriggerType.edgeRising.value,
      0x64, 0, 0, // minTime 100
      0xE8, 0x03, 0, 0, // period 1000
      ...[0x00, 0xC0, 0x00, 0x00], // deadzone 0.75 (16.16 = 0xC000)
      0, 0, 0, 0, // lastSent
      0x2A, 0, 0, 0, // hash/counter
      0, 0, 0, 0, // timeout
    ];
    expect(bytes.length, 32);
    final p = ProviderSubscription.fromBytes(3, bytes);
    expect(p.index, 3);
    expect(p.requesterAddr, 2);
    expect(p.trid, 0x1234);
    expect(p.trigger, TriggerType.edgeRising);
    expect(p.periodMs, 1000);
    expect(p.minTimeMs, 100);
    expect(p.hash, 0x2A);
    expect(p.deadzone, closeTo(0.75, 1e-6));
  });

  test('all trigger types round-trip', () {
    for (final t in TriggerType.values) {
      expect(TriggerType.fromValue(t.value), t);
    }
    expect(TriggerType.edgeAny.isEdge, isTrue);
    expect(TriggerType.deltaPeriodic.isEdge, isFalse);
  });
}
