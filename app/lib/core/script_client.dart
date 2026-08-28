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

  Future<List<int>?> _request(int cid, {List<int> payload = const [], Duration? timeout, bool frag = false}) async {
    try {
      return await _link.request(deviceId, ServiceType.script, cid,
          payload: payload, timeout: timeout ?? _timeout, frag: frag);
    } catch (error) {
      AppDiagnostics.log('script', 'request failed: $error');
      return null;
    }
  }

  /// Slices the value bytes after a BlockMeta header, clamped to the declared size
  /// (the wire payload is padded to 4 bytes).
  List<int> _valueSlice(List<int> reply, int size) {
    if (size <= 0) return <int>[];
    final avail = reply.length - 4;
    return reply.sublist(4, 4 + ((size > avail) ? avail : size));
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
    final meta = BlockMeta.fromBytes(reply, 0);
    return (meta: meta, value: _valueSlice(reply, meta.size));
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
    final meta = BlockMeta.fromBytes(reply, 0);
    return (meta: meta, value: _valueSlice(reply, meta.size));
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
    final meta = BlockMeta.fromBytes(reply, 0);
    return (meta: meta, value: _valueSlice(reply, meta.size));
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

/// Reads the whole script file (CID 15, FRAG stream). The reply carries the echoed
/// script ID followed by the file contents (the reassembly layer strips the
/// fragmentation info). Returns null when there is no script file yet (a status reply
/// is not a file; every valid script file is at least the 32-byte header).
Future<List<int>?> readScriptFile(int id) async {
  final reply = await _request(15, payload: [id],
      timeout: const Duration(seconds: 10));
  if (reply == null || reply.length < 32) return null;
  // Fragment 0 carries the echoed script ID (1 byte) before the contents.
  return reply.sublist(1);
}

/// Rewrites the whole script file (CID 16, FRAG stream). Fragment 0 carries the
/// script ID (the device creates the file); every fragment is acknowledged with the
/// last sequential fragmentation index written, so a lost fragment is resent from
/// that index. Returns true when every fragment was written.
Future<bool> writeScriptFile(int id, List<int> bytes) async {
  const contentSize = 256;
  var totalFrags = (bytes.length + contentSize - 1) ~/ contentSize;
  if (totalFrags == 0) totalFrags = 1; // empty script: still create the file
  var next = 0;
  while (next < totalFrags) {
    final start = next * contentSize;
    final end = (start + contentSize > bytes.length)
        ? bytes.length
        : start + contentSize;
    final payload = <int>[
      ...writeFragInfo(next, totalFrags),
      if (next == 0) id,
      ...bytes.sublist(start, end),
    ];
    final reply = await _request(16, payload: payload, frag: true);
    if (reply == null || reply.length < 2) return false;
    final lastSeq = reply[0] | (reply[1] << 8);
    if (lastSeq == 0xFFFF) return false; // device could not create/write the file
    next = lastSeq + 1;
  }
  return true;
}
}