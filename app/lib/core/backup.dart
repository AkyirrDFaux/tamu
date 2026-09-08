/// Whole-network backup and restore (Docs/App/Backup.md).
///
/// Creates a zipfile containing per-device JSON files built from the devices'
/// Register services (System block + Dynamic/Keyed); restore writes values
/// straight back to the services ("optional live restore").
library;

import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:archive/archive_io.dart';

import 'device_db.dart';
import 'register_client.dart';
import 'dynmem.dart';
import 'keyedmem.dart';
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
        fields: (json['fields'] as List<dynamic>? ?? [])
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

/// Builds a BlockInfo for system block (type=0, inst=0).
Uint8List _makeSystemBi(int field, int key) {
  final bi = (0 << 22) | (0 << 16) | ((field & 0xFF) << 8) | (key & 0xFF);
  return Uint8List(4)
    ..[0] = bi & 0xFF
    ..[1] = (bi >> 8) & 0xFF
    ..[2] = (bi >> 16) & 0xFF
    ..[3] = (bi >> 24) & 0xFF;
}

/// Collects the current System block + Dynamic/Keyed state of one device.
Future<BackupDevice?> captureDevice(int deviceId) async {
  final reg = RegisterClient(deviceId: deviceId);
  final dyn = DynamicMemoryClient(deviceId: deviceId);
  final keyed = KeyedMemoryClient(deviceId: deviceId);

  final entry = DeviceDatabase.instance.byId(deviceId);
  final captured = <BackupBlock>[];

  // --- System block (type 0, inst 0) ---
  // Read all 9 fields (0..8) with appropriate keys
  final sysFields = <int, int>{ // field -> key
    0: 0,  // DeviceType, Caps
    1: 0xFF, // SN
    2: 0,  // ShortAddress
    3: 0,  // TimeFromBoot, Now, TimeOffsetMs, AvgLoop, MaxLoop
    4: 0,  // FreeRAM, TotalFlash
    5: 0,  // FileCount, StorageFlashSize
    6: 0xFF, // Name
    7: 0,  // Reserved
    8: 0,  // AppConnected, ...
  };

  final sysBlockFields = <BackupField>[];
  for (final entry in sysFields.entries) {
    final field = entry.key;
    final key = entry.value;
    final payload = _makeSystemBi(field, key);
    final reply = await reg.request(1, payload: payload);
    if (reply == null || reply.length < 8) continue;
    final meta = BlockMeta.fromBytes(reply, 4);
    // Skip read-only fields (only field 6 Name is writable in System block)
    if (meta.readOnly) continue;
    final value = RegisterClient.valueSlice(reply, meta.size);
    sysBlockFields.add(BackupField(
        index: field,
        flagsAndType: meta.flagsAndType,
        size: meta.size,
        valueHex: value.map((b) => b.toRadixString(16).padLeft(2, '0')).join()));
  }

  if (sysBlockFields.isNotEmpty) {
    captured.add(BackupBlock(
        index: 0,
        name: 'System',
        blockTypeValue: BlockType.system.value,
        fields: sysBlockFields));
  }

  // --- Dynamic blocks ---
  final dynBlocks = await dyn.readBlocks();
  if (dynBlocks != null) {
    for (final block in dynBlocks) {
      // Only save non-deleted, non-none blocks
      if (block.blockType == BlockType.deleted || block.blockType == BlockType.none) continue;
      
      final dynFields = <BackupField>[];
      for (var f = 0; f < block.fieldCount; f++) {
        final field = await dyn.readField(block, f);
        if (field == null) continue;
        if (field.readOnly || field.notSaved) continue;
        dynFields.add(BackupField(
            index: f,
            flagsAndType: field.meta.flagsAndType,
            size: field.meta.size,
            valueHex: field.value.map((b) => b.toRadixString(16).padLeft(2, '0')).join()));
      }
      if (dynFields.isNotEmpty) {
        captured.add(BackupBlock(
            index: block.index,
            name: block.name,
            blockTypeValue: block.blockType.value,
            fields: dynFields));
      }
    }
  }

  // --- Keyed blocks ---
  final keyedBlocks = await keyed.readBlocks();
  if (keyedBlocks != null) {
    for (final block in keyedBlocks) {
      if (block.blockType == BlockType.deleted || block.blockType == BlockType.none) continue;
      
      final keyedFields = <BackupField>[];
      for (var d = 0; d < block.dictCount; d++) {
        final dict = await keyed.readDict(block, d);
        if (dict == null) continue;
        for (final key in dict.keys) {
          final entry = await keyed.readEntry(block, d, key);
          if (entry == null) continue;
          if (entry.readOnly || entry.notSaved) continue;
          // Store dict index in upper bits of field index for restore
          keyedFields.add(BackupField(
              index: (d << 8) | key,
              flagsAndType: entry.meta.flagsAndType,
              size: entry.meta.size,
              valueHex: entry.value.map((b) => b.toRadixString(16).padLeft(2, '0')).join()));
        }
      }
      if (keyedFields.isNotEmpty) {
        captured.add(BackupBlock(
            index: block.index,
            name: block.name,
            blockTypeValue: block.blockType.value,
            fields: keyedFields));
      }
    }
  }

  if (captured.isEmpty) return null;

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
  final reg = RegisterClient(deviceId: backup.id);
  final dyn = DynamicMemoryClient(deviceId: backup.id);
  final keyed = KeyedMemoryClient(deviceId: backup.id);
  
  var written = 0;

  for (final storedBlock in backup.blocks) {
    if (storedBlock.blockTypeValue == BlockType.system.value) {
      // Restore System block fields via Register service
      for (final field in storedBlock.fields) {
        final meta = BlockMeta(flagsAndType: field.flagsAndType);
        if (meta.readOnly) continue;
        
        final bi = _makeSystemBi(field.index, field.flagsAndType == BlockType.system.value ? 0 : 0);
        final payload = [...bi, ...meta.toBytes(), ...field.bytes];
        final reply = await reg.request(2, payload: payload);
        if (reply != null && reply.length >= 8) {
          written++;
        }
      }
      await reg.request(3, payload: [0xFF, 0xFF, 0xFF, 0xFF]); // Save all
    } else if (storedBlock.blockTypeValue == BlockType.undefined.value) {
      // Restore Dynamic blocks
      final liveBlocks = await dyn.readBlocks();
      if (liveBlocks == null) continue;
      final live = liveBlocks.where((b) => b.index == storedBlock.index).firstOrNull;
      if (live == null) continue;
      
      for (final field in storedBlock.fields) {
        final meta = BlockMeta(flagsAndType: field.flagsAndType);
        if (meta.readOnly) continue;
        if (!live.fields.containsKey(field.index)) {
          await dyn.readField(live, field.index);
        }
        final liveField = live.fields[field.index];
        if (liveField == null || liveField.meta.size != field.size) continue;
        final confirmed = await dyn.writeField(live, liveField, field.bytes);
        if (confirmed != null) written++;
      }
      await dyn.save(block: storedBlock.index);
    } else {
      // Restore Keyed blocks
      final liveBlocks = await keyed.readBlocks();
      if (liveBlocks == null) continue;
      final live = liveBlocks.where((b) => b.index == storedBlock.index).firstOrNull;
      if (live == null) continue;
      
      for (final field in storedBlock.fields) {
        final meta = BlockMeta(flagsAndType: field.flagsAndType);
        if (meta.readOnly) continue;
        // Extract dict and key from field index
        final dict = field.index >> 8;
        final key = field.index & 0xFF;
        
        // Ensure dict exists
        if (!live.dicts.containsKey(dict)) {
          await keyed.readDict(live, dict);
        }
        if (!live.entries.containsKey(dict) || !live.entries[dict]!.containsKey(key)) {
          // Key may not exist, but writeEntry will create it
        }
        final entry = live.entries[dict]?[key];
        if (entry == null || entry.meta.size != field.size) {
          // Create new entry if needed
          final newEntry = KeyedEntry(key: key, meta: meta, value: field.bytes);
          live.entries.putIfAbsent(dict, () => {})[key] = newEntry;
          final reply = await keyed.writeKeyValue(live, dict, key, meta, field.bytes);
          if (reply != null) written++;
        } else {
          final reply = await keyed.writeEntry(live, dict, entry, field.bytes);
          if (reply != null) written++;
        }
      }
      await keyed.save(block: storedBlock.index);
    }
  }
  return written;
}