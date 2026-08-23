/// Whole-network backup and restore (Docs/App/Backup.md).
///
/// Creates a zipfile containing per-device JSON files built from the devices'
/// System Memory services; restore writes values straight back to the services
/// ("optional live restore").
library;

import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:archive/archive_io.dart';

import 'device_db.dart';
import 'sysmem.dart';
import 'types.dart';

Future<void> writePlatformFile(String path, List<int> bytes) async {
  await File(path).writeAsBytes(bytes);
}

List<int> readPlatformFile(String path) => File(path).readAsBytesSync();

class BackupField {
  final int index;
  final int flagsAndType; // type + flags as stored on the wire
  final int size; // value length on the wire (BlockMeta.Size)
  final String valueHex;

  BackupField(
      {required this.index,
      required this.flagsAndType,
      required this.size,
      required this.valueHex});

  Map<String, dynamic> toJson() =>
      {'i': index, 'm': flagsAndType, 's': size, 'v': valueHex};

  static BackupField fromJson(Map<String, dynamic> json) => BackupField(
        index: json['i'] as int,
        flagsAndType: json['m'] as int,
        // Older archives (format 1 without 's') carry no size: fall back to the
        // hex payload length so restore keeps working for them.
        size: (json['s'] as int?) ?? _hexLength(json['v'] as String? ?? ''),
        valueHex: json['v'] as String,
      );

  static int _hexLength(String hex) {
    final clean = hex.replaceAll(RegExp(r'[^0-9a-fA-F]'), '');
    return clean.length ~/ 2;
  }

  Uint8List get bytes {
    final clean = valueHex.replaceAll(RegExp(r'[^0-9a-fA-F]'), '');
    return Uint8List.fromList([
      for (var i = 0; i + 1 < clean.length; i += 2)
        int.parse(clean.substring(i, i + 2), radix: 16)
    ]);
  }

  DataType get dataType => BlockMeta(flagsAndType: flagsAndType).dataType;
}

class BackupBlock {
  final int index;
  final String name;
  final int blockTypeValue;
  final List<BackupField> fields;

  BackupBlock({
    required this.index,
    required this.name,
    required this.blockTypeValue,
    required this.fields,
  });

  Map<String, dynamic> toJson() => {
        'i': index,
        'name': name,
        'blockType': blockTypeValue,
        'fields': [for (final f in fields) f.toJson()],
      };

  static BackupBlock fromJson(Map<String, dynamic> json) => BackupBlock(
        index: json['i'] as int,
        name: json['name'] as String,
        blockTypeValue: json['blockType'] as int,
        fields: (json['fields'] as List<dynamic>)
            .map((f) => BackupField.fromJson(f as Map<String, dynamic>))
            .toList(),
      );
}

class BackupDevice {
  final int id;
  final String name;
  final String? serialNumber;
  final List<BackupBlock> blocks;

  BackupDevice({
    required this.id,
    required this.name,
    this.serialNumber,
    required this.blocks,
  });

  Map<String, dynamic> toJson() => {
        'id': id,
        'name': name,
        'serial': serialNumber,
        'blocks': [for (final b in blocks) b.toJson()],
      };

  static BackupDevice fromJson(Map<String, dynamic> json) => BackupDevice(
        id: json['id'] as int,
        name: json['name'] as String? ?? 'Device',
        serialNumber: json['serial'] as String?,
        blocks: (json['blocks'] as List<dynamic>? ?? [])
            .map((b) => BackupBlock.fromJson(b as Map<String, dynamic>))
            .toList(),
      );

  String get fileName => '${id}_${name.replaceAll(RegExp(r'[^A-Za-z0-9._-]'), '_')}.json';
}

/// Collects the current System Memory state of one device.
Future<BackupDevice?> captureDevice(int deviceId) async {
  final client = SystemMemoryClient(deviceId: deviceId);
  final blocks = await client.readBlocks();
  if (blocks == null) return null;

  final entry = DeviceDatabase.instance.byId(deviceId);
  final captured = <BackupBlock>[];
  for (final block in blocks) {
    final fields = <BackupField>[];
    for (var f = 0; f < block.meta.size; f++) {
      final field = await client.readField(block, f);
      if (field == null) continue;
      if (field.readOnly || !field.valid) continue; // RAM-only values never backup
      fields.add(BackupField(
          index: f,
          flagsAndType: field.meta.flagsAndType,
          size: field.meta.size,
          valueHex:
              field.value.map((b) => b.toRadixString(16).padLeft(2, '0')).join()));
    }
    captured.add(BackupBlock(
        index: block.index,
        name: block.name,
        blockTypeValue: block.meta.typeValue,
        fields: fields));
  }
  return BackupDevice(
      id: deviceId,
      name: entry?.name ?? 'Device',
      serialNumber: entry?.serialNumber,
      blocks: captured);
}

Uint8List buildBackupZip(List<BackupDevice> devices) {
  final archive = Archive();
  archive.addFile(ArchiveFile.string(
      'backup.json',
      const JsonEncoder.withIndent('  ').convert({
        'app': 'tamuapp',
        'format': 1,
        'devices': [for (final d in devices) d.toJson()],
      })));
  for (final device in devices) {
    archive.addFile(ArchiveFile.string(device.fileName,
        const JsonEncoder.withIndent('  ').convert(device.toJson())));
  }
  return Uint8List.fromList(ZipEncoder().encode(archive)!);
}

/// Parses a backup zip into its device list.
List<BackupDevice> parseBackupZip(List<int> zipBytes) {
  final archive = ZipDecoder().decodeBytes(zipBytes);
  final manifest =
      archive.files.where((f) => f.name == 'backup.json').firstOrNull;
  if (manifest == null) {
    throw const FormatException('Not a Tamu backup (no backup.json)');
  }
  final data = utf8.decode(manifest.content as List<int>);
  final decoded = jsonDecode(data) as Map<String, dynamic>;
  return (decoded['devices'] as List<dynamic>? ?? [])
      .map((d) => BackupDevice.fromJson(d as Map<String, dynamic>))
      .toList();
}

/// Live-restores a device's blocks straight to its services.
/// Returns the number of successfully written fields.
Future<int> restoreDevice(BackupDevice backup) async {
  final client = SystemMemoryClient(deviceId: backup.id);
  var written = 0;
  final liveBlocks = await client.readBlocks();
  if (liveBlocks == null) return 0;
  for (final storedBlock in backup.blocks) {
    final live = liveBlocks
        .where((b) => b.index == storedBlock.index)
        .firstOrNull;
    if (live == null) continue; // block no longer exists
    for (final field in storedBlock.fields) {
      final meta = BlockMeta(flagsAndType: field.flagsAndType);
      if (meta.readOnly) continue;
      if (!live.fields.containsKey(field.index)) {
        await client.readField(live, field.index);
      }
      final liveField = live.fields[field.index];
      if (liveField == null || liveField.meta.size != meta.size) {
        continue; // incompatible structure
      }
      final confirmed = await client.writeField(live, liveField, field.bytes);
      if (confirmed != null) written++;
    }
    await client.save(block: storedBlock.index);
  }
  return written;
}
