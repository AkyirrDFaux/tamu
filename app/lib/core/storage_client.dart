/// Storage service client (Docs/Services/Storage.md): file table access and
/// file create/delete/resize/rename/read/write.
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

  FileRecord({
    required this.index,
    required this.offset,
    required this.size,
    required this.name,
  });

  bool get isFiletable => index == 0;
}

/// Contents chunk size per stream fragment (the max actual payload of a FRAG
/// packet, Data Formats.md).
const int fileFragContentSize = 112;

class StorageClient {
  final int deviceId;

  StorageClient({required this.deviceId});

  ConnectionManager get _link => ConnectionManager.instance;

  Future<List<int>?> _request(
    int cid, {
    List<int> payload = const [],
    Duration? timeout,
    bool requestFrag = false,
  }) async {
    try {
      // Mutating ops (create/resize/delete) trigger flash erases on slow nodes
      // and can legitimately exceed the default transaction timeout.
      return await _link.request(
        deviceId,
        ServiceType.storage,
        cid,
        payload: payload,
        timeout: timeout ?? const Duration(seconds: 6),
        requestFrag: requestFrag,
      );
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

  /// Reads the file table by reading the ".TABLE  " file directly (CID 5).
  /// The file table is self-describing: the first entry points to itself with its size.
  Future<List<FileRecord>?> readFileTable() async {
    // Read the file table file directly using CID 5 (read file)
    final tableName = '.TABLE  ';
    final reply = await _request(
      5,
      payload: padName(tableName),
      timeout: const Duration(seconds: 10),
    );
    if (reply == null || reply.length < nameLength) return null;
    // The response stream = [name echo (8)][contents...]
    var contents = reply.sublist(nameLength);
    
    final records = <FileRecord>[];
    for (var offset = 0; offset + 16 <= contents.length; offset += 16) {
      final recOffset = uint32FromBytes(contents, offset);
      final size = uint32FromBytes(contents, offset + 4);
      // Unwritten entries are all 0xFF; invalidated ones have offset 0.
      if (recOffset == 0xFFFFFFFF && size == 0xFFFFFFFF) break;
      if (recOffset == 0) continue; // invalidated record
      records.add(
        FileRecord(
          index: records.length,
          offset: recOffset,
          size: size,
          name: unpadName(contents.sublist(offset + 8)),
        ),
      );
    }
    return records;
  }

  /// Creates a file per docs 03.01 CID1
  Future<bool> createFile(String name, int size) async {
    final payload = <int>[...padName(name), ...uint32ToBytes(size)];
    final reply = await _request(1, payload: payload);
    return reply != null && reply.isNotEmpty && reply[0] != 0;
  }

  Future<bool> deleteFile(String name) async {
    final reply = await _request(2, payload: padName(name));
    return reply != null && reply.isNotEmpty && reply[0] != 0;
  }

  Future<bool> renameFile(String oldName, String newName) async {
    final payload = <int>[...padName(oldName), ...padName(newName)];
    final reply = await _request(4, payload: payload);
    return reply != null && reply.isNotEmpty && reply[0] != 0;
  }

  /// Reads the whole file per docs 03.05 CID5
  Future<List<int>?> readFile(String name, {int? size}) async {
    final reply = await _request(
      5,
      payload: padName(name),
      timeout: const Duration(seconds: 10),
    );
    if (reply == null || reply.length < nameLength) return null;
    // The response stream = [name echo (8)][contents...] (the reassembly layer
    // already stripped the fragmentation info from every fragment).
    var contents = reply.sublist(nameLength);
    if (size != null && contents.length > size) {
      contents = contents.sublist(0, size);
    }
    return contents;
  }

  /// Writes a whole file per docs 03.06 CID6. Stream fragments are capped at 64
  /// content bytes (docs: "maximum 64 byte stream fragment"); every fragment carries
  /// the 4-byte frag info so the device can detect out-of-order delivery.
  Future<bool> writeFile(String name, List<int> bytes) async {
    await deleteFile(name);
    if (!await createFile(name, bytes.length)) return false;
    const dataMax = 64;
    var next = 0;
    var offset = 0;
    while (offset < bytes.length) {
      final isFirst = next == 0;
      final end = (offset + dataMax > bytes.length)
          ? bytes.length
          : offset + dataMax;
      final payload = <int>[...writeFragInfo(next, 0xFFFF), if (isFirst) ...padName(name), ...bytes.sublist(offset, end)];
      final reply = await _request(6, payload: payload, requestFrag: true);
      if (reply == null || reply.length < 2) return false;
      final lastSeq = reply[0] | (reply[1] << 8);
      if (lastSeq == 0xFFFF) return false; // device reported no writable target
      next = lastSeq + 1;
      offset = end;
    }
    return true;
  }
}
