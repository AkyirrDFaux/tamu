/// Storage service client (Docs/Services/Storage.md): file table access and
/// file create/delete/resize/rename/read/write.
library;

import 'dart:typed_data';

import 'connection.dart';
import 'device_db.dart';
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

  /// True for fixed (USE_FIXED_STORAGE) devices whose file table is a const array served
  /// at offset 0 (the DAS): there, offset-0 records are the real files. On the full file
  /// system offset 0 marks a superseded/invalidated record (renames/table moves zero the
  /// 4-byte offset but leave the size), so such records are skipped there.
  ///
  /// [fixedStorage] overrides the capability probe (tests); otherwise it is derived from the
  /// device's capabilities.
  final bool fixedStorage;

  StorageClient({required this.deviceId, bool? fixedStorage})
      : fixedStorage = fixedStorage ?? _detectFixedStorage(deviceId);

  static bool _detectFixedStorage(int deviceId) {
    final dev = DeviceDatabase.instance.byId(deviceId);
    // An unclassified device (no entry, or capabilities not read yet) must not be assumed
    // fixed: doing so lists the full FS's invalidated (offset-0) records as real files. The
    // reduced FS is the DAS, which reports its capabilities; the core is the device the app
    // normally connects to first.
    if (dev == null) return false;
    return isFixedStorage(dev.capabilities);
  }

  /// The fixed-vs-full decision from a capability word: a device that reports capabilities
  /// but not `StorageFiles` is on the reduced (fixed) file system. A zero word (capabilities
  /// not read yet) is treated as full, never presumed fixed.
  static bool isFixedStorage(int capabilities) =>
      capabilities != 0 && (capabilities & Capability.storageFiles) == 0;

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

  static String unpadName(List<int> bytes) =>
      normalizeFileName(String.fromCharCodes(bytes.take(nameLength)));

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
    final contents = reply.sublist(nameLength);
    return parseFileTable(contents, fixedStorage: fixedStorage);
  }

  /// Parses 16-byte file-table records. Unwritten entries are all 0xFF and terminate the
  /// table. A record with offset 0 is invalidated on the full file system (the device zeroes
  /// the 4-byte offset on rename/delete/table moves, leaving the size), but is a real fixed
  /// file on the reduced storage.
  static List<FileRecord> parseFileTable(List<int> contents, {required bool fixedStorage}) {
    final records = <FileRecord>[];
    for (var offset = 0; offset + 16 <= contents.length; offset += 16) {
      final recOffset = uint32FromBytes(contents, offset);
      final size = uint32FromBytes(contents, offset + 4);
      if (recOffset == 0xFFFFFFFF && size == 0xFFFFFFFF) break;
      if (!fixedStorage && recOffset == 0) continue; // full FS: offset 0 = invalidated
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

  /// Resizes a file per Docs 03.03 CID3 (full file system only; the reduced FS
  /// has fixed positions and does not answer this command).
  Future<bool> resizeFile(String name, int size) async {
    final payload = <int>[...padName(name), ...uint32ToBytes(size)];
    final reply = await _request(3, payload: payload);
    return reply != null && reply.isNotEmpty && reply[0] != 0;
  }

  /// Formats the whole filesystem per docs 03.00 CID0 (wipes every file).
  Future<bool> format() async {
    final reply = await _request(0);
    return reply != null;
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
  ///
  /// On the full file system the new content is staged under a temporary name first
  /// and only swapped in after a successful write, so a failed create/write cannot
  /// destroy the original. The reduced (fixed) file system has no create/delete
  /// (CIDs 1/2 are compiled out), so the CID-6 fragment loop is used directly.
  Future<bool> writeFile(String name, List<int> bytes) async {
    if (fixedStorage) {
      return _writeFragments(name, bytes);
    }
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

  /// The CID-6 fragment stream (first fragment carries the file name).
  Future<bool> _writeFragments(String name, List<int> bytes) async {
    const dataMax = 64;
    var next = 0;
    var offset = 0;
    while (offset < bytes.length) {
      final isFirst = next == 0;
      final end =
          (offset + dataMax > bytes.length) ? bytes.length : offset + dataMax;
      final payload = <int>[
        ...writeFragInfo(next, 0xFFFF),
        if (isFirst) ...padName(name),
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
