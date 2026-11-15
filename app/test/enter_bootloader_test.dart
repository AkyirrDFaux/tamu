/// Device service CID 4 "enter bootloader": the app's command that puts ONE node back into its
/// bootloader (`lib/core/enter_bootloader.dart`).
///
/// The three things the card pins are here: the CID, the addressed form of the request, and the
/// two shapes the answer can take (an ack, and the node's own refusal). The fourth case - a node
/// that does NOT implement the command - is silence on the wire, and what tells it apart from a
/// lost ack is the bootloader's identify, so that probe is part of every case below.
library;

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader_client.dart';
import 'package:tamuapp/core/enter_bootloader.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/transport.dart';

import 'support/fake_channel.dart';

/// The identify a DAS bootloader answers with: a 14336 B app space in 64 B pages (`dasBootloaderPageSize`).
BootloaderIdentity get _dasBootloader => const BootloaderIdentity(
  appSize: 14336,
  pageSize: dasBootloaderPageSize,
  name: 'DAS v0.1',
);

/// One recorded request, as the channel saw it.
typedef _Sent = ({
  int targetId,
  ServiceType service,
  int cid,
  List<int> payload,
});

/// A channel that records every request and answers through [onRequest].
///
/// [onRequest] returning null is a node that never answered - the shape of BOTH a lost frame and
/// a node that does not implement the CID - which is exactly the case the identify probe has to
/// resolve.
class _RecordingChannel extends FakeTolerantChannel {
  _RecordingChannel(this.onRequest, {this.connected = true});

  Future<PacketResponse?> Function(int targetId, int cid) onRequest;

  /// Whether the channel reports a live link; false is the app before a connect.
  final bool connected;

  final List<_Sent> sent = [];

  @override
  bool get isConnected => connected;

  @override
  Future<PacketResponse> answer(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
  }) async {
    sent.add((
      targetId: targetId,
      service: service,
      cid: functionCid,
      payload: payload,
    ));
    final reply = await onRequest(targetId, functionCid);
    if (reply == null) throw const TransportException('no answer');
    return reply;
  }
}

/// An identify probe that counts its calls and answers [identity].
class _Probe {
  _Probe([this.identity]);

  BootloaderIdentity? identity;
  int calls = 0;

  Future<BootloaderIdentity?> call(Duration timeout) async {
    calls++;
    return identity;
  }
}

void main() {
  tearDown(() => installRequestChannel(null));

  test(
    'the CID is Device service 0x0004 and the command byte is that service',
    () {
      expect(deviceCidEnterBootloader, 0x0004);
      // The CMD word the frame carries: service type in the high byte, CID in the low one.
      expect(makeService(ServiceType.device, deviceCidEnterBootloader), 0x0004);
      expect(
        serviceCidOf(makeService(ServiceType.device, deviceCidEnterBootloader)),
        4,
      );
    },
  );

  test('the request is the addressed form: one node, no payload', () async {
    final channel = _RecordingChannel((_, _) async => wholeReply(const []));
    installRequestChannel(channel);
    final probe = _Probe(_dasBootloader);

    final result = await enterNodeBootloader(2, identify: probe.call);

    expect(channel.sent, hasLength(1));
    final frame = channel.sent.single;
    expect(frame.targetId, 2, reason: 'addressed to ONE node, not a broadcast');
    expect(frame.service, ServiceType.device);
    expect(frame.cid, 4);
    expect(frame.payload, isEmpty, reason: 'the request carries no payload');
    expect(
      result.outcome,
      EnterBootloaderOutcome.accepted,
      reason: result.message,
    );
  });

  test(
    'a complete ack means the node left its app and is a bootloader',
    () async {
      installRequestChannel(
        _RecordingChannel((_, _) async => wholeReply(const [])),
      );
      final probe = _Probe(_dasBootloader);

      final result = await enterNodeBootloader(2, identify: probe.call);

      expect(result.outcome, EnterBootloaderOutcome.accepted);
      expect(result.mode, NodeMode.bootloader);
      expect(result.entered, isTrue);
      expect(
        probe.calls,
        1,
        reason: 'the state change is confirmed by identify',
      );
      expect(result.bootloader!.pageSize, dasBootloaderPageSize);
      expect(result.message, contains('left its app'));
      expect(result.message, contains('64 B pages'));
    },
  );

  test(
    'a refusal (FLAG_FAIL) leaves the node in its app and is not probed',
    () async {
      installRequestChannel(
        _RecordingChannel(
          (_, _) async =>
              const PacketResponse(payload: [], success: false, fail: true),
        ),
      );
      final probe = _Probe(_dasBootloader);

      final result = await enterNodeBootloader(2, identify: probe.call);

      expect(result.outcome, EnterBootloaderOutcome.refused);
      expect(result.mode, NodeMode.app);
      expect(result.entered, isFalse);
      expect(
        probe.calls,
        0,
        reason: 'a refusal needs no identify: the app stays',
      );
      expect(result.message, contains('refused Device CID 4'));
    },
  );

  test(
    'silence with no bootloader answering is "does not implement it"',
    () async {
      // A node built without DEVICE_HAS_BOOTLOADER_ENTRY: the CID falls through to no reply, and no
      // node is in bootloader mode, so the passthrough identify is silent too.
      installRequestChannel(_RecordingChannel((_, _) async => null));
      final probe = _Probe();

      final result = await enterNodeBootloader(2, identify: probe.call);

      expect(result.outcome, EnterBootloaderOutcome.notImplemented);
      expect(result.mode, NodeMode.app, reason: 'its app is still running');
      expect(result.entered, isFalse);
      expect(probe.calls, 1, reason: 'the probe is what proves it never left');
      expect(result.message, contains('does not implement the command'));
    },
  );

  test(
    'a lost ack is not a lost node: the bootloader identify names the state',
    () async {
      installRequestChannel(_RecordingChannel((_, _) async => null));
      final probe = _Probe(_dasBootloader);

      final result = await enterNodeBootloader(2, identify: probe.call);

      expect(result.outcome, EnterBootloaderOutcome.ackLost);
      expect(result.mode, NodeMode.bootloader);
      expect(result.entered, isTrue);
      expect(result.bootloader!.pageSize, dasBootloaderPageSize);
      expect(result.message, contains('no complete ack'));
    },
  );

  test('an ack that arrived incomplete is not a complete ack', () async {
    installRequestChannel(
      _RecordingChannel(
        (_, _) async => const PacketResponse(
          payload: [],
          success: true,
          fail: false,
          stopped: false,
          total: 1,
        ),
      ),
    );
    final probe = _Probe(_dasBootloader);

    final result = await enterNodeBootloader(2, identify: probe.call);

    expect(result.outcome, EnterBootloaderOutcome.ackLost);
    expect(result.mode, NodeMode.bootloader);
    expect(result.detail, contains('incomplete'));
  });

  test('a broadcast address is refused before anything is sent', () async {
    final channel = _RecordingChannel((_, _) async => wholeReply(const []));
    installRequestChannel(channel);
    var probed = false;

    for (final target in [broadcastAddress, invalidAddress]) {
      final result = await enterNodeBootloader(
        target,
        identify: (_) async {
          probed = true;
          return _dasBootloader;
        },
      );
      expect(result.outcome, EnterBootloaderOutcome.notSent);
      expect(result.mode, NodeMode.app);
      expect(result.message, contains('names one node'));
    }
    expect(channel.sent, isEmpty, reason: 'nothing went on the wire');
    expect(probed, isFalse);
  });

  test('no link is not a device verdict', () async {
    var probed = false;
    Future<BootloaderIdentity?> probe(Duration _) async {
      probed = true;
      return null;
    }

    // (a) nothing installed at all, and (b) a channel that says the link is down: both are the
    // app's own state, and neither may read as a node that did not answer.
    installRequestChannel(null);
    var result = await enterNodeBootloader(2, identify: probe);
    expect(result.outcome, EnterBootloaderOutcome.notSent);
    expect(result.mode, NodeMode.app);
    expect(result.message, contains('no link'));

    final channel = _RecordingChannel((_, _) async => null, connected: false);
    installRequestChannel(channel);
    result = await enterNodeBootloader(2, identify: probe);
    expect(result.outcome, EnterBootloaderOutcome.notSent);
    expect(channel.sent, isEmpty, reason: 'nothing went on the wire');

    expect(probed, isFalse, reason: 'with no link there is nothing to probe');
  });

  test(
    'the verdict is the ack, the refusal and the identify, and nothing else',
    () {
      final accepted = enterBootloaderVerdict(
        deviceId: 2,
        acked: true,
        refused: false,
        bootloader: _dasBootloader,
      );
      expect(accepted.outcome, EnterBootloaderOutcome.accepted);
      expect(accepted.mode, NodeMode.bootloader);

      // A refusal outranks an identify that answered for some OTHER node's bootloader.
      final refused = enterBootloaderVerdict(
        deviceId: 2,
        acked: false,
        refused: true,
        bootloader: _dasBootloader,
      );
      expect(refused.outcome, EnterBootloaderOutcome.refused);
      expect(refused.mode, NodeMode.app);

      final silent = enterBootloaderVerdict(
        deviceId: 2,
        acked: false,
        refused: false,
      );
      expect(silent.outcome, EnterBootloaderOutcome.notImplemented);
      expect(silent.mode, NodeMode.app);
    },
  );
}
