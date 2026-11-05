/// The app's request burst is bounded: a batch wider than the core's inbound queue is a LOST
/// request, not a slow one.
///
/// Measured on this rig over BLE on 2026-11-05 (independent probe, requests sent at once):
/// 12 and 16 concurrent register reads were all answered, 20 lost one. The app's start-up sweep
/// fired 20 (four nodes x five System reads) and lost DAS pings outright, so a DAS that was
/// answering subscriptions and buttons read as unreachable.
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/request_gate.dart';

void main() {
  test('no more than `limit` requests are in flight at once', () async {
    final gate = RequestGate(8);
    var inFlight = 0;
    var peak = 0;
    var ran = 0;

    Future<void> work() async {
      await gate.acquire();
      inFlight++;
      ran++;
      if (inFlight > peak) peak = inFlight;
      await Future<void>.delayed(const Duration(milliseconds: 5));
      inFlight--;
      gate.release();
    }

    await Future.wait([for (var i = 0; i < 20; i++) work()]);

    expect(peak, 8, reason: 'the gate holds the burst at its limit');
    expect(ran, 20, reason: 'every request still runs');
    expect(gate.inFlight, 0, reason: 'every slot is given back');
    expect(gate.waiting, 0);
  });

  test('a release hands its slot to the oldest waiter, in order', () async {
    final gate = RequestGate(1);
    await gate.acquire();

    final order = <int>[];
    final waiters = [
      for (var i = 0; i < 3; i++) gate.acquire().then((_) => order.add(i)),
    ];
    expect(gate.inFlight, 1);
    expect(gate.waiting, 3);

    // Each release passes the slot on; the acquirer then releases it in turn.
    for (var i = 0; i < 3; i++) {
      gate.release();
      await waiters[i];
      expect(gate.inFlight, 1, reason: 'the slot was handed over, not dropped');
    }
    gate.release();

    expect(order, [0, 1, 2]);
    expect(gate.inFlight, 0);
    expect(gate.waiting, 0);
  });

  test('a request that finishes at the limit frees the slot for the next one', () async {
    final gate = RequestGate(2);
    await gate.acquire();
    await gate.acquire();

    var third = false;
    final queued = gate.acquire().then((_) => third = true);
    await Future<void>.delayed(Duration.zero);
    expect(third, isFalse, reason: 'the third waits while two are in flight');

    gate.release();
    await queued;
    expect(third, isTrue);
    expect(gate.inFlight, 2);
    gate.release();
    gate.release();
    expect(gate.inFlight, 0);
  });
}
