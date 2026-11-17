/// The flash path takes a verified `.fw.zip` package and refuses a loose image.
///
/// A flash is only as trustworthy as the file that happens to be on disk: the page used to write
/// whatever `.bin` was picked. A package carries the image AND the manifest that describes it -
/// target, address, size, and the digests a device can be compared against - so every check here
/// runs over the packaged bytes BEFORE the first byte is written, and a file that fails one is
/// refused with its own sentence.
///
/// These are the package checks; the write path's own tests (`bootloader_client_test.dart`) and the
/// device's digest are the other half.
library;

import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/bootloader.dart';
import 'package:tamuapp/core/firmware_package.dart';

import 'support/fw_package.dart';

/// The package in [bytes], checked for [target].
FirmwarePackage accept(
  Uint8List bytes, {
  String target = 'Tamu_v2_0A',
  String name = 'package.fw.zip',
}) => verifyFirmwarePackage(bytes: bytes, expectedTarget: target, name: name);

/// The refusal [bytes] earns, or a failure when it was accepted.
PackageRefused refuse(
  Uint8List bytes, {
  String target = 'Tamu_v2_0A',
  String name = 'package.fw.zip',
}) {
  try {
    accept(bytes, target: target, name: name);
  } on PackageRefused catch (refusal) {
    return refusal;
  }
  throw StateError('the package was accepted, and this case is a refusal');
}

/// Whether any failed check's sentence contains every one of [parts].
bool names(PackageRefused refusal, List<String> parts) => refusal.problems.any(
  (problem) => parts.every((part) => problem.contains(part)),
);

void main() {
  group('a good package', () {
    test('verifies, and its manifest is what the flash compares against', () {
      final fake = PackageForTest();
      final package = accept(fake.build());

      expect(package.target, 'Tamu_v2_0A');
      expect(package.address, 0x90000);
      expect(package.version, '26.11.12.4');
      expect(package.buildCommit, 'abc123def456');
      expect(package.size, fake.image.length);
      expect(package.image, fake.image);
      expect(package.crc32, Bootloader.crc32(fake.image));
      expect(package.paddedSize, 1024, reason: '1024 B is already a whole number of chunks');
      expect(
        package.crc32Padded,
        Bootloader.crc32Padded(fake.image),
        reason: 'the digest the device answers with, over the padded image',
      );
    });

    test('verifies with the address spelled the way the packer writes it', () {
      // `pack_firmware.py: TARGETS` writes `0x90000` and `0x08000800`; the reader parses them.
      final core = PackageForTest()..address = '0x90000';
      expect(accept(core.build()).address, 0x90000);
      final node = PackageForTest(target: 'DAS_v0_1')..address = '0x08000800';
      expect(accept(node.build(), target: 'DAS_v0_1').address, 0x08000800);
    });

    test('keeps the digest the device reports for the same image', () {
      // The same bytes the rig's digest answered 0xB674DAED for are the DAS app image; the
      // package's crc32_padded must be the identical value, or the flash compares nonsense.
      final fake = PackageForTest(
        target: 'DAS_v0_1',
        image: Uint8List.fromList(List.generate(64, (i) => i)),
      );
      final package = accept(fake.build(), target: 'DAS_v0_1');
      expect(package.crc32Padded, Bootloader.crc32Padded(fake.image));
      expect(
        package.crc32Padded,
        Bootloader.crc32(fake.image),
        reason: '64 bytes are already a whole number of chunks',
      );
    });
  });

  group('the target mapping is the packer\'s', () {
    test('all four targets sit at their address with their image name', () {
      expect(firmwareTargets['Tamu_v2_0A']!.address, 0x90000);
      expect(firmwareTargets['Tamu_v2_0A']!.image, 'Tamu_v2_0A.bin');
      expect(firmwareTargets['Tamu_bootloader']!.address, 0x10000);
      expect(firmwareTargets['DAS_v0_1']!.address, 0x08000800);
      expect(firmwareTargets['DAS_v0_1']!.image, 'DAS_v0_1.bin');
      expect(firmwareTargets['DAS_bootloader']!.address, 0x08000000);
    });

    test('each route names the target it writes', () {
      expect(routeTarget(directToCore: true), 'Tamu_v2_0A');
      expect(routeTarget(directToCore: false), 'DAS_v0_1');
    });
  });

  group('a damaged package is refused, and says where', () {
    test('one flipped payload byte: the file and the offset', () {
      final fake = PackageForTest();
      final damaged = Uint8List.fromList(fake.image);
      damaged[0x40] = damaged[0x40] ^ 0x5A;
      fake.payload = damaged;

      final refusal = refuse(fake.build(), name: fake.name);

      expect(
        names(refusal, ['Tamu_v2_0A.bin', 'crc32', 'offset 0x00000040']),
        isTrue,
        reason: 'the check names the entry and the byte: ${refusal.problems}',
      );
      expect(
        names(refusal, ['packaged 0x1A', 'the manifest expects 0x40']),
        isTrue,
        reason: 'the packaged byte and the one the digest wants',
      );
      expect(refusal.text, contains('was refused, nothing was written'));
      expect(refusal.text, contains(fake.name));
    });

    test('two flipped payload bytes are not blamed on one', () {
      final fake = PackageForTest();
      final damaged = Uint8List.fromList(fake.image);
      damaged[3] ^= 0x01;
      damaged[150] ^= 0x80;
      fake.payload = damaged;

      final refusal = refuse(fake.build());

      expect(names(refusal, ['crc32']), isTrue);
      expect(
        refusal.text,
        contains('no single byte explains the difference'),
        reason: 'a wrong guess at the offset is worse than none',
      );
    });

    test('a wrong target', () {
      final fake = PackageForTest(target: 'DAS_v0_1');
      final refusal = refuse(fake.build());
      expect(
        names(refusal, [
          'the package is for DAS_v0_1',
          'this route writes Tamu_v2_0A',
        ]),
        isTrue,
      );
    });

    test('a wrong address', () {
      final fake = PackageForTest()..address = '0x08000800';
      final refusal = refuse(fake.build());
      expect(
        names(refusal, [
          'Tamu_v2_0A.bin',
          'address 0x08000800',
          'Tamu_v2_0A is at 0x00090000',
        ]),
        isTrue,
      );
    });

    test('a crc32 that is not the packaged image\'s', () {
      final fake = PackageForTest();
      fake.crc32 = hex32(Bootloader.crc32(fake.image) ^ 0x0000FFFF);
      final refusal = refuse(fake.build());
      expect(
        names(refusal, [
          'crc32',
          'over the packaged image',
          'the manifest says ${fake.crc32}',
        ]),
        isTrue,
      );
    });

    test('a crc32_padded that is not the padded image\'s', () {
      final fake = PackageForTest();
      fake.crc32Padded = hex32(Bootloader.crc32Padded(fake.image) ^ 0x1);
      final refusal = refuse(fake.build());
      expect(
        names(refusal, [
          'crc32_padded',
          'over the 0xFF-padded image',
          'the manifest says ${fake.crc32Padded}',
        ]),
        isTrue,
      );
    });

    test('a size the payload does not have', () {
      final fake = PackageForTest()..size = 199;
      final refusal = refuse(fake.build());
      expect(
        names(refusal, [
          'Tamu_v2_0A.bin',
          '1024 B in the package',
          'the manifest says 199 B',
        ]),
        isTrue,
      );
    });

    test('a padded size the padded image does not have', () {
      final fake = PackageForTest()..paddedSize = 192;
      final refusal = refuse(fake.build());
      expect(names(refusal, ['the padded image is 1024 B']), isTrue);
    });

    test('an entry smuggled in beside the manifest', () {
      final fake = PackageForTest();
      fake.extraEntries['settings.json'] = Uint8List.fromList([1, 2, 3]);
      final refusal = refuse(fake.build());
      expect(
        names(refusal, [
          'settings.json is in the package but not in the manifest',
        ]),
        isTrue,
      );
    });

    test('an image the manifest lists but the archive lacks', () {
      final bytes = storedZip([
        MapEntry(
          'manifest.json',
          Uint8List.fromList(
            ('{"format":1,"target":"Tamu_v2_0A","version":"1",'
                    '"images":[{"file":"Tamu_v2_0A.bin","address":"0x90000",'
                    '"size":4}]}')
                .codeUnits,
          ),
        ),
      ]);
      final refusal = refuse(bytes);
      expect(
        names(refusal, ['Tamu_v2_0A.bin is in the manifest but not']),
        isTrue,
      );
    });

    test('no manifest at all', () {
      final refusal = refuse(
        storedZip([
          MapEntry('Tamu_v2_0A.bin', Uint8List.fromList([1])),
        ]),
      );
      expect(names(refusal, ['carries no manifest.json']), isTrue);
    });

    test('a format this reader does not implement', () {
      final fake = PackageForTest()..format = 2;
      final refusal = refuse(fake.build());
      expect(names(refusal, ['manifest format is 2']), isTrue);
    });
  });

  group('a file that is not a package is refused, never written', () {
    test('a bare .bin, naming the package it wants', () {
      final refusal = refuse(
        Uint8List.fromList(List.generate(64, (i) => i)),
        name: 'DAS_v0_1.bin',
        target: 'DAS_v0_1',
      );
      expect(
        names(refusal, [
          'DAS_v0_1.bin is a bare image',
          'binaries/DAS_v0_1-<version>.fw.zip',
        ]),
        isTrue,
        reason: 'a bare image has no manifest, and the sentence says so',
      );
      expect(refusal.text, contains('nothing was written'));
    });

    test('bytes that are not a zip', () {
      final refusal = refuse(
        Uint8List.fromList(List.generate(2048, (i) => i & 0xFF)),
      );
      expect(names(refusal, ['is not a firmware package']), isTrue);
      expect(refusal.text, contains('no end-of-central-directory record'));
    });

    test('a compressed entry, whose payload offset cannot be trusted', () {
      final bytes = storedZip([
        MapEntry('manifest.json', Uint8List.fromList([123, 125])),
        MapEntry('Tamu_v2_0A.bin', Uint8List.fromList([1, 2, 3])),
      ], method: 8);
      final refusal = refuse(bytes);
      // The first entry the reader meets is the manifest, so it is the one it names.
      expect(
        names(refusal, [
          'manifest.json is compressed (method 8)',
          'a package is stored-only',
        ]),
        isTrue,
      );
    });
  });

  group('the single-byte localisation', () {
    test('finds the offset and the delta at any length and position', () {
      for (final length in [1, 2, 31, 32, 33, 200, 512]) {
        final original = Uint8List.fromList(
          List.generate(length, (i) => (i * 7 + 3) & 0xFF),
        );
        final expected = Bootloader.crc32(original);
        for (final offset in {0, length ~/ 2, length - 1}) {
          final damaged = Uint8List.fromList(original);
          damaged[offset] ^= 0x9C;
          final found = singleByteDifference(damaged, expected);
          expect(found, isNotNull, reason: 'length $length, offset $offset');
          expect(found!.offset, offset);
          expect(found.delta, 0x9C);
        }
      }
    });

    test('says nothing when the digest already matches', () {
      final image = Uint8List.fromList(List.generate(64, (i) => i));
      expect(singleByteDifference(image, Bootloader.crc32(image)), isNull);
    });

    test('finds no single byte when two differ', () {
      final original = Uint8List.fromList(List.generate(64, (i) => i));
      final damaged = Uint8List.fromList(original);
      damaged[10] ^= 0x11;
      damaged[40] ^= 0x22;
      expect(singleByteDifference(damaged, Bootloader.crc32(original)), isNull);
    });
  });
}
