/// A pending connect has two exits (Cancel and a timeout), and a refusal is not a timeout.
///
/// The panel used to leave the spinner up until the platform gave up, and its Cancel button called
/// `disconnect` - which does nothing while `_transport` is still null, because a connecting link
/// is not attached yet. So the attempt ran on behind a hidden row and only its own eventual failure
/// ended it (Akyirr, 2026-10-29: "a pending connection is uncancelable in the app").
library;

import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/connection.dart';
import 'package:tamuapp/core/transport.dart';
import 'package:tamuapp/ui/connection_page.dart';
import 'package:tamuapp/ui/theme.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  const bleLink = DiscoveredLink(
    id: 'AA:BB:CC:DD:EE:01',
    type: LinkType.ble,
    name: 'Tamu core',
  );

  group('connect attempt', () {
    test('the automatic exit waits ten seconds', () {
      expect(connectAttemptTimeout, const Duration(seconds: 10));
    });

    test('cancel abandons the attempt instead of hiding its spinner', () async {
      final manager = ConnectionManager.forTest();
      final transport = _FakeTransport();
      manager.transportFactory = (_) => transport;

      final pending = manager.connectTo(bleLink);
      await _settle();
      expect(manager.isConnecting, isTrue, reason: 'the attempt is in flight');
      expect(
        transport.connectStarted,
        isTrue,
        reason: 'the platform connect is what the panel is waiting on',
      );

      expect(await manager.cancelConnect(), isTrue);

      // The attempt is over as far as the panel is concerned, without the platform answering...
      expect(manager.isConnecting, isFalse);
      expect(manager.connectingTarget, isNull);
      expect(await pending, connectCancelledMessage);
      // ...because the link behind it was dropped, not just forgotten.
      expect(
        transport.closeCount,
        1,
        reason: 'closing the transport stops the platform work',
      );
      expect(
        manager.lastConnectNotice?.end,
        ConnectEnd.cancelled,
        reason: 'the panel is left idle with one plain line',
      );
      expect(
        manager.autoConnectHeldOffUntil,
        isNotNull,
        reason: 'the refresh timer must not restart what the user just abandoned',
      );

      // The platform may still finish its connect afterwards; the abandoned attempt must not
      // progress into a session when it does.
      transport.finishConnect();
      await _settle();
      expect(manager.isConnected, isFalse);
      expect(manager.transport, isNull);
      expect(manager.connectedId, isNull);
      expect(
        transport.closeCount,
        1,
        reason: 'the late answer neither attaches nor re-closes anything',
      );
    });

    test('cancel with nothing pending does nothing', () async {
      final manager = ConnectionManager.forTest();
      expect(await manager.cancelConnect(), isFalse);
      expect(manager.lastConnectNotice, isNull);
    });

    test('the deadline times the attempt out with a plain line', () async {
      final manager = ConnectionManager.forTest()
        ..connectTimeout = const Duration(milliseconds: 20);
      final transport = _FakeTransport();
      manager.transportFactory = (_) => transport;

      final pending = manager.connectTo(bleLink);
      expect(await pending, connectNoAnswerMessage);
      expect(manager.isConnecting, isFalse);
      expect(manager.lastConnectNotice?.end, ConnectEnd.noAnswer);
      expect(
        transport.closeCount,
        1,
        reason: 'the timeout drops the pending connect too',
      );
    });

    test('the timeout holds autoconnect off, the same as Cancel', () async {
      final manager = ConnectionManager.forTest()
        ..connectTimeout = const Duration(milliseconds: 20);
      manager.transportFactory = (_) => _FakeTransport();

      expect(await manager.connectTo(bleLink), connectNoAnswerMessage);
      expect(
        manager.autoConnectHeldOffUntil,
        isNotNull,
        reason: 'otherwise the 1 s refresh restarts the attempt the deadline just ended, '
            'so a device that is off is retried and re-reported forever',
      );
    });

    test('a refusal reads as paired phones, not as no answer', () async {
      final manager = ConnectionManager.forTest();
      final transport = _FakeTransport(
        failure: 'BLE connect failed: GATT error 133',
      );
      manager.transportFactory = (_) => transport;

      expect(await manager.connectTo(bleLink), connectPairingWindowMessage);
      expect(manager.lastConnectNotice?.end, ConnectEnd.refused);
      expect(manager.lastConnectNotice?.isBle, isTrue);
      expect(
        manager.lastConnectNotice?.detail,
        contains('GATT error 133'),
        reason: 'the platform words are kept for the Log page, not shown',
      );
      expect(transport.closeCount, 1);
    });

    test('a platform not-found is no answer, not a refusal', () async {
      final manager = ConnectionManager.forTest();
      manager.transportFactory = (_) =>
          _FakeTransport(failure: 'BLE connect failed: device not found');

      expect(await manager.connectTo(bleLink), connectNoAnswerMessage);
      expect(manager.lastConnectNotice?.end, ConnectEnd.noAnswer);
    });

    test('a USB port failure keeps its own words', () {
      final notice = classifyConnectFailure(
        link: const DiscoveredLink(
          id: '/dev/ttyACM9',
          type: LinkType.usb,
          name: 'USB ACM9',
        ),
        errorText: 'Cannot open /dev/ttyACM9: No such file or directory',
      );
      expect(notice.end, ConnectEnd.failed);
      expect(notice.message, startsWith('Connect failed: Cannot open'));
      expect(
        notice.message,
        isNot(contains('paired')),
        reason: 'a cable is not the device turning a phone away',
      );
    });
  });

  group('connection page', () {
    testWidgets('a pending connect shows Cancel, and Cancel leaves no row', (
      tester,
    ) async {
      final manager = ConnectionManager.forTest();
      final transport = _FakeTransport();
      manager.transportFactory = (_) => transport;
      // A tall window: the panel's cards are laid out, not scrolled.
      tester.view.physicalSize = const Size(900, 1600);
      tester.view.devicePixelRatio = 1.0;
      addTearDown(tester.view.resetPhysicalSize);

      await tester.pumpWidget(
        MaterialApp(
          theme: buildTheme(),
          home: ConnectionPage(manager: manager),
        ),
      );
      await tester.pump();

      unawaited(manager.connectTo(bleLink));
      await tester.pump();

      // Before: a spinner, the target, and an explicit way out.
      final row = find.widgetWithText(ListTile, 'Connecting to Tamu core');
      expect(row, findsOneWidget);
      expect(
        find.descendant(
          of: row,
          matching: find.byType(CircularProgressIndicator),
        ),
        findsOneWidget,
      );
      expect(find.widgetWithText(TextButton, 'Cancel'), findsOneWidget);

      await tester.tap(find.widgetWithText(TextButton, 'Cancel'));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 100));

      // After: no half-open row, no spinner, and one plain line.
      expect(row, findsNothing);
      expect(find.widgetWithText(TextButton, 'Cancel'), findsNothing);
      expect(find.text(connectCancelledMessage), findsOneWidget);
      expect(transport.closeCount, 1);

      await tester.pumpWidget(const SizedBox());
    });

    testWidgets('the timeout takes the pending row away on its own', (
      tester,
    ) async {
      final manager = ConnectionManager.forTest()
        ..connectTimeout = const Duration(milliseconds: 50);
      final transport = _FakeTransport();
      manager.transportFactory = (_) => transport;
      tester.view.physicalSize = const Size(900, 1600);
      tester.view.devicePixelRatio = 1.0;
      addTearDown(tester.view.resetPhysicalSize);

      await tester.pumpWidget(
        MaterialApp(
          theme: buildTheme(),
          home: ConnectionPage(manager: manager),
        ),
      );
      await tester.pump();

      unawaited(manager.connectTo(bleLink));
      await tester.pump();
      final row = find.widgetWithText(ListTile, 'Connecting to Tamu core');
      expect(row, findsOneWidget);

      await tester.pump(const Duration(milliseconds: 60));
      await tester.pump(const Duration(milliseconds: 100));

      expect(row, findsNothing);
      expect(find.text(connectNoAnswerMessage), findsOneWidget);
      expect(transport.closeCount, 1);

      await tester.pumpWidget(const SizedBox());
    });

    testWidgets('a second cancel leaves a line of its own', (tester) async {
      final manager = ConnectionManager.forTest();
      final transport = _FakeTransport();
      manager.transportFactory = (_) => transport;
      tester.view.physicalSize = const Size(900, 1600);
      tester.view.devicePixelRatio = 1.0;
      addTearDown(tester.view.resetPhysicalSize);

      await tester.pumpWidget(
        MaterialApp(
          theme: buildTheme(),
          home: ConnectionPage(manager: manager),
        ),
      );
      await tester.pump();

      // First attempt, cancelled by hand.
      unawaited(manager.connectTo(bleLink));
      await tester.pump();
      await tester.tap(find.widgetWithText(TextButton, 'Cancel'));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 100));
      expect(find.text(connectCancelledMessage), findsOneWidget);

      // That line has had its time on screen.
      tester
          .state<ScaffoldMessengerState>(find.byType(ScaffoldMessenger))
          .removeCurrentSnackBar();
      await tester.pump(const Duration(milliseconds: 400));
      expect(find.text(connectCancelledMessage), findsNothing);

      // The next attempt says the same thing, and must still be able to say it: the cancel
      // notice is a `const` expression, so Dart hands the page the very same object twice.
      unawaited(manager.connectTo(bleLink));
      await tester.pump();
      expect(find.widgetWithText(TextButton, 'Cancel'), findsOneWidget);
      await tester.tap(find.widgetWithText(TextButton, 'Cancel'));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 100));

      expect(find.text(connectCancelledMessage), findsOneWidget);
      expect(
        transport.closeCount,
        2,
        reason: 'each cancel drops the link it was waiting on',
      );

      await tester.pumpWidget(const SizedBox());
    });

    testWidgets('a second timeout leaves a line of its own', (tester) async {
      final manager = ConnectionManager.forTest()
        ..connectTimeout = const Duration(milliseconds: 50);
      final transport = _FakeTransport();
      manager.transportFactory = (_) => transport;
      tester.view.physicalSize = const Size(900, 1600);
      tester.view.devicePixelRatio = 1.0;
      addTearDown(tester.view.resetPhysicalSize);

      await tester.pumpWidget(
        MaterialApp(
          theme: buildTheme(),
          home: ConnectionPage(manager: manager),
        ),
      );
      await tester.pump();

      unawaited(manager.connectTo(bleLink));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 60));
      await tester.pump(const Duration(milliseconds: 100));
      expect(find.text(connectNoAnswerMessage), findsOneWidget);

      tester
          .state<ScaffoldMessengerState>(find.byType(ScaffoldMessenger))
          .removeCurrentSnackBar();
      await tester.pump(const Duration(milliseconds: 400));
      expect(find.text(connectNoAnswerMessage), findsNothing);

      // Same shape, second time: the timeout notice is `const` as well.
      unawaited(manager.connectTo(bleLink));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 60));
      await tester.pump(const Duration(milliseconds: 100));

      expect(find.text(connectNoAnswerMessage), findsOneWidget);
      expect(transport.closeCount, 2);

      await tester.pumpWidget(const SizedBox());
    });

    testWidgets('a failed Bluetooth connect reaches the stale-bond guidance', (
      tester,
    ) async {
      // The shape the firmware's refusal takes on a phone that still holds a bond the device
      // dropped: the platform's own words say the pairing/encryption failed.
      final manager = ConnectionManager.forTest()..seedLinkForTest(bleLink);
      manager.transportFactory = (_) => _FakeTransport(
        failure: 'GATT_INSUFFICIENT_AUTHENTICATION (status 5)',
      );
      tester.view.physicalSize = const Size(900, 1600);
      tester.view.devicePixelRatio = 1.0;
      addTearDown(tester.view.resetPhysicalSize);

      await tester.pumpWidget(
        MaterialApp(
          theme: buildTheme(),
          home: ConnectionPage(manager: manager),
        ),
      );
      await tester.pump();

      await tester.tap(find.text('Tamu core'));
      await tester.pump();
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));

      // The fast read is the one line...
      expect(find.text(connectPairingWindowMessage), findsOneWidget);
      // ...and the guidance behind it is the copy that names the phone-side fix, which the
      // one-liner does not carry: a phone that still holds the old bond must forget it, or
      // opening a pairing window alone does not get it back in.
      expect(
        find.text('This phone is no longer paired with the device'),
        findsOneWidget,
      );
      expect(find.textContaining('Bluetooth settings'), findsWidgets);

      await tester.pumpWidget(const SizedBox());
    });
  });
}

/// Lets pending microtasks run without waiting on a clock.
Future<void> _settle() => Future<void>.delayed(Duration.zero);

/// A link whose platform connect never answers until told to, and that records being closed.
///
/// That is the shape the card is about: a device that is off or out of range leaves the app waiting
/// on a platform call that has no deadline of its own. A [_connectGate] that is never completed is
/// the hang; [closeCount] is the evidence the app dropped the attempt rather than the spinner.
class _FakeTransport implements Transport {
  _FakeTransport({this.failure});

  /// Text thrown by [connect] instead of hanging, for the failure cases.
  final String? failure;

  final _connectGate = Completer<void>();
  final _packets = StreamController<Uint8List>.broadcast();

  bool connectStarted = false;
  int closeCount = 0;

  @override
  String get id => 'AA:BB:CC:DD:EE:01';

  @override
  String get displayName => 'BLE $id';

  @override
  Stream<Uint8List> get linkBytes => _packets.stream;

  @override
  Stream<Uint8List> get packetStream => _packets.stream;

  @override
  Future<void> connect() async {
    connectStarted = true;
    final failure = this.failure;
    if (failure != null) throw TransportException(failure);
    await _connectGate.future;
  }

  /// The platform's own connect finishing after the app stopped waiting.
  void finishConnect() {
    if (!_connectGate.isCompleted) _connectGate.complete();
  }

  @override
  Future<void> send(List<int> streamBytes) async {}

  @override
  Future<void> close() async {
    closeCount++;
  }
}
