/// Bootloader service client (Docs/Services/Bootloader.md, SRV 0x13).
///
/// CIDs:
///   0 — check:       returns bool (true = in bootloader mode)
///   1 — switch:      bool (true = enter, false = leave)
///   2 — device info: vendor info from node enumeration (16 bytes)
///   3 — reader:      u32 frag_idx → u32 frag_idx + 256B data
///   4 — writer:      u32 frag_idx + 256B data → u32 frag_idx
library;

import 'dart:typed_data';

import 'connection.dart';
import 'diagnostics.dart';
import 'protocol.dart';

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
        timeout: const Duration(seconds: 2),
      );
      return reply.isNotEmpty && reply[0] != 0;
    } catch (_) {
      return false;
    }
  }

  /// CID 1: Bootloader Switch.
  /// [enter]: true to enter bootloader mode, false to leave.
  /// Returns true on acknowledgment.
  Future<bool> switchMode({required bool enter}) async {
    try {
      await _link.request(
        deviceId,
        ServiceType.bootloader,
        1,
        payload: Uint8List.fromList([enter ? 1 : 0]),
        timeout: const Duration(seconds: 5),
      );
      return true;
    } catch (_) {
      return false;
    }
  }

  /// CID 2: Device Info.
  /// Returns 16-byte vendor info (u16 device_type + 14-byte serial_number),
  /// or null if the node has not been enumerated.
  Future<Uint8List?> deviceInfo() async {
    try {
      final reply = await _link.request(
        deviceId,
        ServiceType.bootloader,
        2,
        timeout: const Duration(seconds: 2),
      );
      if (reply.length < 16) return null;
      return Uint8List.fromList(reply.sublist(0, 16));
    } catch (_) {
      return null;
    }
  }

  /// CID 3: Bootloader Reader.
  /// Reads a 256-byte fragment from the node's flash at [fragIdx].
  /// Returns the 256-byte data, or null on timeout/error.
  Future<Uint8List?> readFragment(int fragIdx) async {
    final idxBytes = ByteData(4)..setUint32(0, fragIdx, Endian.little);
    try {
      final reply = await _link.request(
        deviceId,
        ServiceType.bootloader,
        3,
        payload: idxBytes.buffer.asUint8List(),
        timeout: const Duration(seconds: 3),
      );
      // Response: u32 frag_idx + 256B data = 260 bytes
      if (reply.length < _fragChunkSize) return null;
      return Uint8List.fromList(reply.sublist(4, 4 + _fragChunkSize));
    } catch (_) {
      return null;
    }
  }

  /// CID 4: Bootloader Writer.
  /// Writes a 256-byte fragment to the node's flash at [fragIdx].
  /// [data] must be exactly 256 bytes (pad with 0xFF if needed).
  /// Returns the acknowledged frag_idx, or null on timeout/error.
  Future<int?> writeFragment(int fragIdx, Uint8List data) async {
    assert(data.length == _fragChunkSize);
    final payload = Uint8List(4 + _fragChunkSize);
    ByteData.view(payload.buffer)
        .setUint32(0, fragIdx, Endian.little);
    payload.setRange(4, 4 + _fragChunkSize, data);
    try {
      final reply = await _link.request(
        deviceId,
        ServiceType.bootloader,
        4,
        payload: payload,
        timeout: const Duration(seconds: 10),
      );
      if (reply.length < 4) return null;
      return ByteData.view(Uint8List.fromList(reply).buffer)
          .getUint32(0, Endian.little);
    } catch (_) {
      return null;
    }
  }

  /// High-level upload: streams [binary] to the node flash.
  /// Returns true when all fragments are acknowledged and verified.
  Future<bool> uploadBinary(
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

      // Pad last fragment with 0xFF (standard flash blank value)
      final chunk = Uint8List(_fragChunkSize);
      chunk.fillRange(0, _fragChunkSize, 0xFF);
      chunk.setRange(0, end - start, binary.sublist(start, end));

      final ack = await writeFragment(frag, chunk);
      if (ack == null || ack != frag) {
        AppDiagnostics.log('bootloader', 'frag $frag/$totalFrags write failed');
        return false;
      }

      onProgress?.call(frag + 1, totalFrags);
    }
    return true;
  }

  /// High-level verify: reads back all fragments and checks against [binary].
  /// Returns true if all fragments match.
  Future<bool> verifyBinary(
    Uint8List binary, {
    void Function(int current, int total)? onProgress,
  }) async {
    final totalFrags = (binary.length + _fragChunkSize - 1) ~/ _fragChunkSize;
    for (var frag = 0; frag < totalFrags; frag++) {
      final data = await readFragment(frag);
      if (data == null) {
        AppDiagnostics.log('bootloader', 'frag $frag/$totalFrags read failed');
        return false;
      }

      final start = frag * _fragChunkSize;
      final end = (start + _fragChunkSize > binary.length)
          ? binary.length
          : start + _fragChunkSize;

      // Check actual data portion
      for (var i = start; i < end; i++) {
        if (data[i - start] != binary[i]) {
          AppDiagnostics.log('bootloader', 'frag $frag verify mismatch at byte ${i - start}');
          return false;
        }
      }

      onProgress?.call(frag + 1, totalFrags);
    }
    return true;
  }
}
