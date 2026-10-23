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

/// The register address an archive ref names, encoded from the ARCHIVE's own block map.
///
/// A restore creates dynamic blocks and their entries as it writes, so a live snapshot taken
/// before those writes does not yet carry them and a ref into a dynamic block fails to resolve.
/// The item is then dropped as "Address not found" with no failure recorded, which silently
/// loses subscriptions - and whole eye blocks - when restoring onto a device that was empty.
/// The archive carries the address in full, so encode it from there; callers fall back to the
/// live lookup for anything the archive does not describe.
///
/// A structural row resolves here exactly like a value-carrying one: the address is built from the
/// row's identity (block type/instance, field, key), never from its value. That is why a dynamic
/// block's non-persistent rows stay in the archive without their values.
int? archiveRefReg(BackupDevice device, BackupBlockRef ref) {
  BackupBlock? block = device.blocks
      .where((b) => b.typeIndex == ref.typeIndex && b.instance == ref.instance)
      .firstOrNull;
  block ??= device.blocks
      .where((b) => b.type == ref.block && b.instance == ref.instance)
      .firstOrNull;
  block ??= device.blocks.where((b) => b.type == ref.block).firstOrNull;
  if (block == null) return null;
  BackupEntry? entry = block.entries
      .where(
        (e) => e.fieldIndex == ref.fieldIndex && e.keyIndex == ref.keyIndex,
      )
      .firstOrNull;
  entry ??= block.entries
      .where((e) => e.field == ref.field && e.key == ref.key)
      .firstOrNull;
  if (entry == null) return null;
  return makeBlockInfo(
    block.typeIndex,
    block.instance,
    entry.fieldIndex,
    entry.keyIndex,
  );
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

  /// The block types whose instance metadata read was lost, so this snapshot cannot say whether
  /// the device carries them ([BlocksRead.unreadTypes]). Such a type is UNREAD, not absent: a
  /// plan must not read it as the wrong device shape.
  final Set<int> unreadTypes;

  LiveDevice({
    required this.id,
    required this.name,
    this.serial,
    this.capabilities = 0,
    required this.blocks,
    this.unreadTypes = const {},
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

/// The live device id an archive address names, or null.
///
/// Addressing is local or exact, not an archive-private space. `0.N` is LOCAL: it names arc N on
/// the local net - the net the rig being restored onto actually runs - so one archive restores
/// onto a rig of any NetID. A `net.arc` address is EXACT, and is what a reference crossing a net
/// boundary has to use; it passes through unchanged. Local is the preferable form, so it must
/// resolve even when the archive does not carry the device it names.
int? resolveArchiveAddress(RestorePlan plan, String address) {
  final id = idFromString(address);
  if (id == null) return null;
  if (idNet(id) != 0) return id;
  // Net 0 is the local-net placeholder, never a real NetID: the System block's NetID trigger
  // rejects 0 outright. The local net is the one the plan's targets are on.
  final node = idDevice(id);
  final local = plan.deviceTargets.values.firstOrNull;
  if (node == 0 || local == null) return null;
  return (idNet(local.id) << 10) | node;
}

/// The identity of one requester entry: the provider's NODE, the source register on that provider
/// and the target register here.
///
/// A requester entry is keyed on the wire by its TRID, which a restore allocates and which nothing
/// in the archive carries - so TRID cannot answer "does the archive name this entry?". What the
/// archive does describe is the subscription itself, and these three fields are its identity. The
/// provider's ADDRESS carries the net it was written under while the firmware re-qualifies it at
/// every use (`SubscriptionsRequester.h`, RequesterProviderAddr), so the node is what identifies
/// the provider: comparing addresses would miss every entry set before a net change.
///
/// The restore writes and the prune read through this one helper, so "named by the archive" and
/// "written by this restore" cannot drift apart.
String requesterKey(int providerAddr, int sourceReg, int targetReg) =>
    '${idDevice(providerAddr)}:$sourceReg:$targetReg';

/// Why a live node has no snapshot, and who it said it was.
///
/// A node that answered its identity but not its block list is NOT absent from the rig: [reason]
/// names the read that failed, and [name]/[serial] are the identity it answered with. Carrying the
/// identity is what lets a plan recognise the archive device the node belongs to when the
/// archive's ADDRESS differs from the live one - a device re-addressed since the archive was
/// captured is the same device, and its unread snapshot must not read as a different rig.
typedef UnreadNode = ({String name, String? serial, String reason});

/// One device's live read: the snapshot, and - when the device answered but its block list did
/// not - why there is no snapshot, along with the identity it answered.
///
/// [device] is null both when the device did not answer a ping (it is not on the bus) and when
/// it answered its identity but not its block list. The two are not the same thing: only the
/// second carries [unread], so a plan can say "the block list was not read" instead of "the
/// target carries no `<Type>` block".
typedef LiveRead = ({LiveDevice? device, UnreadNode? unread});

/// Reads a live device's identity and block/entry layout (metadata only, no values).
///
/// The identity comes from the device itself, so a snapshot does not depend on the app's device
/// database having been refreshed first - and a `dart run` tool with no database at all reads
/// the same snapshot.
Future<LiveRead> readLiveDevice(int deviceId) async {
  const attempts = RegisterClient.snapshotReadAttempts;
  final identity = await readDeviceIdentity(deviceId, readAttempts: attempts);
  // No ping answer: the device is not on the bus, which is not a lost read.
  if (identity == null) return (device: null, unread: null);
  final reg = RegisterClient(deviceId: deviceId, readAttempts: attempts);
  final walked = await walkDeviceBlocks(reg);
  if (walked == null) {
    return (
      device: null,
      unread: (
        name: identity.name,
        serial: identity.serial,
        reason:
            'the target ${idToString(deviceId)} did not answer its block list read '
            '($attempts attempts)',
      ),
    );
  }
  final live = [for (final b in walked.blocks) liveBlockFromVisited(b)];
  return (
    device: LiveDevice(
      id: deviceId,
      name: identity.name,
      serial: identity.serial,
      capabilities: identity.capabilities,
      blocks: live,
      unreadTypes: walked.unreadTypes,
    ),
    unread: null,
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

/// True when a backup file has no semantic form and must be written back from its raw bytes.
///
/// The requester-subscription table has its own semantic form (`RestoreKind.subscription`), a
/// Script file whose slot also carries a semantic script is rebuilt from `RestoreKind.script`, and
/// an LED layout whose file also carries a semantic layout is rebuilt from that layout - for all
/// three, the raw bytes are not the authoritative copy. Every other file is restored as-is,
/// including a Script or a layout whose image failed to parse: it has no semantic form, so its
/// bytes are the only copy and were being dropped.
bool restoresAsRawFile(BackupDevice device, BackupFile file) {
  if (isSubscriptionTable(file.name)) return false;
  if (device.layouts.any((l) => _sameFile(l.file, file.name))) return false;
  if (file.kind != 'Script') return true;
  final slot = scriptFileId(file.name);
  return slot == null || !device.scripts.any((s) => s.slot == slot);
}

/// True when two names are the same storage file, ignoring the 8-byte wire padding a device-side
/// name may carry (`LAY_1` vs `LAY_1   ` vs `LAY_1\0\0\0`).
bool _sameFile(String a, String b) =>
    normalizeFileName(a).toUpperCase() == normalizeFileName(b).toUpperCase();

/// The issue text for an entry the restore skips BY DESIGN - a read-only register the device owns
/// (its identity, its live readings). It is informational, not a lost item, so the apply never
/// counts it as a failure; every other issue means an item the restore was asked to write.
const readOnlyIssue = 'Read Only';

/// True when a captured entry is a row the restore PLANS a value write for.
///
/// Two kinds of row are not: a STRUCTURAL row (a dynamic block's non-persistent entry - see
/// [BackupEntry.structural]) carries no value to write, and a reserved placeholder carries neither
/// a type nor a value. Neither is a restore item: a structural row exists so the block's structure
/// can be rebuilt by [prepareDynamicBlocks] from the same rows, and the firmware resets its value
/// to the default at boot anyway. Shared with the tools that reproduce the plan's item list, so a
/// tool cannot drift from the plan.
bool restoresEntryValue(BackupEntry entry) =>
    !entry.structural &&
    !(entry.value == null && dataTypeFromWord(entry.type) == DataType.none);

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

  /// The live nodes that answered their identity but not their block list, by live node id: why
  /// there is no snapshot, and the identity each one answered with. A source device that matches
  /// one of these was NOT absent from the rig - its snapshot is incomplete, and a plan must say
  /// that rather than let the missing target read as the wrong device shape.
  final Map<int, UnreadNode> unreadDevices;

  RestorePlan({
    required this.devices,
    required this.liveDevices,
    required this.items,
    required this.deviceTargets,
    required this.blockTargets,
    this.unreadDevices = const {},
  });

  /// Why [source]'s live node did not answer its block list, or null when no node that answered
  /// its identity is [source].
  ///
  /// The ladder is [matchLiveDevice]'s as far as an unread node can support it: the serial, then
  /// the address, then the name. An unread node carries no blocks, so there is no block-type step
  /// - and no "any remaining device" last resort either, because blaming a node that is not
  /// [source] would name the wrong read. The identity steps are the point: an archive captured at
  /// one address and restored onto the same device at another (re-addressed since the capture)
  /// printed `NO TARGET` for a node that had just answered, which is the same misdiagnosis of a
  /// failed read as a wrong shape that the unread reason exists to remove.
  String? unreadReasonFor(BackupDevice source) {
    if (source.serial != null) {
      for (final node in unreadDevices.values) {
        if (node.serial == source.serial) return node.reason;
      }
    }
    final byAddress = unreadDevices[source.id];
    if (byAddress != null) return byAddress.reason;
    for (final node in unreadDevices.values) {
      if (node.name.toLowerCase() == source.name.toLowerCase()) {
        return node.reason;
      }
    }
    return null;
  }

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
            item.issue = readOnlyIssue;
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
            item.issue = readOnlyIssue;
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
            // The target sits on the requester, inside a dynamic block the restore itself is
            // about to fill, so the live snapshot can predate it and the lookup fails.
            final targetReg =
                target.resolveRef(s.target) ??
                archiveRefReg(item.device, s.target);
            if (provider == null ||
                provider.resolveRef(s.source) == null ||
                targetReg == null) {
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

/// The ids of every device on the bus: the directly connected core, plus every node its SNDB
/// roster names (id 0 is the unassigned marker, never a device).
Future<List<int>> _networkNodeIds() async {
  final ids = <int>{coreId};
  for (final entry in await sndbEntries()) {
    if (entry.id != 0) ids.add(entry.id);
  }
  return ids.toList()..sort();
}

/// Walks every device on the bus into a live snapshot, in address order.
///
/// Read off the wire rather than from the app's device database, so a plan describes the rig as
/// it is now (and a `dart run` tool can build one). Sequential on purpose: the RS bus is
/// half-duplex and the connection layer serialises requests, so concurrent reads would not
/// overlap.
///
/// A node that answered its identity but not its block list is reported in `unread` instead of
/// being dropped from `live`: dropping it would make a live device look like one that is not on
/// the rig at all.
Future<({List<LiveDevice> live, Map<int, UnreadNode> unread})>
_readLiveDevices() async {
  final live = <LiveDevice>[];
  final unread = <int, UnreadNode>{};
  for (final id in await _networkNodeIds()) {
    final read = await readLiveDevice(id);
    final snapshot = read.device;
    if (snapshot != null) {
      live.add(snapshot);
    } else if (read.unread != null) {
      unread[id] = read.unread!;
    }
  }
  return (live: live, unread: unread);
}

/// Builds a restore plan for [devices] against the currently known devices.
Future<RestorePlan> buildRestorePlan(List<BackupDevice> devices) async {
  final read = await _readLiveDevices();
  final live = read.live;

  final targets = <int, LiveDevice>{};
  for (final source in devices) {
    final match = matchLiveDevice(source, live);
    if (match != null) targets[source.id] = match;
  }

  final items = <RestoreItem>[];
  for (final device in devices) {
    for (final block in device.blocks) {
      for (final entry in block.entries) {
        // A reserved placeholder carries neither a type nor a value, so there is nothing to
        // write; planning it would report it as an unready item on every restore forever.
        // A structural row (a dynamic block's non-persistent entry) is planned the same way -
        // not at all: it carries no value, its only job is the structure that
        // `prepareDynamicBlocks` rebuilds from these same rows.
        if (!restoresEntryValue(entry)) continue;
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
    // The provider entries the device serves are NOT restored: a provider entry is created by
    // the requester that owns it, and the restored requester re-registers itself (CID 0x01 Change
    // on the first tick after it is written, and again at boot), so the node's provider table is
    // rebuilt by that mechanism. Writing it here as well duplicated the work and its read-back
    // verification raced with the node's own table churn (B34).
    for (final sndb in device.sndb) {
      items.add(
        RestoreItem(kind: RestoreKind.sndb, device: device, sndbEntry: sndb),
      );
    }
    // A layout has a semantic form, so its bytes are rebuilt from the semantic map and written
    // through the SAME file item the raw path uses: the layout file is written before the block
    // entries, and the display's LayoutFile entry (an ordinary entry item) is written after it, so
    // the field's trigger loads the layout as it lands. A layout the rebuild cannot describe is
    // reported and skipped rather than written as a truncated map; the capture never produces one
    // (it only stores what the decoder accepted), so this is an edited-archive case.
    for (final layout in device.layouts) {
      try {
        items.add(
          RestoreItem(
            kind: RestoreKind.file,
            device: device,
            file: BackupFile(
              name: layout.file,
              kind: 'LED layout',
              data: base64Encode(layout.toImage()),
            ),
          ),
        );
      } on FormatException catch (error) {
        AppDiagnostics.log(
          'backup',
          'layout ${layout.file} of ${device.name} cannot be rebuilt: $error',
        );
      }
    }
    for (final file in device.files) {
      // A file is skipped only when it has a semantic form: a Script rebuilt from
      // `RestoreKind.script`, a layout rebuilt from `device.layouts`, or the requester-subscription
      // table from `RestoreKind.subscription` (the device regenerates its own `.SUBREQ` when it
      // saves). A Script or layout whose image failed to parse has no semantic form and is written
      // back from its raw bytes - which is how an archive captured before layouts had a semantic
      // form still restores its `LAY_1`.
      if (!restoresAsRawFile(device, file)) continue;
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
    unreadDevices: read.unread,
  );
  plan.resolve();
  // Default selection: restorable items; read-only entries stay informational.
  for (final item in plan.items) {
    item.selected = item.ready;
  }
  return plan;
}

/// Creates the dynamic structure a plan's targets are missing - the blocks when absent, and the
/// entries the backup carries that the device does not - so their values can then be restored: a
/// restore writes entries into a structure that must already exist. A device wiped by a full
/// flash carries none of its dynamic blocks; one brought up by the firmware carries the blocks
/// but only the fields that firmware creates. Without this, an entry at a field the device never
/// made has no live counterpart to match and is dropped as "No matching entry" with no failure
/// recorded.
///
/// The rows it creates are every row the archive carries for a dynamic block - a persistent one
/// (whose value is then written) AND a structural one (whose value the archive does not carry and
/// which therefore only ever lands as the entry's own default). The structure is the persistent
/// part of a dynamic block: its table is device state (`.DT_<hex2>`), so a restore onto a wiped
/// target has to build it from these rows or the block's addresses do not exist at all.
///
/// Re-reads the live devices and re-points the plan afterwards, so it resolves against the
/// structure that now exists; newly restorable items join the default selection. A structure that
/// cannot be created is left alone (its entries stay unready). Returns how many were created.
Future<int> prepareDynamicBlocks(RestorePlan plan) async {
  var created = 0;
  for (final source in plan.devices) {
    final target = plan.deviceTargets[source.id];
    if (target == null) continue;
    final reg = RegisterClient(deviceId: target.id);
    for (final block in source.blocks) {
      if (!isDynamicType(block.typeIndex)) continue;
      final existing = target.findBlock(block);
      final global =
          ((block.typeIndex - dynamicTypeBase) << 6) | block.instance;
      final dyn = DynBlock(
        index: global,
        meta: ValueInfo(type: block.typeIndex),
        name: block.name,
      );
      if (existing == null) {
        if (await reg.createDynamicBlock(block.name, index: global) == null) {
          continue;
        }
      }
      // Only what the device is MISSING is created here: the block when it is absent, and any
      // entry the backup carries that the device does not. A device brought up by the firmware
      // carries just the fields that firmware creates, so an entry at a low field - the
      // subscription outputs at field 0, the eyes at fields 0-2 - has no live counterpart to
      // match and the ordinary entry write drops it as "No matching entry" with no failure
      // recorded. Values are still written by the restore proper; this only makes the addresses
      // exist so they can be.
      final present = <String>{
        for (final e in existing?.entries ?? const <LiveEntry>[])
          '${e.field}:${e.key}',
      };
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
        // Already on the device: the ordinary value write will fill it.
        if (present.contains('${entry.fieldIndex}:${entry.keyIndex}')) continue;
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
        final added = entry.keyIndex == 0
            ? await reg.appendDynamicEntry(
                dyn,
                meta,
                zero,
                index: entry.fieldIndex,
              )
            : await reg.writeDynamicEntry(
                dyn,
                entry.fieldIndex,
                entry.keyIndex,
                meta,
                zero,
              );
        if (added == null) {
          // The entry the backup carries could not be created, so its value has no address to
          // land at either; leaving the item unready makes the apply report it as a failure,
          // instead of the failed create counting as a success and the entry vanishing.
          AppDiagnostics.log(
            'backup',
            'dynamic entry ${block.name}/${entry.field}[${entry.key}] '
                'not created',
          );
          continue;
        }
        created++;
      }
    }
  }
  if (created == 0) return 0;

  final read = await _readLiveDevices();
  final live = read.live;
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

/// Saves every device a restore has written to.
///
/// A write sits in RAM until the device is told to save. Without this the whole run works until
/// the next power cycle and then vanishes: the core recalls requester entries pointing at
/// providers the nodes no longer carry, a node forgets the provider entry that fed one, and a
/// device that carried no subscription entries at all loses every value it was just given. So
/// every device the run touched is saved, not only the ones that carried subscription tables.
Future<void> _persistWrites(Set<int> deviceIds) async {
  for (final id in deviceIds) {
    await RegisterClient(deviceId: id).saveAll();
  }
}

/// Whether a restored script is a function library.
///
/// A library is a FILE other scripts call into, not a program the device runs on its own: the
/// firmware links a global call by reading that file when the USING script loads
/// (`ScriptProgram.h`, link stage: `FileExists` + `ReadFromFile` + `ScriptPropFunctionLib`), and
/// `Docs/Services/Script.md` "Functions and Macros" asks only that the file be present on the
/// target. So a library never needs a slot, and must never hold one.
bool _isFunctionLibrary(RestoreItem item) =>
    item.script!.properties.contains('Function library');

/// Brings up the scripts a restore wrote and reports each one's outcome.
///
/// A restored script is only a stored file: the device runs what it loads, not what it holds. The
/// load-on-boot scripts come back at the next boot, and a script that faulted earlier STAYS in
/// Error until it is explicitly started - a restore does not reboot, so without this a restored
/// rig sits with its automation dead while the restore reports success. So a script this run wrote
/// that the target does NOT already hold in its own slot is loaded, after the libraries are
/// written, and the ones marked to run on load are started.
///
/// A slot that already holds THIS file is left alone: [reloadChangedScripts] has already reloaded
/// it when the image this run wrote differs from the one the device held, and when it does not, the
/// running script is exactly what the archive asks for - a restore must not stop an animation
/// nothing in it changed. That is also why this phase is not a blanket "unload and reload every
/// script": an unchanged loaded script keeps its slot, its state and its place in a running
/// sequence.
///
/// A function library is not loaded at all: it is used through its FILE (see
/// [_isFunctionLibrary]), so it is written and left as a file, and a slot one occupies - put
/// there by an earlier build that loaded every item, or by a user - is released, so the run
/// converges on "file present, slot empty" instead of leaving the library occupying a slot. The
/// items are the scripts THIS run wrote, so a script the user left out is never loaded over
/// whatever the device already holds; and a load that fails is returned with its reason, so the
/// apply can count it rather than let the file write stand for a script that never runs.
Future<List<({RestoreItem item, String? reason})>> _bringUpScripts(
  List<RestoreItem> items,
) async {
  final byDevice = <int, List<RestoreItem>>{};
  for (final item in items) {
    byDevice.putIfAbsent(item.targetDevice!.id, () => []).add(item);
  }
  final results = <({RestoreItem item, String? reason})>[];
  for (final group in byDevice.entries) {
    final client = ScriptClient(deviceId: group.key);
    // Libraries first, so a user script loaded afterwards links against the file as it now
    // stands rather than against an older library the device happened to hold.
    final ordered = [
      ...group.value.where(_isFunctionLibrary),
      ...group.value.where((i) => !_isFunctionLibrary(i)),
    ];
    for (final item in ordered) {
      final slot = item.script!.slot;
      // A script whose slot is unknown was written but cannot be loaded; there is no slot to load
      // it into, which is not a failure of the restore.
      if (slot < 0) {
        results.add((item: item, reason: null));
        continue;
      }
      final loaded = await client.loadedSlotMap();
      if (_isFunctionLibrary(item)) {
        // NEVER loaded: the file is what the using scripts read. Any slot that already holds this
        // file id is released, which is the state the archive asks for - and the one a run on a
        // rig left dirty by an older build has to reach, not leave behind.
        for (final entry in loaded.entries) {
          if (entry.value == slot) await client.unload(entry.key);
        }
        results.add((item: item, reason: null));
        continue;
      }
      // Unload and reload so the script links against the library as it now stands. A script the
      // device loaded at boot did so before the library file was replaced, and a stale link leaves
      // its global-function calls dead while the script itself reports no error. The file id and
      // the loaded slot are independent; a restored file sits at its own slot.
      //
      // A slot that already holds THIS file is left alone: [reloadChangedScripts] has already
      // reloaded it when the image this run wrote differs from the one the device held, and when
      // it does not, the running script is exactly what the archive asks for - a restore must not
      // stop an animation nothing in it changed.
      if (loaded[slot] == slot) {
        results.add((item: item, reason: null));
        continue;
      }
      // The slot is held by a DIFFERENT file - an earlier setup's script the sweep has not
      // reached yet. The archive's script belongs in its own slot, so the occupant goes.
      if (loaded.containsKey(slot)) {
        await client.unload(slot);
      }
      if (!await client.load(slot, slot)) {
        results.add((item: item, reason: 'script $slot would not load'));
        continue;
      }
      if (item.script!.properties.contains('Run on load')) {
        await client.setAction(slot, ScriptAction.start);
      }
      results.add((item: item, reason: null));
    }
  }
  return results;
}

/// One linked global function as a script FILE records it: the library's file id, and the offsets
/// inside that library the function was assembled into.
typedef ScriptLibraryLink = ({
  int library,
  int memoryOffset,
  int instructionOffset,
});

/// The links a script IMAGE carries into function libraries, read from its function-reservation
/// records - one 16-byte record per referenced global function, whose first `u16` is the library's
/// file id ([BackupFunctionReservation.of]).
///
/// `null` means the bytes could not be read as a script file at all (a raw `SCR_XXX` whose image
/// never parsed, which is why it travels as bytes). That is a different answer from "carries no
/// link", and the reload phase words its report from the difference rather than claiming a link it
/// never saw.
List<ScriptLibraryLink>? linkedLibraries(List<int> image) {
  final ScriptFileData data;
  try {
    data = ScriptFileData.parse(image);
  } on FormatException {
    return null;
  }
  final records = data.functionReservations;
  return [
    for (
      var offset = 0;
      offset + scriptFunctionReservationSize <= records.length;
      offset += scriptFunctionReservationSize
    )
      _linkOf(records.sublist(offset, offset + scriptFunctionReservationSize)),
  ];
}

ScriptLibraryLink _linkOf(List<int> record) {
  final reservation = BackupFunctionReservation.of(record);
  return (
    library: reservation.library,
    memoryOffset: reservation.memoryOffset,
    instructionOffset: reservation.instructionOffset,
  );
}

/// One measured link as a report reads it: the library's file name and the offsets the reservation
/// record carries, so the claim names the bytes it came from.
String _linkText(ScriptLibraryLink link) =>
    '${scriptFileName(link.library)} (memory 0x'
    '${link.memoryOffset.toRadixString(16)}, instruction 0x'
    '${link.instructionOffset.toRadixString(16)})';

/// What a restore MEASURED about the script files it wrote: per target, the script file ids whose
/// stored image differs from the image this run wrote, and why it judged them different.
///
/// A reload is a disturbance - it stops whatever was running in that slot and a reloaded script
/// comes back stopped - so the reload phase must never treat "the restore wrote this file" as
/// "this file changed". The image the device holds is read BEFORE the write and compared with the
/// image about to be written; only a real difference lands here. That is what lets a second
/// identical restore leave every running script alone, and it is a measured fact rather than an
/// assumption, as the rule requires.
class ScriptChanges {
  final Map<int, Map<int, String>> _changed = {};

  /// The links each written script IMAGE carried, per target: file id -> [linkedLibraries] of the
  /// bytes the write put there (`null` when those bytes are not readable as a script file).
  ///
  /// Recorded for every script file written, not only the changed ones: a script that did not
  /// change can still be running a stale link into a library that did, and that link is exactly
  /// what this measurement has to see. Nothing here is inferred from the archive's semantic form -
  /// it is read back out of the image, which is what the device will load.
  final Map<int, Map<int, List<ScriptLibraryLink>?>> _links = {};

  /// Notes that script file [fileId] on [targetId] is about to hold different bytes than it did,
  /// with [why] as the evidence the report quotes.
  void note(int targetId, int fileId, String why) => _changed
      .putIfAbsent(targetId, () => <int, String>{})
      .putIfAbsent(fileId, () => why);

  /// Records what the image this run wrote for script file [fileId] on [targetId] links against.
  void noteLinks(
    int targetId,
    int fileId,
    List<ScriptLibraryLink>? links,
  ) => _links.putIfAbsent(targetId, () => {})[fileId] = links;

  /// The changed script file ids of [targetId], as file id -> why.
  Map<int, String> forTarget(int targetId) => _changed[targetId] ?? const {};

  /// The measured links of the script images this run wrote on [targetId], as file id -> links
  /// (`null` when that image cannot be read as a script file). A file id that is absent was not
  /// written by this run, so its links are not this run's to judge.
  Map<int, List<ScriptLibraryLink>?> linksForTarget(int targetId) =>
      _links[targetId] ?? const {};
}

/// Whether two script images are byte-identical - the comparison [ScriptChanges] rests on.
bool sameImage(List<int> a, List<int> b) {
  if (a.length != b.length) return false;
  for (var i = 0; i < a.length; i++) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

/// One target's loaded script slots as a restore leaves them, when the question is not "which
/// slots are stale" (that is [ScriptSlotPrune]) but "what does the device RUN now".
///
/// [reloaded] carries the slots this run unloaded and loaded again, each with the measured fact
/// that made it necessary; [leftAlone] carries the loaded slots whose script the archive carries
/// and this run did not change, so the report says out loud that a running script was not
/// disturbed. A reload the device refuses is in [refused] and in the run's failures - a reload
/// that did not happen is not a success.
class ScriptReloadReport {
  /// The live node id of the target.
  final int deviceId;

  /// The target's name, for a report a person reads.
  final String deviceName;

  /// The script file ids whose image this run changed on the target, as file id -> why.
  final Map<int, String> changed;

  /// The slots unloaded and loaded again, in the order it happened, with the file each holds and
  /// what forced the reload.
  final Map<int, ({int fileId, String why})> reloaded;

  /// The loaded slots left untouched, with the file each holds: the archive carries that script in
  /// that slot, this run did not change it, so the device keeps the state it is in - running,
  /// paused or stopped, exactly as the user left it, which is the point of not disturbing it.
  final Map<int, int> leftAlone;

  /// The slots the device would not unload or would not load again, with the file each held.
  final Map<int, int> refused;

  /// True when no reload phase ran; [reason] says why.
  final bool skipped;
  final String? reason;

  const ScriptReloadReport({
    required this.deviceId,
    required this.deviceName,
    this.changed = const {},
    this.reloaded = const {},
    this.leftAlone = const {},
    this.refused = const {},
    this.skipped = false,
    this.reason,
  });

  String get changedText => changed.isEmpty
      ? '-'
      : changed.entries
            .map((e) => '${scriptFileName(e.key)} (${e.value})')
            .join(', ');

  String get reloadedText => reloaded.isEmpty
      ? '-'
      : reloaded.entries
            .map((e) => 'slot ${e.key} (${scriptFileName(e.value.fileId)}): ${e.value.why}')
            .join(', ');

  String get leftAloneText => ScriptSlotPrune.slotText(leftAlone);

  String get refusedText => ScriptSlotPrune.slotText(refused);
}

/// Reloads every loaded script slot whose backing script this restore changed, and reports what it
/// did with every loaded slot - reloaded, refused, or deliberately left alone.
///
/// A loaded script is a RUNNING script: the device executes the image it loaded, not the file on
/// flash. So a restore that rewrites `SCR_XXX` while a slot holds the old image leaves the rig
/// running stale code - the file is right, the behaviour is not - which is why a changed script has
/// to be unloaded and loaded again. A raw `SCR_XXX` file write is no exception: a script whose
/// image did not parse travels as bytes, and the slot that holds it runs the old bytes just the
/// same.
///
/// The rule this phase acts on, stated once: it reloads a loaded slot only when the script that
/// slot holds CHANGED in this restore ([ScriptChanges], measured before the write), or - the same
/// fact one step removed - when the script's own file carries a function-reservation record naming
/// a function library that changed, because a global call is resolved by reading the library FILE
/// when the using script loads, so an untouched script still holds calls into the old library.
/// That link is MEASURED, not assumed: [linkedLibraries] parses the image this run wrote, and one
/// reservation record per global call names the library's file id, so a script that references
/// nothing (or a library that did not change) is not disturbed by another script's library
/// changing. When the image cannot be read as a script file at all, the report says the link could
/// not be determined instead of claiming one. Anything else is left running, and reported as left
/// alone rather than silently skipped: stopping an animation nothing changed would be a
/// disturbance of its own.
///
/// A loaded slot in Error is reloaded too, with that as the reason: a faulted script stays in
/// Error until it is explicitly started, and a restore does not reboot, so leaving it would report
/// success over automation that is dead (the case [_bringUpScripts] exists for).
///
/// A reload takes the script back to its first instruction, so what was running has to be started
/// again or the rig sits idle. What it was doing is read from the device's report record before
/// the unload - `Running` and `Waiting` are started again, a paused/stopped/finished one is not
/// disturbed further - and a script the archive marks `Run on load` is started regardless, which is
/// the rule [_bringUpScripts] already applies to every script it loads. A library is never loaded
/// (see [_isFunctionLibrary]), so a slot holding one is left to the bring-up, which releases it.
///
/// A slot that holds its script in a slot the archive does not name for it is left to the other two
/// phases as well: the bring-up loads the archive's own slot and [pruneScriptSlots] releases the
/// extra copy, so reloading it here would restart a script that is about to go - or leave the same
/// script running twice.
///
/// Runs BEFORE [_bringUpScripts] on purpose: that phase loads the scripts a target does not hold,
/// so a slot it is about to load is not in the loaded map here and cannot be loaded twice. A target
/// whose slot map cannot be read is reported as a skipped phase and a failure, never as "nothing
/// changed".
Future<({List<ScriptReloadReport> reports, List<String> failures})>
reloadChangedScripts(RestorePlan plan, ScriptChanges changes) async {
  final reports = <ScriptReloadReport>[];
  final failures = <String>[];

  // Per TARGET NODE, as the sweep groups them: slots belong to the node, so two source devices
  // remapped onto one target share one slot table.
  final sourcesByTarget = <int, List<BackupDevice>>{};
  final targets = <int, LiveDevice>{};
  for (final source in plan.devices) {
    final target = plan.deviceTargets[source.id];
    if (target == null) continue;
    // A device without the script role has no slots to reload.
    if (target.capabilities & Capability.scripts == 0) continue;
    targets[target.id] = target;
    sourcesByTarget.putIfAbsent(target.id, () => []).add(source);
  }

  for (final group in sourcesByTarget.entries) {
    final target = targets[group.key]!;
    // The script file ids this run restores on the target ([restoring]), the ones the archive
    // marks as a function library (never loaded, so never reloaded here) and the ones marked to
    // run on load (what a reload starts when the device will not say what the slot was doing).
    final restoring = <int>{};
    final libraries = <int>{};
    final runOnLoad = <int>{};
    for (final source in group.value) {
      for (final item in plan.items) {
        if (item.device.id != source.id) continue;
        if (!item.selected || !item.ready) continue;
        if (item.kind == RestoreKind.script) {
          final slot = item.script!.slot;
          if (slot < 0) continue;
          if (_isFunctionLibrary(item)) {
            libraries.add(slot);
            continue;
          }
          restoring.add(slot);
          if (item.script!.properties.contains('Run on load')) runOnLoad.add(slot);
        } else if (item.kind == RestoreKind.file) {
          // A script whose image did not parse is written back as bytes; its file id is the slot
          // its name names, so it is part of the archive's script set like any other.
          final fileId = scriptFileId(item.file!.name);
          if (fileId != null) restoring.add(fileId);
        }
      }
    }
    // No script of this target is part of the run, so there is nothing to compare or report.
    if (restoring.isEmpty) continue;

    final changed = changes.forTarget(target.id);
    final client = ScriptClient(deviceId: target.id);
    final loaded = await client.readLoadedSlotMap();
    if (loaded == null) {
      reports.add(
        ScriptReloadReport(
          deviceId: target.id,
          deviceName: target.name,
          skipped: true,
          reason: 'the target did not return its slot map',
        ),
      );
      failures.add(
        '${target.name}: scripts not reloaded: the target did not return its '
        'slot map, so a script this run changed may still run its old image',
      );
      continue;
    }

    // A changed library invalidates every link into it (see above), so its file ids are quoted in
    // the reason the other slots were reloaded. Which scripts actually hold such a link is not
    // assumed from "this is one of the archive's scripts" - it is read out of the image this run
    // wrote ([linkedLibraries], recorded in [changes]), one reservation record per global call.
    final librariesChanged = [
      for (final id in changed.keys)
        if (libraries.contains(id)) id,
    ]..sort();
    final links = changes.linksForTarget(target.id);

    final reloaded = <int, ({int fileId, String why})>{};
    final leftAlone = <int, int>{};
    final refused = <int, int>{};

    for (final entry in loaded.entries) {
      final slot = entry.key;
      final fileId = entry.value;
      // A library's slot is the bring-up's to release, never this phase's to load.
      if (libraries.contains(fileId)) continue;
      final why = changed[fileId];
      // A slot that is not the one the archive names for the file it holds is somebody else's to
      // settle: the bring-up loads the archive's slot, and the sweep releases the extra copy. A
      // reload here would either duplicate the script or restart one that is about to go.
      if (slot != fileId) continue;
      // Not one of the archive's scripts and not changed: a leftover, and the sweep owns it.
      if (why == null && !restoring.contains(fileId)) continue;
      // What the slot was doing, read before the reload; null when the device will not say.
      final state = (await client.readState(slot))?.state;
      final faulted = state == ScriptState.error || state == ScriptState.loadFailed;
      // The links THIS image carries into a library that changed - measured from the file's
      // function-reservation records, not inferred from the script being part of the run. A script
      // with no such record links nothing, so no library change can invalidate it.
      final measured = links[fileId];
      final staleLinks = measured == null
          ? const <ScriptLibraryLink>[]
          : [
              for (final link in measured)
                if (librariesChanged.contains(link.library)) link,
            ];
      String? reason;
      if (why != null) {
        reason = 'its image changed ($why)';
      } else if (staleLinks.isNotEmpty) {
        reason =
            'its file holds a function reservation naming '
            '${staleLinks.map(_linkText).join(', ')}, whose image changed';
      } else if (measured == null && librariesChanged.isNotEmpty) {
        // The image cannot be read as a script file, so its reservations cannot be inspected. Run
        // nothing stale rather than claim a link that was never seen - and say why.
        reason =
            'its file cannot be read as a script file, so whether it references '
            '${librariesChanged.map(scriptFileName).join(', ')}, whose image '
            'changed, cannot be determined; reloaded rather than risk a stale link';
      } else if (faulted) {
        reason =
            'it is in ${ScriptState.label(state!)}, and a faulted script '
            'stays there until it is started';
      }
      if (reason == null) {
        // Nothing about this slot changed, so it keeps running: this is the "do not disturb a
        // running animation for no reason" half of the rule, and it is reported, not silent.
        leftAlone[slot] = fileId;
        continue;
      }

      if (!await client.unload(slot)) {
        refused[slot] = fileId;
        failures.add(
          '${target.name}: reloading script slot $slot '
          '(${scriptFileName(fileId)}) failed: the device refused the unload, '
          'so it still runs the image it held ($reason)',
        );
        continue;
      }
      if (!await client.load(fileId, slot)) {
        refused[slot] = fileId;
        failures.add(
          '${target.name}: reloading script slot $slot '
          '(${scriptFileName(fileId)}) failed: the file would not load again, '
          'so the slot is now empty ($reason)',
        );
        continue;
      }
      // A fresh load starts at the first instruction, so a script that was running has to be
      // started again, and a faulted one is started to clear the error - that is what reloading it
      // is FOR. The archive's `Run on load` is the bring-up's own rule and is honoured either way.
      // Anything else (paused, stopped, finished) is not disturbed further.
      final wasActive =
          state == ScriptState.running || state == ScriptState.waiting;
      final start = wasActive || faulted || runOnLoad.contains(fileId);
      var outcome = start ? 'started again' : 'left stopped';
      if (start && !await client.setAction(slot, ScriptAction.start)) {
        outcome = 'reloaded, but the device refused to start it';
        failures.add(
          '${target.name}: script slot $slot (${scriptFileName(fileId)}) was '
          'reloaded but would not start, so it is not running',
        );
      }
      reloaded[slot] = (fileId: fileId, why: '$reason; $outcome');
    }

    reports.add(
      ScriptReloadReport(
        deviceId: target.id,
        deviceName: target.name,
        changed: changed,
        reloaded: reloaded,
        leftAlone: leftAlone,
        refused: refused,
      ),
    );
  }
  return (reports: reports, failures: failures);
}

/// One target's script slots as a restore leaves them: the slots it unloaded because the archive
/// does not name their script, the ones the target would not unload, and the ones it left because
/// this run was not asked to restore them.
///
/// An unload is a DELETION - the device stops running that script - so it is reported with the
/// slot and the file id it held rather than applied silently, exactly as [RequesterPrune] reports
/// the TRIDs it cancels: `before` / `unloaded` / `remaining`, read from the device and not assumed.
class ScriptSlotPrune {
  /// The live node id of the target that was swept.
  final int deviceId;

  /// The target's name, for a report a person reads.
  final String deviceName;

  /// The slots the target held loaded when the sweep read it - after the archive's scripts were
  /// written and loaded - as slot -> `SCR_XXX` file id.
  final Map<int, int> before;

  /// The slots unloaded, in the order they were unloaded, with the file id each held.
  final Map<int, int> unloaded;

  /// The slots unloaded because they held a SECOND copy of one of the archive's scripts, in a slot
  /// the archive does not name for that script, while the archive's own slot held it too: slot ->
  /// the file, and the slot the archive actually names for it (`slot 0 -> SCR_004 at slot 4`).
  ///
  /// The same script loaded twice runs twice - an animation loop or a fan curve twice over - so a
  /// duplicate is a deletion too, and is named here instead of hiding inside [unloaded].
  final Map<int, ({int fileId, int canonical})> duplicates;

  /// The loaded slots that hold an archive script in a slot the archive does not name for it, kept
  /// because the archive's own slot does NOT hold that script (this run's load of it failed).
  /// Deleting the sole surviving copy would delete the script, so it is reported and kept - never
  /// passed off as a clean device, and never unloaded on a guess.
  final Map<int, int> offSlot;

  /// The slots the target would not unload: the command answered FAIL or nothing, so that script
  /// is STILL loaded. Reported as a failure rather than counted as done.
  final Map<int, int> refused;

  /// The slots left loaded because this run was not asked to restore their script (its item is
  /// unticked in the plan, or could not be placed). Those slots are "not selected", NOT "not in
  /// the archive", so the sweep names them instead of unloading them.
  final Map<int, int> keptBySelection;

  /// The slots the target still held loaded after the unloads - read back from the device, not
  /// assumed. Empty when [confirmed] is false, which means the slots could not be read back at
  /// all.
  final Map<int, int> remaining;

  /// True when [remaining] is a real read of the target. False means what the target now runs is
  /// unknown, which is reported rather than passed off as a clean sweep.
  final bool confirmed;

  /// True when no sweep ran; [reason] says why.
  final bool skipped;
  final String? reason;

  const ScriptSlotPrune({
    required this.deviceId,
    required this.deviceName,
    this.before = const {},
    this.unloaded = const {},
    this.duplicates = const {},
    this.offSlot = const {},
    this.refused = const {},
    this.keptBySelection = const {},
    this.remaining = const {},
    this.confirmed = true,
    this.skipped = false,
    this.reason,
  });

  /// A set of slots as a report reads it (`slot 0 (SCR_000), slot 5 (SCR_005)`), or `-` for none.
  static String slotText(Map<int, int> slots) => slots.isEmpty
      ? '-'
      : [
          for (final e in slots.entries)
            'slot ${e.key} (${scriptFileName(e.value)})',
        ].join(', ');

  String get beforeText => slotText(before);
  String get unloadedText => slotText(unloaded);
  String get refusedText => slotText(refused);
  String get keptText => slotText(keptBySelection);

  /// The duplicates as a report reads them, naming the slot the archive actually names for the
  /// script as well as the copy that went: `slot 0 (SCR_004, also loaded at slot 4)`.
  String get duplicatesText => duplicates.isEmpty
      ? '-'
      : [
          for (final e in duplicates.entries)
            'slot ${e.key} (${scriptFileName(e.value.fileId)}, also loaded at '
                'slot ${e.value.canonical})',
        ].join(', ');

  /// The kept off-slot copies, with the slot the archive names for each: they are still loaded and
  /// the device is not in the archive's shape, so the report says so rather than looking clean.
  String get offSlotText => offSlot.isEmpty
      ? '-'
      : [
          for (final e in offSlot.entries)
            'slot ${e.key} (${scriptFileName(e.value)}, the archive names slot '
                '${e.value})',
        ].join(', ');

  /// The still-loaded slots as a report reads them, or `unconfirmed` when the device could not be
  /// read back - never an empty map standing in for "I could not look".
  String get remainingText => confirmed ? slotText(remaining) : 'unconfirmed';
}

/// Unloads every loaded script slot the archive for that target does not name, then reports each
/// one, so the device runs the archive's scripts and nothing else.
///
/// [_bringUpScripts] walks the ARCHIVE's scripts only: it unloads and reloads each of those slots
/// and releases a slot a function library occupies. A slot holding a script the archive does not
/// carry is therefore never even looked at, and a loaded script is a RUNNING script - which is how
/// the rig kept running an akyirr-era `Temperature regulation` after bambi's archive (slots 1, 2,
/// 3, 4 and 10, no such script) was restored over it. The archive was right; the restore was
/// additive. This is the mirror rule [pruneRequesterTable] applies to the requester table.
///
/// What the archive NAMES is a slot: a stored script's identity on the device is its `SCR_XXX`
/// file id and the restore loads file N into slot N, so the archive names a slot for every script
/// it carries - as a semantic script, and as a raw `SCR_XXX` file whose image did not parse and is
/// written back as bytes. A loaded copy of an archive script in some OTHER slot is therefore not
/// the archive's shape either: it is a duplicate of the copy this run just loaded, and the sweep
/// settles it (see the predicate below) rather than leave one script running from two slots.
///
/// The predicate this phase acts on, stated once: it unloads a loaded slot unless that slot holds
/// the archive's script IN THE SLOT THE ARCHIVE NAMES FOR IT - i.e. the slot number equals the
/// `SCR_XXX` file id sitting in it, and that file id is one this run restores. Everything else the
/// target holds is either
///
/// * a leftover from an earlier setup (neither the slot nor the file is in the archive's set), or
/// * a SECOND copy of one of the archive's scripts, in a slot the archive does not name for it
///   (`scripts_page.dart` loads into the lowest FREE slot, so SCR_004 can sit in slot 0), which
///   the archive's own slot may also hold - and two slots running one script run it twice, so the
///   extra copy goes and is named in [ScriptSlotPrune.duplicates].
///
/// Two loaded states are kept instead, and each is named in the report:
///
/// * a slot behind an unticked checkbox - "not selected", not "not in the archive"
///   ([ScriptSlotPrune.keptBySelection]) - otherwise unticking one script box would unload the
///   very script the user meant to keep, which is precisely the kind of silent deletion this
///   reporting exists to rule out;
/// * the only loaded copy of an archive script in a slot the archive does not name for it, when
///   the archive's own slot does not hold it either (this run's load failed): deleting the
///   surviving copy would delete the script, so it is reported
///   ([ScriptSlotPrune.offSlot]) rather than unloaded on a guess.
///
/// The phase also declines outright, with a reason, on a target whose plan carries no item for an
/// archive script: then this run is not the whole of that device's scripts and there is nothing to
/// compare against.
///
/// The sweep is per TARGET NODE, not per archive device: slots belong to the node, so two archive
/// devices remapped onto one target share one slot table and their sets are unioned - sweeping per
/// source device would let the first pass unload a slot the second one's archive names. Nothing
/// here touches another target's slots, and a function library is untouched too: it is named by
/// the archive, so it is one of the slots this run restores, and [_bringUpScripts] already
/// released the slot it had wrongly occupied.
///
/// A target whose slot map cannot be read is reported as a skipped sweep, not as a clean device:
/// "could not look" is not "holds nothing", and that is the read this whole phase rests on.
Future<({List<ScriptSlotPrune> prunes, List<String> failures})>
pruneScriptSlots(RestorePlan plan) async {
  final prunes = <ScriptSlotPrune>[];
  final failures = <String>[];

  // The archive's script slots per device, split three ways: the ones the plan carries an item for
  // at all ([covered]), the ones this run restores ([restored]) and the ones it was told to leave
  // out - an unticked, or unplaceable, item ([leftOut]). The split IS the predicate: "restored"
  // is the set a loaded slot has to be outside of to be a leftover, and "leftOut" is what stops an
  // unticked script from reading as one.
  final covered = <int, Set<int>>{};
  final restored = <int, Set<int>>{};
  final leftOut = <int, Set<int>>{};
  void note(int deviceId, int slot, {required bool restoring}) {
    covered.putIfAbsent(deviceId, () => <int>{}).add(slot);
    final into = restoring ? restored : leftOut;
    into.putIfAbsent(deviceId, () => <int>{}).add(slot);
  }

  for (final item in plan.items) {
    final restoring = item.selected && item.ready;
    if (item.kind == RestoreKind.script) {
      note(item.device.id, item.script!.slot, restoring: restoring);
    } else if (item.kind == RestoreKind.file && item.file!.kind == 'Script') {
      // A script whose image did not parse travels as a raw file and is written back as bytes; it
      // still sits at the slot its name names, so it counts as one of the archive's scripts.
      final slot = scriptFileId(item.file!.name);
      if (slot != null) note(item.device.id, slot, restoring: restoring);
    }
  }

  // The live target each source device is restored onto, grouped by that target (see above).
  final sourcesByTarget = <int, List<BackupDevice>>{};
  final targets = <int, LiveDevice>{};
  for (final source in plan.devices) {
    final target = plan.deviceTargets[source.id];
    if (target == null) continue;
    // A device without the script role has no slot table to sweep, and every one of its script
    // items was already reported as unsupported by the plan.
    if (target.capabilities & Capability.scripts == 0) continue;
    targets[target.id] = target;
    sourcesByTarget.putIfAbsent(target.id, () => []).add(source);
  }

  for (final group in sourcesByTarget.entries) {
    final target = targets[group.key]!;
    // The archive's script slots for this target, the ones this run restores and the ones it was
    // told to leave out - unioned over every source device restored onto this node.
    final restoring = <int>{};
    final leavingOut = <int>{};
    final uncovered = <int>{};
    for (final source in group.value) {
      final slots = <int>{for (final script in source.scripts) script.slot};
      for (final file in source.files) {
        if (file.kind != 'Script') continue;
        final slot = scriptFileId(file.name);
        if (slot != null) slots.add(slot);
      }
      for (final slot in slots) {
        // Every archive script must be accounted for by an item. One that is not means this run
        // is not the whole of that device's scripts, and sweeping would be a guess about slots
        // nothing in this run decided.
        if (!(covered[source.id] ?? const <int>{}).contains(slot)) {
          uncovered.add(slot);
        }
      }
      restoring.addAll(restored[source.id] ?? const <int>{});
      leavingOut.addAll(leftOut[source.id] ?? const <int>{});
    }
    if (uncovered.isNotEmpty) {
      final missing = uncovered.toList()..sort();
      prunes.add(
        ScriptSlotPrune(
          deviceId: target.id,
          deviceName: target.name,
          skipped: true,
          reason:
              'the plan carries no item for script slot(s) '
              '${missing.join(', ')} the archive holds, so this run is not the '
              "whole of the target's scripts",
        ),
      );
      continue;
    }

    final client = ScriptClient(deviceId: target.id);
    final before = await client.readLoadedSlotMap();
    if (before == null) {
      prunes.add(
        ScriptSlotPrune(
          deviceId: target.id,
          deviceName: target.name,
          confirmed: false,
          skipped: true,
          reason: 'the target did not return its slot map',
        ),
      );
      failures.add(
        '${target.name}: script slots not swept: the target did not return its '
        'slot map (no reply or FAIL), so a script the archive does not carry may '
        'still be loaded and running',
      );
      continue;
    }

    // What each loaded entry is, by the predicate above: the archive's script in the archive's slot
    // (nothing to do), a copy in the wrong slot, an unticked script the user kept, or a leftover.
    final kept = <int, int>{};
    final doomed = <int, int>{};
    final duplicates = <int, ({int fileId, int canonical})>{};
    final offSlot = <int, int>{};
    for (final e in before.entries) {
      final slot = e.key;
      final fileId = e.value;
      // The slot the archive names for this file, or null when the archive does not carry it.
      final namedSlot = restoring.contains(fileId) ? fileId : null;
      // The archive's script, in the slot the archive names for it: that is the shape this run is
      // after, so there is nothing to settle.
      if (namedSlot == slot) continue;
      // A slot the user left out (or one holding a file they left out) is "not selected", not
      // "not in the archive": naming it is all this phase may do.
      if (leavingOut.contains(slot) || leavingOut.contains(fileId)) {
        kept[slot] = fileId;
        continue;
      }
      if (namedSlot != null) {
        if (before[namedSlot] == namedSlot) {
          // The archive's own slot holds this script too: this one is a duplicate, and two slots
          // running one script run it twice.
          duplicates[slot] = (fileId: fileId, canonical: namedSlot);
          doomed[slot] = fileId;
        } else {
          // The archive's slot does NOT hold it (this run's load of that slot failed), so this is
          // the only copy running: keep it and say so, never delete the script on a guess.
          offSlot[slot] = fileId;
        }
        continue;
      }
      // Neither the slot nor the file is one this run restores: an earlier setup's leftover.
      doomed[slot] = fileId;
    }
    if (doomed.isEmpty) {
      // Nothing to unload, so nothing to read back: a second restore of the same archive is a
      // no-op, and one that unticks a script issues no unload for it either.
      prunes.add(
        ScriptSlotPrune(
          deviceId: target.id,
          deviceName: target.name,
          before: before,
          keptBySelection: kept,
          offSlot: offSlot,
          remaining: before,
        ),
      );
      continue;
    }

    final unloaded = <int, int>{};
    final refused = <int, int>{};
    for (final e in doomed.entries) {
      if (await client.unload(e.key)) {
        unloaded[e.key] = e.value;
      } else {
        refused[e.key] = e.value;
        failures.add(
          '${target.name}: unloading script slot ${e.key} '
          '(${scriptFileName(e.value)}) failed (the device refused it or did not '
          'answer), so that script is still loaded',
        );
      }
    }

    // Read back rather than assume: the point of reporting is to catch an unload the device did
    // not actually apply. A read-back that does not answer is a failure too - what the target now
    // runs would otherwise be reported as known when nobody managed to look.
    final after = await client.readLoadedSlotMap();
    if (after == null) {
      failures.add(
        '${target.name}: could not confirm the script sweep: the slot map could '
        'not be read back after ${unloaded.length} unload(s), so what the target '
        'now runs is unknown',
      );
    } else {
      for (final e in unloaded.entries) {
        if (after.containsKey(e.key)) {
          failures.add(
            '${target.name}: script slot ${e.key} (${scriptFileName(e.value)}) '
            'is still loaded after the unload',
          );
        }
      }
    }
    prunes.add(
      ScriptSlotPrune(
        deviceId: target.id,
        deviceName: target.name,
        before: before,
        unloaded: unloaded,
        duplicates: duplicates,
        offSlot: offSlot,
        refused: refused,
        keptBySelection: kept,
        remaining: after ?? const {},
        confirmed: after != null,
      ),
    );
  }
  return (prunes: prunes, failures: failures);
}

/// One target's requester table as a restore leaves it: the entries it cancelled, what the target
/// held before and after, and why the table was left alone when it was.
///
/// A restore result carries one of these per target, because a cancellation is a DELETION: it is
/// reported with its TRIDs rather than applied silently, so a table that lost rows shows it.
class RequesterPrune {
  /// The live node id of the target that was pruned.
  final int deviceId;

  /// The target's name, for a report a person reads.
  final String deviceName;

  /// The TRIDs the target held before the prune.
  final List<int> before;

  /// The TRIDs cancelled, in the order they were cancelled.
  final List<int> cancelled;

  /// The TRIDs the target still holds after the save - read back from the device, not assumed.
  /// Empty when [confirmed] is false, which means the table could not be read back at all.
  final List<int> remaining;

  /// True when [remaining] is a real read of the target. False means the read-back itself failed,
  /// so what the target now holds is unknown: the prune is reported as unconfirmed rather than as
  /// a clean table.
  final bool confirmed;

  /// True when the table was left untouched; [reason] says why.
  final bool skipped;
  final String? reason;

  const RequesterPrune({
    required this.deviceId,
    required this.deviceName,
    this.before = const [],
    this.cancelled = const [],
    this.remaining = const [],
    this.confirmed = true,
    this.skipped = false,
    this.reason,
  });

  /// The cancelled TRIDs as a report reads them (`0x1000, 0x1002`).
  String get cancelledText => _tridText(cancelled);

  /// The TRIDs the target held before the prune, as a report reads them.
  String get beforeText => _tridText(before);

  /// The remaining TRIDs as a report reads them, or `unconfirmed` when the table could not be read
  /// back - never an empty list standing in for "I could not look".
  String get remainingText => confirmed ? _tridText(remaining) : 'unconfirmed';

  static String _tridText(List<int> trids) => trids.isEmpty
      ? '-'
      : trids.map((t) => '0x${t.toRadixString(16)}').join(', ');
}

/// Cancels every requester entry the plan's targets hold that the archive does not name, then makes
/// that durable, so the target's requester table equals the archive's the moment the restore
/// finishes instead of at the next boot (Docs/App/Backup.md).
///
/// The two tools are the device's own (Docs/Services/Subscriptions.md "Requester commands"):
///  * Set subscription (CID 0x11) with a None trigger cancels one entry by TRID
///    ([SubscriptionClient.cancelRequesterSubscription], `RequesterSubscription.cancel`).
///  * Save all subscriptions (CID 0x13) rewrites `.SUBREQ` from the live table, so a reboot cannot
///    recall a row this run cancelled. The firmware's cancel path rewrites the file too
///    (`SubscriptionsControl.h`), so the save is the explicit durability step rather than the only
///    one - and it is the one whose own result is checked and reported.
/// Recall all (CID 0x12) is deliberately NOT used: it reads `.SUBREQ` over the live table, which
/// is the opposite direction and would undo the cancels.
///
/// What is "named by the archive" is decided by [requesterKey] over the identities the archive
/// carries, resolved exactly as the writes resolve them - not by TRID, which the archive does not
/// carry. So an entry the same restore just created is IN the wanted set and survives, and a second
/// restore of the same archive finds nothing to cancel (and issues no save).
///
/// A device whose archive names a subscription that does not resolve here (its provider is off the
/// bus, say) is left alone and reported: without that identity the live entry it would match cannot
/// be told apart from a stale one, and a delete on a guess is exactly what the previous "never
/// prune" rule existed to prevent.
///
/// The same goes for a table that cannot be READ: "could not look" is not "holds nothing". A target
/// that does not return its requester table (no link, timeout, FAIL) is reported as a skipped prune
/// with no cancels and no save, and one whose table cannot be read back after the save is reported
/// as unconfirmed - never as a clean, empty table.
Future<({List<RequesterPrune> prunes, List<String> failures})>
pruneRequesterTable(RestorePlan plan) async {
  final prunes = <RequesterPrune>[];
  final failures = <String>[];

  for (final source in plan.devices) {
    final target = plan.deviceTargets[source.id];
    if (target == null) continue;
    // A device without the requester role has no table to prune; every subscription item was
    // already reported as unsupported.
    if (target.capabilities & Capability.subscriptionRequest == 0) continue;

    final wanted = <String>{};
    String? unresolved;
    for (final sub in source.requesterSubscriptions) {
      final provider = resolveArchiveAddress(plan, sub.provider);
      final providerDev = providerDeviceFor(plan.liveDevices, sub.provider);
      final sourceReg = providerDev?.resolveRef(sub.source);
      final targetReg =
          target.resolveRef(sub.target) ?? archiveRefReg(source, sub.target);
      if (provider == null || sourceReg == null || targetReg == null) {
        unresolved = 'from ${sub.provider}';
        break;
      }
      wanted.add(requesterKey(provider, sourceReg, targetReg));
    }
    if (unresolved != null) {
      prunes.add(
        RequesterPrune(
          deviceId: target.id,
          deviceName: target.name,
          skipped: true,
          reason:
              "the archive names a subscription that does not resolve "
              '($unresolved)',
        ),
      );
      failures.add(
        '${target.name}: requester table not pruned: the archive names a '
        'subscription that does not resolve ($unresolved)',
      );
      continue;
    }

    final subs = SubscriptionClient(deviceId: target.id);
    final held = await subs.readRequesterSubscriptions();
    if (held == null) {
      // A table the target would not return is NOT an empty table. Pruning against an unread one
      // would cancel nothing, issue no save and report `before=[] cancelled=[] remaining=[]` -
      // a clean-looking restore that leaves exactly the stale rows this exists to remove. So the
      // device is left alone and the failure is reported.
      prunes.add(
        RequesterPrune(
          deviceId: target.id,
          deviceName: target.name,
          confirmed: false,
          skipped: true,
          reason: 'the target did not return its requester table',
        ),
      );
      failures.add(
        '${target.name}: requester table not pruned: the target did not return '
        'its requester table (no reply or FAIL), so the entries the archive does '
        'not name are still there',
      );
      continue;
    }
    final before = [for (final e in held) e.trid];
    final doomed = [
      for (final e in held)
        if (!wanted.contains(
          requesterKey(e.providerAddr, e.sourceReg, e.targetReg),
        ))
          e,
    ];
    if (doomed.isEmpty) {
      // Nothing to cancel, so nothing to save: a second restore of the same archive is a no-op.
      prunes.add(
        RequesterPrune(
          deviceId: target.id,
          deviceName: target.name,
          before: before,
          remaining: before,
        ),
      );
      continue;
    }

    final cancelled = <int>[];
    for (final entry in doomed) {
      if (await subs.cancelRequesterSubscription(entry.trid)) {
        cancelled.add(entry.trid);
      } else {
        failures.add(
          '${target.name}: cancelling requester entry 0x'
          '${entry.trid.toRadixString(16)} failed (the device refused it or did '
          'not answer)',
        );
      }
    }
    // The save comes AFTER every cancel, and only when at least one landed: it states the table as
    // it now stands, so it can never be what re-adds an entry.
    if (cancelled.isNotEmpty && !await subs.saveRequesterSubscriptions()) {
      failures.add(
        '${target.name}: requester save-all after ${cancelled.length} cancel(s) '
        'failed, so a reboot may recall them',
      );
    }

    // Read back rather than assume: the point of reporting is to catch a cancel the device did not
    // actually apply. A read-back that does not answer is a failure too - reporting `remaining=[]`
    // for a table nobody managed to read would make the "still present" check below vacuous.
    final after = await subs.readRequesterSubscriptions();
    if (after == null) {
      failures.add(
        '${target.name}: could not confirm the requester save: the table could '
        'not be read back after ${cancelled.length} cancel(s) (no reply or '
        'FAIL), so what the target now holds is unknown',
      );
    }
    final remaining = after == null
        ? const <int>[]
        : [for (final e in after) e.trid];
    if (after != null) {
      for (final trid in cancelled) {
        if (remaining.contains(trid)) {
          failures.add(
            '${target.name}: requester entry 0x${trid.toRadixString(16)} is '
            'still in the table after the cancel and the save',
          );
        }
      }
    }
    prunes.add(
      RequesterPrune(
        deviceId: target.id,
        deviceName: target.name,
        before: before,
        cancelled: cancelled,
        remaining: remaining,
        confirmed: after != null,
      ),
    );
  }
  return (prunes: prunes, failures: failures);
}

/// Applies the ready + selected items of [plan]. Returns the number of written
/// items and failures.
Future<
  ({
    int written,
    int failed,
    List<String> failures,
    List<RequesterPrune> requesterPrune,
    List<ScriptSlotPrune> scriptSlots,
    List<ScriptReloadReport> scriptReload,
  })
>
applyRestorePlan(RestorePlan plan) async {
  // A dynamic block's values can only be written into a block that exists.
  await prepareDynamicBlocks(plan);

  var written = 0;
  var failed = 0;
  final failures = <String>[];
  // What this run measured about the script images it wrote (see [ScriptChanges]): the reload phase
  // acts on the difference between what the device HELD and what was written, not on the fact that
  // a restore wrote it.
  final changes = ScriptChanges();
  final register = <int, RegisterClient>{};
  final storage = <int, StorageClient>{};
  final subscriptions = <int, SubscriptionClient>{};
  final sndbWritten = <String>{};
  // The devices this run has actually written to. A write sits in RAM until the device is told to
  // save, so the run saves every device it touched at the end (B15).
  final writtenDevices = <int>{};
  // Script items whose FILE is written but whose outcome is decided by the load at the end: the
  // load runs after the values are in place, so the item is counted only once it has loaded.
  final scriptItems = <RestoreItem>[];
  // TRIDs this run has handed out. The table read can lag the writes made moments before it, so
  // trusting the read alone lets two items choose the same TRID and the second silently replaces
  // the first - which loses an entry when restoring onto a wiped device.
  final allocatedTrids = <int>{};
  // One write per distinct (provider, source, target): an archive can carry the same
  // subscription twice (a device that already held a duplicate captures it), and writing both
  // leaves two entries racing for one source with the second never firing. The identity is
  // `requesterKey`'s, the same one the prune compares against, so a collapsed duplicate and an
  // archived one are the same subscription to both.
  final seenSubKeys = <String>{};

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
      writtenDevices.add(item.targetDevice!.id);
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

  // The target's requester table is pruned after the items are written (see
  // `pruneRequesterTable`): every entry the archive does not name is cancelled and the result
  // saved, so the table equals the archive's when this returns rather than at the next boot. The
  // prune runs LAST on purpose - an entry this run just created is then already in the table AND
  // in the archive's identity set, so it is not a candidate; and it is skipped, with a reported
  // failure, for a device whose archive names a subscription that does not resolve (the case the
  // previous "never prune" rule guarded against, now reported instead of silently tolerated).

  for (final item in ordered) {
    // An unready item is never written, so if it is skipped here it is counted in neither
    // `written` nor `failed` - a caller reading `failed 0` cannot tell it was dropped, which is
    // how a restore reported success while losing entries. Only the by-design read-only skip is
    // not a loss; every other issue means an item the restore was asked to write.
    if (!item.ready) {
      if (item.issue != readOnlyIssue) {
        failItem(item, '${describe(item)}: ${item.issue}');
      }
      continue;
    }
    if (!item.selected) continue;
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
          final image = item.script!.toDraft().toImage();
          // Measure BEFORE the write, so "changed" is a fact about this device rather than an
          // assumption about restores: what it held now against what is about to be written.
          final held = await store.readFile(name);
          if (!await store.writeFile(name, image)) {
            failItem(item, describe(item));
            break;
          }
          if (slot >= 0) {
            // Measured from the bytes this write put there, for EVERY script (changed or not): a
            // loaded script that did not change can still hold a link into a library that did.
            changes.noteLinks(target.id, slot, linkedLibraries(image));
            if (held == null) {
              changes.note(
                target.id,
                slot,
                'the device did not return $name, so its old image is unknown',
              );
            } else if (!sameImage(held, image)) {
              changes.note(
                target.id,
                slot,
                'the device held ${held.length} byte(s), this run wrote '
                    '${image.length}',
              );
            }
          }
        } catch (error) {
          AppDiagnostics.log(
            'backup',
            'script ${item.script!.functionName} failed: $error',
          );
          failItem(item, describe(item));
          break;
        }
        writtenDevices.add(target.id);
        // The file is only half of a script: the device runs what it LOADS, and the load happens
        // last, after the values are in place so the script reads them. Account for the item there,
        // so a load that fails is reported instead of hidden behind the successful write.
        scriptItems.add(item);
      case RestoreKind.subscription:
        final sub = subscriptions.putIfAbsent(
          target.id,
          () => SubscriptionClient(deviceId: target.id),
        );
        final s = item.subscription!;
        // The provider address is archive-side and follows the local-vs-exact model: a local
        // `0.N` names arc N on the net this rig runs - net 0 is the placeholder and no device
        // sits on it (`resolveArchiveAddress`) - while an exact `net.arc` passes through. It keys
        // BOTH the requester entry's provider address and the provider entry's requester address,
        // so a literal `0.2` here would install both against a provider on net 0. The provider
        // DEVICE is still matched by node (ignoring the net), which is what resolves the provider.
        final provider = resolveArchiveAddress(plan, s.provider);
        final providerDev = providerDeviceFor(plan.liveDevices, s.provider);
        final source = providerDev?.resolveRef(s.source);
        final targetReg =
            target.resolveRef(s.target) ?? archiveRefReg(item.device, s.target);
        if (source == null || targetReg == null || provider == null) {
          failItem(item, describe(item));
          break;
        }
        // One entry per distinct identity: the archive can name the same subscription twice
        // (captured from a device that already held a duplicate), and a second entry would sit
        // inert behind the first.
        if (!seenSubKeys.add(requesterKey(provider, source, targetReg))) {
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
        // A TRID the device does not already use. It keys requester entries by TRID, so a blind
        // local counter collides with the entries the device carries and the writes then
        // overwrite one another - a restore silently replaces subscriptions instead of adding
        // them, and the replaced ones look like subscriptions that never update.
        final usedTrids = <int>{...mine.map((e) => e.trid), ...allocatedTrids};
        var freeTrid = tridSubBase;
        while (usedTrids.contains(freeTrid) && freeTrid < tridSubMax) {
          freeTrid++;
        }
        allocatedTrids.add(freeTrid);
        final index = match?.index ?? mine.length;
        final entry = RequesterSubscription(
          index: index,
          providerAddr: provider,
          trid: match?.trid ?? freeTrid,
          targetReg: targetReg,
          sourceReg: source,
          trigger: TriggerType.fromWord(s.trigger) ?? TriggerType.periodic,
          periodMs: s.periodMs,
          minTimeMs: s.minTimeMs,
          deadzone: s.deadzone,
        );
        final ok = await sub.setRequesterSubscription(index, entry: entry);
        if (!ok) {
          failItem(item, describe(item));
          break;
        }
        writtenDevices.add(target.id);
        written++;
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
        done(await sndbWrite(unhexBytes(e.serial), id), item, describe(item));
      case RestoreKind.file:
        final store = storage.putIfAbsent(
          target.id,
          () => StorageClient(deviceId: target.id),
        );
        if (item.file!.name == '.TABLE') break;
        try {
          // A raw `SCR_XXX` is a script too: its image did not parse, so it travels as bytes, but
          // the slot that holds it runs whatever this write put there. Measure the same way the
          // semantic path does, so the reload phase can act on a raw-file rewrite as well.
          final fileId = scriptFileId(item.file!.name);
          final held = fileId == null
              ? null
              : await store.readFile(item.file!.name);
          final wrote = await store.writeFile(item.file!.name, item.file!.bytes);
          if (wrote && fileId != null) {
            // The bytes are the image: read its links the same way the semantic path does, so a
            // raw `SCR_XXX` that does reference a library is reloaded on a library change too.
            changes.noteLinks(
              target.id,
              fileId,
              linkedLibraries(item.file!.bytes),
            );
            if (held == null) {
              changes.note(
                target.id,
                fileId,
                'the device did not return ${item.file!.name}, so its old '
                    'image is unknown',
              );
            } else if (!sameImage(held, item.file!.bytes)) {
              changes.note(
                target.id,
                fileId,
                'the device held ${held.length} byte(s), this run wrote '
                    '${item.file!.bytes.length}',
              );
            }
          }
          done(wrote, item, describe(item));
        } catch (error) {
          AppDiagnostics.log(
            'backup',
            'file ${item.file!.name} failed: $error',
          );
          failItem(item, describe(item));
        }
    }
  }
  // The requester table is part of the snapshot, so it is brought to the archive's before the
  // device is saved: every entry the archive does not name is cancelled through the device's own
  // Set-with-None command and then persisted with its Save-all. A cancel that fails, a save that
  // fails and a cancelled TRID still in the table afterwards are all reported - a delete that
  // cannot be seen is the failure mode this exists to remove.
  final pruned = await pruneRequesterTable(plan);
  for (final failure in pruned.failures) {
    failed++;
    if (failures.length < 100) failures.add(failure);
  }
  // Every device this run wrote to is saved - the writes sit in RAM until then - and the scripts
  // are loaded last, so they read the values the restore has just written.
  await _persistWrites(writtenDevices);
  // Then the reload rule: a script this run CHANGED is unloaded and loaded again, because the
  // device runs the image it loaded rather than the file on flash - a restore that only rewrote the
  // file would leave the rig on stale code. It runs BEFORE the bring-up so a slot the bring-up is
  // about to fill is not loaded twice, and it reloads nothing it has not measured as changed, so a
  // running script the archive did not touch is left alone.
  final reloadedScripts = await reloadChangedScripts(plan, changes);
  for (final failure in reloadedScripts.failures) {
    failed++;
    if (failures.length < 100) failures.add(failure);
  }
  for (final brought in await _bringUpScripts(scriptItems)) {
    if (brought.reason == null) {
      written++;
    } else {
      failItem(brought.item, brought.reason!);
    }
  }
  // Then the mirror of the bring-up: a slot holding a script the archive does not carry is
  // unloaded, so the device runs the archive's scripts and nothing else. It runs AFTER the reload
  // phase and the bring-up on purpose - the slots those phases put the archive's own scripts in are
  // then already occupied by them, so only an earlier setup's leftovers are candidates - and every
  // unload is reported, because an unload is a deletion and a silent one is as bad as a silent
  // delete of a requester entry.
  final swept = await pruneScriptSlots(plan);
  for (final failure in swept.failures) {
    failed++;
    if (failures.length < 100) failures.add(failure);
  }
  return (
    written: written,
    failed: failed,
    failures: failures,
    requesterPrune: pruned.prunes,
    scriptSlots: swept.prunes,
    scriptReload: reloadedScripts.reports,
  );
}
