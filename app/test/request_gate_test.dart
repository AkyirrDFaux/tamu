/// The app's request burst is bounded: a batch wider than the core's inbound queue is a LOST
/// request, not a slow one.
///
/// Measured on this rig over BLE on 2026-11-05 (independent probe, requests sent at once):
/// 12 and 16 concurrent register reads were all answered, 20 lost one. The app's start-up sweep
/// fired 20 (four nodes x five System reads) and lost DAS pings outright, so a DAS that was
/// answering subscriptions and buttons read as unreachable.
///
/// The second half of the file is the other bound: a slot that is never given back must not
/// wedge the link, and a request that waits for one must not wait forever.
library;

import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/request_gate.dart';

void main() {
  test('no more than `limit` requests are in flight at once', () async {
    final gate = RequestGate(8);
    var inFlight = 0;
    var peak = 0;
    var ran = 0;

    Future<void> work() async {
      final slot = await gate.acquire();
      inFlight++;
      ran++;
      if (inFlight > peak) peak = inFlight;
      await Future<void>.delayed(const Duration(milliseconds: 5));
      inFlight--;
      slot.release();
    }

    await Future.wait([for (var i = 0; i < 20; i++) work()]);

    expect(peak, 8, reason: 'the gate holds the burst at its limit');
    expect(ran, 20, reason: 'every request still runs');
    expect(gate.inFlight, 0, reason: 'every slot is given back');
    expect(gate.waiting, 0);
  });

  test('a release hands its slot to the oldest waiter, in order', () async {
    final gate = RequestGate(1);
    final first = await gate.acquire();

    final order = <int>[];
    final held = <RequestSlot>[];
    for (var i = 0; i < 3; i++) {
      unawaited(
        gate.acquire().then((slot) {
          held.add(slot);
          order.add(i);
        }),
      );
    }
    expect(gate.inFlight, 1);
    expect(gate.waiting, 3);
    await Future<void>.delayed(Duration.zero);

    // Each release passes the slot on; the acquirer then releases it in turn.
    for (var i = 0; i < 3; i++) {
      (i == 0 ? first : held[i - 1]).release();
      await Future<void>.delayed(Duration.zero);
      expect(gate.inFlight, 1, reason: 'the slot was handed over, not dropped');
    }
    held.last.release();

    expect(order, [0, 1, 2]);
    expect(gate.inFlight, 0);
    expect(gate.waiting, 0);
  });

  test(
    'a request that finishes at the limit frees the slot for the next one',
    () async {
      final gate = RequestGate(2);
      final a = await gate.acquire();
      final b = await gate.acquire();

      var third = false;
      final queued = gate.acquire().then((slot) {
        third = true;
        return slot;
      });
      await Future<void>.delayed(Duration.zero);
      expect(third, isFalse, reason: 'the third waits while two are in flight');

      a.release();
      final c = await queued;
      expect(third, isTrue);
      expect(gate.inFlight, 2);
      b.release();
      c.release();
      expect(gate.inFlight, 0);
    },
  );

  group('a stalled request cannot wedge the link', () {
    test('releaseAll takes every slot back and fails the waiters', () async {
      final gate = RequestGate(1);
      final held = await gate.acquire();
      final waiting = gate.acquire(wait: const Duration(seconds: 2));

      gate.releaseAll(StateError('Disconnected'));

      await expectLater(waiting, throwsStateError);
      expect(gate.inFlight, 0, reason: 'the parked slot is gone with the link');
      expect(gate.waiting, 0, reason: 'nobody waits for a link that is gone');

      // The stale slot's late release must not eat the next request's slot.
      final next = await gate.acquire();
      expect(gate.inFlight, 1);
      held.release();
      expect(
        gate.inFlight,
        1,
        reason: 'a slot from the old session owns nothing in the new one',
      );
      next.release();
      expect(gate.inFlight, 0);
    });

    test('a slot released twice gives its budget back once', () async {
      final gate = RequestGate(1);
      final slot = await gate.acquire();
      slot.release();
      slot.release();
      expect(gate.inFlight, 0);
    });

    test(
      'a queued request fails with its own error, not a silent wait',
      () async {
        final gate = RequestGate(1);
        final held = await gate.acquire();

        await expectLater(
          gate.acquire(wait: const Duration(milliseconds: 20)),
          throwsA(isA<RequestQueueTimeout>()),
        );
        expect(gate.waiting, 0, reason: 'the expired waiter leaves the queue');
        expect(gate.inFlight, 1);
        held.release();
        expect(gate.inFlight, 0);
        expect(
          const RequestQueueTimeout(
            waited: Duration(seconds: 5),
            inFlight: 8,
            limit: 8,
          ).toString(),
          'waited 5000 ms for a request slot and none came free '
          '(8 of 8 were still held by earlier requests)',
        );
      },
    );
  });
}
