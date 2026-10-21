/// Semantic LED layout for backups (Docs/App/Backup.md; Docs/Modules and Blocks/LED Display.md).
///
/// A layout file (`LAY_1`) is the LED display's spatial map. Its wire form (Docs "Layout File",
/// read by the firmware's `LoadLayoutFromStorage`) is a 3-byte header - u8 brightness limit,
/// u8 width, u8 height - then `width * height` u16 little-endian LED indices, row first, with
/// `0xFFFF` for a cell no LED covers.
///
/// The layout has a semantic form, so a backup carries the decoded map and the restore rebuilds
/// the file byte for byte: the raw bytes never also travel (the rule the scripts follow).
///
/// The brightness limit is stored as the BYTE the file carries (178), not as its percentage. The
/// firmware reads that byte as a ceiling - 178/255 = 69.8 %, shown as 70 % - so an archive that
/// stored 70 % would round back to a different byte and rebuild a different file. The percentage
/// is derived for display ([brightnessPercent]) and is never stored.
library;

import 'dart:typed_data';

/// Bytes before the index table: brightness limit, width, height.
const int layoutHeaderSize = 3;

/// The wire value of a cell no LED covers (Docs "Layout File": "An index of `0xFFFF` means a
/// missing LED").
const int layoutMissingLed = 0xFFFF;

/// Bound on the index table a layout file may carry (`MaxLayoutEntries` in the reference header,
/// `tamu-setup/Vysi1 Layout Reference.h`): the firmware refuses a larger table, so a larger file
/// is not given a semantic form either and the capture keeps its bytes.
const int layoutMaxCells = 256;

/// A layout file decoded into its semantic form: the header in words and the LED map.
class BackupLayout {
  /// The storage file the layout is written to ("LAY_1").
  final String file;

  /// Header byte 0: the brightness ceiling, 0-255, which the firmware reads as a percentage
  /// (178 = 70 %). The byte is what round-trips, so it is stored as it is.
  final int brightnessLimit;

  /// Header byte 1.
  final int width;

  /// Header byte 2.
  final int height;

  /// The LED index of every cell, row first: `width * height` entries, `null` for a cell no LED
  /// covers (wire `0xFFFF`). An entry is a 0-based LED index, which is what the wire carries and
  /// what the file's own documentation means by an index - not the 1-based form the firmware's
  /// compiled-in default array (`LayoutVysiv1_0`) is written in.
  final List<int?> leds;

  BackupLayout({
    required this.file,
    required this.brightnessLimit,
    required this.width,
    required this.height,
    required this.leds,
  });

  /// The header byte as the percentage a person reads (178 -> 70 %). Display only: the archive
  /// keeps the byte, because 69.8 % would not rebuild 178.
  int get brightnessPercent => (brightnessLimit * 100 + 127) ~/ 255;

  /// Number of cells the header declares.
  int get cells => width * height;

  /// The LED index at [row], [column], or null for a cell no LED covers.
  int? ledAt(int row, int column) {
    final at = row * width + column;
    if (at < 0 || at >= leds.length) return null;
    return leds[at];
  }

  Map<String, dynamic> toJson() => {
    'file': file,
    'brightnessLimit': brightnessLimit,
    'width': width,
    'height': height,
    'leds': leds,
  };

  static BackupLayout fromJson(Map<String, dynamic> json) => BackupLayout(
    file: json['file'] as String? ?? '',
    brightnessLimit: (json['brightnessLimit'] as num?)?.toInt() ?? 0,
    width: (json['width'] as num?)?.toInt() ?? 0,
    height: (json['height'] as num?)?.toInt() ?? 0,
    leds: [
      for (final led in json['leds'] as List? ?? const [])
        led is num ? led.toInt() : null,
    ],
  );

  /// Decodes a layout [file]'s [bytes].
  ///
  /// Throws [FormatException] when the bytes are not a layout in the only format the firmware
  /// accepts - the 3-byte header (`Docs "Layout File"`). That is the fallback condition the
  /// capture relies on: a file this rejects (an older 2-byte-header file, a truncated or padded
  /// file, a table larger than the firmware will load) has no semantic form and keeps its bytes.
  static BackupLayout fromImage(String file, List<int> bytes) {
    if (bytes.length < layoutHeaderSize) {
      throw FormatException(
        'layout $file is ${bytes.length} B, shorter than the $layoutHeaderSize-byte header',
      );
    }
    final brightnessLimit = bytes[0];
    final width = bytes[1];
    final height = bytes[2];
    if (width == 0 || height == 0) {
      throw FormatException('layout $file declares a ${width}x$height map');
    }
    final cells = width * height;
    if (cells > layoutMaxCells) {
      throw FormatException(
        'layout $file declares $cells cells, past the $layoutMaxCells-cell table limit',
      );
    }
    // Exact size, because the rebuild has to reproduce the file: a shorter file is truncated and a
    // longer one carries trailing bytes the semantic form does not describe. The old 2-byte-header
    // layout lands here: its map starts where this format reads the height.
    final expected = layoutHeaderSize + cells * 2;
    if (bytes.length != expected) {
      throw FormatException(
        'layout $file is ${bytes.length} B but a ${width}x$height map is $expected B',
      );
    }
    final leds = <int?>[];
    for (var i = 0; i < cells; i++) {
      final at = layoutHeaderSize + i * 2;
      final value = bytes[at] | (bytes[at + 1] << 8);
      leds.add(value == layoutMissingLed ? null : value);
    }
    return BackupLayout(
      file: file,
      brightnessLimit: brightnessLimit,
      width: width,
      height: height,
      leds: leds,
    );
  }

  /// Rebuilds the file's bytes.
  ///
  /// Throws [FormatException] when this entry cannot describe a file (a map that does not match
  /// its header, or an index the wire cannot hold), so a corrupt semantic entry is reported rather
  /// than written as a truncated layout.
  Uint8List toImage() {
    if (width == 0 || height == 0) {
      throw FormatException('layout $file declares a ${width}x$height map');
    }
    if (leds.length != cells) {
      throw FormatException(
        'layout $file carries ${leds.length} cells but its header declares ${width}x$height',
      );
    }
    final out = Uint8List(layoutHeaderSize + cells * 2);
    out[0] = brightnessLimit & 0xFF;
    out[1] = width & 0xFF;
    out[2] = height & 0xFF;
    for (var i = 0; i < cells; i++) {
      final led = leds[i] ?? layoutMissingLed;
      if (led < 0 || led > layoutMissingLed) {
        throw FormatException('layout $file carries LED index $led');
      }
      final at = layoutHeaderSize + i * 2;
      out[at] = led & 0xFF;
      out[at + 1] = (led >> 8) & 0xFF;
    }
    return out;
  }
}
