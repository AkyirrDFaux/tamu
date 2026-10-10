/// The run log every HIL run writes (Docs/HIL Tests.md, "Logs").
///
/// One file per run under `test/hil/logs/`, a folder kept out of version
/// control. It carries the rig inventory, each test's result and the reason
/// behind every skip.
library;

import 'dart:io';

class HilLog {
  final StringBuffer _buf = StringBuffer();
  final DateTime started = DateTime.now();

  /// Absolute path of the file this run writes, resolved at construction.
  final String path;

  HilLog()
      : path = _resolvePath(DateTime.now());

  static String _resolvePath(DateTime now) {
    final dir = Directory('test/hil/logs');
    if (!dir.existsSync()) dir.createSync(recursive: true);
    final stamp = now.toIso8601String().replaceAll(':', '-').split('.').first;
    return '${dir.path}/hil_$stamp.log';
  }

  void line(String text) => _buf.writeln(text);

  void section(String title) {
    _buf.writeln();
    _buf.writeln('== $title');
  }

  /// Writes the buffered log to disk. Safe to call once, from `tearDownAll`.
  void write() {
    try {
      File(path).writeAsStringSync(_buf.toString());
      stdout.writeln('[hil] log written to $path');
    } catch (e) {
      stdout.writeln('[hil] could not write log to $path: $e');
    }
  }
}
