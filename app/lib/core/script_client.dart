/// Script service client (Docs/Services/Script.md).
///
/// The manager (SRV Script, CID 0-17 / 64+) exposes script enumeration, file
/// read/write and per-script state/IO. Scripts are stored as files named
/// "SCR" + 3-digit id, so enumeration reuses the Storage file table.
library;

import 'connection.dart';
import 'diagnostics.dart';
import 'protocol.dart';
import 'storage_client.dart';
import 'types.dart';

/// Script states (Docs/Services/Script.md "State").
class ScriptStateCode {
  static const stopped = 0;
  static const running = 1;
  static const paused = 2;
  static const waiting = 3;
  static const finished = 4;
  static const error = 5;

  static String label(int state) => switch (state) {
        stopped => 'Stopped',
        running => 'Running',
        paused => 'Paused',
        waiting => 'Waiting',
        finished => 'Finished',
        error => 'Error',
        _ => 'Unknown',
      };
}

class ScriptClient {
  final int deviceId;

  ScriptClient({required this.deviceId});

  ConnectionManager get _link => ConnectionManager.instance;
  Duration get _timeout => const Duration(seconds: 6);

  Future<List<int>?> _request(int cid, {List<int> payload = const []}) async {
    try {
      return await _link.request(deviceId, ServiceType.script, cid,
          payload: payload, timeout: _timeout);
    } catch (error) {
      AppDiagnostics.log('script', 'request failed: $error');
      return null;
    }
  }

  /// Script IDs present in the storage file table (hole-tolerant).
  Future<List<int>> scriptIds() async {
    final records = await StorageClient(deviceId: deviceId).readFileTable();
    if (records == null) return const [];
    final ids = <int>[];
    for (final r in records) {
      if (r.name.length >= 6 &&
          r.name.startsWith('SCR') &&
          int.tryParse(r.name.substring(3, 6)) != null) {
        ids.add(int.parse(r.name.substring(3, 6)));
      }
    }
    ids.sort();
    return ids;
  }

  Future<int?> count() async {
    final reply = await _request(0);
    if (reply == null || reply.isEmpty) return null;
    return reply[0];
  }

  Future<String?> readName(int id) async {
    final reply = await _request(1, payload: [id]);
    // A 1-byte status (0xFF) means the script file does not exist.
    if (reply == null || reply.length < 16) return null;
    // char[16], NUL-padded on the wire.
    return String.fromCharCodes(reply).replaceAll('\x00', '').trimRight();
  }

  Future<({int inputs, int outputs})?> readIoSize(int id) async {
    final reply = await _request(2, payload: [id]);
    if (reply == null || reply.length < 2) return null;
    return (inputs: reply[0], outputs: reply[1]);
  }

  Future<int?> readState(int id) async {
    final reply = await _request(3, payload: [id]);
    if (reply == null || reply.isEmpty) return null;
    return reply[0];
  }

  /// Sets a script's state (Running / Paused / Stopped).
  Future<bool> setState(int id, int state) async {
    final reply = await _request(4, payload: [id, state]);
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  Future<({BlockMeta meta, List<int> value})?> readInput(int id, int idx) async {
    final reply = await _request(5, payload: [id, idx]);
    if (reply == null || reply.length < 4) return null;
    return (meta: BlockMeta.fromBytes(reply, 0), value: reply.sublist(4));
  }

  Future<bool> writeInput(
      int id, int idx, BlockMeta meta, List<int> value) async {
    final reply = await _request(6,
        payload: [id, idx, ...meta.toBytes(), ...value]);
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  Future<({BlockMeta meta, List<int> value})?> readOutput(int id, int idx) async {
    final reply = await _request(7, payload: [id, idx]);
    if (reply == null || reply.length < 4) return null;
    return (meta: BlockMeta.fromBytes(reply, 0), value: reply.sublist(4));
  }

  Future<({int variables, int instructions})?> getInfo(int id) async {
    final reply = await _request(8, payload: [id]);
    if (reply == null || reply.length < 3) return null;
    return (
      variables: reply[0],
      instructions: reply[1] | (reply[2] << 8),
    );
  }

  Future<({BlockMeta meta, List<int> value})?> readVariable(
      int id, int idx) async {
    final reply = await _request(9, payload: [id, idx]);
    if (reply == null || reply.length < 4) return null;
    return (meta: BlockMeta.fromBytes(reply, 0), value: reply.sublist(4));
  }

  Future<bool> writeVariable(
      int id, int idx, BlockMeta meta, List<int> value) async {
    final reply = await _request(10,
        payload: [id, idx, ...meta.toBytes(), ...value]);
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  Future<int?> getCurrentInstruction(int id) async {
    final reply = await _request(11, payload: [id]);
    if (reply == null || reply.length < 2) return null;
    return reply[0] | (reply[1] << 8);
  }

  Future<bool> moveToInstruction(int id, int instruction) async {
    final reply =
        await _request(12, payload: [id, instruction & 0xFF, (instruction >> 8) & 0xFF]);
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  /// Creates a script (id or auto-assign when null). Returns the assigned id.
  Future<int?> createScript({int? id}) async {
    final reply = await _request(13, payload: [id ?? 0xFF]);
    if (reply == null || reply.isEmpty || reply[0] == 0) return null;
    return reply[0];
  }

  Future<bool> deleteScript(int id) async {
    final reply = await _request(14, payload: [id]);
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  /// Reads the whole script file (CID 15, streamed; reassembled by the link layer).
  /// Returns null when there is no script file yet (a status reply is not a file;
  /// every valid script file is at least the 32-byte header).
  Future<List<int>?> readScriptFile(int id) async {
    final reply = await _request(15, payload: [id]);
    if (reply == null || reply.length < 32) return null;
    return reply;
  }

  /// Rewrites the whole script file (open stream with expected size -> chunks ->
  /// close). Returns true when the close confirms the stream completed.
  Future<bool> writeScriptFile(int id, List<int> bytes) async {
    final reply = await _request(16,
        payload: [id, ...uint32ToBytes(bytes.length)]);
    if (reply == null || reply.isEmpty || reply[0] == 0) return false;
    final streamCid = reply[0];

    const chunkSize = 240;
    for (var offset = 0; offset < bytes.length; offset += chunkSize) {
      final end = (offset + chunkSize > bytes.length)
          ? bytes.length
          : offset + chunkSize;
      try {
        await _link.sendNoReply(deviceId, ServiceType.script, streamCid,
            payload: bytes.sublist(offset, end));
      } catch (error) {
        AppDiagnostics.log('script', 'stream write failed: $error');
        return false;
      }
    }

    final close = await _request(17, payload: [id]);
    return close != null;
  }
}