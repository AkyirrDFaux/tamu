import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/types.dart';

/// Guards the subscription wire codecs (Docs/Services/Subscriptions.md): requester entries
/// are 28 bytes, provider entries 32 bytes, both with a trailing Deadzone (Number).
void main() {
  test('requester entry is 28 bytes with deadzone', () {
    final sub = RequesterSubscription(
      index: 0,
      providerAddr: 2,
      trid: 0xFA00,
      targetReg: makeBlockInfo(dynamicTypeForIndex(0), dynamicInstanceForIndex(0), 0, 0),
      sourceReg: makeBlockInfo(8, 0, 3, 0),
      trigger: TriggerType.deltaPeriodic,
      periodMs: 500,
      minTimeMs: 100,
      deadzone: 1.5,
    );
    final payload = sub.toCreatePayload();
    expect(payload.length, 28);
    // providerAddr(2) trid(2) table(16: sourceReg+trigger+minTime+period+deadzone)
    // targetReg(4) timeout(4)
    final decoded = RequesterSubscription.fromBytes(0, payload);
    expect(decoded.providerAddr, 2);
    expect(decoded.trigger, TriggerType.deltaPeriodic);
    expect(decoded.periodMs, 500);
    expect(decoded.minTimeMs, 100);
    expect(decoded.deadzone, closeTo(1.5, 1e-6));
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
