import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';

import '../core/backup.dart';
import '../core/backup_format.dart';
import '../core/connection.dart';
import '../core/device_db.dart';
import '../core/host_files.dart';
import '../core/notifications.dart';
import '../core/script_file.dart';
import '../core/types.dart';
import 'theme.dart';
import 'viewer_kit.dart';
import 'widgets.dart';

/// Backup page (Docs/App/Backup.md): unified whole-network backup and restore
/// as a zipfile of per-device JSON files, with per-part selection of synced
/// items and target remapping when compatible.
class BackupPage extends StatefulWidget {
  const BackupPage({super.key});

  @override
  State<BackupPage> createState() => _BackupPageState();
}

class _BackupPageState extends State<BackupPage> {
  final _db = DeviceDatabase.instance;
  final _selected = <int>{};
  bool _busy = false;
  bool _includeFiles = true;

  void _snack(String message) => showSnack(context, message);

  Future<void> _createBackup() async {
    if (_selected.isEmpty) return;
    setState(() => _busy = true);
    try {
      final devices = <BackupDevice>[];
      for (final id in _selected) {
        final captured = await captureDevice(id, includeFiles: _includeFiles);
        if (captured != null) devices.add(captured);
      }
      if (devices.isEmpty) {
        _snack('No device responded');
        return;
      }
      final zip = buildBackupZip(devices);
      final target = await saveBytesWithPicker(
        fileName:
            'tamu_backup_${DateTime.now().toIso8601String().substring(0, 10)}.zip',
        bytes: zip,
        allowedExtensions: ['zip'],
      );
      if (target == null) return; // cancelled
      final items = devices.fold<int>(
        0,
        (sum, d) => sum + d.blocks.fold<int>(0, (s, b) => s + b.entries.length),
      );
      _snack(
        'Backup saved (${devices.length} device(s), $items register entries)',
      );
      notifyAppEvent(
        'Backup finished',
        'Backup saved (${devices.length} device(s), $items register entries)',
      );
    } catch (error) {
      _snack('Backup failed: $error');
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  Future<void> _restoreBackup() async {
    final result = await FilePicker.pickFiles(
      type: FileType.custom,
      allowedExtensions: ['zip'],
    );
    final file = result.singleOrNull;
    if (file == null) return;
    setState(() => _busy = true);
    List<BackupDevice> devices;
    try {
      final bytes = await file.readAsBytes();
      devices = parseBackupZip(bytes);
    } catch (error) {
      _snack('Restore failed: $error');
      if (mounted) setState(() => _busy = false);
      return;
    }

    RestorePlan plan;
    try {
      plan = await buildRestorePlan(devices);
    } catch (error) {
      _snack('Could not build the restore plan: $error');
      if (mounted) setState(() => _busy = false);
      return;
    }
    if (mounted) setState(() => _busy = false);
    if (!mounted) return;

    final applied = await Navigator.of(context).push<bool>(
      MaterialPageRoute(builder: (_) => RestorePlanPage(plan: plan)),
    );
    if (applied == true && mounted) {
      _snack('Restore applied');
    }
  }

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: Listenable.merge([_db, ConnectionManager.instance]),
      builder: (context, _) {
        final connected = ConnectionManager.instance.isConnected;
        final devices = _db.all;
        return Scaffold(
          appBar: AppBar(
            title: const Text('Backup'),
            leading: const ShellDrawerButton(),
          ),
          body: !connected
              ? const Center(child: Text('Not connected'))
              : Column(
                  children: [
                    Padding(
                      padding: const EdgeInsets.fromLTRB(12, 12, 12, 0),
                      child: Row(
                        children: [
                          Text(
                            '${_selected.length} of ${devices.length} selected',
                            style: Theme.of(context).textTheme.bodySmall,
                          ),
                          const Spacer(),
                          TextButton(
                            onPressed: () => setState(
                              () => _selected.length == devices.length
                                  ? _selected.clear()
                                  : _selected.addAll(devices.map((d) => d.id)),
                            ),
                            child: Text(
                              _selected.length == devices.length &&
                                      devices.isNotEmpty
                                  ? 'Deselect all'
                                  : 'Select all',
                            ),
                          ),
                        ],
                      ),
                    ),
                    SwitchListTile(
                      dense: true,
                      title: const Text('Include device files'),
                      value: _includeFiles,
                      onChanged: (v) => setState(() => _includeFiles = v),
                    ),
                    const Divider(height: 1),
                    Expanded(
                      child: ListView.builder(
                        itemCount: devices.length,
                        itemBuilder: (context, index) {
                          final device = devices[index];
                          return CheckboxListTile(
                            secondary: Icon(deviceTypeIcon(device.type)),
                            title: Text(device.name),
                            subtitle: Text(
                              '${idToString(device.id)} - ${device.type.label}',
                            ),
                            value: _selected.contains(device.id),
                            onChanged: device.stale
                                ? null
                                : (value) => setState(() {
                                    value == true
                                        ? _selected.add(device.id)
                                        : _selected.remove(device.id);
                                  }),
                          );
                        },
                      ),
                    ),
                  ],
                ),
          bottomNavigationBar: SafeArea(
            child: Padding(
              padding: const EdgeInsets.all(12),
              child: Row(
                children: [
                  Expanded(
                    child: FilledButton.icon(
                      onPressed: _busy || _selected.isEmpty
                          ? null
                          : _createBackup,
                      icon: const Icon(Icons.archive),
                      label: const Text('Create backup'),
                    ),
                  ),
                  const SizedBox(width: 12),
                  Expanded(
                    child: FilledButton.icon(
                      onPressed: _busy || !connected ? null : _restoreBackup,
                      icon: const Icon(Icons.restore),
                      label: const Text('Restore'),
                    ),
                  ),
                ],
              ),
            ),
          ),
        );
      },
    );
  }
}

/// Per-part restore: pick which items to write and (optionally) remap a source
/// device/block to a different live one when compatible.
class RestorePlanPage extends StatefulWidget {
  final RestorePlan plan;

  const RestorePlanPage({super.key, required this.plan});

  @override
  State<RestorePlanPage> createState() => _RestorePlanPageState();
}

class _RestorePlanPageState extends State<RestorePlanPage> {
  RestorePlan get plan => widget.plan;
  bool _busy = false;

  /// The view filter. [PlanFilter.diff] by default: a restore plan exists to show what it would
  /// change, not the hundred rows it would rewrite with the same value.
  PlanFilter _filter = PlanFilter.diff;

  void _reselectDevice(int sourceId, LiveDevice device) {
    setState(() {
      plan.deviceTargets[sourceId] = device;
      plan.blockTargets.removeWhere((key, _) => key.startsWith('$sourceId:'));
      plan.resolve();
    });
  }

  void _reselectBlock(int sourceDeviceId, BackupBlock block, LiveBlock target) {
    setState(() {
      plan.blockTargets[RestorePlan.blockKey(sourceDeviceId, block)] = target;
      plan.resolve();
    });
  }

  Future<void> _apply() async {
    setState(() => _busy = true);
    try {
      final result = await applyRestorePlan(plan);
      final cancelledTrids = [
        for (final prune in result.requesterPrune) ...prune.cancelled,
      ];
      // An unloaded script slot stops a running script just as a cancelled requester entry stops a
      // feed, so it is reported with its slot and file id rather than left silent.
      final unloadedSlots = [
        for (final prune in result.scriptSlots) ...prune.unloaded.keys,
      ];
      final keptSlots = [
        for (final prune in result.scriptSlots) ...prune.keptBySelection.keys,
      ];
      // An entry the page listed as a DELETE and the user turned off is kept, so it is counted
      // with the others the run deliberately left alone rather than vanishing from the report.
      final keptTrids = [
        for (final prune in result.requesterPrune) ...prune.kept,
      ];
      // A duplicate is what made the same script run twice, so it is named with the slot the
      // archive actually names for it rather than folded into the plain unload count.
      final duplicateSlots = [
        for (final prune in result.scriptSlots)
          for (final e in prune.duplicates.entries)
            'slot ${e.key} (duplicate of ${scriptFileName(e.value.fileId)} at '
                'slot ${e.value.canonical})',
      ];
      // A reload is what makes a changed script actually run, so the slots it touched are part of
      // the run's outcome - and so is the count it left alone, which is the evidence that unchanged
      // running scripts were not disturbed.
      final reloadedSlots = [
        for (final report in result.scriptReload) ...report.reloaded.keys,
      ];
      if (mounted) {
        showSnack(
          context,
          'Restored ${result.written} item(s)'
          // A cancellation is a deletion, so it is never silent: the count and the TRIDs are
          // reported with the run (Docs/App/Backup.md).
          '${cancelledTrids.isEmpty ? '' : ', cancelled ${cancelledTrids.length} '
                    'requester entr${cancelledTrids.length == 1 ? 'y' : 'ies'} '
                    '(${cancelledTrids.map((t) => '0x${t.toRadixString(16)}').join(', ')})'}'
          // Same for an unload: the slot is what the device stops running.
          '${unloadedSlots.isEmpty ? '' : ', unloaded ${unloadedSlots.length} '
                    'script slot${unloadedSlots.length == 1 ? '' : 's'} the archive '
                    'does not carry (${unloadedSlots.map((s) => 'slot $s').join(', ')})'}'
          // A duplicate ran one script twice; the slot the archive names for it is named too.
          '${duplicateSlots.isEmpty ? '' : ', unloaded ${duplicateSlots.length} '
                    'duplicate script slot${duplicateSlots.length == 1 ? '' : 's'} '
                    '(${duplicateSlots.join(', ')})'}'
          // ... and a slot left alone because it was not part of this run says so, so it cannot be
          // mistaken for one the archive carries.
          '${keptSlots.isEmpty ? '' : ', left ${keptSlots.length} '
                    'unselected script slot${keptSlots.length == 1 ? '' : 's'} '
                    'loaded'}'
          '${keptTrids.isEmpty ? '' : ', left ${keptTrids.length} '
                    'unselected requester '
                    'entr${keptTrids.length == 1 ? 'y' : 'ies'} '
                    '(${keptTrids.map((t) => '0x${t.toRadixString(16)}').join(', ')})'}'
          // A reloaded slot is running the code this restore wrote; saying so separates "the file
          // is new" from "the device runs it".
          '${reloadedSlots.isEmpty ? '' : ', reloaded ${reloadedSlots.length} '
                    'changed script slot${reloadedSlots.length == 1 ? '' : 's'} '
                    '(${reloadedSlots.map((s) => 'slot $s').join(', ')})'}'
          '${result.failed > 0 ? ', ${result.failed} failed' : ''}',
        );
        Navigator.of(context).pop(true);
      }
    } catch (error) {
      if (mounted) showSnack(context, 'Restore failed: $error');
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  @override
  Widget build(BuildContext context) {
    final view = plan.view(_filter);
    return Scaffold(
      appBar: AppBar(title: const Text('Restore plan')),
      body: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          RestoreFilterBar(
            plan: plan,
            filter: _filter,
            onFilter: (filter) => setState(() => _filter = filter),
            onClass: (kind, selected) =>
                setState(() => plan.setClassSelected(kind, selected)),
          ),
          const Divider(height: 1),
          Expanded(child: _body(view)),
        ],
      ),
      bottomNavigationBar: _footer(view),
    );
  }

  /// The plan, filtered: one section per source device, then the rows it shows.
  ///
  /// A device with nothing to show keeps its section under `all` and `diff` and says why (it is
  /// the answer the user came for: this device already matches), and is dropped under a
  /// single-class filter, where an empty section would only be noise.
  Widget _body(PlanView view) {
    if (view.isEmpty) return _emptyState();
    final sections = <Widget>[];
    for (final device in plan.devices) {
      final items = [
        for (final item in view.items)
          if (item.device.id == device.id) item,
      ];
      final removals = [
        for (final removal in view.removals)
          if (removal.device.id == device.id) removal,
      ];
      if (items.isEmpty &&
          removals.isEmpty &&
          _filter != PlanFilter.all &&
          _filter != PlanFilter.diff) {
        continue;
      }
      sections.add(_deviceSection(device, items, removals));
    }
    return ListView(children: sections);
  }

  /// What the page says when the filter shows nothing.
  ///
  /// The default view of an archive that already matches is empty by definition, so it offers the
  /// way out instead of leaving the user on a blank page: `all` is where the untouched rows live.
  Widget _emptyState() {
    final matches = plan.items.isNotEmpty || plan.removals.isNotEmpty;
    if (_filter == PlanFilter.diff && matches) {
      return Center(
        child: Padding(
          padding: Insets.page,
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              const Icon(
                Icons.check_circle_outline,
                size: IconSize.xl,
                color: kSuccess,
              ),
              const SizedBox(height: Spacing.md),
              Text(
                'No differences',
                style: Theme.of(context).textTheme.titleSmall,
              ),
              const SizedBox(height: Spacing.xs),
              Text(
                'Every item in this archive already matches the device.',
                style: Theme.of(context).textTheme.bodySmall,
                textAlign: TextAlign.center,
              ),
              TextButton(
                onPressed: () => setState(() => _filter = PlanFilter.all),
                child: const Text('Show all items'),
              ),
            ],
          ),
        ),
      );
    }
    return StateMessage.empty(
      message: 'Nothing under ${_filter.label.toLowerCase()}',
      details: 'Switch the filter to see what the plan carries.',
    );
  }

  /// The footer: the counts of the FILTERED view, the deletions counted apart, and the commit.
  Widget _footer(PlanView view) {
    final theme = Theme.of(context);
    // What the Restore button commits is the whole plan, not only the rows on screen - the filter
    // is a view, so switching it must never change what the run would write.
    final deletions = plan.removals.where((r) => r.selected).length;
    final total = plan.selectedCount + deletions;
    return SafeArea(
      child: Padding(
        padding: const EdgeInsets.all(Spacing.lg),
        child: Row(
          children: [
            Expanded(
              child: Column(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(
                    '${view.selected} of ${view.shown} shown selected',
                    style: theme.textTheme.bodySmall,
                  ),
                  if (view.deletes > 0)
                    Text(
                      '${view.deletesSelected} of ${view.deletes} to delete',
                      style: theme.textTheme.bodySmall?.copyWith(
                        color: kError,
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                  if (view.unavailable > 0)
                    Text(
                      '${view.unavailable} unavailable',
                      style: theme.textTheme.bodySmall,
                    ),
                ],
              ),
            ),
            FilledButton.icon(
              onPressed: _busy || total == 0 ? null : _apply,
              icon: const Icon(Icons.restore),
              label: const Text('Restore'),
            ),
          ],
        ),
      ),
    );
  }

  Widget _deviceSection(
    BackupDevice device,
    List<RestoreItem> items,
    List<RestoreRemoval> removals,
  ) {
    final target = plan.deviceTargets[device.id];
    final entries = items.where((i) => i.kind == RestoreKind.entry).toList();
    final blocks = <BackupBlock>{for (final i in entries) i.block!}.toList();
    final scripts = items.where((i) => i.kind == RestoreKind.script).toList();
    final subs = items
        .where((i) => i.kind == RestoreKind.subscription)
        .toList();
    final sndb = items.where((i) => i.kind == RestoreKind.sndb).toList();
    final files = items.where((i) => i.kind == RestoreKind.file).toList();
    return Card(
      margin: const EdgeInsets.fromLTRB(
        Spacing.lg,
        Spacing.xs,
        Spacing.lg,
        Spacing.md,
      ),
      child: ExpansionTile(
        initiallyExpanded: true,
        title: Text('${device.name} (${device.type})'),
        subtitle: Padding(
          padding: const EdgeInsets.only(top: Spacing.xs),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              _deviceTargetPicker(device, target),
              Padding(
                padding: const EdgeInsets.only(top: Spacing.xs),
                child: Text(
                  _deviceAmounts(device),
                  style: Theme.of(context).textTheme.bodySmall,
                ),
              ),
            ],
          ),
        ),
        children: [
          if (items.isEmpty && removals.isEmpty)
            Padding(
              padding: Insets.row,
              child: Text(
                _filter == PlanFilter.diff
                    ? 'No differences: this device already matches the archive.'
                    : 'Nothing under ${_filter.label.toLowerCase()} for this device.',
                style: Theme.of(context).textTheme.bodySmall,
              ),
            ),
          for (final block in blocks) _blockSection(device, block, entries),
          if (scripts.isNotEmpty) _group('Scripts', scripts),
          if (sndb.isNotEmpty) _group('SNDB', sndb),
          if (files.isNotEmpty) _group('Files', files),
          if (subs.isNotEmpty) _group('Subscriptions', subs),
          // The deletions last and named as such: what the device would STOP doing, kept apart
          // from what it would be given.
          if (removals.isNotEmpty) _removalGroup(removals),
        ],
      ),
    );
  }

  /// The device's diff, counted by class: `3 add · 1 replace · 2 delete · 5 same`.
  ///
  /// Counted over the WHOLE plan for this device, not over the filtered rows, so the line says
  /// what the device is like even while a filter hides most of it.
  String _deviceAmounts(BackupDevice device) {
    final counts = <DiffKind, int>{};
    for (final item in plan.items) {
      if (item.device.id != device.id || item.diff == null) continue;
      counts[item.diff!] = (counts[item.diff!] ?? 0) + 1;
    }
    final deletes = plan.removals.where((r) => r.device.id == device.id).length;
    if (deletes > 0) counts[DiffKind.remove] = deletes;
    final parts = [
      for (final kind in DiffKind.values)
        if ((counts[kind] ?? 0) > 0) '${counts[kind]} ${_amountWord(kind)}',
    ];
    return parts.isEmpty ? 'nothing to compare' : parts.join(' · ');
  }

  /// How a class reads in the device line: an action for the three that change something, and
  /// "unchanged" for the one that does not.
  static String _amountWord(DiffKind kind) => switch (kind) {
    DiffKind.add => 'to add',
    DiffKind.change => 'to replace',
    DiffKind.remove => 'to delete',
    DiffKind.same => 'unchanged',
  };

  Widget _deviceTargetPicker(BackupDevice device, LiveDevice? target) {
    return DropdownButton<int>(
      isExpanded: true,
      value: target?.id,
      hint: const Text('No target'),
      items: [
        for (final live in plan.liveDevices)
          DropdownMenuItem(
            value: live.id,
            child: Text(
              '${live.name} (${idToString(live.id)})',
              overflow: TextOverflow.ellipsis,
            ),
          ),
      ],
      onChanged: (id) {
        final live = plan.liveDevices.where((d) => d.id == id).firstOrNull;
        if (live != null) _reselectDevice(device.id, live);
      },
    );
  }

  Widget _group(String title, List<RestoreItem> items) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: Spacing.lg),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Padding(
            padding: const EdgeInsets.only(
              top: Spacing.md,
              bottom: Spacing.hairline,
            ),
            child: Text(title, style: Theme.of(context).textTheme.titleSmall),
          ),
          for (final item in items) _itemTile(item),
        ],
      ),
    );
  }

  /// The deletion rows of one device, under their own heading and in the failure colour: an
  /// unload or a cancel stops something the rig is doing, so it is never listed among the writes
  /// without a word.
  Widget _removalGroup(List<RestoreRemoval> removals) {
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: Spacing.lg),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Padding(
            padding: const EdgeInsets.only(
              top: Spacing.md,
              bottom: Spacing.hairline,
            ),
            child: Text(
              'Delete from the device',
              style: Theme.of(
                context,
              ).textTheme.titleSmall?.copyWith(color: kError),
            ),
          ),
          for (final removal in removals) _removalTile(removal),
        ],
      ),
    );
  }

  Widget _blockSection(
    BackupDevice device,
    BackupBlock block,
    List<RestoreItem> entries,
  ) {
    final items = entries.where((i) => identical(i.block, block)).toList();
    final head = items.isNotEmpty ? items.first : null;
    final targetDevice = plan.deviceTargets[device.id];
    final options = targetDevice == null
        ? <LiveBlock>[]
        : plan.compatibleBlocks(targetDevice, block);
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: Spacing.lg),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Expanded(
                child: Text(
                  '${block.type} "${block.name}" #${block.instance}',
                  style: Theme.of(context).textTheme.titleSmall,
                ),
              ),
              if (options.length > 1)
                DropdownButton<LiveBlock>(
                  value: head?.targetBlock,
                  hint: const Text('Target'),
                  items: [
                    for (final live in options)
                      DropdownMenuItem(
                        value: live,
                        child: Text(
                          '${live.name} #${live.instance}',
                          overflow: TextOverflow.ellipsis,
                        ),
                      ),
                  ],
                  onChanged: (live) {
                    if (live != null) _reselectBlock(device.id, block, live);
                  },
                ),
            ],
          ),
          for (final item in items) _itemTile(item),
        ],
      ),
    );
  }

  /// One archive row: its class in the action word the box commits to (`ADD`, `REPLACE`,
  /// `SAME`), the icon and ink of that class, then the row's title and its detail.
  ///
  /// The row stays the page's own [CheckboxListTile] - the shared row for a plan - and the class
  /// is carried by tokens ([TamuChip]'s measured tones, the status inks), not by a colour of its
  /// own: a ticked `change` row says REPLACE, a ticked `add` row says ADD.
  Widget _itemTile(RestoreItem item) {
    final kind = item.diff;
    return CheckboxListTile(
      dense: true,
      value: item.selected,
      onChanged: item.ready
          ? (v) => setState(() => item.selected = v == true)
          : null,
      secondary: Icon(
        _classIcon(kind),
        size: IconSize.md,
        color: _classInk(kind),
      ),
      title: Row(
        children: [
          if (kind != null) ...[
            TamuChip(kind.word.toUpperCase(), tone: _classTone(kind)),
            const SizedBox(width: Spacing.sm),
          ],
          Expanded(child: Text(_title(item), overflow: TextOverflow.ellipsis)),
        ],
      ),
      subtitle: Text(
        _subtitle(item),
        overflow: TextOverflow.ellipsis,
        style: item.issue != null
            ? Theme.of(context).textTheme.bodySmall?.copyWith(color: kWarning)
            : null,
      ),
    );
  }

  /// One deletion row: what the device holds and the archive does not. Unticking it keeps it.
  Widget _removalTile(RestoreRemoval removal) {
    return CheckboxListTile(
      dense: true,
      value: removal.selected,
      onChanged: (v) => setState(() => removal.selected = v == true),
      secondary: const Icon(
        Icons.delete_outline,
        size: IconSize.md,
        color: kError,
      ),
      title: Row(
        children: [
          TamuChip(removal.diff.word.toUpperCase(), tone: ChipTone.error),
          const SizedBox(width: Spacing.sm),
          Expanded(child: Text(removal.label, overflow: TextOverflow.ellipsis)),
        ],
      ),
      subtitle: Text(
        removal.detail,
        overflow: TextOverflow.ellipsis,
        style: Theme.of(context).textTheme.bodySmall?.copyWith(color: kError),
      ),
    );
  }

  /// The icon of a class: a plus, a swap, a tick or a bin.
  IconData _classIcon(DiffKind? kind) => switch (kind) {
    DiffKind.add => Icons.add_circle_outline,
    DiffKind.change => Icons.sync_alt,
    DiffKind.same => Icons.check_circle_outline,
    DiffKind.remove => Icons.delete_outline,
    null => Icons.help_outline,
  };

  /// The ink of a class, from the status tokens.
  Color _classInk(DiffKind? kind) => switch (kind) {
    DiffKind.add => kSuccess,
    DiffKind.change => kWarning,
    DiffKind.same => kTextMuted,
    DiffKind.remove => kError,
    null => kTextDisabled,
  };

  /// The chip tone of a class: success, warning, subtle or error - the tones [TamuChip] already
  /// carries and the contrast audit already measures.
  ChipTone _classTone(DiffKind kind) => switch (kind) {
    DiffKind.add => ChipTone.success,
    DiffKind.change => ChipTone.warning,
    DiffKind.same => ChipTone.subtle,
    DiffKind.remove => ChipTone.error,
  };

  String _title(RestoreItem item) {
    switch (item.kind) {
      case RestoreKind.entry:
        return '${item.entry!.field} / ${item.entry!.key}';
      case RestoreKind.script:
        return '${item.script!.functionName.isEmpty ? 'Script' : item.script!.functionName} '
            '(${scriptFileName(item.script!.slot)})';
      case RestoreKind.subscription:
        return '${item.subscription!.trigger} from ${item.subscription!.provider}';
      case RestoreKind.sndb:
        return 'SNDB ${item.sndbEntry!.address}';
      case RestoreKind.file:
        return item.file!.name;
    }
  }

  /// The row's second line: why it is a difference, then the value it carries.
  ///
  /// For a `change` the note comes first ("the device holds a different value"), because that is
  /// the decision the box commits to; the archived value follows so the row still says what would
  /// be written.
  String _subtitle(RestoreItem item) {
    if (item.issue != null) return item.issue!;
    final detail = _detail(item);
    final note = item.diffNote;
    return note == null ? detail : '$note - $detail';
  }

  String _detail(RestoreItem item) {
    switch (item.kind) {
      case RestoreKind.entry:
        final e = item.entry!;
        final unit = (e.unit ?? '').isEmpty ? '' : ' ${e.unit}';
        return '${e.type} = ${_preview(e.value)}$unit';
      case RestoreKind.script:
        return '${item.script!.inputs.length} in, ${item.script!.outputs.length} out, '
            '${item.script!.lines.length} lines';
      case RestoreKind.subscription:
        final s = item.subscription!;
        return '${s.source.block} / ${s.source.field} -> ${s.target.block} / ${s.target.field}';
      case RestoreKind.sndb:
        return item.sndbEntry!.serial;
      case RestoreKind.file:
        return '${item.file!.kind} (${item.file!.bytes.length} B)';
    }
  }

  String _preview(Object? value) {
    final text = value?.toString() ?? 'null';
    return text.length > 50 ? '${text.substring(0, 47)}...' : text;
  }
}

/// The restore plan's filter bar: the five view modes (each with its own count) and the class
/// toggles that tick or untick a whole class in one gesture.
///
/// Layout rule (the responsive policy, `Docs/App/General Info.md`): both groups are [Wrap]s, so
/// they take one line when the window is wide and move onto the next line when it is not - the bar
/// never stretches a row past the width it was given, and it is measured at 360x640 and 800x360.
/// The measurements are the evidence, not the rule: nothing here tests a pixel width.
class RestoreFilterBar extends StatelessWidget {
  const RestoreFilterBar({
    super.key,
    required this.plan,
    required this.filter,
    required this.onFilter,
    required this.onClass,
  });

  final RestorePlan plan;
  final PlanFilter filter;

  /// A view mode was chosen.
  final ValueChanged<PlanFilter> onFilter;

  /// A whole class was ticked ([selected] true) or unticked.
  final void Function(DiffKind kind, bool selected) onClass;

  /// The classes the bar can toggle: the three actionable ones. `same` rows are left ticked and
  /// are unticked one row at a time - a restore always writes them, so a class switch over them
  /// would be a control with no effect on the device.
  static const List<DiffKind> toggledKinds = [
    DiffKind.add,
    DiffKind.change,
    DiffKind.remove,
  ];

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Padding(
      padding: const EdgeInsets.fromLTRB(
        Spacing.lg,
        Spacing.md,
        Spacing.lg,
        Spacing.sm,
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Wrap(
            spacing: Spacing.sm,
            runSpacing: Spacing.xs,
            children: [
              for (final mode in PlanFilter.values)
                ChoiceChip(
                  label: Text('${mode.label} ${plan.view(mode).shown}'),
                  selected: mode == filter,
                  onSelected: (_) => onFilter(mode),
                  // Compact so the bar does not eat a short window: the chips stack when the
                  // width runs out, and a 48 px tap target per line would make that stack taller
                  // than the rows it filters.
                  visualDensity: VisualDensity.compact,
                  materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
                  padding: const EdgeInsets.symmetric(horizontal: Spacing.sm),
                ),
            ],
          ),
          Padding(
            padding: const EdgeInsets.only(top: Spacing.sm),
            child: Wrap(
              spacing: Spacing.sm,
              runSpacing: Spacing.xs,
              crossAxisAlignment: WrapCrossAlignment.center,
              children: [
                Text('Include', style: theme.textTheme.bodySmall),
                for (final kind in toggledKinds)
                  FilterChip(
                    avatar: Icon(
                      _icon(kind),
                      size: IconSize.sm,
                      color: _ink(kind),
                    ),
                    // Plural on purpose: the bar also carries a filter MODE per class, and two
                    // controls reading `Add 3` would be two different things wearing one name.
                    label: Text(
                      '${kind.word}s ${plan.classCount(kind)}',
                      style: theme.textTheme.labelSmall?.copyWith(
                        color: _ink(kind),
                      ),
                    ),
                    selected: plan.classSelected(kind),
                    onSelected: (value) => onClass(kind, value),
                    visualDensity: VisualDensity.compact,
                    materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
                    padding: const EdgeInsets.symmetric(horizontal: Spacing.sm),
                  ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  static IconData _icon(DiffKind kind) => switch (kind) {
    DiffKind.add => Icons.add_circle_outline,
    DiffKind.change => Icons.sync_alt,
    DiffKind.remove => Icons.delete_outline,
    DiffKind.same => Icons.check_circle_outline,
  };

  static Color _ink(DiffKind kind) => switch (kind) {
    DiffKind.add => kSuccess,
    DiffKind.change => kWarning,
    DiffKind.remove => kError,
    DiffKind.same => kTextMuted,
  };
}
