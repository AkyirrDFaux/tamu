/// Synthetic `.fw.zip` packages for the verification suites, and the stored-zip writer behind them.
///
/// A package is a real stored zip, built here byte by byte the way `pack_firmware.py:
/// write_package` builds one (entries stored, local header + payload, then the central directory
/// and its end record), so the reader under test meets the layout a packaged build has rather
/// than a library's idea of a zip.
///
/// [PackageForTest] holds both halves of a package apart on purpose: [image] is what the manifest
/// describes, [payload] is what the archive actually carries. A case that models a damaged
/// package keeps the manifest honest and breaks the payload; the verifier must notice either way.
library;

import 'dart:convert';
import 'dart:typed_data';

import 'package:tamuapp/core/bootloader.dart';
import 'package:tamuapp/core/firmware_package.dart';

/// One package under construction. Every field defaults to what the packer would write for
/// [image], so a case makes exactly one thing wrong by setting it.
class PackageForTest {
  PackageForTest({this.target = 'Tamu_v2_0A', Uint8List? image})
    : image = image ?? Uint8List.fromList(List.generate(200, (i) => i & 0xFF));

  final String target;

  /// The image the manifest describes.
  final Uint8List image;

  /// The bytes the archive carries; null means [image]. Set it to model a damaged payload.
  Uint8List? payload;

  int? format = 1;
  String version = '26.11.12.4';
  String buildCommit = 'abc123def456';

  /// Manifest overrides. Null means "what the packer would write".
  String? address;
  int? size;
  String? crc32;
  int? paddedSize;
  String? crc32Padded;
  String? imageName;

  /// Entries smuggled into the archive that the manifest does not list.
  final extraEntries = <String, Uint8List>{};

  /// The archived bytes (the manifest's digests are over [image], never over these).
  Uint8List get stored => payload ?? image;

  /// The file name the packer gives this package.
  String get name => '$target-$version.fw.zip';

  String get imageFile => imageName ?? firmwareTargets[target]!.image;

  /// The archive, as bytes.
  Uint8List build() {
    final two = firmwareTargets[target]!;
    final manifest = <String, Object?>{
      'format': format,
      'target': target,
      'version': version,
      'build_commit': buildCommit,
      'built_utc': '2026-11-12T10:00:00Z',
      'images': [
        <String, Object?>{
          'file': imageFile,
          'address': address ?? addressFor(two.address),
          'size': size ?? image.length,
          'crc32': crc32 ?? hex32(Bootloader.crc32(image)),
          'padded_size': paddedSize ?? Bootloader.paddedLength(image.length),
          'crc32_padded': crc32Padded ?? hex32(Bootloader.crc32Padded(image)),
        },
      ],
    };
    return storedZip([
      MapEntry(
        'manifest.json',
        Uint8List.fromList(utf8.encode(jsonEncode(manifest))),
      ),
      MapEntry(imageFile, stored),
      for (final extra in extraEntries.entries)
        MapEntry(extra.key, extra.value),
    ]);
  }
}

/// The manifest's spelling of an address: `0x` + eight hex digits.
String addressFor(int address) =>
    '0x${address.toRadixString(16).padLeft(8, '0')}';

/// `0x` + eight uppercase hex digits, the spelling the refusals use.
String hex32(int value) =>
    '0x${(value & 0xFFFFFFFF).toRadixString(16).padLeft(8, '0').toUpperCase()}';

/// A zip whose entries are [entries], in the order given, all stored under [method].
///
/// [method] is a parameter so a case can hand the reader a compressed entry (method 8) and watch
/// it refuse: the packaged payload's offset is only checkable while an entry is stored.
Uint8List storedZip(
  List<MapEntry<String, Uint8List>> entries, {
  int method = 0,
}) {
  final body = BytesBuilder();
  final central = BytesBuilder();
  for (final entry in entries) {
    final name = utf8.encode(entry.key);
    final data = entry.value;
    final offset = body.length;
    final crc = Bootloader.crc32(data);

    final local = ByteData(30)
      ..setUint32(0, 0x04034B50, Endian.little)
      ..setUint16(4, 20, Endian.little) // version needed
      ..setUint16(6, 0, Endian.little) // flags
      ..setUint16(8, method, Endian.little)
      ..setUint16(10, 0, Endian.little) // time
      ..setUint16(12, 0x21, Endian.little) // 1980-01-01
      ..setUint32(14, crc, Endian.little)
      ..setUint32(18, data.length, Endian.little) // compressed size
      ..setUint32(22, data.length, Endian.little) // uncompressed size
      ..setUint16(26, name.length, Endian.little)
      ..setUint16(28, 0, Endian.little); // extra field length
    body
      ..add(local.buffer.asUint8List())
      ..add(name)
      ..add(data);

    final dir = ByteData(46)
      ..setUint32(0, 0x02014B50, Endian.little)
      ..setUint16(4, 20, Endian.little) // version made by
      ..setUint16(6, 20, Endian.little) // version needed
      ..setUint16(8, 0, Endian.little) // flags
      ..setUint16(10, method, Endian.little)
      ..setUint16(12, 0, Endian.little) // time
      ..setUint16(14, 0x21, Endian.little) // 1980-01-01
      ..setUint32(16, crc, Endian.little)
      ..setUint32(20, data.length, Endian.little) // compressed size
      ..setUint32(24, data.length, Endian.little) // uncompressed size
      ..setUint16(28, name.length, Endian.little)
      ..setUint16(30, 0, Endian.little) // extra field length
      ..setUint16(32, 0, Endian.little) // comment length
      ..setUint16(34, 0, Endian.little) // disk number
      ..setUint16(36, 0, Endian.little) // internal attributes
      ..setUint32(38, 0, Endian.little) // external attributes
      ..setUint32(42, offset, Endian.little); // local header offset
    central
      ..add(dir.buffer.asUint8List())
      ..add(name);
  }

  final bodyBytes = body.takeBytes();
  final dirBytes = central.takeBytes();
  final end = ByteData(22)
    ..setUint32(0, 0x06054B50, Endian.little)
    ..setUint16(4, 0, Endian.little) // this disk
    ..setUint16(6, 0, Endian.little) // directory disk
    ..setUint16(8, entries.length, Endian.little)
    ..setUint16(10, entries.length, Endian.little)
    ..setUint32(12, dirBytes.length, Endian.little)
    ..setUint32(16, bodyBytes.length, Endian.little)
    ..setUint16(20, 0, Endian.little); // comment length

  return (BytesBuilder()
        ..add(bodyBytes)
        ..add(dirBytes)
        ..add(end.buffer.asUint8List()))
      .takeBytes();
}
