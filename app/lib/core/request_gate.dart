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
/// A slot is a [RequestSlot], not a bare count: the caller gives back the slot it took, and a
/// slot taken before a link went away can never be handed back into the next session's budget
/// (see [RequestGate.releaseAll]). Holding the count instead let one platform write that never
/// returned park a slot forever, and the gate had no way to take it back.
///
/// Pure Dart and free-standing so the bound, the queue and the reset can be tested without a
/// link or a clock.
library;

import 'dart:async';
import 'dart:collection';

/// Thrown by [RequestGate.acquire] when its queue bound expires: the link is held by earlier
/// requests and this one never got a slot.
///
/// Its own type, so a caller can tell "the link is busy" from "the device did not answer" - the
/// two mean different things on screen, and the first one is not a device fault at all.
class RequestQueueTimeout implements Exception {
  const RequestQueueTimeout({
    required this.waited,
    required this.inFlight,
    required this.limit,
  });

  /// How long the request waited for a slot.
  final Duration waited;

  /// Slots still held when the wait expired.
  final int inFlight;

  /// The gate's limit.
  final int limit;

  @override
  String toString() =>
      'waited ${waited.inMilliseconds} ms for a request slot and none came free '
      '($inFlight of $limit were still held by earlier requests)';
}

/// The right to have one request in flight, as handed out by [RequestGate.acquire].
///
/// [release] is idempotent: a request releases its slot in a `finally`, and the link's own
/// teardown may already have taken every slot back. A slot from a session that has since ended
/// (the gate was reset) owns nothing and does nothing.
class RequestSlot {
  RequestSlot._(this._gate, this._generation);

  final RequestGate _gate;
  final int _generation;
  bool _released = false;

  /// True once [release] has run (or the link took the slot back).
  bool get released => _released;

  /// Gives the slot back - to the oldest waiter when one is queued, to the budget otherwise.
  void release() {
    if (_released) return;
    _released = true;
    _gate._release(this);
  }
}

class RequestGate {
  RequestGate(this.limit) : assert(limit > 0);

  /// How many requests may be in flight at once.
  final int limit;

  int _inFlight = 0;
  int _generation = 0;
  final Queue<Completer<RequestSlot>> _waiters =
      Queue<Completer<RequestSlot>>();

  /// Requests currently holding a slot.
  int get inFlight => _inFlight;

  /// Requests queued behind the bound.
  int get waiting => _waiters.length;

  /// Bumped by [releaseAll]. A slot from an older generation is stale.
  int get generation => _generation;

  /// Takes a slot, waiting for one when [limit] are in use.
  ///
  /// The future completes when the slot is OWNED by the caller - a release hands its slot
  /// straight over to the oldest waiter, so a request never has to re-check and can never be
  /// starved by a newcomer.
  ///
  /// [wait] bounds that queueing: when it expires the request is failed with
  /// [RequestQueueTimeout] instead of waiting for a slot that may never come. A caller that
  /// omits it can wait indefinitely, so every request path in the app passes one.
  Future<RequestSlot> acquire({Duration? wait}) {
    if (_inFlight < limit) {
      _inFlight++;
      return Future<RequestSlot>.value(RequestSlot._(this, _generation));
    }
    final waiter = Completer<RequestSlot>();
    _waiters.add(waiter);
    if (wait == null) return waiter.future;
    return waiter.future.timeout(
      wait,
      onTimeout: () {
        _waiters.remove(waiter);
        throw RequestQueueTimeout(
          waited: wait,
          inFlight: _inFlight,
          limit: limit,
        );
      },
    );
  }

  /// Gives every slot back and fails everyone waiting.
  ///
  /// This is the link's teardown: the requests that held slots are answered with their own
  /// error by the connection, and the slots they still think they hold belong to a session that
  /// no longer exists. Without this a request parked before its own timeout - a platform write
  /// that never returns - would keep its slot for the rest of the app's life, and eight of them
  /// would wedge every later request in `acquire()` with no error to report.
  void releaseAll(Object error) {
    _generation++;
    _inFlight = 0;
    final waiters = _waiters.toList();
    _waiters.clear();
    for (final waiter in waiters) {
      if (!waiter.isCompleted) waiter.completeError(error);
    }
  }

  void _release(RequestSlot slot) {
    // A slot taken before the link was torn down has nothing to give back: the budget it was
    // taken from is gone, and counting its release against the new session would let more than
    // [limit] requests run at once.
    if (slot._generation != _generation) return;
    while (_waiters.isNotEmpty) {
      final waiter = _waiters.removeFirst();
      if (waiter.isCompleted) continue; // its queue bound already expired
      // The slot moves to the waiter; the count does not change.
      waiter.complete(RequestSlot._(this, _generation));
      return;
    }
    if (_inFlight > 0) _inFlight--;
  }
}
