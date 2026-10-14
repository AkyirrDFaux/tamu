part of 'backup.dart';

/// True when a captured file's bytes are read from the device into the raw archive list.
///
/// A script file carries a semantic form and is read whatever the caller's "include device files"
/// choice; a raw device file is read only when the caller asked for it.
bool readCapturedFile(String name, {required bool includeFiles}) =>
    includeFiles || backupFileKind(name) == 'Script';

/// True when a captured file is dropped from the raw archive list.
///
/// A file the semantic pass already archived (a parsed script), the requester-subscription table
/// and the device's own persistence files all have a semantic form, so a raw copy would only let a
/// foreign device's bytes overwrite a target. "Include device files" off drops the raw device files
/// too - but never a script: a parsed one is rebuilt from `RestoreKind.script`, and one whose image
/// failed to parse keeps its bytes as the only copy.
bool dropCapturedFile(
  String name, {
  required bool includeFiles,
  required bool archivedSemantically,
}) =>
    archivedSemantically ||
    isSubscriptionTable(name) ||
    isDevicePersistence(name) ||
    (!includeFiles && backupFileKind(name) != 'Script');

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

  // Scripts are semantic, so the file read always runs; the "include device files" option is
  // applied below to the raw files only. A mutable copy: the semantic pass drops entries, and
  // `_captureFiles` returns a const list when the file table is empty, which cannot be modified.
  final files = <BackupFile>[
    ...await _captureFiles(deviceId, maxFileBytes, includeFiles: includeFiles),
  ];

  // Scripts: decode every SCR_XXX file into the semantic form and DROP the binary copy. A file is
  // archived only when it has no semantic form; the restore rebuilds it from that form
  // (`BackupScript.toDraft().toImage()`) and skips a matching file item, so keeping both would
  // store the same script twice and let one overwrite the other. A file that fails to parse keeps
  // its bytes, which is the right fallback: the binary is then the last good copy.
  final scripts = <BackupScript>[];
  final semanticallyArchived = <String>{};
  for (final file in files) {
    if (file.kind != 'Script') continue;
    final slot = scriptFileId(file.name);
    if (slot == null) continue;
    try {
      scripts.add(BackupScript.fromImage(slot, file.bytes));
      semanticallyArchived.add(file.name);
    } catch (error) {
      AppDiagnostics.log('backup', 'script ${file.name} parse failed: $error');
    }
  }
  // Keep only files that have no semantic form. Device-owned persistence (a dotted name - `.SV`,
  // `.SNREG`, `.DT_xx`/`.DV_xx`, `.TABLE`) is dropped too: the firmware writes it itself and the
  // restore rebuilds it semantically, so a raw copy would let a foreign device's persistent space
  // be written over a target. LAY_1 and user files have no semantic form and are still archived.
  files.removeWhere(
    (f) => dropCapturedFile(
      f.name,
      includeFiles: includeFiles,
      archivedSemantically: semanticallyArchived.contains(f.name),
    ),
  );

  // Subscriptions (requester is restorable; provider is informational).
  final subClient = SubscriptionClient(deviceId: deviceId);
  final requesters = <BackupRequesterSubscription>[];
  final providers = <BackupProviderSubscription>[];
  try {
    for (final s in await subClient.getRequesterSubscriptions()) {
      requesters.add(
        BackupRequesterSubscription(
          provider: idToString(s.providerAddr),
          trigger: s.trigger.label,
          periodMs: s.periodMs,
          minTimeMs: s.minTimeMs,
          deadzone: s.deadzone,
          source: _blockRef(s.sourceReg),
          target: _blockRef(s.targetReg),
        ),
      );
    }
  } catch (error) {
    AppDiagnostics.log('backup', 'requester subscriptions read failed: $error');
  }
  try {
    for (final s in await subClient.getProviderSubscriptions()) {
      providers.add(
        BackupProviderSubscription(
          requester: idToString(s.requesterAddr),
          trid: s.trid,
          trigger: s.trigger.label,
          periodMs: s.periodMs,
          minTimeMs: s.minTimeMs,
          deadzone: s.deadzone,
          source: _blockRef(s.sourceReg),
        ),
      );
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

Future<List<BackupFile>> _captureFiles(
  int deviceId,
  int maxFileBytes, {
  bool includeFiles = true,
}) async {
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
    // A script is read whatever the file option says; a raw device file is not read when the
    // caller did not ask for device files.
    if (!readCapturedFile(name, includeFiles: includeFiles)) continue;
    if (record.size > maxFileBytes) {
      AppDiagnostics.log(
        'backup',
        'skipping large file $name (${record.size} B)',
      );
      continue;
    }
    try {
      final bytes = await storage.readFile(name, size: record.size);
      if (bytes == null) continue;
      files.add(
        BackupFile(
          name: name,
          kind: backupFileKind(name),
          data: base64Encode(bytes),
        ),
      );
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
