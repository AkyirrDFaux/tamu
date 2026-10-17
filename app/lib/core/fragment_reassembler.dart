/// Reply reassembly for the App Interface (Docs/Services/App Interface.md "fragmentation";
/// Docs/RSBus and Packets.md "FRAG").
///
/// Pure Dart and transport-independent: it is the state machine every reply path shares, so a
/// fragmented stream is reassembled identically by the app's connection and by a plain
/// `dart run` tool's own channel.
library;

import 'protocol.dart';

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

  const FragmentComplete({
    required this.payload,
    required this.success,
    required this.fail,
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
/// This is the pure state machine behind the reply path, split out so the framing can be
/// exercised without a live transport.
class FragmentReassembler {
  final Map<int, List<int>> _buffers = {};
  // Expected next fragment index + declared total for each in-flight reply.
  final Map<int, ({int next, int total})> _frag = {};

  /// Opens (or restarts) a reply buffer for transaction [txId]; called when a request
  /// goes out, so an empty reply is distinguishable from no reply.
  void begin(int txId) {
    _buffers[txId] = <int>[];
    _frag.remove(txId);
  }

  /// Forgets any reassembly state for [txId] (reply completed, timed out or cancelled).
  void discard(int txId) {
    _buffers.remove(txId);
    _frag.remove(txId);
  }

  /// Drops every in-flight reply (the link went down).
  void clear() {
    _buffers.clear();
    _frag.clear();
  }

  /// Feeds one response [frame] for transaction [txId].
  FragmentEvent feed(int txId, PacketFrame frame) {
    final buffer = _buffers.putIfAbsent(txId, () => <int>[]);
    if (frame.isFrag) {
      if (frame.payload.length < 4) {
        discard(txId);
        return const FragmentAbort('FRAG packet without fragment info');
      }
      final info = fragInfoOf(frame.payload);
      final state = _frag[txId];
      if (state == null) {
        if (info.current != 0) {
          discard(txId);
          return FragmentAbort('stream starts at fragment ${info.current}');
        }
      } else if (info.current != state.next || info.total != state.total) {
        discard(txId);
        return FragmentAbort(
          'fragment ${info.current}/${info.total} out of order '
          '(expected ${state.next}/${state.total})',
        );
      }
      _frag[txId] = (next: info.current + 1, total: info.total);
      buffer.addAll(frame.payload.sublist(4));
    } else {
      buffer.addAll(frame.payload);
    }
    if (!frame.isStop) return const FragmentPending();
    discard(txId);
    return FragmentComplete(
      payload: buffer,
      success: frame.isSuccess,
      fail: frame.isFail,
    );
  }
}
