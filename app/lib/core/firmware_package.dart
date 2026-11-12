/// Firmware packages: `binaries/<target>-<version>.fw.zip` (firmware/tools/pack_firmware.py).
///
/// A package carries the image plus `manifest.json` (format 1): the target, the version, the
/// build commit and, per image, the address it belongs at and the digests of its bytes. The
/// Update page writes NOTHING from a file it has not checked against that manifest - the
/// package's own bytes are the only evidence, so they are read here instead of through a
/// streaming unzip (the entries are stored, so each payload's offset is checkable; this reader
/// mirrors `pack_firmware.py: read_stored_zip`).
///
/// The checks, each with its own sentence in [PackageRefused.problems]:
///
///  * the file is a stored `.fw.zip`, never a bare image;
///  * `format` is 1, the format this reader implements;
///  * `target` is the route's target, and the image's `address` is that target's address;
///  * `size` is the packaged payload length;
///  * `crc32` is the digest over the packaged image;
///  * `padded_size`/`crc32_padded` describe the image padded to whole 32-byte chunks with 0xFF.
///    `crc32_padded` is the value the device's digest command answers with (card t_5e16d932),
///    so it is what a flash compares against;
///  * no entry sits in the archive that the manifest does not list.
///
/// CRC-32/ISO-HDLC: reflected polynomial `0xEDB88320`, init and final XOR `0xFFFFFFFF`, the same
/// value zlib and the device compute - [Bootloader.crc32], never a second implementation.
library;

import 'dart:convert';
import 'dart:typed_data';

import 'bootloader.dart';

/// One build target: the address its image is loaded at, and the file name its manifest uses.
///
/// The four are `pack_firmware.py: TARGETS`. The two bootloader targets are listed so a package
/// naming one is refused with its real address rather than as an unknown target; no flash route
/// in this app writes them (the bootloader protocol's offsets are relative to the app space).
class FirmwareTarget {
  const FirmwareTarget(this.name, this.address, this.image);

  final String name;
  final int address;
  final String image;
}

const Map<String, FirmwareTarget> firmwareTargets = {
  'Tamu_v2_0A': FirmwareTarget('Tamu_v2_0A', 0x90000, 'Tamu_v2_0A.bin'),
  'Tamu_bootloader': FirmwareTarget(
    'Tamu_bootloader',
    0x10000,
    'Tamu_bootloader.bin',
  ),
  'DAS_v0_1': FirmwareTarget('DAS_v0_1', 0x08000800, 'DAS_v0_1.bin'),
  'DAS_bootloader': FirmwareTarget(
    'DAS_bootloader',
    0x08000000,
    'DAS_bootloader.bin',
  ),
};

/// The target the core's own bootloader writes: direct USB to the connected core.
const String coreTarget = 'Tamu_v2_0A';

/// The target the bootloader passthrough writes: a node on the core's bus is a DAS.
const String nodeTarget = 'DAS_v0_1';

/// The target the route the user chose writes - what the package's manifest has to name.
String routeTarget({required bool directToCore}) =>
    directToCore ? coreTarget : nodeTarget;

/// A package that was checked against its manifest and may be written.
///
/// [image] is the packaged payload AS PACKAGED: the bytes the manifest's digests describe, which
/// are also the bytes to flash.
class FirmwarePackage {
  const FirmwarePackage({
    required this.name,
    required this.target,
    required this.version,
    required this.buildCommit,
    required this.builtUtc,
    required this.image,
    required this.address,
    required this.crc32,
    required this.crc32Padded,
  });

  /// The file the user picked, for the page and for a refusal.
  final String name;

  final String target;
  final String version;
  final String buildCommit;
  final String builtUtc;

  final Uint8List image;

  /// The address the manifest gives the image: the target's own address.
  final int address;

  /// The digest over [image] as packaged.
  final int crc32;

  /// The digest over [image] padded to 32-byte chunks with 0xFF - the value the device's digest
  /// command returns for this image, and the one a flash compares against.
  final int crc32Padded;

  int get size => image.length;

  int get paddedSize => Bootloader.paddedLength(image.length);

  /// The package a flash route wants, named for a refusal: `binaries/<target>-<version>.fw.zip`.
  static String wantedName(String target) =>
      'binaries/$target-<version>.fw.zip';
}

/// A file this app will not write: [problems] holds one sentence per failed check, in the order
/// the checks ran, so a page can show them all instead of the first.
class PackageRefused implements Exception {
  const PackageRefused(this.name, this.problems);

  /// The file that was refused, as the user picked it.
  final String name;

  final List<String> problems;

  /// The refusal as one sentence per check, prefixed with the file and the outcome.
  String get text {
    final head = name.isEmpty ? 'That file' : name;
    return '$head was refused, nothing was written: ${problems.join(' ')}';
  }

  @override
  String toString() => text;
}

/// Reads [bytes] as a package and checks every manifest field against the packaged bytes.
///
/// [expectedTarget] is the route's target ([routeTarget]); [name] is the picked file's name, used
/// for the refusals (an empty name is allowed, and skips the bare-image check).
///
/// Throws [PackageRefused] with every failed check when the package may not be written. Nothing
/// is written by this function or by anything it returns to: the caller only ever flashes
/// [FirmwarePackage.image] after this returned a value.
FirmwarePackage verifyFirmwarePackage({
  required Uint8List bytes,
  required String expectedTarget,
  String name = '',
}) {
  final expect = firmwareTargets[expectedTarget];
  if (expect == null) {
    throw ArgumentError.value(
      expectedTarget,
      'expectedTarget',
      'unknown target',
    );
  }
  final label = name.isEmpty ? 'That file' : name;

  // A bare image is the exact mistake this page has to stop: the bytes may even be right, but
  // there is no manifest to check them against - so name the package that carries one.
  if (name.isNotEmpty && !name.toLowerCase().endsWith('.zip')) {
    throw PackageRefused(name, [
      '$label is a bare image; choose the package that carries it - '
          '${FirmwarePackage.wantedName(expectedTarget)}',
    ]);
  }

  final StoredZip zip;
  try {
    zip = readStoredZip(bytes);
  } on FormatException catch (error) {
    throw PackageRefused(name, [
      '$label is not a firmware package: ${error.message}',
    ]);
  }

  final manifestBytes = zip.entries['manifest.json'];
  if (manifestBytes == null) {
    throw PackageRefused(name, [
      '$label carries no manifest.json '
          '(entries: ${_entryNames(zip.order)})',
    ]);
  }

  final Object? decoded;
  try {
    decoded = jsonDecode(utf8.decode(manifestBytes));
  } on FormatException catch (error) {
    throw PackageRefused(name, [
      'manifest.json in $label is not valid JSON: ${error.message}',
    ]);
  }
  if (decoded is! Map) {
    throw PackageRefused(name, [
      'manifest.json in $label is not a JSON object',
    ]);
  }

  final problems = <String>[];

  // 1. The format this reader implements.
  if (decoded['format'] != 1) {
    problems.add(
      'manifest format is ${decoded['format']}, this app verifies 1',
    );
  }

  // 2. The target the route chose.
  final target = decoded['target'];
  if (target is! String || !firmwareTargets.containsKey(target)) {
    problems.add(
      'manifest target $target is not a target this app knows '
      '(${firmwareTargets.keys.join(', ')})',
    );
  } else if (target != expectedTarget) {
    problems.add(
      'the package is for $target, but this route writes $expectedTarget '
      'at ${_hex(expect.address)}',
    );
  }

  final version = decoded['version'];
  if (version is! String || version.isEmpty) {
    problems.add('the manifest names no package version');
  }

  // 3. Per image: name, address, size and both digests over the packaged bytes.
  final images = decoded['images'];
  final listed = <String>{};
  Uint8List? payload;
  for (final raw in images is List ? images : const []) {
    if (raw is! Map) {
      problems.add('a manifest image entry is not an object');
      continue;
    }
    final file = raw['file'];
    if (file is! String || file.isEmpty) {
      problems.add('a manifest image entry names no file');
      continue;
    }
    listed.add(file);
    if (file != expect.image) {
      problems.add(
        'the manifest lists $file, $expectedTarget\'s image is ${expect.image}',
      );
    }
    final body = zip.entries[file];
    if (body == null) {
      problems.add(
        '$file is in the manifest but not in the package '
        '(entries: ${_entryNames(zip.order)})',
      );
      continue;
    }
    final address = _parseNumber(raw['address']);
    if (address != expect.address) {
      problems.add(
        '$file: the manifest gives address ${raw['address']}, '
        '$expectedTarget is at ${_hex(expect.address)}',
      );
    }
    if (raw['size'] != body.length) {
      problems.add(
        '$file: ${body.length} B in the package, '
        'the manifest says ${raw['size']} B',
      );
    }
    final crc = Bootloader.crc32(body);
    final saidCrc = _parseNumber(raw['crc32']);
    if (saidCrc != crc) {
      problems.add(
        '$file: crc32 ${_hex(crc)} over the packaged image, '
        'the manifest says ${raw['crc32']}${_localise(body, crc, saidCrc)}',
      );
    }
    final paddedSize = Bootloader.paddedLength(body.length);
    if (raw['padded_size'] != paddedSize) {
      problems.add(
        '$file: the padded image is $paddedSize B, '
        'the manifest says ${raw['padded_size']} B',
      );
    }
    final crcPadded = Bootloader.crc32Padded(body);
    if (_parseNumber(raw['crc32_padded']) != crcPadded) {
      problems.add(
        '$file: crc32_padded ${_hex(crcPadded)} over the 0xFF-padded image, '
        'the manifest says ${raw['crc32_padded']}',
      );
    }
    if (file == expect.image && payload == null) payload = body;
  }
  if (images is! List || images.isEmpty) {
    problems.add('the manifest lists no image');
  }
  if (payload == null && listed.contains(expect.image)) {
    problems.add('the package carries no ${expect.image}');
  }

  // 4. Nothing rides along: every entry is the manifest's or the manifest itself.
  for (final entry in zip.order) {
    if (entry != 'manifest.json' && !listed.contains(entry)) {
      problems.add('$entry is in the package but not in the manifest');
    }
  }

  if (problems.isNotEmpty) throw PackageRefused(name, problems);

  final image = payload!;
  return FirmwarePackage(
    name: name,
    target: expectedTarget,
    version: version as String,
    buildCommit: decoded['build_commit'] is String
        ? decoded['build_commit'] as String
        : '',
    builtUtc: decoded['built_utc'] is String
        ? decoded['built_utc'] as String
        : '',
    image: image,
    address: expect.address,
    crc32: Bootloader.crc32(image),
    crc32Padded: Bootloader.crc32Padded(image),
  );
}

// --- a single differing byte ----------------------------------------------------------------

/// Where ONE byte explains the difference between the packaged bytes and the manifest's digest.
class CrcByteDifference {
  const CrcByteDifference(this.offset, this.delta);

  /// Index of the byte that differs.
  final int offset;

  /// The manifest's byte XOR the packaged one (never 0).
  final int delta;
}

/// The one packaged byte whose value alone makes [packaged]'s CRC differ from [expectedCrc], or
/// null when no single byte explains it (several bytes differ, or the manifest is simply wrong).
///
/// CRC-32 is linear over GF(2): for equal-length data `crc(a) ^ crc(b)` is the raw CRC of `a ^ b`
/// with no init and no final XOR. A single differing byte at offset `i` therefore shows up as
/// `table[delta]` propagated through the `length - 1 - i` zero bytes after it, and the reverse
/// walk below undoes one zero-byte step at a time until the state IS a table entry. Each hit is
/// confirmed by recomputing the whole CRC with that byte flipped, so a coincidence cannot be
/// reported as the damaged offset.
CrcByteDifference? singleByteDifference(Uint8List packaged, int expectedCrc) {
  if (packaged.isEmpty) return null;
  final actual = Bootloader.crc32(packaged);
  if (actual == expectedCrc) return null;
  final byTable = <int, int>{};
  for (var delta = 1; delta < 256; delta++) {
    byTable[Bootloader.crc32Step(0, delta)] = delta;
  }
  var state = (actual ^ expectedCrc) & 0xFFFFFFFF;
  for (var step = 0; step < packaged.length; step++) {
    final delta = byTable[state];
    if (delta != null) {
      final at = packaged.length - 1 - step;
      if (Bootloader.crc32(_withByteXored(packaged, at, delta)) ==
          expectedCrc) {
        return CrcByteDifference(at, delta);
      }
    }
    state = _undoZeroByte(state);
  }
  return null;
}

/// [bytes] with byte [offset] XOR'd by [delta] - the candidate repair, built only to be checked.
Uint8List _withByteXored(Uint8List bytes, int offset, int delta) {
  final out = Uint8List.fromList(bytes);
  out[offset] = out[offset] ^ delta;
  return out;
}

/// One byte of CRC-32/ISO-HDLC undone: the state a zero byte was processed from.
///
/// The forward step over one bit is `c' = (c >> 1) ^ (poly if c & 1)`, so the top bit of `c'` IS
/// the bit shifted out - which is what makes the walk reversible.
int _undoZeroByte(int state) {
  var c = state & 0xFFFFFFFF;
  for (var bit = 0; bit < 8; bit++) {
    final low = (c >> 31) & 1;
    c = (((c ^ (low != 0 ? 0xEDB88320 : 0)) << 1) | low) & 0xFFFFFFFF;
  }
  return c;
}

/// The localisation clause for a `crc32` mismatch, or '' when no single byte explains it.
String _localise(Uint8List body, int actualCrc, int? saidCrc) {
  if (saidCrc == null) return '';
  final found = singleByteDifference(body, saidCrc);
  if (found == null) {
    return ' (no single byte explains the difference; the packaged image is '
        'not the one the manifest digested)';
  }
  final packaged = body[found.offset];
  return ' (one byte differs at offset ${_hex(found.offset)}: packaged '
      '${_hexByte(packaged)}, the manifest expects '
      '${_hexByte(packaged ^ found.delta)})';
}

// --- a stored-only zip, read from its bytes -------------------------------------------------

/// A zip read straight from its bytes: entries by name, in central-directory order.
class StoredZip {
  const StoredZip(this.entries, this.order);

  final Map<String, Uint8List> entries;
  final List<String> order;
}

/// Reads a stored (uncompressed) zip from [data] the way [readStoredZip] in `pack_firmware.py`
/// does: central directory first, then each entry's local header, so a tampered payload is
/// returned as bytes to be checked instead of rejected by a library's own CRC test.
///
/// Throws [FormatException] when the bytes are not such an archive (no end-of-central-directory
/// record, a compressed entry, a bad local header, a truncated directory).
StoredZip readStoredZip(Uint8List data) {
  final view = ByteData.sublistView(data);
  var eocd = -1;
  for (var i = data.length - 22; i >= 0; i--) {
    if (data[i] == 0x50 &&
        data[i + 1] == 0x4B &&
        data[i + 2] == 0x05 &&
        data[i + 3] == 0x06) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) {
    throw const FormatException(
      'no end-of-central-directory record, so it is not a zip at all',
    );
  }
  final cdSize = view.getUint32(eocd + 12, Endian.little);
  final cdOff = view.getUint32(eocd + 16, Endian.little);
  if (cdOff + cdSize > data.length) {
    throw FormatException(
      'its central directory ($cdSize B at ${_hex(cdOff)}) runs past the '
      'end of the file',
    );
  }
  final entries = <String, Uint8List>{};
  final order = <String>[];
  var pos = cdOff;
  while (pos < cdOff + cdSize) {
    if (pos + 46 > data.length ||
        !_at(data, pos, const [0x50, 0x4B, 0x01, 0x02])) {
      throw FormatException('corrupt central directory at ${_hex(pos)}');
    }
    final method = view.getUint16(pos + 10, Endian.little);
    final csize = view.getUint32(pos + 20, Endian.little);
    final usize = view.getUint32(pos + 24, Endian.little);
    final nlen = view.getUint16(pos + 28, Endian.little);
    final elen = view.getUint16(pos + 30, Endian.little);
    final clen = view.getUint16(pos + 32, Endian.little);
    final local = view.getUint32(pos + 42, Endian.little);
    if (pos + 46 + nlen > data.length) {
      throw FormatException('corrupt central directory at ${_hex(pos)}');
    }
    final name = utf8.decode(
      data.sublist(pos + 46, pos + 46 + nlen),
      allowMalformed: true,
    );
    if (method != 0) {
      throw FormatException(
        'entry $name is compressed (method $method); a package is '
        'stored-only',
      );
    }
    if (csize != usize) {
      throw FormatException(
        'entry $name: stored entry with size $csize != $usize',
      );
    }
    if (local + 30 > data.length ||
        !_at(data, local, const [0x50, 0x4B, 0x03, 0x04])) {
      throw FormatException('entry $name: bad local header at ${_hex(local)}');
    }
    final hlen = view.getUint16(local + 26, Endian.little);
    final xlen = view.getUint16(local + 28, Endian.little);
    final start = local + 30 + hlen + xlen;
    if (start + csize > data.length) {
      throw FormatException('entry $name: its payload runs past the end');
    }
    entries[name] = Uint8List.sublistView(data, start, start + csize);
    order.add(name);
    pos += 46 + nlen + elen + clen;
  }
  return StoredZip(entries, order);
}

bool _at(Uint8List data, int pos, List<int> magic) {
  if (pos < 0 || pos + magic.length > data.length) return false;
  for (var i = 0; i < magic.length; i++) {
    if (data[pos + i] != magic[i]) return false;
  }
  return true;
}

// --- small text helpers ---------------------------------------------------------------------

/// `0x` + eight uppercase hex digits.
String _hex(int value) =>
    '0x${(value & 0xFFFFFFFF).toRadixString(16).padLeft(8, '0').toUpperCase()}';

String _hexByte(int value) =>
    '0x${(value & 0xFF).toRadixString(16).padLeft(2, '0').toUpperCase()}';

/// A manifest number: a JSON integer, or a `0x…` hex string (the packer writes addresses and
/// digests as hex strings). Null when it is neither.
int? _parseNumber(Object? value) {
  if (value is int) return value;
  if (value is String) {
    final text = value.startsWith('0x') || value.startsWith('0X')
        ? value.substring(2)
        : value;
    return int.tryParse(text, radix: 16);
  }
  return null;
}

String _entryNames(List<String> order) =>
    order.isEmpty ? 'none' : order.join(', ');
