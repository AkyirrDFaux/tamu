/// Read-only view of the device's *stored* (Saved) values - the "Backup" half of the
/// Register page's Current/Backup toggle (Docs/App/Service views/Register.md: "Save button
/// (current) / Recall button (backup)").
///
/// Current view shows what is live in RAM; this shows what a Save actually persisted:
/// static/System fields from `.SV` and dynamic entries from `.DT_`/`.DV_`. Fields with no
/// stored entry are shown explicitly as "not backed up" rather than hidden, so a missing
/// save is visible instead of looking like a normal empty field.
library;

import 'package:flutter/material.dart';

import '../core/block_registry.dart' show blockInfoFor;
import '../core/device_backup.dart';
import '../core/system_schema.dart' show systemFieldKeys, systemKeyName;
import '../core/types.dart';
import 'theme.dart' show kSurfaceAlt;
import 'value_editor.dart' show dataTypeLabel, formatValue;

/// Which set of values the Register page shows (docs Register.md appbar).
enum RegisterViewMode { current, backup }

/// One rendered field row of the backup view.
class _BackupRow {
  final String label;
  final String value;
  final String detail;
  final bool stored;
  final int field;
  final int key;
  const _BackupRow({
    required this.label,
    required this.value,
    required this.detail,
    required this.stored,
    required this.field,
    required this.key,
  });
}

class RegisterBackupView extends StatelessWidget {
  final DeviceBackup backup;

  /// The device's blocks in the page's order (System included, dynamic tombstones too).
  final List<({int type, int inst, ValueInfo meta, String name})?> blocks;

  /// Recalls one stored field into RAM. Null disables the per-field recall buttons.
  final Future<void> Function(int blockType, int inst, int field, int key)? onRecall;

  final bool busy;

  /// Whether the device has the System NetID field (a core). A node has no NetID row.
  final bool hasNetId;

  const RegisterBackupView({
    super.key,
    required this.backup,
    required this.blocks,
    this.onRecall,
    this.busy = false,
    this.hasNetId = true,
  });

  @override
  Widget build(BuildContext context) {
    final children = <Widget>[];
    for (final b in blocks) {
      if (b == null) continue;
      // Dynamic tombstone slots carry no block and are hidden, like the live view.
      if (isHiddenRegisterSlot(b.type, b.meta)) continue;
      if (b.type == 0) {
        children.add(_card(b, _systemRows()));
      } else if (isScriptType(b.type)) {
        children.add(_card(b, const []));
      } else if (isDynamicType(b.type)) {
        children.add(_card(b, _dynamicRows(b.inst)));
      } else {
        children.add(_card(b, _staticRows(b)));
      }
    }
    if (children.isEmpty) {
      return const Center(child: Text('No blocks found'));
    }
    return SingleChildScrollView(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(10, 10, 10, 24),
        child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: children),
      ),
    );
  }

  /// Static blocks persist their Persistent fields in the `.SV` space; the decoder already
  /// resolved each field's offset against this same block list.
  List<_BackupRow> _staticRows(({int type, int inst, ValueInfo meta, String name}) b) {
    final info = blockInfoFor(BlockType.fromValue(b.type));
    final rows = <_BackupRow>[];
    for (var f = 0; f < b.meta.size; f++) {
      final name = info?.field(f)?.name;
      final entry = backup.staticField(b.type, b.inst, f);
      rows.add(_row(
        label: name ?? 'Field $f',
        entry: entry,
        field: f,
        key: 0xFF,
      ));
    }
    return rows;
  }

  List<_BackupRow> _systemRows() {
    // Only Name (6) and, on a core with the NetID field, NetID (7) are Save targets.
    final fields = hasNetId
        ? const [systemNameField, systemNetIdField]
        : const [systemNameField];
    final rows = <_BackupRow>[];
    for (final field in fields) {
      final keys = systemFieldKeys[field];
      final key = keys == null || keys.isEmpty ? 0xFF : keys.keys.first;
      final entry = backup.staticField(0, 0, field);
      rows.add(_row(
        label: systemKeyName(field, key),
        entry: entry,
        field: field,
        key: key,
      ));
    }
    return rows;
  }

  /// Dynamic blocks persist through .DT_/.DV_. The stored *table* lists every entry, so a
  /// volatile entry can be reported as "not persisted" rather than as a missing save.
  List<_BackupRow> _dynamicRows(int inst) {
    final table = backup.dynamicTableFor(inst);
    if (table == null) {
      return [
        const _BackupRow(
          label: 'Block',
          value: 'not backed up',
          detail: 'no saved table for this slot',
          stored: false,
          field: 0,
          key: 0,
        )
      ];
    }
    final rows = <_BackupRow>[];
    for (final f in backup.dynamicFieldsFor(inst)) {
      for (final k in backup.dynamicKeysFor(inst, f)) {
        final entry = backup.dynamicTableEntry(inst, f, k);
        rows.add(_row(
          label: 'Field $f, key $k',
          entry: backup.dynamicField(inst, f, k),
          field: f,
          key: k,
          notPersisted: entry != null && !entry.persistent,
        ));
      }
    }
    if (rows.isEmpty) {
      return [
        const _BackupRow(
          label: 'Block',
          value: 'not backed up',
          detail: 'the saved table has no entries',
          stored: false,
          field: 0,
          key: 0,
        )
      ];
    }
    return rows;
  }

  _BackupRow _row({
    required String label,
    required BackupEntry? entry,
    required int field,
    required int key,
    bool notPersisted = false,
  }) {
    if (entry == null) {
      return _BackupRow(
        label: label,
        value: notPersisted ? 'not persisted' : 'not backed up',
        detail: notPersisted ? 'volatile entry (no stored value)' : 'no saved value',
        stored: false,
        field: field,
        key: key,
      );
    }
    return _BackupRow(
      label: label,
      value: formatValue(entry.meta.dataType, entry.value),
      detail: dataTypeLabel(entry.meta.dataType),
      stored: true,
      field: field,
      key: key,
    );
  }

  Widget _card(({int type, int inst, ValueInfo meta, String name}) b, List<_BackupRow> rows) {
    final typeLabel = b.type == 0 ? 'System' : BlockType.fromValue(b.type).label;
    return Card(
      color: kSurfaceAlt,
      margin: const EdgeInsets.only(bottom: 8),
      clipBehavior: Clip.antiAlias,
      child: Column(children: [
        ListTile(
          dense: true,
          title: Text(
            '${b.name.isEmpty ? typeLabel : b.name}  ·  $typeLabel'
            '${b.inst == 0 ? '' : ' [${b.inst}]'}',
            style: const TextStyle(fontWeight: FontWeight.w600, fontSize: 13),
          ),
          subtitle: Text(
              isScriptType(b.type)
                  ? 'scripts are not backed up'
                  : (rows.isEmpty ? 'not backed up' : '${rows.where((r) => r.stored).length}'
                      ' of ${rows.length} stored'),
              style: const TextStyle(fontSize: 10)),
        ),
        if (rows.isNotEmpty)
          Material(
            color: Colors.black26,
            child: Column(children: [
              for (final r in rows)
                ListTile(
                  dense: true,
                  contentPadding: const EdgeInsets.only(left: 16, right: 8),
                  title: Text('${r.label}: ${r.value}',
                      style: TextStyle(
                          fontFamily: 'monospace',
                          fontSize: 12,
                          color: r.stored ? Colors.white : Colors.white38)),
                  subtitle: Text(r.detail,
                      style: const TextStyle(fontSize: 10, color: Colors.white38)),
                  trailing: (r.stored && onRecall != null)
                      ? IconButton(
                          tooltip: 'Recall this value from backup',
                          icon: const Icon(Icons.restore, size: 18),
                          onPressed: busy
                              ? null
                              : () => onRecall!(b.type, b.inst, r.field, r.key),
                        )
                      : null,
                ),
            ]),
          ),
      ]),
    );
  }
}
