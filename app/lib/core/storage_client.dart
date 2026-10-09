/// Storage service client (Docs/Services/Storage.md): file table access and
/// file create/delete/resize/rename/read/write.
library;

import 'dart:typed_data';

import 'connection.dart';
import 'diagnostics.dart';
import 'protocol.dart';
import 'types.dart';

/// The single file-name normalizer: strips only the 8-byte wire padding (trailing
/// spaces or, from older renames, NULs), never interior characters, so a stored name
/// such as `A B` survives intact.
String normalizeFileName(String name) {
  var end = name.length;
  while (end > 0) {
    final c = name.codeUnitAt(end - 1);
    if (c == 0x20 || c == 0x00) {
      end--;
    } else {
      break;
    }
  }
  return name.substring(0, end);
}

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

  /// The firmware answers status commands with the packet success/fail flags instead of a
  /// payload, so the outcome is in the frame rather than the bytes.
  Future<bool> _status(
    int cid, {
    List<int> payload = const [],
    Duration? timeout,
    bool requestFrag = false,
  }) async {
    try {
      final response = await _link.requestWithFlags(
        deviceId,
        ServiceType.storage,
        cid,
        payload: payload,
        timeout: timeout ?? const Duration(seconds: 6),
        requestFrag: requestFrag,
      );
      return response.success;
    } catch (error) {
      AppDiagnostics.log('storage', 'request failed: $error');
      return false;
    }
  }

  static const nameLength = 8;

  /// The documented "maximum 64 byte stream fragment" content cap (Docs/Services/Storage.md).
  static const fragmentContentMax = 64;

  /// Pads/truncates a file name to the wire format (8 bytes, space padded).
  static Uint8List padName(String name) {
    final bytes = Uint8List(nameLength)..fillRange(0, nameLength, 0x20);
    final raw = name.codeUnits;
    for (var i = 0; i < nameLength && i < raw.length; i++) {
      bytes[i] = raw[i] & 0xFF;
    }
    return bytes;
  }

  static String unpadName(List<int> bytes) =>
      normalizeFileName(String.fromCharCodes(bytes.take(nameLength)));

  /// Strips the per-fragment name echo from a reassembled CID-5 stream (B12). Every fragment
  /// is [name (8)][content, at most fragmentContentMax] and the reassembly layer already
  /// removed the 4-byte frag info, so the groups are walked from the front (each fragment but
  /// the last is full).
  static List<int> stripFragmentNames(List<int> reply) {
    final out = <int>[];
    var pos = 0;
    while (pos + nameLength <= reply.length) {
      pos += nameLength; // drop this fragment's name echo
      final end = (pos + fragmentContentMax > reply.length)
          ? reply.length
          : pos + fragmentContentMax;
      out.addAll(reply.sublist(pos, end));
      pos = end;
    }
    return out;
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
    // Each fragment = [name echo (8)][contents...] (B12).
    final contents = stripFragmentNames(reply);
    return parseFileTable(contents);
  }

  /// Parses 16-byte file-table records. Unwritten entries are all 0xFF and terminate the
  /// table. A record with offset 0 is a superseded/invalidated entry (rename/delete/table
  /// moves zero the 4-byte offset but leave the size), so it is skipped - on every device,
  /// since all targets run the full multi-file filesystem now.
  static List<FileRecord> parseFileTable(List<int> contents) {
    final records = <FileRecord>[];
    for (var offset = 0; offset + 16 <= contents.length; offset += 16) {
      final recOffset = uint32FromBytes(contents, offset);
      final size = uint32FromBytes(contents, offset + 4);
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
    return _status(1, payload: payload);
  }

  Future<bool> deleteFile(String name) async {
    return _status(2, payload: padName(name));
  }

  Future<bool> renameFile(String oldName, String newName) async {
    final payload = <int>[...padName(oldName), ...padName(newName)];
    return _status(4, payload: payload);
  }

  /// Resizes a file per Docs 03.03 CID3.
  Future<bool> resizeFile(String name, int size) async {
    final payload = <int>[...padName(name), ...uint32ToBytes(size)];
    return _status(3, payload: payload);
  }

  /// Formats the whole filesystem per docs 03.00 CID0 (wipes every file).
  Future<bool> format() async {
    return _status(0);
  }

  /// Reads the whole file per docs 03.05 CID5
  Future<List<int>?> readFile(String name, {int? size}) async {
    final reply = await _request(
      5,
      payload: padName(name),
      timeout: const Duration(seconds: 10),
    );
    if (reply == null || reply.length < nameLength) return null;
    // Every fragment = [name echo (8)][contents...] (B12); the reassembly layer already
    // stripped the 4-byte frag info from each.
    var contents = stripFragmentNames(reply);
    if (size != null && contents.length > size) {
      contents = contents.sublist(0, size);
    }
    return contents;
  }

  /// Writes a whole file per docs 03.06 CID6. Stream fragments are capped at 64
  /// content bytes (docs: "maximum 64 byte stream fragment"); every fragment carries
  /// the 4-byte frag info so the device can detect out-of-order delivery.
  ///
  /// The new content is staged under a temporary name first and only swapped in after a
  /// successful write, so a failed create/write cannot destroy the original.
  Future<bool> writeFile(String name, List<int> bytes) async {
    final temp = _tempName(name);
    await deleteFile(temp); // clear a stale temp from a previous failed write
    if (!await createFile(temp, bytes.length)) return false;
    if (!await _writeFragments(temp, bytes)) {
      await deleteFile(temp);
      return false;
    }
    // Replace only once the new content is safely staged.
    await deleteFile(name);
    if (!await renameFile(temp, name)) {
      await deleteFile(temp);
      return false;
    }
    return true;
  }

  /// The CID-6 fragment stream; every fragment carries the file name (B12).
  Future<bool> _writeFragments(String name, List<int> bytes) async {
    const dataMax = fragmentContentMax;
    var next = 0;
    var offset = 0;
    while (offset < bytes.length) {
      final end =
          (offset + dataMax > bytes.length) ? bytes.length : offset + dataMax;
      final payload = <int>[
        ...writeFragInfo(next, 0xFFFF),
        ...padName(name), // every fragment carries the name
        ...bytes.sublist(offset, end),
      ];
      final reply = await _request(6, payload: payload, requestFrag: true);
      if (reply == null || reply.length < 2) return false;
      final lastSeq = reply[0] | (reply[1] << 8);
      if (lastSeq == 0xFFFF) return false; // device reported no writable target
      next = lastSeq + 1;
      offset = end;
    }
    return true;
  }

  /// A staging name that fits the 8-byte wire field and is unlikely to collide.
  static String _tempName(String name) {
    final base = name.length <= 7 ? name : name.substring(0, 7);
    return '~$base';
  }
}
