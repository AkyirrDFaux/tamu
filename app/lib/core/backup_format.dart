/// Semantic backup archive model (Docs/App/Backup.md).
///
/// "Creates a zipfile containing per device JSON files if backing up the entire
/// network." Each device JSON holds the whole functionality set semantically: the
/// entire Register (System/static/dynamic/serialised blocks and their entries), the
/// Subscriptions tables, the Scripts, the LED layouts, the SNDB (cores) and the device
/// file system.
///
/// Pure data model + JSON; capture/restore I/O lives in `backup.dart`.
library;

import 'dart:convert';
import 'dart:typed_data';

import 'backup_layout.dart';
import 'backup_script.dart';
import 'types.dart';

/// Format version of the per-device JSON.
const int backupFormatVersion = 2;

// ---------------------------------------------------------------------------
// Register entries
// ---------------------------------------------------------------------------

class BackupEntry {
  final String field; // field name in words
  final int fieldIndex; // literal index (for targeting)
  final String key; // key name in words
  final int keyIndex;
  final String type; // data type word
  final List<String> flags;
  final String? unit;
  final Object? value;
  final int size; // wire byte length (size-flexible types)

  /// True for a STRUCTURAL row: a dynamic block entry whose identity, type, flags and size are
  /// archived but whose value is not. Its JSON carries no `value` key at all.
  ///
  /// A dynamic block's table (`.DT_<hex2>`) is persistent device state, separate from its value
  /// space (`.DV_<hex2>`; firmware `MemoryDynamic.h`), and a restore rebuilds that table from the
  /// archived rows (`prepareDynamicBlocks`) before it can write any value. A row whose flags lack
  /// `Persistent` holds a value the firmware resets to its default at boot, so the archive keeps
  /// the row (the structure is what a restore needs) and drops the value: the capture stores it
  /// as a structural row and the restore plans no value write for it.
  ///
  /// A static block has a compile-time layout (`Docs/Services/Register.md`), so it never carries a
  /// structural row: a non-persistent field there is not captured at all.
  final bool structural;

  BackupEntry({
    required this.field,
    required this.fieldIndex,
    required this.key,
    required this.keyIndex,
    required this.type,
    required this.flags,
    this.unit,
    required this.value,
    required this.size,
    this.structural = false,
  });

  bool get readOnly => flags.contains('Read Only');
  bool get persistent => flags.contains('Persistent');

  Map<String, dynamic> toJson() => {
    'field': field,
    'fieldIndex': fieldIndex,
    'key': key,
    'keyIndex': keyIndex,
    'type': type,
    if (flags.isNotEmpty) 'flags': flags,
    if (unit != null && unit!.isNotEmpty) 'unit': unit,
    // A structural row has no value, so the key is absent rather than null: the archive says
    // "this row exists" without carrying a reading the device will discard at boot.
    if (!structural) 'value': value,
    'size': size,
  };

  static BackupEntry fromJson(Map<String, dynamic> json) => BackupEntry(
    field: json['field'] as String? ?? 'Field ${json['fieldIndex']}',
    fieldIndex: (json['fieldIndex'] as num?)?.toInt() ?? 0,
    key: json['key'] as String? ?? 'Key ${json['keyIndex']}',
    keyIndex: (json['keyIndex'] as num?)?.toInt() ?? 0,
    type: json['type'] as String? ?? 'Undefined',
    flags: (json['flags'] as List?)?.cast<String>() ?? const [],
    unit: json['unit'] as String?,
    value: json['value'],
    // An absent `value` key means a structural row; every value-carrying row keeps the key
    // (a reserved placeholder writes it as null), so both shapes round-trip unchanged.
    structural: !json.containsKey('value'),
    size: (json['size'] as num?)?.toInt() ?? 0,
  );
}

class BackupBlock {
  final String type; // block type word
  final int typeIndex;
  final int instance;
  final String name;
  final bool isDynamic;
  final List<BackupEntry> entries;

  BackupBlock({
    required this.type,
    required this.typeIndex,
    required this.instance,
    required this.name,
    required this.isDynamic,
    required this.entries,
  });

  Map<String, dynamic> toJson() => {
    'type': type,
    'typeIndex': typeIndex,
    'instance': instance,
    'name': name,
    if (isDynamic) 'dynamic': true,
    'entries': [for (final e in entries) e.toJson()],
  };

  static BackupBlock fromJson(Map<String, dynamic> json) => BackupBlock(
    type: json['type'] as String? ?? 'Undefined',
    typeIndex: (json['typeIndex'] as num?)?.toInt() ?? 0,
    instance: (json['instance'] as num?)?.toInt() ?? 0,
    name: json['name'] as String? ?? '',
    isDynamic: json['dynamic'] == true,
    entries: (json['entries'] as List? ?? [])
        .map((e) => BackupEntry.fromJson(e as Map<String, dynamic>))
        .toList(),
  );
}

// ---------------------------------------------------------------------------
// Subscriptions (semantic)
// ---------------------------------------------------------------------------

/// A Register address in words: block type, instance, field and key.
class BackupBlockRef {
  final String block;
  final int typeIndex;
  final int instance;
  final String field;
  final int fieldIndex;
  final String key;
  final int keyIndex;

  const BackupBlockRef({
    required this.block,
    required this.typeIndex,
    required this.instance,
    required this.field,
    required this.fieldIndex,
    required this.key,
    required this.keyIndex,
  });

  Map<String, dynamic> toJson() => {
    'block': block,
    'typeIndex': typeIndex,
    'instance': instance,
    'field': field,
    'fieldIndex': fieldIndex,
    'key': key,
    'keyIndex': keyIndex,
  };

  static BackupBlockRef fromJson(Map<String, dynamic> json) => BackupBlockRef(
    block: json['block'] as String? ?? 'Undefined',
    typeIndex: (json['typeIndex'] as num?)?.toInt() ?? 0,
    instance: (json['instance'] as num?)?.toInt() ?? 0,
    field: json['field'] as String? ?? 'Field ${json['fieldIndex']}',
    fieldIndex: (json['fieldIndex'] as num?)?.toInt() ?? 0,
    key: json['key'] as String? ?? 'Key ${json['keyIndex']}',
    keyIndex: (json['keyIndex'] as num?)?.toInt() ?? 0,
  );
}

/// Requester-side subscription (what the device asked to receive).
class BackupRequesterSubscription {
  final String provider; // "net.device"
  final String trigger; // trigger word
  final int periodMs;
  final int minTimeMs;
  final double deadzone;
  final BackupBlockRef source;
  final BackupBlockRef target;

  const BackupRequesterSubscription({
    required this.provider,
    required this.trigger,
    required this.periodMs,
    required this.minTimeMs,
    required this.deadzone,
    required this.source,
    required this.target,
  });

  Map<String, dynamic> toJson() => {
    'provider': provider,
    'trigger': trigger,
    'periodMs': periodMs,
    'minTimeMs': minTimeMs,
    'deadzone': deadzone,
    'source': source.toJson(),
    'target': target.toJson(),
  };

  static BackupRequesterSubscription fromJson(Map<String, dynamic> json) =>
      BackupRequesterSubscription(
        provider: json['provider'] as String? ?? '0.0',
        trigger: json['trigger'] as String? ?? 'Periodic',
        periodMs: (json['periodMs'] as num?)?.toInt() ?? 0,
        minTimeMs: (json['minTimeMs'] as num?)?.toInt() ?? 0,
        deadzone: (json['deadzone'] as num?)?.toDouble() ?? 0,
        source: BackupBlockRef.fromJson(json['source'] as Map<String, dynamic>),
        target: BackupBlockRef.fromJson(json['target'] as Map<String, dynamic>),
      );
}

/// Provider-side subscription (installed by a remote requester: the entry the device
/// serves, which the restore rebuilds so a requester that is not present to re-register
/// still finds the node sending).
class BackupProviderSubscription {
  final String requester; // "net.device"
  final int trid; // subscription identity; the provider table is keyed by it
  final String trigger;
  final int periodMs;
  final int minTimeMs;
  final double deadzone;
  final BackupBlockRef source;

  const BackupProviderSubscription({
    required this.requester,
    required this.trid,
    required this.trigger,
    required this.periodMs,
    required this.minTimeMs,
    required this.deadzone,
    required this.source,
  });

  Map<String, dynamic> toJson() => {
    'requester': requester,
    'trid': trid,
    'trigger': trigger,
    'periodMs': periodMs,
    'minTimeMs': minTimeMs,
    'deadzone': deadzone,
    'source': source.toJson(),
  };

  static BackupProviderSubscription fromJson(Map<String, dynamic> json) =>
      BackupProviderSubscription(
        requester: json['requester'] as String? ?? '0.0',
        trid: (json['trid'] as num?)?.toInt() ?? 0,
        trigger: json['trigger'] as String? ?? 'Periodic',
        periodMs: (json['periodMs'] as num?)?.toInt() ?? 0,
        minTimeMs: (json['minTimeMs'] as num?)?.toInt() ?? 0,
        deadzone: (json['deadzone'] as num?)?.toDouble() ?? 0,
        source: BackupBlockRef.fromJson(json['source'] as Map<String, dynamic>),
      );
}

// ---------------------------------------------------------------------------
// SNDB (Serial Number Database)
// ---------------------------------------------------------------------------

class BackupSndbEntry {
  final String serial;
  final String address; // "net.device"

  const BackupSndbEntry({required this.serial, required this.address});

  Map<String, dynamic> toJson() => {'serial': serial, 'address': address};

  static BackupSndbEntry fromJson(Map<String, dynamic> json) => BackupSndbEntry(
    serial: json['serial'] as String? ?? '',
    address: json['address'] as String? ?? '0.0',
  );
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

class BackupFile {
  final String name;
  final String kind; // semantic file kind (e.g. "LED layout")
  final String data; // base64

  BackupFile({required this.name, required this.kind, required this.data});

  Uint8List get bytes => base64Decode(data);

  Map<String, dynamic> toJson() => {'name': name, 'kind': kind, 'data': data};

  static BackupFile fromJson(Map<String, dynamic> json) => BackupFile(
    name: json['name'] as String? ?? '',
    kind: json['kind'] as String? ?? 'Binary',
    data: json['data'] as String? ?? '',
  );
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

class BackupDevice {
  final int format;
  final String? created;
  final String type; // device type word
  final int typeId;
  final int id;
  final String name;
  final String? serial;
  final List<BackupBlock> blocks;
  final List<BackupScript> scripts;

  /// LED layouts, decoded from their files (`LAY_1`) into the semantic map. A layout listed here
  /// is rebuilt on restore and never also travels in [files] - the same rule the scripts follow.
  /// Absent in an archive captured before layouts had a semantic form: such an archive carries the
  /// file's bytes in [files] instead, and the restore writes them as it does today.
  final List<BackupLayout> layouts;
  final List<BackupRequesterSubscription> requesterSubscriptions;
  final List<BackupProviderSubscription> providerSubscriptions;
  final List<BackupSndbEntry> sndb;
  final List<BackupFile> files;

  BackupDevice({
    this.format = backupFormatVersion,
    this.created,
    required this.type,
    required this.typeId,
    required this.id,
    required this.name,
    this.serial,
    required this.blocks,
    this.scripts = const [],
    this.layouts = const [],
    this.requesterSubscriptions = const [],
    this.providerSubscriptions = const [],
    this.sndb = const [],
    this.files = const [],
  });

  String get address => idToString(id);

  Map<String, dynamic> toJson() => {
    'app': 'tamuapp',
    'format': format,
    'semantic': true,
    if (created != null) 'created': created,
    'type': type,
    'typeId': typeId,
    'id': id,
    'address': address,
    'name': name,
    'serial': serial,
    'blocks': [for (final b in blocks) b.toJson()],
    'scripts': [for (final s in scripts) s.toJson()],
    // Omitted when empty, so a device with no layout (and any archive captured before layouts had
    // a semantic form) is written exactly as before.
    if (layouts.isNotEmpty) 'layouts': [for (final l in layouts) l.toJson()],
    'subscriptions': {
      'requester': [for (final s in requesterSubscriptions) s.toJson()],
      'provider': [for (final s in providerSubscriptions) s.toJson()],
    },
    'sndb': [for (final s in sndb) s.toJson()],
    'files': [for (final f in files) f.toJson()],
  };

  static BackupDevice fromJson(Map<String, dynamic> json) {
    // Development phase: no pre-release archives ship, so only the current
    // semantic format is read. The legacy numeric-format rejection was dropped
    // along with the other pre-release archive branches.
    //
    // The format version stays 2 with the semantic layout added: `layouts` is an ADDITIVE key, and
    // an archive without it (every archive captured before layouts had a semantic form, and the
    // setup archives) reads as a device whose layouts are absent - its `LAY_1` is in `files` and
    // the restore writes those bytes as before. So no archive in the wild is broken by the change
    // and none needs a version-gated branch: the two shapes coexist under one version.
    final format = (json['format'] as num?)?.toInt() ?? backupFormatVersion;
    final subs = json['subscriptions'] as Map<String, dynamic>? ?? const {};
    return BackupDevice(
      format: format,
      created: json['created'] as String?,
      type: json['type'] as String? ?? 'Unknown',
      typeId: (json['typeId'] as num?)?.toInt() ?? 0,
      id:
          (json['id'] as num?)?.toInt() ??
          _parseAddress(json['address'] as String?),
      name: json['name'] as String? ?? 'Device',
      serial: json['serial'] as String?,
      blocks: (json['blocks'] as List? ?? [])
          .map((b) => BackupBlock.fromJson(b as Map<String, dynamic>))
          .toList(),
      scripts: (json['scripts'] as List? ?? [])
          .map((s) => BackupScript.fromJson(s as Map<String, dynamic>))
          .toList(),
      layouts: (json['layouts'] as List? ?? [])
          .map((l) => BackupLayout.fromJson(l as Map<String, dynamic>))
          .toList(),
      requesterSubscriptions: (subs['requester'] as List? ?? [])
          .map(
            (s) =>
                BackupRequesterSubscription.fromJson(s as Map<String, dynamic>),
          )
          .toList(),
      providerSubscriptions: (subs['provider'] as List? ?? [])
          .map(
            (s) =>
                BackupProviderSubscription.fromJson(s as Map<String, dynamic>),
          )
          .toList(),
      sndb: (json['sndb'] as List? ?? [])
          .map((s) => BackupSndbEntry.fromJson(s as Map<String, dynamic>))
          .toList(),
      files: (json['files'] as List? ?? [])
          .map((f) => BackupFile.fromJson(f as Map<String, dynamic>))
          .toList(),
    );
  }

  static int _parseAddress(String? address) =>
      address == null ? 0 : (idFromString(address) ?? 0);

  String get fileName =>
      '${id}_${name.replaceAll(RegExp(r'[^A-Za-z0-9._-]'), '_')}.json';
}
