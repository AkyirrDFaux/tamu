/// Bootloader service client (Docs/Services/Bootloader.md).
///
/// Two methods: check if device is in bootloader mode, and stream a firmware
/// binary via FRAG chunks. No software entry — user must hold boot button
/// and reset the device manually.
library;

import 'dart:typed_data';

import 'connection.dart';
import 'diagnostics.dart';
import 'protocol.dart';

/// Firmware chunk size per FRAG fragment (matches firmware MAX_FRAG_CONTENT_SIZE).
const int _fragChunkSize = 256;

class BootloaderClient {
  final int deviceId;

  BootloaderClient({required this.deviceId});

  ConnectionManager get _link => ConnectionManager.instance;

  /// CID 0: Bootloader Check.
  /// Returns true if the device is currently in bootloader mode.
  Future<bool> check() async {
    try {
      final reply = await _link.request(
        deviceId,
        ServiceType.bootloader,
        0,
        payload: Uint8List(4),
        timeout: const Duration(seconds: 2),
      );
      if (reply.length < 5) return false;
      return reply[4] != 0;
    } catch (_) {
      return false;
    }
  }

  /// CID 1: App Write (FRAG stream).
  /// Streams the firmware binary to the device in 256-byte FRAG chunks.
  /// [onProgress] is called with (current fragment, total fragments).
  /// Returns true when all fragments are acknowledged.
  Future<bool> writeBinary(
    Uint8List binary, {
    void Function(int current, int total)? onProgress,
  }) async {
    final totalFrags = (binary.length + _fragChunkSize - 1) ~/ _fragChunkSize;
    if (totalFrags == 0) return true;

    for (var frag = 0; frag < totalFrags; frag++) {
      final start = frag * _fragChunkSize;
      final end = (start + _fragChunkSize > binary.length)
          ? binary.length
          : start + _fragChunkSize;

      // Payload: FRAG info (4 bytes) + data chunk
      final payload = Uint8List(4 + (end - start));
      // Fragment info: u16 current + u16 total (little-endian)
      payload[0] = frag & 0xFF;
      payload[1] = (frag >> 8) & 0xFF;
      payload[2] = totalFrags & 0xFF;
      payload[3] = (totalFrags >> 8) & 0xFF;
      payload.setRange(4, 4 + (end - start), binary.sublist(start, end));

      final reply = await _link.request(
        deviceId,
        ServiceType.bootloader,
        1,
        payload: payload,
        frag: true,
        timeout: const Duration(seconds: 10),
      );
      if (reply.length < 2) {
        AppDiagnostics.log('bootloader', 'frag $frag/$totalFrags write failed');
        return false;
      }
      final lastSeq = reply[0] | (reply[1] << 8);
      if (lastSeq == 0xFFFF) {
        AppDiagnostics.log('bootloader', 'device reported no writable target');
        return false;
      }

      onProgress?.call(frag + 1, totalFrags);
    }
    return true;
  }
}
