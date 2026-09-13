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

  static final List<DiagEvent> _events = [];

  /// Recent events, oldest first.
  static List<DiagEvent> get events => List.unmodifiable(_events);

  static void log(String source, String message) {
    final event = DiagEvent(DateTime.now(), source, message);
    _events.add(event);
    if (_events.length > capacity) _events.removeAt(0);
  }

  /// Dumps the ring as a multi-line string (bug reports, test failure output).
  static String dump() => _events.map((e) => e.toString()).join('\n');
}
