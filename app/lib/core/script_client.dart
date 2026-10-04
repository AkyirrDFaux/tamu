/// Script service client (Docs/Services/Script.md, management commands 0x050X) plus the
/// Register access to a loaded script's block (the Scripts range 0x3F4-0x3F7).
library;

import 'dart:typed_data';

import 'connection.dart';
import 'diagnostics.dart';
import 'protocol.dart';
import 'register_client.dart';
import 'types.dart';

/// A loaded-script register entry (ValueInfo + value).
class ScriptEntry {
  final ValueInfo meta;
  final List<int> value;

  const ScriptEntry({required this.meta, required this.value});
}

class ScriptClient {
  final int deviceId;

  ScriptClient({required this.deviceId});

  ConnectionManager get _link => ConnectionManager.instance;

  Future<List<int>?> _request(
    ServiceType service,
    int cid, {
    List<int> payload = const [],
    Duration? timeout,
  }) async {
    try {
      return await _link.request(deviceId, service, cid,
          payload: payload, timeout: timeout ?? const Duration(seconds: 3));
    } catch (error) {
      AppDiagnostics.log('script', 'request failed: $error');
      return null;
    }
  }

  // ---- Management commands (0x0500-0x0507) ----

  /// CID 0: the file ids of the currently loaded scripts (uint16 each).
  Future<List<int>> loadedScripts() async {
    final reply = await _request(ServiceType.script, 0);
    if (reply == null || reply.isEmpty) return const [];
    final count = reply[0];
    final fileIds = <int>[];
    for (var i = 0; i < count && 2 + 2 * i < reply.length; i++) {
      fileIds.add(reply[1 + 2 * i] | (reply[2 + 2 * i] << 8));
    }
    return fileIds;
  }

  /// CID 1: loads `SCR_<fileId>` (uint16) into slot `loadedId` (uint8). The device answers
  /// Success - the caller picked the slot, so it already knows it. The file id and the slot are
  /// independent: many files (SCR_XXX) exist, at most [maxScripts] load at once.
  Future<bool> load(int fileId, int loadedId) async {
    final reply = await _request(ServiceType.script, 1,
        payload: [fileId & 0xFF, (fileId >> 8) & 0xFF, loadedId & 0xFF]);
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  /// CID 2: unloads a loaded script.
  Future<bool> unload(int loadedId) async {
    final reply = await _request(ServiceType.script, 2, payload: [loadedId & 0xFF]);
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  /// CID 3: a loaded script's state and its last error code (0 = OK).
  Future<({int state, int error})?> readState(int loadedId) async {
    final reply = await _request(ServiceType.script, 3, payload: [loadedId & 0xFF]);
    if (reply == null || reply.isEmpty) return null;
    return (state: reply[0], error: reply.length > 1 ? reply[1] : 0);
  }

  /// CID 4: sets a loaded script's state.
  Future<bool> setState(int loadedId, int state) async {
    final reply =
        await _request(ServiceType.script, 4, payload: [loadedId & 0xFF, state & 0xFF]);
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  /// CID 5: instruction counter + variable RAM (editor debug).
  Future<({int instructionCounter, List<int> variables})?> readInternalState(int loadedId) async {
    final reply = await _request(ServiceType.script, 5, payload: [loadedId & 0xFF]);
    if (reply == null || reply.length < 4) return null;
    final ic = uint32FromBytes(reply, 0);
    return (instructionCounter: ic, variables: reply.sublist(4));
  }

  /// CID 6: moves the instruction counter (editor debug).
  Future<bool> moveToInstruction(int loadedId, int instruction) async {
    final payload = <int>[loadedId & 0xFF, ...uint32ToBytes(instruction)];
    final reply = await _request(ServiceType.script, 6, payload: payload);
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  /// CID 7: writes a variable (editor debug).
  Future<bool> writeVariable(int loadedId, int variableId, List<int> value) async {
    final payload = <int>[loadedId & 0xFF, variableId & 0xFF, ...value];
    final reply = await _request(ServiceType.script, 7, payload: payload);
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  // ---- Register access (the Scripts range 0x3F4-0x3F7, addressed by the global slot) ----

  static Uint8List _bi(int inst, int field, int key) => blockInfoBytes(
      scriptTypeForIndex(inst), scriptInstanceForIndex(inst), field, key);

  /// The keys (entity indexes) of a script field: the block's Field&Key list (Register CID 0),
  /// filtered to that field.
  late final RegisterClient _reg = RegisterClient(deviceId: deviceId);

  Future<List<int>> enumerateKeys(int inst, int field) async {
    return (await _reg.enumerateKeys(
            scriptTypeForIndex(inst), scriptInstanceForIndex(inst), field)) ??
        const [];
  }

  /// Reads one Register entry (CID 1). Returns null when the entry does not exist.
  Future<ScriptEntry?> readEntry(int inst, int field, int key) async {
    final reply = await _request(
      ServiceType.register,
      RegisterCid.read,
      payload: _bi(inst, field, key),
    );
    if (reply == null || reply.length < 8) return null;
    final meta = ValueInfo.fromBytes(reply, 4, key);
    return ScriptEntry(
        meta: meta, value: RegisterClient.valueSlice(reply, meta.size));
  }

  /// Reads the block meta of a loaded script (field 0xFF): snapshot name + field count.
  Future<({ValueInfo meta, String name})?> readBlockMeta(int inst) async {
    final reply = await _request(
      ServiceType.register,
      RegisterCid.read,
      payload: _bi(inst, 0xFF, 0),
    );
    if (reply == null || reply.length < 8) return null;
    final meta = ValueInfo.fromBytes(reply, 4);
    final name = decodePaddedString(reply.sublist(8));
    return (meta: meta, name: name);
  }

  /// Writes one Register entry (CID 2), e.g. a script input (field 1) or variable (3).
  Future<bool> writeEntry(int inst, int field, int key, ValueInfo meta, List<int> value) async {
    final payload = <int>[..._bi(inst, field, key), ...meta.toBytes(), ...value];
    final reply = await _request(ServiceType.register, RegisterCid.write, payload: payload);
    // A failure replies with a single 0xFF status; a success echoes the request payload
    // (BlockInfo + ValueInfo + value). The echoed BlockInfo starts with the key byte, so it
    // is not a reliable success flag (a successful key-0 write echoes 0x00 too).
    return reply != null && reply.length >= 8;
  }
}
