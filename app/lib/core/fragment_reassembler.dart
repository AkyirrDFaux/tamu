/// Reply reassembly for the App Interface (Docs/Services/App Interface.md "fragmentation";
/// Docs/RSBus and Packets.md "FRAG").
///
/// Pure Dart and transport-independent: it is the state machine every reply path shares, so a
/// fragmented stream is reassembled identically by the app's connection and by a plain
/// `dart run` tool's own channel.
library;

import 'protocol.dart';

/// One received fragment: the index the device declared and the body bytes after the 4-byte
/// fragmentation info.
///
/// The index is kept because a lost fragment is a HOLE in a byte stream, not a shorter stream:
/// a reader that knows its fragment stride can place each body at `index * stride`, so a reply
/// that lost one fragment still yields every other byte at the right offset. The storage client
/// depends on exactly that - the core's file table is 16 kB, i.e. 256 fragments over BLE.
class FragmentChunk {
  final int index;
  final List<int> body;

  const FragmentChunk(this.index, this.body);
}

/// The result of feeding one response frame into a [FragmentReassembler].
sealed class FragmentEvent {
  const FragmentEvent();
}

/// More fragments are still expected before the reply completes.
class FragmentPending extends FragmentEvent {
  const FragmentPending();
}

/// The reply completed: [payload] is the reassembled service data (its per-fragment
/// info stripped), carrying the packet-level SUCCESS/FAIL flags of the STOP frame.
class FragmentComplete extends FragmentEvent {
  final List<int> payload;
  final bool success;
  final bool fail;

  /// The bodies received, in arrival order, with their declared fragment indexes. Empty for a
  /// single-packet reply, which carries no fragmentation at all.
  final List<FragmentChunk> chunks;

  /// Fragment indexes the declared total announced and that never arrived, ascending. Only a
  /// [FragmentReassembler.begin] call with `tolerant: true` (or [FragmentReassembler.finish])
  /// can produce a non-empty list - a strict reply aborts instead.
  final List<int> missing;

  /// True when the reply is known to be incomplete: a fragment never arrived, or the stream
  /// ended without its STOP frame. A caller that must not accept half a reply checks this.
  bool get partial => missing.isNotEmpty;

  const FragmentComplete({
    required this.payload,
    required this.success,
    required this.fail,
    this.chunks = const [],
    this.missing = const [],
  });
}

/// A reply whose fragment sequence is inconsistent (a lost, duplicated,
/// out-of-order or short fragment) - dropped rather than allowed to silently
/// truncate or corrupt the reassembled stream. [reason] is logged by the caller.
class FragmentAbort extends FragmentEvent {
  final String reason;

  const FragmentAbort(this.reason);
}

/// Reassembles a fragmented service reply into one payload.
///
/// A reply streams as FRAG-flagged frames. Each FRAG payload begins with four bytes
/// of fragmentation info - a little-endian `u16 current` fragment index and a `u16
/// total` fragment count - which is stripped as the fragments accumulate. The frame
/// with STOP set ends the reply. A non-FRAG frame is the payload verbatim (a single
/// frame carries START|STOP). Fragment index/count consistency is checked, so a stream
/// that starts at a non-zero index, jumps, or changes its declared total is aborted.
///
/// A **tolerant** stream (opt-in per request) keeps the fragments it got instead of aborting on
/// a gap: a lost fragment is recorded in [FragmentComplete.missing], and [FragmentComplete.chunks]
/// keeps the received bodies with their declared indexes. That is what makes a long read survive
/// over BLE - the core answers a 16 kB file from one handler into an 8 kB transmit ring and drops
/// whole frames when the link back-pressures, so the tail is routinely missing. It stays opt-in
/// so a short reply cannot be half-accepted by accident.
///
/// This is the pure state machine behind the reply path, split out so the framing can be
/// exercised without a live transport.
class FragmentReassembler {
  final Map<int, List<int>> _buffers = {};
  final Map<int, List<FragmentChunk>> _chunks = {};
  final Set<int> _tolerant = {};
  // Declared total for each in-flight reply, plus the next index a STRICT stream expects.
  final Map<int, ({int next, int total})> _frag = {};

  /// Opens (or restarts) a reply buffer for transaction [txId]; called when a request
  /// goes out, so an empty reply is distinguishable from no reply.
  ///
  /// [tolerant] keeps a reply with a lost fragment usable (see the class comment).
  void begin(int txId, {bool tolerant = false}) {
    _buffers[txId] = <int>[];
    _chunks.remove(txId);
    _frag.remove(txId);
    if (tolerant) {
      _tolerant.add(txId);
    } else {
      _tolerant.remove(txId);
    }
  }

  /// Forgets any reassembly state for [txId] (reply completed, timed out or cancelled).
  void discard(int txId) {
    _buffers.remove(txId);
    _chunks.remove(txId);
    _frag.remove(txId);
    _tolerant.remove(txId);
  }

  /// Drops every in-flight reply (the link went down).
  void clear() {
    _buffers.clear();
    _chunks.clear();
    _frag.clear();
    _tolerant.clear();
  }

  /// True while [txId]'s reply has at least one fragment and has not completed: the state a
  /// stream is in between its first fragment and its STOP frame.
  bool inProgress(int txId) => _frag.containsKey(txId);

  /// Ends a reply that stopped arriving without its STOP frame, returning what was reassembled.
  ///
  /// This is how a truncated stream becomes a usable partial reply instead of burning the whole
  /// request timeout: the core emits a long reply from one handler, so the tail (STOP included)
  /// can be lost to its transmit ring. Returns null when nothing was buffered - "no reply at
  /// all" must stay a timeout.
  ///
  /// The completed reply reports `success: true` (no FAIL flag was ever seen) with [missing]
  /// listing the fragments that never arrived, so [FragmentComplete.partial] is true and no
  /// caller can mistake it for a whole reply.
  FragmentComplete? finish(int txId) {
    final state = _frag[txId];
    if (_buffers[txId] == null || state == null) {
      discard(txId);
      return null;
    }
    return _complete(txId, success: true, fail: false);
  }

  /// Feeds one response [frame] for transaction [txId].
  FragmentEvent feed(int txId, PacketFrame frame) {
    final buffer = _buffers.putIfAbsent(txId, () => <int>[]);
    final tolerant = _tolerant.contains(txId);
    if (frame.isFrag) {
      if (frame.payload.length < 4) {
        discard(txId);
        return const FragmentAbort('FRAG packet without fragment info');
      }
      final info = fragInfoOf(frame.payload);
      final state = _frag[txId];
      if (info.total > 0 && info.current >= info.total) {
        discard(txId);
        return FragmentAbort(
          'fragment ${info.current}/${info.total} is out of range',
        );
      }
      if (state == null) {
        if (info.current != 0) {
          discard(txId);
          return FragmentAbort('stream starts at fragment ${info.current}');
        }
      } else if (info.total != state.total) {
        discard(txId);
        return FragmentAbort(
          'fragment ${info.current}/${info.total} changes the declared total '
          '(${state.total})',
        );
      } else if (!tolerant && info.current != state.next) {
        discard(txId);
        return FragmentAbort(
          'fragment ${info.current}/${info.total} out of order '
          '(expected ${state.next}/${state.total})',
        );
      }
      final chunks = _chunks.putIfAbsent(txId, () => <FragmentChunk>[]);
      if (tolerant && chunks.any((c) => c.index == info.current)) {
        // A duplicate: its body is already placed. The device never repeats a fragment, so
        // this is a link-layer re-transmission, not new data.
        return frame.isStop ? _complete(txId) : const FragmentPending();
      }
      chunks.add(FragmentChunk(info.current, frame.payload.sublist(4)));
      buffer.addAll(frame.payload.sublist(4));
      _frag[txId] = (next: info.current + 1, total: info.total);
    } else {
      buffer.addAll(frame.payload);
    }
    if (!frame.isStop) return const FragmentPending();
    return _complete(txId, success: frame.isSuccess, fail: frame.isFail);
  }

  FragmentComplete _complete(
    int txId, {
    bool success = true,
    bool fail = false,
  }) {
    final buffer = _buffers[txId] ?? <int>[];
    final chunks = _chunks[txId] ?? const <FragmentChunk>[];
    final total = _frag[txId]?.total ?? 0;
    final seen = {for (final c in chunks) c.index};
    final missing = [
      if (chunks.isNotEmpty)
        for (var i = 0; i < total; i++)
          if (!seen.contains(i)) i,
    ];
    discard(txId);
    return FragmentComplete(
      payload: buffer,
      success: success,
      fail: fail,
      chunks: List<FragmentChunk>.unmodifiable(chunks),
      missing: List<int>.unmodifiable(missing),
    );
  }
}
