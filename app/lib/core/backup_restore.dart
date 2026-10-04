part of 'backup.dart';

class LiveEntry {
  final int field;
  final int key;
  final String fieldName;
  final String keyName;
  final ValueInfo meta;
  final FieldInfo? info;

  LiveEntry({
    required this.field,
    required this.key,
    required this.fieldName,
    required this.keyName,
    required this.meta,
    this.info,
  });

  bool get readOnly => meta.readOnly;
}

class LiveBlock {
  final int type;
  final int instance;
  final String name;
  final bool isDynamic;
  final List<LiveEntry> entries;

  LiveBlock({
    required this.type,
    required this.instance,
    required this.name,
    required this.isDynamic,
    required this.entries,
  });

  String get typeWord => blockTypeWord(type);
}

/// A live device snapshot for restore matching.
class LiveDevice {
  final int id;
  final String name;
  final String? serial;
  final int capabilities;
  final List<LiveBlock> blocks;

  LiveDevice({
    required this.id,
    required this.name,
    this.serial,
    this.capabilities = 0,
    required this.blocks,
  });

  LiveBlock? findBlock(BackupBlock source) {
    if (source.typeIndex == systemBlockTypeValue) {
      return blocks.where((b) => b.type == systemBlockTypeValue).firstOrNull;
    }
    final sameType = blocks.where((b) => b.type == source.typeIndex).toList();
    if (sameType.isEmpty) return null;
    return sameType.where((b) => b.instance == source.instance).firstOrNull ??
        sameType
            .where((b) => b.name.toLowerCase() == source.name.toLowerCase())
            .firstOrNull ??
        sameType.first;
  }

  /// Resolves a semantic subscription address to a live BlockInfo, or null.
  int? resolveRef(BackupBlockRef ref) {
    final block = blocks.where((b) => b.type == ref.typeIndex).firstOrNull ??
        blocks.where((b) => b.typeWord == ref.block).firstOrNull;
    if (block == null) return null;
    LiveEntry? entry;
    for (final e in block.entries) {
      if (e.fieldName == ref.field && e.keyName == ref.key) {
        entry = e;
        break;
      }
    }
    entry ??= block.entries
        .where((e) => e.field == ref.fieldIndex && e.key == ref.keyIndex)
        .firstOrNull;
    if (entry == null) return null;
    return makeBlockInfo(block.type, block.instance, entry.field, entry.key);
  }
}

/// Reads a live device's block/entry layout (metadata only, no values).
Future<LiveDevice?> readLiveDevice(int deviceId) async {
  final entry = DeviceDatabase.instance.byId(deviceId);
  final reg = RegisterClient(deviceId: deviceId);
  final walked = await walkDeviceBlocks(reg);
  if (walked == null) return null;
  final live = [for (final b in walked) liveBlockFromVisited(b)];
  return LiveDevice(
    id: deviceId,
    name: entry?.displayName ?? 'Device ${idToString(deviceId)}',
    serial: entry?.serialNumber,
    capabilities: entry?.capabilities ?? 0,
    blocks: live,
  );
}

/// Resolves the bytes to write for a source entry against a live target entry,
/// or null when the target is incompatible (wrong type, unknown enum name, ...).
List<int>? resolveEntryBytes(BackupEntry source, LiveEntry target) {
  final sourceType = dataTypeFromWord(source.type);
  if (sourceType == null) return null;
  final targetType = target.meta.dataType;
  final text = (sourceType == DataType.string ||
          sourceType == DataType.filename) &&
      (targetType == DataType.string || targetType == DataType.filename);
  if (!text && sourceType != targetType) return null;
  return decodeSemantic(targetType, source.value,
      info: target.info, size: target.meta.size);
}

// ---------------------------------------------------------------------------
// Restore plan
// ---------------------------------------------------------------------------

enum RestoreKind { entry, script, subscription, sndb, file }

/// A resolved restore target for one source item.
class RestoreItem {
  final RestoreKind kind;
  final BackupDevice device;
  final BackupBlock? block;
  final BackupEntry? entry;
  final BackupScript? script;
  final BackupRequesterSubscription? subscription;
  final BackupSndbEntry? sndbEntry;
  final BackupFile? file;

  bool selected;
  LiveDevice? targetDevice;
  LiveBlock? targetBlock;
  LiveEntry? targetEntry;
  int? targetRef; // resolved source/target register address for subscriptions
  String? issue;

  RestoreItem({
    required this.kind,
    required this.device,
    this.block,
    this.entry,
    this.script,
    this.subscription,
    this.sndbEntry,
    this.file,
    this.selected = true,
  });

  bool get ready => issue == null;
}

/// A restore plan: every source item with a resolved live target and an optional
/// issue explaining why it cannot be restored.
class RestorePlan {
  final List<BackupDevice> devices;
  final List<LiveDevice> liveDevices;
  final List<RestoreItem> items;
  final Map<int, LiveDevice> deviceTargets; // source device id -> live device
  final Map<String, LiveBlock> blockTargets; // "deviceId:type:instance" -> live block

  RestorePlan({
    required this.devices,
    required this.liveDevices,
    required this.items,
    required this.deviceTargets,
    required this.blockTargets,
  });

  static String blockKey(int deviceId, BackupBlock block) =>
      '$deviceId:${block.typeIndex}:${block.instance}';

  List<LiveBlock> compatibleBlocks(LiveDevice device, BackupBlock source) =>
      device.blocks.where((b) => b.type == source.typeIndex).toList();

  int get selectedCount => items.where((i) => i.selected && i.ready).length;
  int get issueCount => items.where((i) => i.issue != null).length;

  /// Re-resolves every item against the current [deviceTargets].
  void resolve() {
    for (final item in items) {
      item.targetDevice = deviceTargets[item.device.id];
      item.targetBlock = null;
      item.targetEntry = null;
      item.targetRef = null;
      item.issue = null;
      final target = item.targetDevice;
      if (target == null) {
        item.issue = 'No target device';
        continue;
      }
      switch (item.kind) {
        case RestoreKind.entry:
          final entry = item.entry!;
          if (entry.readOnly) {
            item.issue = 'Read Only';
            break;
          }
          final override = blockTargets[blockKey(item.device.id, item.block!)];
          item.targetBlock = override ?? target.findBlock(item.block!);
          if (item.targetBlock == null) {
            item.issue = 'No compatible block';
            break;
          }
          item.targetEntry = _matchEntry(entry, item.targetBlock!);
          if (item.targetEntry == null) {
            item.issue = 'No matching entry';
          } else if (item.targetEntry!.readOnly) {
            item.issue = 'Read Only';
          } else if (resolveEntryBytes(entry, item.targetEntry!) == null) {
            item.issue = 'Incompatible value';
          }
        case RestoreKind.script:
          if (target.capabilities & Capability.scripts == 0) {
            item.issue = 'No script support';
          }
        case RestoreKind.subscription:
          if (target.capabilities & Capability.subscriptionRequest == 0) {
            item.issue = 'No subscription support';
          } else {
            final s = item.subscription!;
            if (target.resolveRef(s.source) == null ||
                target.resolveRef(s.target) == null) {
              item.issue = 'Address not found';
            }
          }
        case RestoreKind.sndb:
          if (target.capabilities & Capability.core == 0) {
            item.issue = 'Not a core';
          }
        case RestoreKind.file:
          // The target must advertise the StorageFiles capability (full file
          // create/delete/rename/resize).
          if (target.capabilities & Capability.storageFiles == 0) {
            item.issue = 'No file support';
          }
          break;
      }
    }
  }

  LiveEntry? _matchEntry(BackupEntry entry, LiveBlock block) {
    for (final e in block.entries) {
      if (e.fieldName == entry.field && e.keyName == entry.key) return e;
    }
    for (final e in block.entries) {
      if (e.field == entry.fieldIndex && e.key == entry.keyIndex) return e;
    }
    return null;
  }
}

/// Builds a restore plan for [devices] against the currently known devices.
Future<RestorePlan> buildRestorePlan(List<BackupDevice> devices) async {
  final live = <LiveDevice>[];
  for (final device in DeviceDatabase.instance.all) {
    if (device.stale) continue;
    final snapshot = await readLiveDevice(device.id);
    if (snapshot != null) live.add(snapshot);
  }

  final targets = <int, LiveDevice>{};
  for (final source in devices) {
    LiveDevice? match;
    if (source.serial != null) {
      match = live.where((d) => d.serial == source.serial).firstOrNull;
    }
    match ??= live.where((d) => d.id == source.id).firstOrNull;
    match ??= live
        .where((d) => d.name.toLowerCase() == source.name.toLowerCase())
        .firstOrNull;
    match ??= live.where((d) => d.blocks.any((b) =>
        b.type == source.blocks.firstOrNull?.typeIndex)).firstOrNull;
    match ??= live.firstOrNull;
    if (match != null) targets[source.id] = match;
  }

  final items = <RestoreItem>[];
  for (final device in devices) {
    for (final block in device.blocks) {
      for (final entry in block.entries) {
        items.add(RestoreItem(
            kind: RestoreKind.entry, device: device, block: block, entry: entry));
      }
    }
    for (final script in device.scripts) {
      items.add(RestoreItem(kind: RestoreKind.script, device: device, script: script));
    }
    for (final sub in device.requesterSubscriptions) {
      items.add(RestoreItem(
          kind: RestoreKind.subscription, device: device, subscription: sub));
    }
    for (final sndb in device.sndb) {
      items.add(RestoreItem(kind: RestoreKind.sndb, device: device, sndbEntry: sndb));
    }
    for (final file in device.files) {
      // Scripts are restored semantically (`RestoreKind.script`); restoring the raw
      // SCR_ file too would let it overwrite the rebuilt image.
      if (file.kind == 'Script') continue;
      items.add(RestoreItem(kind: RestoreKind.file, device: device, file: file));
    }
  }

  final plan = RestorePlan(
    devices: devices,
    liveDevices: live,
    items: items,
    deviceTargets: targets,
    blockTargets: {},
  );
  plan.resolve();
  // Default selection: restorable items; read-only entries stay informational.
  for (final item in plan.items) {
    item.selected = item.ready;
  }
  return plan;
}

/// Applies the ready + selected items of [plan]. Returns the number of written
/// items and failures.
Future<({int written, int failed})> applyRestorePlan(RestorePlan plan) async {
  var written = 0;
  var failed = 0;
  final register = <int, RegisterClient>{};
  final storage = <int, StorageClient>{};
  final subscriptions = <int, SubscriptionClient>{};
  final subIndex = <int, int>{};
  final sndbWritten = <String>{};

  void done(bool ok) {
    if (ok) {
      written++;
    } else {
      failed++;
    }
  }

  for (final item in plan.items) {
    if (!item.selected || !item.ready) continue;
    final target = item.targetDevice!;
    switch (item.kind) {
      case RestoreKind.entry:
        final reg = register.putIfAbsent(
            target.id, () => RegisterClient(deviceId: target.id));
        final bytes = resolveEntryBytes(item.entry!, item.targetEntry!);
        if (bytes == null) {
          failed++;
          break;
        }
        final meta = ValueInfo(
          type: item.targetEntry!.meta.type,
          flags: item.targetEntry!.meta.flags,
          key: item.targetEntry!.key,
          size: bytes.length,
        );
        final block = item.targetBlock!;
        if (block.isDynamic) {
          final dyn = DynBlock(
              index: block.instance,
              meta: ValueInfo(type: block.type),
              name: block.name);
          done(await reg.writeDynamicEntry(
                  dyn, item.targetEntry!.field, item.targetEntry!.key, meta, bytes) !=
              null);
        } else {
          done(await reg.writeBlockField(block.type, block.instance,
                  item.targetEntry!.field, item.targetEntry!.key, meta, bytes) !=
              null);
        }
      case RestoreKind.script:
        final store = storage
            .putIfAbsent(target.id, () => StorageClient(deviceId: target.id));
        final slot = item.script!.slot;
        final name = scriptFileName(slot);
        try {
          done(await store.writeFile(name, item.script!.toDraft().toImage()));
        } catch (error) {
          AppDiagnostics.log('backup', 'script ${item.script!.functionName} failed: $error');
          failed++;
        }
      case RestoreKind.subscription:
        final sub = subscriptions.putIfAbsent(
            target.id, () => SubscriptionClient(deviceId: target.id));
        final s = item.subscription!;
        final source = target.resolveRef(s.source);
        final targetReg = target.resolveRef(s.target);
        final provider = idFromString(s.provider);
        if (source == null || targetReg == null || provider == null) {
          failed++;
          break;
        }
        final index = subIndex[target.id] ?? 0;
        final entry = RequesterSubscription(
          index: index,
          providerAddr: provider,
          trid: 0,
          targetReg: targetReg,
          sourceReg: source,
          trigger: TriggerType.fromWord(s.trigger) ?? TriggerType.periodic,
          periodMs: s.periodMs,
          minTimeMs: s.minTimeMs,
          deadzone: s.deadzone,
        );
        final ok = await sub.setRequesterSubscription(index, entry: entry);
        if (ok) subIndex[target.id] = index + 1;
        done(ok);
      case RestoreKind.sndb:
        final e = item.sndbEntry!;
        final key = '${e.serial}:${e.address}';
        if (!sndbWritten.add(key)) {
          break; // the SNDB is global; write each entry once
        }
        final id = idFromString(e.address);
        done(id != null &&
            await DeviceDatabase.instance.sndbWrite(unhexBytes(e.serial), id));
      case RestoreKind.file:
        final store = storage
            .putIfAbsent(target.id, () => StorageClient(deviceId: target.id));
        if (item.file!.name == '.TABLE') break;
        try {
          done(await store.writeFile(item.file!.name, item.file!.bytes));
        } catch (error) {
          AppDiagnostics.log('backup', 'file ${item.file!.name} failed: $error');
          failed++;
        }
    }
  }
  return (written: written, failed: failed);
}
