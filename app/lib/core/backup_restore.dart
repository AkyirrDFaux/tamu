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

/// The live entry matching a semantic (field/key) address: by field/key NAME first, then by
/// the raw field/key INDEX. Shared by [LiveDevice.resolveRef] and [RestorePlan._matchEntry] so
/// the two cannot drift.
LiveEntry? _matchLiveEntry(
  List<LiveEntry> entries, {
  required String fieldName,
  required String keyName,
  required int field,
  required int key,
}) {
  for (final e in entries) {
    if (e.fieldName == fieldName && e.keyName == keyName) return e;
  }
  for (final e in entries) {
    if (e.field == field && e.key == key) return e;
  }
  return null;
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

  String get typeWord => blockTypeDisplayName(type);
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
    // Match the type AND the instance: a bank of blocks shares one type (the DAS's resistive
    // channels, the eyes), so matching the type alone always resolves to the first instance.
    var block = blocks
        .where((b) => b.type == ref.typeIndex && b.instance == ref.instance)
        .firstOrNull;
    // A ref may name a banked block by its service word (a capture made before the banked
    // types, or a descriptor carrying the app's marker): match the word AND the instance, so
    // two blocks of one kind - the two eyes - never swap.
    block ??= blocks
        .where((b) => b.typeWord == ref.block && b.instance == ref.instance)
        .firstOrNull;
    block ??= blocks.where((b) => b.typeWord == ref.block).firstOrNull;
    if (block == null) return null;
    final entry = _matchLiveEntry(
      block.entries,
      fieldName: ref.field,
      keyName: ref.key,
      field: ref.fieldIndex,
      key: ref.keyIndex,
    );
    if (entry == null) return null;
    return makeBlockInfo(block.type, block.instance, entry.field, entry.key);
  }
}

/// The live device a subscription's provider address names, or null.
///
/// The stored `provider` is a WIRE address (net << 10 | node), while [LiveDevice.id] is the
/// database id, whose low 10 bits carry the same node number - so the two must be compared as
/// node numbers. Comparing the raw values never matches: node 3 against address 1027 ("1.3").
LiveDevice? providerDeviceFor(List<LiveDevice> live, String providerAddress) {
  final id = idFromString(providerAddress);
  if (id == null) return null;
  final node = idDevice(id);
  return live.where((d) => idDevice(d.id) == node).firstOrNull;
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
  final text =
      (sourceType == DataType.text ||
          sourceType == DataType.name ||
          sourceType == DataType.filename) &&
      (targetType == DataType.text ||
          targetType == DataType.name ||
          targetType == DataType.filename);
  if (!text && sourceType != targetType) return null;
  return decodeSemantic(
    targetType,
    source.value,
    info: target.info,
    size: target.meta.size,
  );
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
  String? issue;

  /// The validated write payload computed during [RestorePlan.resolve] and reused at write
  /// time; cleared whenever the plan re-resolves. The computation is deterministic, so caching
  /// it cannot change the result.
  List<int>? resolvedBytes;

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
  final Map<String, LiveBlock>
  blockTargets; // "deviceId:type:instance" -> live block

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
      item.resolvedBytes = null;
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
          } else {
            // Validate the value now and keep the bytes for the write, so `applyRestorePlan`
            // does not recompute the same decode.
            final bytes = resolveEntryBytes(entry, item.targetEntry!);
            if (bytes == null) {
              item.issue = 'Incompatible value';
            } else {
              item.resolvedBytes = bytes;
            }
          }
        case RestoreKind.script:
          if (target.capabilities & Capability.scripts == 0) {
            item.issue = 'No script support';
          }
        case RestoreKind.subscription:
          if (target.capabilities & Capability.subscriptionRequest == 0) {
            item.issue = 'No subscription support';
          } else {
            // A requester entry's source lives on the PROVIDER device and its target on the
            // requester, so each ref resolves against its own device.
            final s = item.subscription!;
            final provider = providerDeviceFor(liveDevices, s.provider);
            if (provider == null ||
                provider.resolveRef(s.source) == null ||
                target.resolveRef(s.target) == null) {
              item.issue = 'Address not found';
            }
          }
        case RestoreKind.sndb:
          if (target.capabilities & Capability.core == 0) {
            item.issue = 'Not a core';
          }
        case RestoreKind.file:
          // Every device has storage files, so there is nothing to gate on.
          break;
      }
    }
  }

  LiveEntry? _matchEntry(BackupEntry entry, LiveBlock block) => _matchLiveEntry(
    block.entries,
    fieldName: entry.field,
    keyName: entry.key,
    field: entry.fieldIndex,
    key: entry.keyIndex,
  );
}

/// The live device a source device restores into: serial first (exact identity), then the
/// id, the name, a device carrying the source's first block type, and finally any live device.
LiveDevice? matchLiveDevice(BackupDevice source, List<LiveDevice> live) {
  if (source.serial != null) {
    final bySerial = live.where((d) => d.serial == source.serial).firstOrNull;
    if (bySerial != null) return bySerial;
  }
  return live.where((d) => d.id == source.id).firstOrNull ??
      live
          .where((d) => d.name.toLowerCase() == source.name.toLowerCase())
          .firstOrNull ??
      live
          .where(
            (d) => d.blocks.any(
              (b) => b.type == source.blocks.firstOrNull?.typeIndex,
            ),
          )
          .firstOrNull ??
      live.firstOrNull;
}

/// Walks every non-stale device into a live snapshot, in device order.
///
/// Sequential on purpose: the RS bus is half-duplex and the connection layer serialises
/// requests, so concurrent reads would not overlap.
Future<List<LiveDevice>> _readLiveDevices() async {
  final live = <LiveDevice>[];
  for (final device in DeviceDatabase.instance.all) {
    if (device.stale) continue;
    final snapshot = await readLiveDevice(device.id);
    if (snapshot != null) live.add(snapshot);
  }
  return live;
}

/// Builds a restore plan for [devices] against the currently known devices.
Future<RestorePlan> buildRestorePlan(List<BackupDevice> devices) async {
  final live = await _readLiveDevices();

  final targets = <int, LiveDevice>{};
  for (final source in devices) {
    final match = matchLiveDevice(source, live);
    if (match != null) targets[source.id] = match;
  }

  final items = <RestoreItem>[];
  for (final device in devices) {
    for (final block in device.blocks) {
      for (final entry in block.entries) {
        items.add(
          RestoreItem(
            kind: RestoreKind.entry,
            device: device,
            block: block,
            entry: entry,
          ),
        );
      }
    }
    for (final script in device.scripts) {
      items.add(
        RestoreItem(kind: RestoreKind.script, device: device, script: script),
      );
    }
    for (final sub in device.requesterSubscriptions) {
      items.add(
        RestoreItem(
          kind: RestoreKind.subscription,
          device: device,
          subscription: sub,
        ),
      );
    }
    for (final sndb in device.sndb) {
      items.add(
        RestoreItem(kind: RestoreKind.sndb, device: device, sndbEntry: sndb),
      );
    }
    for (final file in device.files) {
      // Scripts are restored semantically (`RestoreKind.script`); restoring the raw
      // SCR_ file too would let it overwrite the rebuilt image.
      if (file.kind == 'Script') continue;
      items.add(
        RestoreItem(kind: RestoreKind.file, device: device, file: file),
      );
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

/// Creates the dynamic blocks a plan's targets are missing, filling their entry structure from
/// the backup, so their values can then be restored - a restore writes entries into a block
/// that must already exist. A device wiped by a full flash carries none of its dynamic blocks,
/// and without this none of their values would restore.
///
/// Re-reads the live devices and re-points the plan afterwards, so it resolves against the
/// blocks that now exist; newly restorable items join the default selection. A block that
/// cannot be created is left alone (its entries stay unready). Returns how many were created.
Future<int> prepareDynamicBlocks(RestorePlan plan) async {
  var created = 0;
  for (final source in plan.devices) {
    final target = plan.deviceTargets[source.id];
    if (target == null) continue;
    final reg = RegisterClient(deviceId: target.id);
    for (final block in source.blocks) {
      if (!isDynamicType(block.typeIndex)) continue;
      if (target.findBlock(block) != null) continue;
      final global =
          ((block.typeIndex - dynamicTypeBase) << 6) | block.instance;
      final dyn = DynBlock(
        index: global,
        meta: ValueInfo(type: block.typeIndex),
        name: block.name,
      );
      if (await reg.createDynamicBlock(block.name, index: global) == null) {
        continue;
      }
      // Recreate the block's field/key structure. `appendDynamicEntry` writes through a
      // BlockInfo whose key is always 0, so it creates one entry per FIELD; a keyed entry
      // (the eyes have several keys per field) has to go through `writeDynamicEntry`, which
      // carries the key in its address.
      final seen = <String>{};
      final entries = [...block.entries]
        ..sort(
          (a, b) => a.fieldIndex == b.fieldIndex
              ? a.keyIndex.compareTo(b.keyIndex)
              : a.fieldIndex.compareTo(b.fieldIndex),
        );
      for (final entry in entries) {
        if (!seen.add('${entry.fieldIndex}:${entry.keyIndex}')) continue;
        final type = dataTypeFromWord(entry.type);
        if (type == null) continue;
        final meta = ValueInfo(
          type: type.value,
          key: entry.keyIndex,
          size: entry.size,
          // The flags decide whether the value reaches the .DV persistent space (and so
          // survives the boot recall): dropping them restores the structure but loses every
          // value on the next reboot.
          flags: flagsFromWords(entry.flags),
        );
        final zero = List<int>.filled(entry.size, 0);
        if (entry.keyIndex == 0) {
          await reg.appendDynamicEntry(
            dyn,
            meta,
            zero,
            index: entry.fieldIndex,
          );
        } else {
          await reg.writeDynamicEntry(
            dyn,
            entry.fieldIndex,
            entry.keyIndex,
            meta,
            zero,
          );
        }
      }
      created++;
    }
  }
  if (created == 0) return 0;

  final live = await _readLiveDevices();
  plan.liveDevices
    ..clear()
    ..addAll(live);
  plan.deviceTargets.clear();
  for (final source in plan.devices) {
    final match = matchLiveDevice(source, live);
    if (match != null) plan.deviceTargets[source.id] = match;
  }
  final readyBefore = {for (final item in plan.items) item: item.ready};
  plan.resolve();
  for (final item in plan.items) {
    if (item.ready && !(readyBefore[item] ?? false)) item.selected = true;
  }
  return created;
}

/// Applies the ready + selected items of [plan]. Returns the number of written
/// items and failures.
Future<({int written, int failed, List<String> failures})> applyRestorePlan(
  RestorePlan plan,
) async {
  // A dynamic block's values can only be written into a block that exists.
  await prepareDynamicBlocks(plan);

  var written = 0;
  var failed = 0;
  final failures = <String>[];
  final register = <int, RegisterClient>{};
  final storage = <int, StorageClient>{};
  final subscriptions = <int, SubscriptionClient>{};
  final subIndex = <int, int>{};
  final sndbWritten = <String>{};
  // One write per distinct (provider, source, target): an archive can carry the same
  // subscription twice (a device that already held a duplicate captures it), and writing both
  // leaves two entries racing for one source with the second never firing.
  final seenSubKeys = <String>{};

  String subKey(int providerAddr, int sourceReg, int targetReg) =>
      '${idDevice(providerAddr)}:$sourceReg:$targetReg';

  // A restore is a snapshot: a requester subscription the target carries but the archive does
  // not name is cancelled, so the device ends with exactly the archive's set. Left behind, an
  // old entry and the restored one race for the same source and one of them goes inert.
  final wantedSubs = <int, Set<String>>{};
  for (final item in plan.items) {
    if (item.kind != RestoreKind.subscription || !item.ready) continue;
    final t = item.targetDevice;
    final s = item.subscription;
    if (t == null || s == null) continue;
    final providerId = idFromString(s.provider);
    final providerDev = providerDeviceFor(plan.liveDevices, s.provider);
    final src = providerDev?.resolveRef(s.source);
    final tgt = t.resolveRef(s.target);
    if (providerId == null || src == null || tgt == null) continue;
    (wantedSubs[t.id] ??= <String>{}).add(subKey(providerId, src, tgt));
  }

  // One item could not be written; record what it was so a run can be diagnosed.
  void failItem(RestoreItem item, String why) {
    failed++;
    if (failures.length < 100) {
      failures.add('${item.device.name}: ${item.kind.name} $why');
    }
  }

  void done(bool ok, RestoreItem item, String what) {
    if (ok) {
      written++;
    } else {
      failItem(item, what);
    }
  }

  String describe(RestoreItem i) => switch (i.kind) {
    RestoreKind.entry => '${i.block?.name}/${i.entry?.field}[${i.entry?.key}]',
    RestoreKind.script => 'script slot ${i.script?.slot}',
    RestoreKind.subscription => 'from ${i.subscription?.provider}',
    RestoreKind.sndb => 'sndb ${i.sndbEntry?.serial}',
    RestoreKind.file => 'file ${i.file?.name}',
  };

  // Files first: an entry may name one (the LED display's layout file name is only accepted
  // once the file exists), then the block entries, then the rest.
  int order(RestoreKind kind) => switch (kind) {
    RestoreKind.file => 0,
    RestoreKind.sndb => 1,
    RestoreKind.script => 2,
    RestoreKind.entry => 3,
    RestoreKind.subscription => 4,
  };
  final ordered = [...plan.items]
    ..sort((a, b) => order(a.kind).compareTo(order(b.kind)));

  // Cancel what the archive does not name, before writing the archive's own entries.
  for (final entry in wantedSubs.entries) {
    final targetId = entry.key;
    final client = subscriptions.putIfAbsent(
      targetId,
      () => SubscriptionClient(deviceId: targetId),
    );
    // Keep the first live entry per identity; cancel anything the archive does not name, and
    // any duplicate identity - a second entry behind the first never fires, which is how a
    // subscription "exists" yet never updates.
    final seenLive = <String>{};
    for (final live in await client.getRequesterSubscriptions()) {
      final key = subKey(live.providerAddr, live.sourceReg, live.targetReg);
      if (entry.value.contains(key) && seenLive.add(key)) continue;
      await client.cancelRequesterSubscription(live.trid);
    }
  }

  for (final item in ordered) {
    if (!item.selected || !item.ready) continue;
    final target = item.targetDevice!;
    switch (item.kind) {
      case RestoreKind.entry:
        final reg = register.putIfAbsent(
          target.id,
          () => RegisterClient(deviceId: target.id),
        );
        // The bytes were validated during resolve() and cached on the item.
        final bytes = item.resolvedBytes;
        if (bytes == null) {
          failItem(item, describe(item));
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
            name: block.name,
          );
          done(
            await reg.writeDynamicEntry(
                  dyn,
                  item.targetEntry!.field,
                  item.targetEntry!.key,
                  meta,
                  bytes,
                ) !=
                null,
            item,
            describe(item),
          );
        } else {
          done(
            await reg.writeBlockField(
                  block.type,
                  block.instance,
                  item.targetEntry!.field,
                  item.targetEntry!.key,
                  meta,
                  bytes,
                ) !=
                null,
            item,
            describe(item),
          );
        }
      case RestoreKind.script:
        final store = storage.putIfAbsent(
          target.id,
          () => StorageClient(deviceId: target.id),
        );
        final slot = item.script!.slot;
        final name = scriptFileName(slot);
        try {
          done(
            await store.writeFile(name, item.script!.toDraft().toImage()),
            item,
            describe(item),
          );
        } catch (error) {
          AppDiagnostics.log(
            'backup',
            'script ${item.script!.functionName} failed: $error',
          );
          failItem(item, describe(item));
        }
      case RestoreKind.subscription:
        final sub = subscriptions.putIfAbsent(
          target.id,
          () => SubscriptionClient(deviceId: target.id),
        );
        final s = item.subscription!;
        final provider = idFromString(s.provider); // the wire form, written back below
        final providerDev = providerDeviceFor(plan.liveDevices, s.provider);
        final source = providerDev?.resolveRef(s.source);
        final targetReg = target.resolveRef(s.target);
        if (source == null || targetReg == null || provider == null) {
          failItem(item, describe(item));
          break;
        }
        // One entry per distinct identity: the archive can name the same subscription twice
        // (captured from a device that already held a duplicate), and a second entry would sit
        // inert behind the first.
        if (!seenSubKeys.add(subKey(provider, source, targetReg))) {
          written++;
          break;
        }
        // A requester entry's identity is its TRID, so allocate it here instead of leaving it
        // to the client: the client picks "the lowest free" from a read of the table, and in a
        // restore that read does not yet show the entries written moments before, so every
        // entry would take the same TRID and overwrite the one before it.
        // A restore is a snapshot, so it must not stack a second entry beside an identical
        // one: an archive subscription naming the same (provider, source, target) reuses the
        // target's own entry and TRID. Writing a fresh TRID instead left two subscriptions
        // racing for one source, with one of them inert.
        final mine = await sub.getRequesterSubscriptions();
        final match = mine
            .where(
              (e) =>
                  idDevice(e.providerAddr) == idDevice(provider) &&
                  e.sourceReg == source &&
                  e.targetReg == targetReg,
            )
            .firstOrNull;
        final next = subIndex[target.id] ?? 0;
        final index = match?.index ?? next;
        final entry = RequesterSubscription(
          index: index,
          providerAddr: provider,
          trid: match?.trid ?? tridSubBase + next,
          targetReg: targetReg,
          sourceReg: source,
          trigger: TriggerType.fromWord(s.trigger) ?? TriggerType.periodic,
          periodMs: s.periodMs,
          minTimeMs: s.minTimeMs,
          deadzone: s.deadzone,
        );
        final ok = await sub.setRequesterSubscription(index, entry: entry);
        // Only a fresh entry consumes a slot; a reused one keeps its own index.
        if (ok && match == null) subIndex[target.id] = next + 1;
        done(ok, item, describe(item));
      case RestoreKind.sndb:
        final e = item.sndbEntry!;
        final key = '${e.serial}:${e.address}';
        if (!sndbWritten.add(key)) {
          break; // the SNDB is global; write each entry once
        }
        final id = idFromString(e.address);
        // A device is not an entry in its own SNDB (the firmware refuses it), so skip the
        // target's own row rather than counting it as a failure.
        if (id == null || id == target.id) break;
        done(
          await DeviceDatabase.instance.sndbWrite(unhexBytes(e.serial), id),
          item,
          describe(item),
        );
      case RestoreKind.file:
        final store = storage.putIfAbsent(
          target.id,
          () => StorageClient(deviceId: target.id),
        );
        if (item.file!.name == '.TABLE') break;
        try {
          done(
            await store.writeFile(item.file!.name, item.file!.bytes),
            item,
            describe(item),
          );
        } catch (error) {
          AppDiagnostics.log(
            'backup',
            'file ${item.file!.name} failed: $error',
          );
          failItem(item, describe(item));
        }
    }
  }
  return (written: written, failed: failed, failures: failures);
}
