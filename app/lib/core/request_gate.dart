/// A bound on how many requests one link keeps in flight at once.
///
/// Why it exists: the core parses every app frame into a fixed inbound queue and drops what
/// does not fit (`APP_RX_QUEUE_DEPTH`, firmware/src/Core/Functions/AppInterface.h). A burst
/// larger than that queue is not "slower", it is a LOST request: the caller waits out its
/// timeout and reports the device as unresponsive.
///
/// Measured on this rig over BLE (2026-11-05, each burst sent at once, 3 s for the replies):
/// 12 and 16 concurrent register reads were all answered, 20 lost one. The app's discovery
/// sweep fires 20 (four nodes x five System reads) and lost pings outright, so a perfectly
/// healthy DAS read as unreachable.
///
/// Pure Dart and free-standing so the bound can be tested without a link or a clock.
library;

import 'dart:async';
import 'dart:collection';

class RequestGate {
  RequestGate(this.limit) : assert(limit > 0);

  /// How many requests may be in flight at once.
  final int limit;

  int _inFlight = 0;
  final Queue<Completer<void>> _waiters = Queue<Completer<void>>();

  /// Requests currently holding a slot.
  int get inFlight => _inFlight;

  /// Requests queued behind the bound.
  int get waiting => _waiters.length;

  /// Takes a slot, waiting for one when [limit] are in use.
  ///
  /// The future completes when the slot is OWNED by the caller - a release hands its slot
  /// straight over to the oldest waiter, so a request never has to re-check and can never be
  /// starved by a newcomer.
  Future<void> acquire() {
    if (_inFlight < limit) {
      _inFlight++;
      return Future<void>.value();
    }
    final waiter = Completer<void>();
    _waiters.add(waiter);
    return waiter.future;
  }

  /// Gives a slot back (or passes it to the next waiter).
  void release() {
    if (_waiters.isNotEmpty) {
      _waiters.removeFirst().complete();
      return;
    }
    if (_inFlight > 0) _inFlight--;
  }
}
