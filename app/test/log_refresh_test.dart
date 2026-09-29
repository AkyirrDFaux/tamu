import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/ui/log_page.dart';

void main() {
  testWidgets('a quiet log view polls progressively less often, and a change resets it',
      (tester) async {
    var fetches = 0;
    var payload = <int>[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]; // one 12-byte LogRecord

    await tester.pumpWidget(MaterialApp(
      home: LogViewerPage(deviceId: 1, logFetcher: (id) async {
        fetches++;
        return List<int>.from(payload);
      }),
    ));
    await tester.pumpAndSettle();
    expect(fetches, 1, reason: 'the page fetches once on open');

    final state = tester.state<State>(find.byType(LogViewerPage));
    // Drive the auto-refresh hook directly: the real timer lives in AutoRefreshButton, so
    // pumping it would just be a slower way to call this.
    for (var i = 0; i < 20; i++) {
      await (state as dynamic).onAutoRefresh();
    }
    final quietFetches = fetches;
    // 21 ticks: the first fetches, then the skip grows 1, 2, 3 ..., so a handful of polls
    // instead of one per tick - but never zero, the view must stay live.
    expect(quietFetches, lessThanOrEqualTo(8),
        reason: 'an unchanged reply must back the poll off (was $quietFetches fetches)');
    expect(quietFetches, greaterThanOrEqualTo(4),
        reason: 'it must still poll, just rarely (was $quietFetches fetches)');

    // A device that logs again is picked up within one skip period - that delay is the point of
    // the backoff - and from then on the cadence is live again.
    payload = <int>[...payload, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24];
    for (var i = 0; i < 10; i++) {
      await (state as dynamic).onAutoRefresh();
    }
    final afterChange = fetches;
    expect(afterChange, greaterThan(quietFetches),
        reason: 'a change must be picked up within one skip period');
  });
}
