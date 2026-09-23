/// Host-side file IO helpers shared by the backup and storage pages.
///
/// On desktop the file picker returns a path the app writes itself. On
/// Android/iOS the picker returns a Storage Access Framework handle and writes
/// the bytes for us, so [saveBytesWithPicker] only writes a path on desktop.
library;

import 'dart:io';
import 'dart:typed_data';

import 'package:file_picker/file_picker.dart';

import 'platform_caps.dart';

Future<void> writePlatformFile(String path, List<int> bytes) async {
  await File(path).writeAsBytes(bytes);
}

List<int> readPlatformFile(String path) => File(path).readAsBytesSync();

/// Shows a save dialog and stores [bytes]. Returns the chosen location, or null
/// when the user cancels.
///
/// On mobile the picker writes the bytes itself (the returned value is a
/// content URI, not a filesystem path); on desktop the app writes the returned
/// path.
Future<String?> saveBytesWithPicker({
  required String fileName,
  required List<int> bytes,
  List<String>? allowedExtensions,
}) async {
  final target = await FilePicker.saveFile(
    fileName: fileName,
    type: allowedExtensions == null ? FileType.any : FileType.custom,
    allowedExtensions: allowedExtensions,
    bytes: isMobile ? Uint8List.fromList(bytes) : null,
  );
  if (target == null) return null;
  if (!isMobile) await writePlatformFile(target, bytes);
  return target;
}
