/// Storage service client (Docs/Services/Storage.md): file table access and
/// file create/delete/resize/rename/read.
library;

import 'dart:typed_data';

import 'connection.dart';
import 'diagnostics.dart';
import 'protocol.dart';
import 'types.dart';

/// One file table entry (Filerecord: Offset u32 | Filesize u32 | Name 8 bytes).
class FileRecord {
  final int index;
  final int offset;
  final int size;
  final String name;

  FileRecord(
      {required this.index,
      required this.offset,
      required this.size,
      required this.name});

  bool get isFiletable => index == 0;
}

class StorageClient {
  final int deviceId;

  StorageClient({required this.deviceId});

  ConnectionManager get _link => ConnectionManager.instance;

  Future<List<int>?> _request(int cid, {List<int> payload = const []}) async {
    try {
      // Mutating ops (create/resize/delete) trigger flash erases on slow nodes
      // and can legitimately exceed the default transaction timeout.
      return await _link.request(deviceId, ServiceType.storage, cid,
          payload: payload, timeout: const Duration(seconds: 6));
    } catch (error) {
      AppDiagnostics.log('storage', 'request failed: $error');
      return null;
    }
  }

  static const nameLength = 8;

  /// Pads/truncates a file name to the wire format (8 bytes, space padded).
  static Uint8List padName(String name) {
    final bytes = Uint8List(nameLength)..fillRange(0, nameLength, 0x20);
    final raw = name.codeUnits;
    for (var i = 0; i < nameLength && i < raw.length; i++) {
      bytes[i] = raw[i] & 0xFF;
    }
    return bytes;
  }

  static String unpadName(List<int> bytes) {
    final text = String.fromCharCodes(bytes.take(nameLength));
    return text.replaceAll(' ', '').trim();
  }

  /// Reads the whole file table (CID 0, streamed Filerecords of 16 bytes each).
  Future<List<FileRecord>?> readFileTable() async {
    final reply = await _request(0, payload: []);
    if (reply == null) return null;
    final records = <FileRecord>[];
    for (var offset = 0; offset + 16 <= reply.length; offset += 16) {
      final recOffset = uint32FromBytes(reply, offset);
      final size = uint32FromBytes(reply, offset + 4);
      // Unwritten entries are all 0xFF; invalidated ones have offset 0.
      if (recOffset == 0xFFFFFFFF && size == 0xFFFFFFFF) break;
      if (recOffset == 0) continue; // invalidated record
      records.add(FileRecord(
          index: records.length,
          offset: recOffset,
          size: size,
          name: unpadName(reply.sublist(offset + 8))));
    }
    return records;
  }

  /// Creates a file (CID 2). Returns success flag from the device.
  Future<bool> createFile(String name, int size) async {
    final payload = <int>[...padName(name), ...uint32ToBytes(size)];
    final reply = await _request(2, payload: payload);
    return reply != null && reply.isNotEmpty && reply[0] != 0;
  }

  Future<bool> deleteFile(String name) async {
    final reply = await _request(3, payload: padName(name));
    // Delete replies carry an EMPTY payload on success (unlike create/resize,
    // which return a status byte); null still means no answer at all.
    return reply != null;
  }

  Future<bool> renameFile(String oldName, String newName) async {
    final payload = <int>[...padName(oldName), ...padName(newName)];
    final reply = await _request(5, payload: payload);
    return reply != null && reply.isNotEmpty && reply[0] != 0;
  }

  /// Reads up to `maxBytes` of a file starting at `offset` (CID 6).
  Future<List<int>?> readFile(String name,
      {int offset = 0, int maxBytes = 4096}) async {
    final payload = <int>[
      ...padName(name),
      ...uint32ToBytes(offset),
      ...uint32ToBytes(maxBytes),
    ];
    return await _request(6, payload: payload);
  }
}
