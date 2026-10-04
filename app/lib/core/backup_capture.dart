part of 'backup.dart';

Future<BackupDevice?> captureDevice(
  int deviceId, {
  bool includeFiles = true,
  int maxFileBytes = defaultMaxFileBytes,
}) async {
  final entry = DeviceDatabase.instance.byId(deviceId);
  final reg = RegisterClient(deviceId: deviceId);
  final captured = <BackupBlock>[];

  final walked = await walkDeviceBlocks(reg);
  if (walked != null) {
    for (final b in walked) {
      final block = backupBlockFromVisited(b);
      if (block.entries.isNotEmpty) captured.add(block);
    }
  }

  final files = includeFiles
      ? await _captureFiles(deviceId, maxFileBytes)
      : <BackupFile>[];

  // Scripts: decode every SCR_XXX file into the semantic form (the file is kept too).
  final scripts = <BackupScript>[];
  for (final file in files) {
    if (file.kind != 'Script') continue;
    final slot = _scriptSlot(file.name);
    if (slot == null) continue;
    try {
      scripts.add(BackupScript.fromImage(slot, file.bytes));
    } catch (error) {
      AppDiagnostics.log('backup', 'script ${file.name} parse failed: $error');
    }
  }

  // Subscriptions (requester is restorable; provider is informational).
  final subClient = SubscriptionClient(deviceId: deviceId);
  final requesters = <BackupRequesterSubscription>[];
  final providers = <BackupProviderSubscription>[];
  try {
    for (final s in await subClient.getRequesterSubscriptions()) {
      requesters.add(BackupRequesterSubscription(
        provider: idToString(s.providerAddr),
        trigger: s.trigger.label,
        periodMs: s.periodMs,
        minTimeMs: s.minTimeMs,
        deadzone: s.deadzone,
        source: _blockRef(s.sourceReg),
        target: _blockRef(s.targetReg),
      ));
    }
  } catch (error) {
    AppDiagnostics.log('backup', 'requester subscriptions read failed: $error');
  }
  try {
    for (final s in await subClient.getProviderSubscriptions()) {
      providers.add(BackupProviderSubscription(
        requester: idToString(s.requesterAddr),
        trigger: s.trigger.label,
        periodMs: s.periodMs,
        minTimeMs: s.minTimeMs,
        deadzone: s.deadzone,
        source: _blockRef(s.sourceReg),
      ));
    }
  } catch (error) {
    AppDiagnostics.log('backup', 'provider subscriptions read failed: $error');
  }

  // SNDB lives on cores.
  final sndb = <BackupSndbEntry>[];
  if (entry?.isCore ?? false) {
    for (final (id, serial) in await DeviceDatabase.instance.sndbEntries()) {
      sndb.add(BackupSndbEntry(serial: serial, address: idToString(id)));
    }
  }

  if (captured.isEmpty &&
      files.isEmpty &&
      scripts.isEmpty &&
      requesters.isEmpty &&
      providers.isEmpty &&
      sndb.isEmpty) {
    return null;
  }

  return BackupDevice(
    created: DateTime.now().toIso8601String(),
    type: (entry?.type ?? DeviceType.unknown).label,
    typeId: (entry?.type ?? DeviceType.unknown).value,
    id: deviceId,
    name: entry?.displayName ?? 'Device ${idToString(deviceId)}',
    serial: entry?.serialNumber,
    blocks: captured,
    scripts: scripts,
    requesterSubscriptions: requesters,
    providerSubscriptions: providers,
    sndb: sndb,
    files: files,
  );
}

Future<List<BackupFile>> _captureFiles(int deviceId, int maxFileBytes) async {
  final storage = StorageClient(deviceId: deviceId);
  final table = await storage.readFileTable();
  if (table == null) return const [];
  final files = <BackupFile>[];
  final seen = <String>{};
  for (final record in table) {
    final name = normalizeFileName(record.name);
    if (name.isEmpty || name == '.TABLE') continue;
    // The file table can hold several records with the same name (a stale record is
    // not always zeroed); capture each file once.
    if (!seen.add(name)) continue;
    if (record.size > maxFileBytes) {
      AppDiagnostics.log('backup', 'skipping large file $name (${record.size} B)');
      continue;
    }
    try {
      final bytes = await storage.readFile(name, size: record.size);
      if (bytes == null) continue;
      files.add(BackupFile(
        name: name,
        kind: backupFileKind(name),
        data: base64Encode(bytes),
      ));
    } catch (error) {
      AppDiagnostics.log('backup', 'file $name read failed: $error');
    }
  }
  return files;
}

// ---------------------------------------------------------------------------
// Archive zip (one JSON per device, no aggregate manifest)
// ---------------------------------------------------------------------------

/// Adds a UTF-8 JSON entry with the CORRECT uncompressed size. `ArchiveFile.string`
/// stores UTF-8 bytes but sizes by UTF-16 code units, so any non-ASCII character
/// (e.g. the "±" in the accelerometer range labels) yields a wrong size field and
/// strict unzippers reject the archive with a CRC error. Build the entry from the
/// encoded bytes instead.
void _addJsonEntry(Archive archive, String name, String content) {
  final bytes = Uint8List.fromList(utf8.encode(content));
  archive.addFile(ArchiveFile(name, bytes.length, bytes));
}

Uint8List buildBackupZip(List<BackupDevice> devices) {
  if (devices.isEmpty) {
    throw const FormatException('Nothing to back up');
  }
  final zip = Archive();
  final used = <String>{};
  for (final device in devices) {
    var name = device.fileName;
    var n = 1;
    while (!used.add(name)) {
      name = '${device.id}_${n}_${device.fileName}';
      n++;
    }
    _addJsonEntry(zip, name, const JsonEncoder().convert(device.toJson()));
  }
  return Uint8List.fromList(ZipEncoder().encode(zip)!);
}

/// Parses a backup zip into its devices (one JSON per device).
List<BackupDevice> parseBackupZip(List<int> zipBytes) {
  final archive = ZipDecoder().decodeBytes(zipBytes);
  final devices = <BackupDevice>[];
  for (final file in archive.files) {
    final name = file.name;
    if (name.endsWith('/') || !name.toLowerCase().endsWith('.json')) continue;
    final text = utf8.decode(file.content as List<int>);
    final json = jsonDecode(text) as Map<String, dynamic>;
    devices.add(BackupDevice.fromJson(json));
  }
  if (devices.isEmpty) {
    throw const FormatException('Not a Tamu backup (no device files)');
  }
  return devices;
}

// ---------------------------------------------------------------------------
// Live device snapshot (target matching)
// ---------------------------------------------------------------------------

/// One live register entry of a device, used to resolve a restore target.
