/// Shared wire scaffolding for the System/Dynamic/Keyed memory clients.
///
/// The three services speak the same request/reply shapes - BlockIndex echo,
/// BlockMeta + name/value replies, single status-byte ops - so the plumbing
/// lives here once instead of being copied per client.
library;

import 'connection.dart';
import 'diagnostics.dart';
import 'protocol.dart';
import 'types.dart';

abstract class MemoryClientBase {
  final int deviceId;

  MemoryClientBase({required this.deviceId});

  ConnectionManager get _link => ConnectionManager.instance;

  /// Service to address (systemMemory / dynamicMemory / keyedMemory).
  ServiceType get service;

  /// Diagnostic tag for this memory flavour.
  String get logTag;

  Duration get _requestTimeout => const Duration(seconds: 2);

  /// Sends one request and returns the reply payload (or null on failure).
  /// Subclasses use this for the ops that don't share a common shape.
  Future<List<int>?> request(int cid,
      {List<int> payload = const [], Duration? timeout}) async {
    try {
      return await _link.request(deviceId, service, cid,
          payload: payload, timeout: timeout ?? _requestTimeout);
    } catch (error) {
      AppDiagnostics.log(logTag, 'request failed: $error');
      return null;
    }
  }

  /// Block-list summary (CID 2, invalid index): BlockIndex echo + 1-byte count.
  Future<int?> readBlockCount() async {
    final summary = await request(2, payload: const BlockIndex().toBytes());
    if (summary == null || summary.length < 5) return null;
    return summary[4];
  }

  /// Block-level meta + name read (CID 2, block index only). Null on failure.
  Future<({BlockMeta meta, String name})?> readBlockMetaPayload(
      int block) async {
    final reply = await request(2, payload: BlockIndex(block: block).toBytes());
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    // BlockMeta.Size here is the field map count, NOT the name length; the name
    // fills the rest of the payload. Strip the 4-byte wire padding (NUL bytes).
    final name = reply.length > 8
        ? String.fromCharCodes(reply.sublist(8)).replaceAll('\x00', '')
        : 'Block $block';
    return (meta: meta, name: name);
  }

  /// Field/dict/entry value read (CID 2). Returns BlockMeta + value bytes.
  Future<({BlockMeta meta, List<int> value})?> readValue(BlockIndex index) async {
    final reply = await request(2, payload: index.toBytes());
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    return (meta: meta, value: _valueSlice(reply, meta.size));
  }

  /// Value write (CID 3): BlockIndex + BlockMeta + value. Returns the confirmed
  /// value bytes (empty on a valid empty-value write) or null on failure.
  Future<List<int>?> writeValue(BlockIndex index, BlockMeta meta,
      List<int> value, {Duration? timeout}) async {
    final reply = await request(3,
        payload: [...index.toBytes(), ...meta.toBytes(), ...value],
        timeout: timeout);
    if (reply == null || reply.length < 8) return null;
    final echoMeta = BlockMeta.fromBytes(reply, 4);
    return _valueSlice(reply, echoMeta.size);
  }

  /// Backup value read (CID 4): the value as STORED in the device's backup file
  /// (not the live value). Reply is BlockIndex + BlockMeta + value, like CID 2.
  Future<({BlockMeta meta, List<int> value})?> readBackupValue(
      BlockIndex index) async {
    final reply = await request(4, payload: index.toBytes());
    if (reply == null || reply.length < 8) return null;
    final meta = BlockMeta.fromBytes(reply, 4);
    return (meta: meta, value: _valueSlice(reply, meta.size));
  }

  /// Slices the value bytes after the [BlockIndex + BlockMeta] header, clamped to the
  /// declared [size] (the wire payload is padded to 4 bytes).
  List<int> _valueSlice(List<int> reply, int size) {
    if (size <= 0) return <int>[];
    final avail = reply.length - 8;
    return reply.sublist(8, 8 + ((size > avail) ? avail : size));
  }

  /// Save (CID 5) / Recall (CID 6) with an invalid block = whole registry.
  /// The device answers with a single status byte: 0 on success, 0xFF on failure.
  Future<bool> memoryOp(int cid, int? block) async {
    final reply = await request(cid,
        payload: BlockIndex(block: block ?? invalidBlock).toBytes());
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }
}