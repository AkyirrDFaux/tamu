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

  FileRecord(
      {required this.index,
      required this.offset,
      required this.size,
      required this.name});

  bool get isFiletable => index == 0;
}

/// Contents chunk size per stream fragment (the max actual payload of a FRAG
/// packet, Data Formats.md).
const int fileFragContentSize = 256;

class StorageClient {
  final int deviceId;

  StorageClient({required this.deviceId});

  ConnectionManager get _link => ConnectionManager.instance;

  Future<List<int>?> _request(int cid,
      {List<int> payload = const [], Duration? timeout, bool frag = false}) async {
    try {
      // Mutating ops (create/resize/delete) trigger flash erases on slow nodes
      // and can legitimately exceed the default transaction timeout.
      return await _link.request(deviceId, ServiceType.storage, cid,
          payload: payload,
          timeout: timeout ?? const Duration(seconds: 6),
          frag: frag);
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

  /// Reads the whole file table (CID 0, FRAG-streamed Filerecords of 16 bytes each).
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

  /// Reads the whole file (CID 6, FRAG stream). The reply carries the echoed
  /// name followed by the file contents; the last fragment's 4-byte wire padding
  /// is trimmed using [size] (the file size, e.g. from the file table). Pass
  /// [size] to get exact contents.
  Future<List<int>?> readFile(String name, {int? size}) async {
    final reply = await _request(6, payload: padName(name),
        timeout: const Duration(seconds: 10));
    if (reply == null || reply.length < nameLength) return null;
    // The response stream = [name echo (8)][contents...] (the reassembly layer
    // already stripped the fragmentation info from every fragment).
    var contents = reply.sublist(nameLength);
    if (size != null && contents.length > size) {
      contents = contents.sublist(0, size);
    }
    return contents;
  }

  /// Writes a whole file (CID 7, FRAG stream). Deletes any existing file, creates it
  /// with the exact size (the device clamps fragment writes to it), then streams the
  /// contents in 256-byte fragments, each acknowledged with the last sequential
  /// fragment index written. Returns true when every fragment was written.
  Future<bool> writeFile(String name, List<int> bytes) async {
    await deleteFile(name); // overwrite semantics
    if (!await createFile(name, bytes.length)) return false;
    final totalFrags = (bytes.length + fileFragContentSize - 1) ~/ fileFragContentSize;
    if (totalFrags == 0) return true; // empty file: nothing to stream
    var next = 0;
    while (next < totalFrags) {
      final start = next * fileFragContentSize;
      final end = (start + fileFragContentSize > bytes.length)
          ? bytes.length
          : start + fileFragContentSize;
      final payload = <int>[
        ...writeFragInfo(next, totalFrags),
        if (next == 0) ...padName(name),
        ...bytes.sublist(start, end),
      ];
      final reply = await _request(7, payload: payload, frag: true);
      if (reply == null || reply.length < 2) return false;
      final lastSeq = reply[0] | (reply[1] << 8);
      if (lastSeq == 0xFFFF) return false; // device reported no writable target
      next = lastSeq + 1;
    }
    return true;
  }
}