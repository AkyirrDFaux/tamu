/// A serial link to the rig's core, for command-line tools
/// (Docs/Services/App Interface.md).
///
/// The app's own connection stack (`connection.dart`, `transport.dart`) is Flutter-coupled -
/// a ChangeNotifier, plugin channels - so a plain `dart run` program cannot use it. This is the
/// same transaction layer over one serial port: the USB link framing (`usb_frame.dart`), the
/// packet stream parser (`protocol.dart`) and the reply reassembler (`fragment_reassembler.dart`)
/// are the shared pure-Dart pieces, so a request sent from here is byte-identical to the app's.
///
/// `flutter_libserialport` is a Flutter plugin, but its Dart API is plain FFI: the only thing it
/// needs from Flutter is its native `libserialport.so`, which a Linux build leaves in the debug
/// bundle (see [preloadLibserialport]).
library;

import 'dart:async';
import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_libserialport/flutter_libserialport.dart';
import 'package:tamuapp/core/fragment_reassembler.dart';
import 'package:tamuapp/core/protocol.dart';
import 'package:tamuapp/core/request_channel.dart';
import 'package:tamuapp/core/usb_frame.dart';

/// A link failure: the port would not open/configure, or a request timed out.
class LinkException implements Exception {
  final String message;
  const LinkException(this.message);

  @override
  String toString() => message;
}

/// Preloads the bundled `libserialport.so` so `flutter_libserialport`'s own lookup resolves.
///
/// The package dlopen()s the library by its soname, which only works when the loader already
/// knows that name; loading it by absolute path first (its SONAME is `libserialport.so`, so the
/// later by-name lookup matches the loaded object) is what makes the package usable outside a
/// Flutter build. Returns the path loaded, or null when none was found - a system libserialport
/// may still satisfy the package.
String? preloadLibserialport() {
  for (final path in _libserialportCandidates()) {
    try {
      DynamicLibrary.open(path);
      return path;
    } catch (_) {
      continue;
    }
  }
  return null;
}

/// Where a Flutter Linux build leaves the library: `<arch>/{debug,release}/bundle/lib`.
Iterable<String> _libserialportCandidates() sync* {
  final arch = Directory('build/linux');
  if (!arch.existsSync()) return;
  for (final entry in arch.listSync()) {
    if (entry is! Directory) continue;
    for (final mode in const ['debug', 'release']) {
      yield '${entry.path}/$mode/bundle/lib/libserialport.so';
    }
  }
}

/// The serial ports that could be the rig's core, best first: an Espressif USB
/// JTAG/serial device by its stable by-id path (the `ttyACM<N>` numbering moves between
/// sessions and on replug), then anything the platform enumerates.
List<String> corePortCandidates() {
  final out = <String>[];
  final byId = Directory('/dev/serial/by-id');
  if (byId.existsSync()) {
    for (final entry in byId.listSync()) {
      if (entry is! Link && entry is! File) continue;
      if (!entry.path.contains('Espressif')) continue;
      try {
        out.add(File(entry.path).resolveSymbolicLinksSync());
      } catch (_) {
        continue;
      }
    }
  }
  try {
    for (final name in SerialPort.availablePorts) {
      if (out.contains(name)) continue;
      out.add(name);
    }
  } catch (_) {
    // No libserialport: the caller reports it.
  }
  return out;
}

/// The link surface a command-line tool uses: the request channel the service clients talk
/// through, plus an explicit shutdown.
///
/// Shutdown is part of the surface because an open serial port keeps the Dart isolate alive: a
/// tool that returns without closing hangs instead of exiting. Naming it here keeps the old bug
/// (a refusal that returned with the port still open, killed at 400 s) out of reach, and lets the
/// CLI's exit paths be tested without a rig - a fake link counts its closes.
abstract class RigLink implements RequestChannel {
  /// Closes the port and fails anything still waiting for a reply. Idempotent.
  Future<void> close();
}

/// The receive path: USB link frames, then the packet stream (Docs/Services/App Interface.md).
///
/// The core answers INSIDE the USB link framing (`0xFA | CRC8 | Length | payload | 0xBF`), so a
/// reply needs both stages - [UsbFrameParser] to unwrap the link frame, then
/// [PacketStreamParser] to decode the packet. `UsbTransport` composes exactly these two stages
/// for the app (`transport.dart`); this is that composition for the command-line link, and the
/// channel feeds every incoming byte through it.
///
/// Skipping the first stage is not a near miss: the packet parser takes framing bytes for packet
/// bytes, resyncs through a frame it can never complete and returns nothing, so every request
/// times out against a perfectly healthy rig.
class RigReceivePath {
  final UsbFrameParser _frames = UsbFrameParser();
  final PacketStreamParser _packets = PacketStreamParser();

  /// Called with the unwrapped packet-stream bytes (before parsing), so a tool can bank what the
  /// device actually sent.
  void Function(List<int> bytes)? onStreamBytes;

  /// Feeds wire bytes and returns the frames they completed (usually none, or one per reply).
  List<PacketFrame> feed(List<int> bytes) {
    final stream = _frames.feed(bytes);
    if (stream.isEmpty) return const [];
    onStreamBytes?.call(stream);
    return _packets.feed(stream);
  }
}

/// One request/response channel over a serial port (the core, at address 1).
class RigChannel implements RigLink, TolerantRequestChannel {
  RigChannel._(this._port, this._reader);

  final SerialPort _port;
  final SerialPortReader _reader;
  StreamSubscription<Uint8List>? _sub;
  final RigReceivePath _receive = RigReceivePath();
  final FragmentReassembler _reassembler = FragmentReassembler();
  final Map<int, Completer<PacketResponse>> _pending = {};
  final Set<int> _tolerant = {};
  final Map<int, Timer> _stall = {};
  int _nextTxId = tridAppBase;
  Function(List<int>)? _listener;
  bool _open = true;
  String? lastError;

  /// The packet-stream bytes this channel received, when [capture] is set: the same bytes the
  /// app's parser sees, so a tool can bank a real reply as a fixture.
  void Function(List<int> bytes)? capture;

  /// Mirrors the app's link (`ConnectionManager.fragmentStallTimeout`): a stream that stops
  /// arriving is closed with what came, so a truncated reply is a PARTIAL answer instead of a
  /// 10 s timeout.
  static const Duration fragmentStallTimeout = Duration(milliseconds: 500);

  /// Opens [portName] and starts reading. Opening asserts DTR/RTS, which resets the core, so
  /// the caller must wait for it to answer a ping before sending anything else.
  static Future<RigChannel> open(String portName) async {
    final port = SerialPort(portName);
    if (!port.openReadWrite()) {
      final error = SerialPort.lastError;
      port.dispose();
      throw LinkException('cannot open $portName: $error');
    }
    // The same configuration the app applies: 115200 8N1 with BOTH control lines asserted (the
    // state the ESP32-C3 runs stably in). xonXoff must be set explicitly - upstream
    // sp_new_config() leaves it uninitialized, so an unset value randomly fails sp_set_config.
    Object? configError;
    for (var attempt = 0; attempt < 5; attempt++) {
      try {
        port.config = SerialPortConfig()
          ..baudRate = busBaudRate
          ..bits = 8
          ..stopBits = 1
          ..parity = SerialPortParity.none
          ..xonXoff = SerialPortXonXoff.disabled
          ..rts = SerialPortRts.on
          ..dtr = SerialPortDtr.on;
        configError = null;
        break;
      } catch (error) {
        configError = error;
        await Future<void>.delayed(const Duration(milliseconds: 300));
      }
    }
    if (configError != null) {
      try {
        port.close();
      } catch (_) {}
      port.dispose();
      throw LinkException('cannot configure $portName: $configError');
    }
    final channel = RigChannel._(port, SerialPortReader(port));
    channel._receive.onStreamBytes = (bytes) => channel.capture?.call(bytes);
    channel._sub = channel._reader.stream.listen(
      channel._onBytes,
      onError: (Object error) {
        channel.lastError = '$error';
        channel._failPending('link error: $error');
      },
      onDone: () => channel._failPending('link closed'),
    );
    return channel;
  }

  @override
  bool get isConnected => _open;

  @override
  void setSubscriptionListener(Function(List<int>)? listener) =>
      _listener = listener;

  @override
  int takeTxId() {
    for (var guard = 0; guard <= tridAppMax - tridAppBase; guard++) {
      final txId = _nextTxId;
      _nextTxId = _nextTxId >= tridAppMax ? tridAppBase : _nextTxId + 1;
      if (!_pending.containsKey(txId)) return txId;
    }
    throw const LinkException('no free transaction IDs');
  }

  @override
  Future<List<int>> request(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
    bool requestFrag = false,
    int? transactionId,
  }) async => (await requestWithFlags(
    targetId,
    service,
    functionCid,
    payload: payload,
    timeout: timeout,
    requestFrag: requestFrag,
    transactionId: transactionId,
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
    bool tolerant = false,
  }) async {
    if (!_open) throw const LinkException('link closed');
    // A caller-supplied transaction ID lets several requests share one TRID (the Subscriptions
    // client registers both halves of a subscription under the same one).
    final txId = transactionId ?? takeTxId();
    if (_pending.containsKey(txId)) {
      throw LinkException(
        'transaction ID 0x${txId.toRadixString(16)} already in use',
      );
    }
    final frame = PacketFrame.single(
      targetId: targetId,
      srvTarget: makeService(service, functionCid),
      srvSource: txId,
      response: false,
      payload: payload,
      requestFrag: requestFrag,
    );
    final completer = Completer<PacketResponse>();
    _pending[txId] = completer;
    _reassembler.begin(txId, tolerant: tolerant);
    if (tolerant) {
      _tolerant.add(txId);
    } else {
      _tolerant.remove(txId);
    }
    try {
      await send(frame.toBytes());
      return await completer.future.timeout(
        timeout,
        onTimeout: () {
          _pending.remove(txId);
          _tolerant.remove(txId);
          _stall.remove(txId)?.cancel();
          _reassembler.discard(txId);
          throw LinkException('${service.name} CID $functionCid timed out');
        },
      );
    } catch (error) {
      _pending.remove(txId);
      _tolerant.remove(txId);
      _stall.remove(txId)?.cancel();
      _reassembler.discard(txId);
      rethrow;
    }
  }

  /// A request whose reply may arrive incomplete (see [TolerantRequestChannel]) - what the
  /// storage client asks through, so a tool exercises the app's own read policy.
  @override
  Future<PacketResponse> requestTolerant(
    int targetId,
    ServiceType service,
    int functionCid, {
    List<int> payload = const [],
    Duration timeout = const Duration(seconds: 2),
  }) => requestWithFlags(
    targetId,
    service,
    functionCid,
    payload: payload,
    timeout: timeout,
    tolerant: true,
  );

  /// Closes [txId]'s stream when nothing has arrived for [fragmentStallTimeout], exactly as the
  /// app's link does: a tolerant request gets the prefix, a strict one fails.
  void _armStall(int txId) {
    _stall[txId]?.cancel();
    if (!_reassembler.inProgress(txId)) return;
    _stall[txId] = Timer(fragmentStallTimeout, () {
      _stall.remove(txId);
      final completer = _pending[txId];
      if (completer == null || completer.isCompleted) return;
      final truncated = _reassembler.finish(txId);
      final tolerant = _tolerant.remove(txId);
      _pending.remove(txId);
      if (tolerant) {
        completer.complete(
          PacketResponse(
            payload: truncated?.payload ?? const <int>[],
            success: true,
            fail: false,
            chunks: truncated?.chunks ?? const [],
            missing: truncated?.missing ?? const [],
            stopped: false,
            total: truncated?.total ?? 0,
          ),
        );
      } else {
        completer.completeError(
          LinkException(
            'fragment stream stopped after '
            '${truncated?.chunks.length ?? 0} fragment(s)',
          ),
        );
      }
    });
  }

  /// Puts packet-stream bytes on the wire, framed per the USB link (max 60 bytes each).
  Future<void> send(List<int> streamBytes) async {
    if (!_open) throw const LinkException('link closed');
    for (var offset = 0; offset < streamBytes.length; offset += usbMaxPayload) {
      final end = (offset + usbMaxPayload).clamp(offset, streamBytes.length);
      _port.write(
        Uint8List.fromList(buildUsbFrame(streamBytes.sublist(offset, end))),
        timeout: 200,
      );
    }
  }

  void _onBytes(Uint8List bytes) {
    final List<PacketFrame> frames;
    try {
      frames = _receive.feed(bytes);
    } on FormatException catch (error) {
      lastError = 'packet parse error: $error';
      return;
    }
    for (final frame in frames) {
      // Firmware-pushed subscription value updates are addressed by subscription TRID, not by
      // an app transaction ID.
      if (serviceTypeOf(frame.srvTarget) == ServiceType.subscriptions &&
          serviceCidOf(frame.srvTarget) == 0) {
        final listener = _listener;
        if (listener != null) {
          listener(
            frame.isFrag && frame.payload.length >= 4
                ? frame.payload.sublist(4)
                : frame.payload,
          );
          continue;
        }
      }
      if (!frame.isResponse) continue;
      final txId = frame.srvSource;
      final completer = _pending.remove(txId);
      if (completer == null || completer.isCompleted) continue;
      switch (_reassembler.feed(txId, frame)) {
        case FragmentPending():
          _pending[txId] = completer;
          _armStall(txId);
        case FragmentComplete(
          :final payload,
          :final success,
          :final fail,
          :final chunks,
          :final missing,
          :final stopped,
          :final total,
        ):
          _stall.remove(txId)?.cancel();
          _tolerant.remove(txId);
          completer.complete(
            PacketResponse(
              payload: payload,
              success: success,
              fail: fail,
              chunks: chunks,
              missing: missing,
              stopped: stopped,
              total: total,
            ),
          );
        case FragmentAbort(:final reason):
          _stall.remove(txId)?.cancel();
          _tolerant.remove(txId);
          if (!completer.isCompleted) {
            completer.completeError(
              LinkException('fragment stream aborted: $reason'),
            );
          }
      }
    }
  }

  void _failPending(String reason) {
    for (final completer in _pending.values) {
      if (!completer.isCompleted) {
        completer.completeError(LinkException(reason));
      }
    }
    _pending.clear();
    for (final timer in _stall.values) {
      timer.cancel();
    }
    _stall.clear();
    _tolerant.clear();
    _reassembler.clear();
  }

  @override
  Future<void> close() async {
    // Idempotent: the CLI closes on every exit path, and a second close must not free the
    // native port handle twice.
    if (!_open && _sub == null) return;
    _open = false;
    _failPending('link closed');
    await _sub?.cancel();
    _sub = null;
    _reader.close();
    try {
      _port.close();
    } catch (_) {}
    _port.dispose();
  }
}
