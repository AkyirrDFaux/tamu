/// Script service client (Docs/Services/Script.md, management commands 0x050X) plus the
/// Register access to a loaded script's block (the Scripts range 0x3F4-0x3F7).
library;

import 'dart:typed_data';

import 'connection.dart';
import 'diagnostics.dart';
import 'protocol.dart';
import 'register_client.dart';
import 'script_file.dart';
import 'types.dart';

/// A loaded-script register entry (ValueInfo + value).
class ScriptEntry {
  final ValueInfo meta;
  final List<int> value;

  const ScriptEntry({required this.meta, required this.value});
}

/// CID 4 action enum (Docs/Services/Script.md management command 4: "Script ID, action ...
/// The action enum drives start, stop, pause and resume; clears the error").
///
/// The members correspond to the Service state-op instructions (Start 9 .. Resume 12) and to
/// the app's Start / Pause / Continue / Stop buttons, so the instruction set and the action
/// enum stay aligned. There is no Error action: the Service `Error` instruction (opcode 13,
/// inside a script) is not something the app commands over CID 4.
class ScriptAction {
  static const start = 9;
  static const stop = 10;
  static const pause = 11;
  static const resume = 12;

  static String label(int action) => switch (action) {
        start => 'Start',
        stop => 'Stop',
        pause => 'Pause',
        resume => 'Resume',
        _ => 'Action $action',
      };
}

/// A CID 3 report record (Docs/Services/Script.md "Report Record"): `State (u8) | Depth (u8) |
/// Line (u16)` = 4 bytes, with `Instruction (u32) | Error (u32)` appended only on an error =
/// 12 bytes. The depth byte pads the record so `line` sits on a 2-byte boundary and the error's
/// two 32-bit fields land on 4-byte boundaries (line at offset 2, instruction at 4, error at 8).
class ScriptReport {
  /// The script state ([ScriptState]).
  final int state;

  /// The call depth (the number of active frames).
  final int depth;

  /// The instruction counter - the deepest frame's position, where execution actually is.
  final int line;

  /// The offending instruction's own encoding, `1 + ((category << 8) | opcode)`, or 0 when the
  /// record names no instruction (an engine-level fault, or no error).
  final int instruction;

  /// The instruction-specific error code, or 0 for no error. Its meaning is defined by the
  /// instruction named by [instruction].
  final int error;

  const ScriptReport({
    required this.state,
    required this.depth,
    required this.line,
    this.instruction = 0,
    this.error = 0,
  });

  /// True when the record names an error (it carried the instruction/error fields).
  bool get hasError => instruction != 0 || error != 0;

  /// Decodes the [instruction] encoding `1 + ((category << 8) | opcode)` back into
  /// `(category, opcode)`, so the UI can look the instruction up. Returns null when the encoding
  /// is 0 - a fault not attributable to an instruction (engine-level), or no fault at all - so
  /// the caller shows the raw error code alone. This maps the encoding only; it does not name
  /// error codes (that catalogue is undecided - keep the raw code).
  static (int category, int opcode)? decodeInstruction(int instruction) {
    if (instruction == 0) return null;
    final encoded = instruction - 1;
    return (encoded >> 8, encoded & 0xFF);
  }
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

  /// True when a management command replies FLAG_SUCCESS. These commands carry no status
  /// byte; the device sets the packet SUCCESS/FAIL flags instead. A transport failure or a
  /// FLAG_FAIL reply is false, via [PacketResponse.ok].
  Future<bool> _requestOk(
    ServiceType service,
    int cid, {
    List<int> payload = const [],
    Duration? timeout,
  }) async {
    try {
      final response = await _link.requestWithFlags(deviceId, service, cid,
          payload: payload, timeout: timeout ?? const Duration(seconds: 3));
      return response.ok; // success && !fail, matching the subscriptions client
    } catch (error) {
      AppDiagnostics.log('script', 'request failed: $error');
      return false;
    }
  }

  // ---- Management commands (0x0500-0x0507) ----

  /// CID 0: the file ids of the currently loaded scripts (uint16 each). The payload
  /// length carries the count - there is no count byte (Docs/Services/Script.md CID 0).
  Future<List<int>> loadedScripts() async {
    final reply = await _request(ServiceType.script, 0);
    if (reply == null || reply.isEmpty) return const [];
    final count = reply.length ~/ 2;
    final fileIds = <int>[];
    for (var i = 0; i < count; i++) {
      fileIds.add(reply[2 * i] | (reply[2 * i + 1] << 8));
    }
    return fileIds;
  }

  /// CID 1: loads `SCR_<fileId>` (uint16) into slot `loadedId` (uint8). The device answers
  /// Success - the caller picked the slot, so it already knows it. The file id and the slot are
  /// independent: many files (SCR_XXX) exist, at most [maxScripts] load at once.
  Future<bool> load(int fileId, int loadedId) async {
    return _requestOk(ServiceType.script, 1,
        payload: [fileId & 0xFF, (fileId >> 8) & 0xFF, loadedId & 0xFF]);
  }

  /// CID 2: unloads a loaded script.
  Future<bool> unload(int loadedId) async {
    return _requestOk(ServiceType.script, 2, payload: [loadedId & 0xFF]);
  }

  /// CID 3: a loaded script's report record (Docs "Report Record"). 4 bytes normally -
  /// state, call depth and the deepest frame's line - or 12 bytes on an error, which appends
  /// the offending instruction and the instruction-specific error code. Returns null when the
  /// device does not answer (fewer than the 4-byte base record).
  Future<ScriptReport?> readState(int loadedId) async {
    final reply = await _request(ServiceType.script, 3, payload: [loadedId & 0xFF]);
    if (reply == null || reply.length < 4) return null;
    final onError = reply.length >= 12;
    return ScriptReport(
      state: reply[0],
      depth: reply[1],
      line: reply[2] | (reply[3] << 8),
      instruction: onError ? uint32FromBytes(reply, 4) : 0,
      error: onError ? uint32FromBytes(reply, 8) : 0,
    );
  }

  /// CID 4: sets a loaded script's state. The wire carries the action enum (Docs
  /// CID 4: "Script ID, action ... drives start, stop, pause and resume; clears the error"),
  /// so this convenience maps a target [ScriptState] to its matching action.
  Future<bool> setState(int loadedId, int state) async {
    final action = switch (state) {
      ScriptState.stopped => ScriptAction.stop,
      ScriptState.running => ScriptAction.start,
      ScriptState.paused => ScriptAction.pause,
      _ => null,
    };
    if (action == null) return false;
    return setAction(loadedId, action);
  }

  /// CID 4: applies a [ScriptAction] to a loaded script (Docs CID 4: "Script ID, action ...
  /// drives start, stop, pause and resume; clears the error").
  Future<bool> setAction(int loadedId, int action) async {
    return _requestOk(ServiceType.script, 4,
        payload: [loadedId & 0xFF, action & 0xFF]);
  }

  /// CID 5: the variable RAM and the entire instruction-counter call stack (editor debug),
  /// replacing the single instruction counter (which moved to CID 3's report record). Streamed
  /// as `Depth (u8) | variable RAM | Depth x Line (u16)`: the leading depth byte mirrors the
  /// report record and delimits the two variable-length parts, so both decode from the reply
  /// alone (Docs: "Variable RAM, instruction-counter stack (stream)").
  ///
  /// Returns null when the reply is too short to hold the depth byte and its stack entries.
  Future<({List<int> callStack, List<int> variables})?> readInternalState(
      int loadedId) async {
    final reply = await _request(ServiceType.script, 5, payload: [loadedId & 0xFF]);
    if (reply == null || reply.isEmpty) return null;
    final depth = reply[0];
    final stackBytes = 2 * depth;
    if (reply.length < 1 + stackBytes) return null;
    // The call stack (one u16 per frame, outermost first) precedes the variable RAM.
    final callStack = <int>[];
    for (var i = 0; i < depth; i++) {
      final off = 1 + 2 * i;
      callStack.add(reply[off] | (reply[off + 1] << 8));
    }
    final variables = reply.sublist(1 + stackBytes);
    return (callStack: callStack, variables: variables);
  }

  /// CID 6: moves the instruction counter (editor debug).
  Future<bool> moveToInstruction(int loadedId, int instruction) async {
    final payload = <int>[loadedId & 0xFF, ...uint32ToBytes(instruction)];
    return _requestOk(ServiceType.script, 6, payload: payload);
  }

  /// CID 7: writes a variable (editor debug).
  Future<bool> writeVariable(int loadedId, int variableId, List<int> value) async {
    final payload = <int>[loadedId & 0xFF, variableId & 0xFF, ...value];
    return _requestOk(ServiceType.script, 7, payload: payload);
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
