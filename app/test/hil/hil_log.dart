/// The run log every HIL run writes (Docs/HIL Tests.md, "Logs").
///
/// One file per run under `test/hil/logs/`, a folder kept out of version
/// control. It carries the rig inventory, each test's result and the reason
/// behind every skip.
///
/// Only the [keepRuns] newest are kept: bench work produces dozens of runs a
/// day, and nothing else prunes the folder, so it grew without bound.
library;

import 'dart:io';

class HilLog {
  /// Where runs are recorded, relative to the runner's working directory.
  static const String logDir = 'test/hil/logs';

  /// How many run logs to keep. Recent runs are what a bench session reviews.
  static const int keepRuns = 20;

  final StringBuffer _buf = StringBuffer();
  final DateTime started = DateTime.now();

  /// Absolute path of the file this run writes, resolved at construction.
  final String path;

  HilLog() : path = _resolvePath(DateTime.now());

  static String _resolvePath(DateTime now) {
    final dir = Directory(logDir);
    if (!dir.existsSync()) dir.createSync(recursive: true);
    final stamp = now.toIso8601String().replaceAll(':', '-').split('.').first;
    return '${dir.path}/hil_$stamp.log';
  }

  void line(String text) => _buf.writeln(text);

  void section(String title) {
    _buf.writeln();
    _buf.writeln('== $title');
  }

  /// Writes the buffered log to disk, then prunes older runs. Safe to call
  /// once, from `tearDownAll`.
  void write() {
    try {
      File(path).writeAsStringSync(_buf.toString());
      stdout.writeln('[hil] log written to $path');
      _pruneOldRuns();
    } catch (e) {
      stdout.writeln('[hil] could not write log to $path: $e');
    }
  }

  /// Deletes every run log but the [keepRuns] newest.
  ///
  /// The name carries an ISO timestamp, so sorting names sorts by run time. Only
  /// files this harness writes are considered - a `.log` whose name starts with
  /// `hil_` - so anything else in the folder is left alone. A failure here is
  /// reported rather than swallowed: a folder that quietly stops being pruned is
  /// the bug this fixes.
  void _pruneOldRuns() {
    try {
      final runs =
          Directory(
              logDir,
            ).listSync().whereType<File>().where(_isRunLog).toList()
            ..sort((a, b) => a.path.compareTo(b.path));
      final excess = runs.length - keepRuns;
      if (excess <= 0) return;
      for (final old in runs.take(excess)) {
        old.deleteSync();
      }
      stdout.writeln('[hil] pruned $excess older run log(s)');
    } catch (e) {
      stdout.writeln('[hil] could not prune old run logs: $e');
    }
  }

  /// Whether [file] is one of this harness's run logs.
  static bool _isRunLog(File file) =>
      file.path.endsWith('.log') &&
      file.uri.pathSegments.last.startsWith('hil_');
}
