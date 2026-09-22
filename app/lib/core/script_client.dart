/// Script service client (Docs/Services/Script.md, management commands 0x050X) plus the
/// Register access to a loaded script's block (type 0x3FE).
library;

import 'dart:typed_data';

import 'connection.dart';
import 'diagnostics.dart';
import 'protocol.dart';
import 'register_client.dart';
import 'types.dart';

/// Block type of a loaded script (Docs/Services/Register.md: "Scripts 0x3FE").
const int scriptBlockType = 0x3FE;

/// A loaded-script register entry (ValueInfo + value).
class ScriptEntry {
  final BlockMeta meta;
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

  /// CID 0: the slots of the currently loaded scripts.
  Future<List<int>> loadedScripts() async {
    final reply = await _request(ServiceType.script, 0);
    if (reply == null || reply.isEmpty) return const [];
    final count = reply[0];
    final slots = <int>[];
    for (var i = 0; i < count && 1 + i < reply.length; i++) {
      slots.add(reply[1 + i]);
    }
    return slots;
  }

  /// CID 1: loads `SCR_<fileId>` and returns its loaded ID (slot), or null on failure.
  Future<int?> load(int fileId) async {
    final reply = await _request(ServiceType.script, 1, payload: [fileId & 0xFF]);
    if (reply == null || reply.isEmpty || reply[0] == 0xFF) return null;
    return reply[0];
  }

  /// CID 2: unloads a loaded script.
  Future<bool> unload(int loadedId) async {
    final reply = await _request(ServiceType.script, 2, payload: [loadedId & 0xFF]);
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  /// CID 3: reads a loaded script's state.
  Future<int?> readState(int loadedId) async {
    final reply = await _request(ServiceType.script, 3, payload: [loadedId & 0xFF]);
    if (reply == null || reply.isEmpty) return null;
    return reply[0];
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

  // ---- Register access (block type 0x3FE) ----

  static Uint8List _bi(int inst, int field, int key) => blockInfoBytes(scriptBlockType, inst, field, key);

  /// Registers enum level 1: the loaded slots (same list as [loadedScripts]).
  Future<List<int>> enumerateInstances() async {
    final reply = await _request(
      ServiceType.register,
      0,
      payload: [1, ..._bi(0, 0, 0)],
    );
    if (reply == null || reply.length < 5) return const [];
    final count = reply[4];
    final slots = <int>[];
    for (var i = 0; i < count && 5 + i < reply.length; i++) {
      slots.add(reply[5 + i]);
    }
    return slots;
  }

  /// Registers enum level 3: the keys (entity indexes) of a script field.
  Future<List<int>> enumerateKeys(int inst, int field) async {
    final reply = await _request(
      ServiceType.register,
      0,
      payload: [3, ..._bi(inst, field, 0)],
    );
    if (reply == null || reply.length < 5) return const [];
    final count = reply[4];
    final keys = <int>[];
    for (var i = 0; i < count && 5 + i < reply.length; i++) {
      keys.add(reply[5 + i]);
    }
    return keys;
  }

  /// Reads one Register entry (CID 1). Returns null when the entry does not exist.
  Future<ScriptEntry?> readEntry(int inst, int field, int key) async {
    final reply = await _request(
      ServiceType.register,
      1,
      payload: _bi(inst, field, key),
    );
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    return ScriptEntry(
        meta: meta, value: RegisterClient.valueSlice(reply, meta.size));
  }

  /// Reads the block meta of a loaded script (field 0xFF): snapshot name + field count.
  Future<({BlockMeta meta, String name})?> readBlockMeta(int inst) async {
    final reply = await _request(
      ServiceType.register,
      1,
      payload: _bi(inst, 0xFF, 0),
    );
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    final raw = reply.sublist(8).takeWhile((b) => b != 0).toList();
    final name = String.fromCharCodes(raw).trimRight();
    return (meta: meta, name: name);
  }

  /// Writes one Register entry (CID 2), e.g. a script input (field 1) or variable (3).
  Future<bool> writeEntry(int inst, int field, int key, BlockMeta meta, List<int> value) async {
    final payload = <int>[..._bi(inst, field, key), ...meta.toBytes(), ...value];
    final reply = await _request(ServiceType.register, 2, payload: payload);
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  /// CID 8: reads a loaded script's error code (0 = none).
  Future<int?> readError(int loadedId) async {
    final reply = await _request(ServiceType.script, 8, payload: [loadedId & 0xFF]);
    if (reply == null || reply.isEmpty) return null;
    return reply[0];
  }
}
