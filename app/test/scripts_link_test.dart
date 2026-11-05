/// The Scripts page's first load over a link that is not up yet.
///
/// A Bluetooth session is what makes this reachable: a BLE connect takes seconds and a BLE link
/// drops, so the page is opened with no link far more often than over USB. Its first load used to
/// send the slot map anyway, read the request layer's "Not connected" as the device staying
/// silent, and sit on "The loaded scripts could not be read" - an error about a device it never
/// reached (Akyirr, 2026-11-05: "the script page error was about initial load failing").
library;

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/fragment_reassembler.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/transport.dart';
import 'package:tamuapp/ui/scripts_page.dart';
import 'package:tamuapp/ui/theme.dart';

/// The link's state as the page sees it, plus the notification that carries a change.
class _Link extends ChangeNotifier {
  bool up;
  _Link(this.up);

  void set(bool value) {
    up = value;
    notifyListeners();
  }
}

/// A device that answers the Script slot map with an empty one (nothing loaded). Every request is
/// counted, so a page that sent something with no link is visible.
class _Device implements TolerantRequestChannel {
  _Device({required this.link});

  final _Link link;
  int requests = 0;

  /// Runs inside the slot map answer, before it returns: how a link that goes away mid-read is
  /// driven.
  void Function()? onSlotMap;

  @override
  bool get isConnected => true;

  @override
  int takeTxId() => 0xF700;

  @override
  void setSubscriptionListener(Function(List<int>)? listener) {}

  @override
  Future<List<int>> request(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
    bool requestFrag = false,
    int? transactionId,
  }) async => (await requestTolerant(
    targetId,
    service,
    functionCid,
    payload: payload,
    timeout: timeout,
  )).payload;

  @override
  Future<PacketResponse> requestWithFlags(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
    bool requestFrag = false,
    int? transactionId,
  }) => requestTolerant(
    targetId,
    service,
    functionCid,
    payload: payload,
    timeout: timeout,
  );

  @override
  Future<PacketResponse> requestTolerant(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
  }) async {
    requests++;
    if (service == ServiceType.script && functionCid == 0) {
      onSlotMap?.call();
      if (!link.up) throw const TransportException('Not connected');
      // An empty slot map: every slot free, which is "nothing is loaded".
      final body = List<int>.filled(32, 0xFF);
      return PacketResponse(
        payload: body,
        success: true,
        fail: false,
        chunks: [FragmentChunk(0, body)],
        stopped: true,
        total: 1,
      );
    }
    throw const TransportException('Not connected');
  }
}

Widget _app(Widget home) => MaterialApp(theme: buildTheme(), home: home);

void main() {
  late _Link link;
  late _Device device;

  setUp(() {
    link = _Link(false);
    device = _Device(link: link);
    installRequestChannel(device);
    addTearDown(() => installRequestChannel(null));
  });

  testWidgets('a page opened with no link waits, and loads when the link '
      'comes up', (tester) async {
    await tester.pumpWidget(
      _app(
        ScriptsPage(
          deviceId: 1,
          deviceName: 'Core',
          isConnected: () => link.up,
          linkChanges: link,
        ),
      ),
    );
    await tester.pump();

    // Nothing was asked of a link that is not there, and the page does not call the device silent.
    expect(device.requests, 0);
    expect(find.text('No link to the device'), findsOneWidget);
    expect(find.text('The loaded scripts could not be read'), findsNothing);

    // The link comes up (a BLE connect finishing, or a dropped session returning): the page loads
    // by itself, with no tap.
    link.set(true);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 50));

    expect(device.requests, 1, reason: 'the slot map, once the link is up');
    expect(find.text('No scripts loaded'), findsOneWidget);
    expect(find.text('No link to the device'), findsNothing);
  });

  testWidgets('the waiting state has its own way out', (tester) async {
    await tester.pumpWidget(
      _app(
        ScriptsPage(
          deviceId: 1,
          deviceName: 'Core',
          isConnected: () => link.up,
          linkChanges: link,
        ),
      ),
    );
    await tester.pump();

    await tester.tap(find.text('Try again'));
    await tester.pump();

    // Still no link: the same wait, still nothing sent.
    expect(device.requests, 0);
    expect(find.text('No link to the device'), findsOneWidget);
  });

  testWidgets('a link that goes away mid-load is not "the device did not '
      'answer"', (tester) async {
    link.up = true;
    device.onSlotMap = () => link.set(false);

    await tester.pumpWidget(
      _app(
        ScriptsPage(
          deviceId: 1,
          deviceName: 'Core',
          isConnected: () => link.up,
          linkChanges: link,
        ),
      ),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 50));

    expect(find.text('The loaded scripts could not be read'), findsNothing);
    expect(find.text('No link to the device'), findsOneWidget);
  });

  testWidgets('a device that answers nothing while the link is up is still an '
      'error with a retry', (tester) async {
    link.up = true;
    // The link is up the whole time; the device just does not answer the slot map.
    device.onSlotMap = () => throw const TransportException('no answer');

    await tester.pumpWidget(
      _app(
        ScriptsPage(
          deviceId: 1,
          deviceName: 'Core',
          isConnected: () => link.up,
          linkChanges: link,
        ),
      ),
    );
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 50));

    expect(
      find.text('The loaded scripts could not be read'),
      findsOneWidget,
      reason: 'a silent device on a live link is a failure, not a wait',
    );
    expect(find.text('Try again'), findsOneWidget);
  });
}
