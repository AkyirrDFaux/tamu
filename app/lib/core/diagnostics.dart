/// Lightweight diagnostic event ring shared by transports, the transaction
/// layer and the service clients.
///
/// Everything that would otherwise vanish into `debugPrint` (timeouts, parse
/// errors, unexpected replies, link state changes) lands here, so failures are
/// reportable after the fact - from tests, the UI log page or a bug report.
library;

/// One recorded diagnostics event.
class DiagEvent {
  final DateTime time;
  final String source;
  final String message;

  const DiagEvent(this.time, this.source, this.message);

  @override
  String toString() =>
      '${time.toIso8601String().substring(11, 23)} [$source] $message';
}

/// Append-only ring of recent events (oldest dropped).
class AppDiagnostics {
  static const int capacity = 200;

  // Fixed-size ring: `_start` is the oldest slot, `_count` the number of live
  // events (<= capacity). Appends are O(1); no list shifting.
  static final List<DiagEvent?> _events = List<DiagEvent?>.filled(capacity, null);
  static int _start = 0;
  static int _count = 0;

  /// Recent events, oldest first.
  static List<DiagEvent> get events => List.unmodifiable([
        for (var i = 0; i < _count; i++) _events[(_start + i) % capacity]!,
      ]);

  static void log(String source, String message) {
    final event = DiagEvent(DateTime.now(), source, message);
    if (_count < capacity) {
      _events[(_start + _count) % capacity] = event;
      _count++;
    } else {
      _events[_start] = event;
      _start = (_start + 1) % capacity;
    }
  }

  /// Dumps the ring as a multi-line string (bug reports, test failure output).
  static String dump() => events.map((e) => e.toString()).join('\n');
}
