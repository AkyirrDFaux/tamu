/// Bootloader transport over BLE to the device's bootloader service.
///
/// Connects to the bootloader's BLE service and exchanges bootloader frames
/// (Docs/Services/Bootloader.md).
///
/// The bootloader uses a custom GATT service:
/// - Service UUID: 54414D55-0001-4B4C-8000-00805F9B34FB
/// - RX characteristic (peer -> bootloader write): 54414D55-0002-4B4C-8000-00805F9B34FB
/// - TX characteristic (bootloader -> peer notify): 54414D55-0003-4B4C-8000-00805F9B34FB
///
/// Frames are the bootloader raw format (Docs/Services/Bootloader.md):
///   [0xCA] [control] [offset LE] [32B payload opt] [0xBC]
library;

import 'dart:async';
import 'dart:typed_data';

import 'package:universal_ble/universal_ble.dart';

import 'bootloader.dart';
import 'bootloader_client.dart';
import 'transport.dart';

/// Bootloader transport over BLE to the device's bootloader.
class BleBootloaderTransport implements BootloaderTransport {
  final String deviceId;
  final Duration connectTimeout;

  // Bootloader BLE service UUIDs (Docs/Services/Bootloader.md, CoreBootloader.cpp)
  static const _svcUuid = '54414d55-0001-4b4c-8000-00805f9b34fb';
  static const _txUuid = '54414d55-0003-4b4c-8000-00805f9b34fb';

  BleBootloaderTransport({required this.deviceId, this.connectTimeout = const Duration(seconds: 15)});

  final _frameController = StreamController<Uint8List>.broadcast();
  late final StreamSubscription _valueSub;
  late final StreamSubscription _connSub;
  int _mtu = 247;
  bool _closed = false;

  Future<void> connect() async {
    _closed = false;
    await _connectOnce();
  }

  Future<void> _connectOnce() async {
    await UniversalBle.connect(deviceId);
    await UniversalBle.discoverServices(deviceId);

    _valueSub = UniversalBle.characteristicValueStream(deviceId, _txUuid)
        .listen((value) => _frameController.add(value));

    await UniversalBle.subscribeNotifications(deviceId, _svcUuid, _txUuid);

    _connSub = UniversalBle.connectionStream(deviceId).listen((connected) {
      if (!connected && !_closed) _frameController.addError(TransportException('BLE bootloader link lost'));
    });

    try {
      final mtu = await UniversalBle.requestMtu(deviceId, 512);
      _mtu = mtu.clamp(23, 512);
    } catch (_) {}
  }

  @override
  Future<BootloaderIdentity?> identify(Duration timeout) async {
    final reply = await _sendFrame(Bootloader.encodeIdentifyRequest(), timeout);
    if (reply == null) return null;
    final size = Bootloader.appSize(reply);
    final page = Bootloader.pageSize(reply);
    final name = Bootloader.name(reply);
    return BootloaderIdentity(appSize: size, pageSize: page, name: name);
  }

  @override
  Future<Uint8List> readChunk(int offset, Duration timeout) async {
    final frame = Bootloader.encodeReadRequest(offset);
    final reply = await _sendFrame(frame, timeout);
    if (reply == null || reply.length < Bootloader.dataSize) {
      throw PassthroughFailure(PassthroughReason.unreported, 'read 0x${offset.toRadixString(16)}');
    }
    return Uint8List.fromList(reply.sublist(Bootloader.headerSize, Bootloader.headerSize + Bootloader.payloadSize));
  }

  @override
  Future<int> erasePage(int offset, Duration timeout) async {
    final frame = Bootloader.encodeEraseRequest(offset);
    final reply = await _sendFrame(frame, timeout);
    if (reply == null || reply.length < Bootloader.readRequestSize) {
      throw PassthroughFailure(PassthroughReason.unreported, 'erase 0x${offset.toRadixString(16)}');
    }
    final page = Bootloader.offset(reply);
    if (Bootloader.ackFailed(page)) {
      throw PassthroughFailure(PassthroughReason.nodeRefused, 'erase 0x${offset.toRadixString(16)}',
        code: PassthroughReason.nodeRefused.code);
    }
    return page;
  }

  @override
  Future<WriteAck> writeChunk(int offset, List<int> data, Duration timeout) async {
    final frame = Bootloader.encodeWrite(offset, Bootloader.padPayload(data));
    final reply = await _sendFrame(frame, timeout);
    if (reply == null || reply.length < Bootloader.readRequestSize) {
      return WriteAck(false);
    }
    final ackOffset = Bootloader.offset(reply);
    return WriteAck(ackOffset == offset, acked: ackOffset);
  }

  @override
  Future<int> crc(Duration timeout) async {
    final frame = Bootloader.encodeCrcRequest();
    final reply = await _sendFrame(frame, timeout);
    if (reply == null || reply.length < Bootloader.crcResponseSize) {
      return 0;
    }
    return Bootloader.crc(reply);
  }

  @override
  Future<bool> jump(Duration timeout) async {
    final frame = Bootloader.encodeJumpRequest();
    final reply = await _sendFrame(frame, timeout);
    return reply != null;
  }

  @override
  Future<bool> enterBootloader(Duration timeout) async {
    final frame = Bootloader.encodeBootloaderEntryRequest();
    final reply = await _sendFrame(frame, timeout);
    return reply != null;
  }

  /// Sends a bootloader frame and waits for the matching response frame.
  /// Bootloader frames are 7 bytes (header) or 39 bytes (with payload).
  Future<Uint8List?> _sendFrame(List<int> request, Duration timeout) async {
    final completer = Completer<Uint8List?>();
    final pending = <int>[];

    _frameController.stream.listen((chunk) {
      pending.addAll(chunk);
      while (pending.isNotEmpty) {
        if (pending[0] != Bootloader.start) {
          pending.removeAt(0);
          continue;
        }
        final expected = pending.length >= 2
            ? (pending[pending.length - 1] == Bootloader.end
                ? pending.length
                : (pending.length >= 3 && (pending[2] == Bootloader.cmdReadResponse || pending[2] == Bootloader.cmdWriteResponse)
                    ? Bootloader.dataSize
                    : Bootloader.readRequestSize))
            : pending.length;
        if (pending.length < expected) break;
        final frame = pending.sublist(0, expected);
        pending.removeRange(0, expected);
        final cmd = Bootloader.decode(frame);
        if (cmd != Bootloader.invalid) {
          completer.complete(Uint8List.fromList(frame));
          return;
        }
        pending.removeAt(0);
      }
    });

    try {
      final mtu = _mtu - 3;
      final bytes = Uint8List.fromList(request);
      for (var i = 0; i < bytes.length; i += mtu) {
        final end = (i + mtu).clamp(i, bytes.length);
        await UniversalBle.write(
          deviceId,
          Bootloader.svcUuid,
          Bootloader.rxUuid,
          bytes.sublist(i, end),
          withoutResponse: false,
        );
      }
      return await completer.future.timeout(timeout);
    } on TimeoutException {
      return null;
    } finally {
      completer.complete(null);
    }
  }

  Future<void> close() async {
    _closed = true;
    await _valueSub.cancel();
    await _connSub.cancel();
    _frameController.close();
    try { await UniversalBle.disconnect(deviceId); } catch (_) {}
  }
}