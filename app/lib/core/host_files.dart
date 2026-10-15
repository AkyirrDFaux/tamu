/// Host-side file IO helpers shared by the backup and storage pages.
///
/// On desktop the file picker exposes a local path the app can read itself; on
/// Android/iOS it returns a content URI, so reads go through the `PlatformFile`
/// the picker returns. [saveBytesWithPicker] lets the picker write the bytes on
/// every platform (file_picker 13 hands it the payload).
library;

import 'dart:io';
import 'dart:typed_data';

import 'package:file_picker/file_picker.dart';

Future<void> writePlatformFile(String path, List<int> bytes) async {
  await File(path).writeAsBytes(bytes);
}

List<int> readPlatformFile(String path) => File(path).readAsBytesSync();

/// Shows a save dialog and stores [bytes]. Returns the chosen location, or null
/// when the user cancels.
///
/// file_picker 13 takes the payload in [FilePicker.saveFile] and writes it
/// itself on every platform, returning a [Uri] (a content URI on mobile, a file
/// URI on desktop) - so the app no longer writes the file after the dialog.
Future<String?> saveBytesWithPicker({
  required String fileName,
  required List<int> bytes,
  List<String>? allowedExtensions,
}) async {
  final target = await FilePicker.saveFile(
    fileName: fileName,
    type: allowedExtensions == null ? FileType.any : FileType.custom,
    allowedExtensions: allowedExtensions,
    bytes: Uint8List.fromList(bytes),
  );
  return target?.toString();
}
