/// Mounts the memory-backup viewer the way a page hands it a file.
///
/// `file_viewers_test` and `membackup_view_test` are two suites over the same widget (the `.SV`,
/// SUBREQ and `.DT_` decoders) and each carried its own copy of this mount - the theme, the
/// scaffold and the `takeException` check that makes a throwing decoder a test failure rather than
/// a green run with an empty screen.
library;

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/ui/file_viewers.dart';
import 'package:tamuapp/ui/theme.dart';

/// Renders [data] as [name] in the viewer and fails if it threw while rendering.
Future<void> pumpMemoryBackup(
  WidgetTester tester,
  String name,
  List<int> data,
) async {
  await tester.pumpWidget(
    MaterialApp(
      theme: buildTheme(),
      home: Scaffold(
        body: MemoryBackupView(fileName: name, data: data),
      ),
    ),
  );
  await tester.pump();
  expect(tester.takeException(), isNull, reason: '$name threw while rendering');
}
