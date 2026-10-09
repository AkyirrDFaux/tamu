import 'dart:async';

import 'package:flutter/material.dart';

import '../core/block_registry.dart';
import '../core/connection.dart';
import '../core/device_backup.dart';
import '../core/register_client.dart';
import '../core/render_dict.dart' show geometryDictType, geometryKeysForShape, isRenderDictType, renderDictKeyName, renderKeyFieldInfo, textureKeysForType;
import '../core/script_file.dart' show ScriptField;
import '../core/storage_client.dart' show FileRecord, StorageClient, normalizeFileName;
import '../core/types.dart';
import '../core/script_client.dart';
import 'register_backup_view.dart';
import 'theme.dart';
import 'value_editor.dart' show dataTypeLabel, formatValue, showValueEditor;
import 'system_block_view.dart';import 'widgets.dart';

part 'register_page_tiles.dart';
part 'register_page_edit.dart';

/// Register service view (Docs/App/Service views/Register.md):
/// System block, static blocks, and dynamic memory.
class RegisterPage extends StatefulWidget {
  final int deviceId;
  final bool hasDynamicMemory;

  /// Test seam: the refresh paths only run on a live link
  /// (`ConnectionManager.instance.isConnected`), which a widget test cannot fake. Defaults to
  /// the real manager.
  final bool Function()? isConnected;

  /// Test seam: how the page builds its register client. Defaults to a real [RegisterClient].
  final RegisterClient Function(int deviceId)? clientFactory;

  const RegisterPage({super.key, required this.deviceId, this.hasDynamicMemory = true,
      this.isConnected, this.clientFactory});

  @override
  State<RegisterPage> createState() => _RegisterPageState();
}

class _RegisterPageState extends State<RegisterPage>
    with AutoRefreshMixin<RegisterPage> {
  late final RegisterClient _client = (widget.clientFactory ??
      (int id) => RegisterClient(deviceId: id))(widget.deviceId);

  /// Stores [type, instance, meta, name] for each block
  List<({int type, int inst, ValueInfo meta, String name})?>? _blockMetas;
  final Map<int, Map<int, ({ValueInfo meta, List<int> value})?>> _fieldCache = {};
  final Map<int, List<int>> _dynamicFields = {};
  final Map<int, Map<int, List<int>>> _dynamicKeys = {};

  /// Script blocks (the Scripts range 0x3F4-0x3F7): cacheKey -> field -> keys/entity indexes.
  final Map<int, Map<int, List<int>>> _scriptKeys = {};
  String? _error;
  final Set<int> _expanded = {};
  bool _refreshing = false;

  bool get _connected =>
      (widget.isConnected ?? (() => ConnectionManager.instance.isConnected))();
  bool _busy = false;
  bool _editMode = false;

  /// Docs/App/Service views/Register.md: a Current/Backup view toggle in the appbar.
  /// Current shows the live RAM values; Backup shows what a Save persisted (decoded from
  /// the device's `.SV` / .DT_ / .DV_ files).
  RegisterViewMode _viewMode = RegisterViewMode.current;
  DeviceBackup? _backup;
  bool _backupBusy = false;
  String? _backupError;

  /// The static blocks' persistent fields, read from the device (the `.SV` layout source).
  /// Re-read with the topology so the app never mirrors the firmware's compile-time table.
  StaticFieldLayout _staticFields = const {};

  /// Reads the static field layout once per topology generation.
  Future<StaticFieldLayout> _ensureStaticFields() async {
    if (_staticFields.isEmpty) {
      _staticFields = await _client.readStaticFieldLayout();
    }
    return _staticFields;
  }

  bool get _backupMode => _viewMode == RegisterViewMode.backup;

  /// Runs a long operation (Save/Recall) with the busy spinner shown in the app bar.
  Future<bool> _withBusy(Future<bool> Function() task) async {
    if (_busy) return false;
    setState(() => _busy = true);
    try {
      return await task();
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  @override
  void initState() {
    super.initState();
    _refreshAll();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      // Docs/App/Service views/Register.md: the memory view auto-refreshes at 0.5 s.
      if (mounted) applyAuto(const Duration(milliseconds: 500));
    });
  }

  /// The auto-refresh re-reads the topology every this many ticks (counted rather than timed,
  /// so it is deterministic and does not depend on the wall clock).
  static const int _topologyEveryTicks = 10;
  int _ticksSinceTopology = 0;

  /// The auto-refresh (docs: "automatically refreshes the visible view") re-reads *values*
  /// only. The block/field **topology** is re-read every [_topologyEveryTicks] ticks:
  /// enumerating the types, the instances and a meta per block is ~8-12 bus round-trips for
  /// data that only changes when a block or field is created, deleted, reordered or re-typed -
  /// and every one of those paths refreshes explicitly anyway. A view with nothing expanded
  /// therefore costs no traffic at all.
  @override
  Future<void> onAutoRefresh() async {
    if (_backupMode || !_connected) {
      await _refreshAll();
      return;
    }
    if (_ticksSinceTopology >= _topologyEveryTicks) {
      await _refreshAll();
    } else {
      _ticksSinceTopology++;
      await _refreshValuesOnly();
    }
  }

  /// Re-reads the visible blocks' values without touching the block/field topology.
  Future<void> _refreshValuesOnly() async {
    if (_refreshing) return;
    _refreshing = true;
    try {
      await _loadVisibleFields();
    } finally {
      _refreshing = false;
    }
  }

  void _snack(String message) {
    if (!mounted) return;
    showSnack(context, message);
  }

  /// State update used by the part-file extensions: `setState` is @protected and an
  /// extension is not a subclass of State, so the tiles/edit actions go through here.
  void _rebuild(VoidCallback fn) => setState(fn);

  /// Full refresh: re-reads the block/field topology *and* the visible values.
  Future<void> _refreshAll() async {
    if (_refreshing) return;
    _refreshing = true;
    try {
      if (!_connected) return;
      if (_backupMode) {
        // In Backup view the data on show is the stored data, so a refresh reloads it.
        await _loadBackup();
        return;
      }
      // Script blocks are not in the type list (the Script service owns their slots), so
      // pass its loaded list to keep them in the view.
      final blocks = await _client.readBlocks(
          scriptSlots: await ScriptClient(deviceId: widget.deviceId).loadedScripts());
      if (!mounted) return;
      if (blocks == null) {
        setState(() => _error = 'Device did not respond');
        return;
      }
      _error = null;
      _ticksSinceTopology = 0;
      // Preserve cache for blocks that still exist
      final newMetas = <({int type, int inst, ValueInfo meta, String name})?>[];
      final oldCache = Map<int, Map<int, ({ValueInfo meta, List<int> value})?>>.from(_fieldCache);
      _fieldCache.clear();
      for (final b in blocks) {
        if (b == null) { newMetas.add(null); continue; }
        final cacheKey = (b.type << 8) | b.inst;
        if (oldCache.containsKey(cacheKey)) {
          _fieldCache[cacheKey] = oldCache[cacheKey]!;
        }
        newMetas.add(b);
      }
      _blockMetas = newMetas;
      _staticFields = const {}; // re-read the layout with the new topology
      await _ensureStaticFields();
      await _loadVisibleFields();
    } finally {
      _refreshing = false;
    }
  }
Future<void> _loadVisibleFields() async {
    if (_backupMode) return; // Backup view renders the decoded backup, not live fields.
    final blocks = _blockMetas;
    if (blocks == null) return;
    for (var i = 0; i < blocks.length; i++) {
      if (!_expanded.contains(i)) continue;
      final block = blocks[i];
      if (block != null) {
        await _loadBlockFields(block.type, block.inst, block, forceRefresh: true);
        if (i < blocks.length - 1) await Future.delayed(const Duration(milliseconds: 50));
      }
    }
    if (mounted) setState(() {});

  }

  /// Switches the Current/Backup view (docs Register.md appbar). Entering Backup reads the
  /// stored values; returning to Current drops the live cache so a Recall is picked up.
  Future<void> _setViewMode(RegisterViewMode mode) async {
    if (mode == _viewMode) return;
    setState(() {
      _viewMode = mode;
      if (mode == RegisterViewMode.current) _fieldCache.clear();
    });
    if (mode == RegisterViewMode.backup) {
      await _loadBackup();
    } else {
      await _refreshAll();
    }
  }

  /// Reads and decodes the device's backup files: `.SV` for the static/System fields
  /// and `.DT_`/`.DV_` for each dynamic slot (docs Register.md "Save").
  Future<void> _loadBackup() async {
    if (_backupBusy) return;
    setState(() {
      _backupBusy = true;
      _backupError = null;
    });
    try {
      final store = StorageClient(deviceId: widget.deviceId);
      final table = await store.readFileTable();
      List<int>? sv;
      final tables = <int, List<int>>{};
      final values = <int, List<int>>{};
      for (final f in table ?? const <FileRecord>[]) {
        final name = normalizeFileName(f.name).toUpperCase();
        if (name == '.SV') {
          sv = await store.readFile(f.name, size: f.size);
        } else if (name.startsWith('.DT_') || name.startsWith('.DV_')) {
          final slot = int.tryParse(name.substring(4).trim(), radix: 16);
          if (slot == null) continue;
          final bytes = await store.readFile(f.name, size: f.size);
          if (bytes == null) continue;
          (name.startsWith('.DT_') ? tables : values)[slot] = bytes;
        }
      }
      // `.SV` is a raw mirror of the static persistent space, so the registry must be the same
      // ordered list the layout is computed from: every non-System block, in page order.
      final registry = staticRegistryOf(_blockMetas);
      final dynamic = <int, ({List<int> table, List<int> values})>{
        for (final slot in tables.keys)
          slot: (table: tables[slot]!, values: values[slot] ?? const []),
      };
      final decoded = DeviceBackup.decode(
          sv: sv,
          staticRegistry: registry,
          staticFields: await _ensureStaticFields(),
          dynamic: dynamic,
          hasNetId: _hasNetId);
      if (!mounted) return;
      setState(() => _backup = decoded);
    } catch (e) {
      if (!mounted) return;
      setState(() => _backupError = '$e');
    } finally {
      if (mounted) setState(() => _backupBusy = false);
    }
  }

  /// Recalls one stored field into RAM (the Backup view's per-entry button). The wire only
  /// carries `Recall All`, so a single recall is a register write of the stored value.
  Future<void> _recallBackupField(int blockType, int inst, int field, int key) async {
    final ok = await _withBusy(() => _writeStoredFieldToRam(blockType, inst, field, key));
    _snack(ok ? 'Recalled' : 'Recall failed');
    if (ok && mounted) {
      // The live cache no longer matches RAM; drop the recalled entry (dynamic entries are
      // cached at field*256 + key, static/System entries at the field index).
      final entryKey = isDynamicType(blockType) ? field * 256 + key : field;
      setState(() => _fieldCache[(blockType << 8) | inst]?.remove(entryKey));
    }
  }

  /// The static registry the `.SV` layout is computed from: every non-System block, in
  /// enumeration order (the same list the backup decoder is given).
  List<({int type, int inst})> get _staticRegistry => staticRegistryOf(_blockMetas);

  /// The device has the System NetID field (a core) when its System block reports >= 8 fields;
  /// it changes the System segment size in the `.SV` space.
  bool get _hasNetId => hasNetIdFor(_blockMetas);

  /// Reads one storage file's bytes, or null when it is missing.
  Future<List<int>?> _readFile(String name) async {
    final store = StorageClient(deviceId: widget.deviceId);
    final table = await store.readFileTable();
    for (final f in table ?? const <FileRecord>[]) {
      if (normalizeFileName(f.name).toUpperCase() == name) {
        return store.readFile(f.name, size: f.size);
      }
    }
    return null;
  }

  /// Writes one field's live value into its backup file - the app-side half of a partial save
  /// (docs Register.md: "partial saving ... app with direct file writes"). `.SV` for the
  /// System/static fields, at the field's computed offset; `.DV_<xx>` for a dynamic entry.
  Future<bool> _writeFieldToBackup(int blockType, int inst, int field, int key) async {
    final cacheKey = (blockType << 8) | inst;
    final isDynamic = isDynamicType(blockType);
    final live = _fieldCache[cacheKey]?[isDynamic ? field * 256 + key : field];
    if (live == null) return false; // the live value has to be known to save it
    final store = StorageClient(deviceId: widget.deviceId);

    if (isDynamic) {
      final suffix = inst.toRadixString(16).toUpperCase().padLeft(2, '0');
      final tableBytes = await _readFile('.DT_$suffix');
      final values = await _readFile('.DV_$suffix');
      if (tableBytes == null) return false;
      final table = decodeDynamicTable(tableBytes);
      if (table == null) return false;
      final next = dvSaveField(table, values ?? const [], field, key, live.value);
      if (next == null) return false;
      return store.writeFile('.DV_$suffix', next);
    }

    // `.SV` is the raw static persistent space: write the field at its computed offset.
    final layout =
        StaticSpaceLayout.fromRegistry(_staticRegistry, await _ensureStaticFields(),
            hasNetId: _hasNetId);
    final size = layout.sizeOf(blockType, field);
    if (size == null) return false;
    var value = live.value;
    if (value.length < size) {
      // Fixed-size character fields are null-padded on the wire (Docs/Services/Register.md);
      // numeric buffers stay 0.
      value = [...value, ...List<int>.filled(size - value.length, 0)];
    }
    final sv = await _readFile('.SV') ?? const <int>[];
    final next = svSaveField(sv, layout, blockType, inst, field, value);
    if (next == null) return false;
    return store.writeFile('.SV', next);
  }

  /// Writes one stored value back into RAM with a register write - the app-side half of a
  /// partial recall.
  Future<bool> _writeStoredFieldToRam(int blockType, int inst, int field, int key) async {
    final backup = _backup;
    if (backup == null) return false;
    final isDynamic = isDynamicType(blockType);
    final stored = isDynamic
        ? backup.dynamicField(inst, field, key)
        : backup.staticField(blockType == systemBlockTypeValue ? 0 : blockType, inst, field);
    if (stored == null || stored.value.isEmpty) return false;
    final meta = ValueInfo(
        type: stored.meta.type, flags: stored.meta.flags, key: key, size: stored.value.length);
    if (isDynamic) {
      final block = await _client.readDynamicBlockMeta(inst);
      if (block == null) return false;
      return await _client.writeDynamicEntry(block, field, key, meta, stored.value) != null;
    }
    return await _client.writeBlockField(blockType, inst, field, key, meta, stored.value) != null;
  }

  /// Persists the whole device to its backup (CID 4 "Save All", docs Register.md): the System
  /// block's persistent fields, every static block, and every dynamic block's .DT_/.DV_ files.
  Future<bool> _saveAll() => _client.saveAll();

  /// Recalls the whole device from its backup (CID 3 "Recall All").
  Future<bool> _recallAll() => _client.recallAll();

  /// Saves one persistent field to its backup (the docs' per-value Save). The wire only
  /// carries `Save All`, so this is the app-side half: see [_writeFieldToBackup].
  Future<void> _saveField(int blockType, int inst,
      ({int type, int inst, ValueInfo meta, String name})? block, int fieldIndex) async {
    final ok = await _withBusy(() => _writeFieldToBackup(blockType, inst, fieldIndex, 0));
    _snack(ok ? 'Saved' : 'Save failed');
  }

  /// Recalls one stored field into RAM (the docs' per-value Recall).
  Future<void> _recallField(int blockType, int inst,
      ({int type, int inst, ValueInfo meta, String name})? block, int fieldIndex) async {
    final ok = await _withBusy(() => _writeStoredFieldToRam(blockType, inst, fieldIndex, 0));
    _snack(ok ? 'Recalled' : 'Recall failed');
    if (ok) {
      await _loadBlockFields(blockType, inst, block, forceRefresh: true);
      if (mounted) setState(() {});
    }
  }

  Future<void> _loadBlockFields(int blockType, int instance, ({int type, int inst, ValueInfo meta, String name})? block, {bool forceRefresh = false}) async {
    if (block == null) return;
    final cacheKey = (blockType << 8) | instance;
    final cache = _fieldCache.putIfAbsent(cacheKey, () => {});
    final fields = _dynamicFields.putIfAbsent(instance, () => <int>[]);

    if (forceRefresh) {
      cache.clear();
      fields.clear();
    }

    // Dynamic blocks use the flat Field&Key model: enumerate the distinct fields and
    // read every (field, key) entry (cached as cache[field*256 + key]).
    if (isDynamicType(blockType)) {
      final fieldList = await _client.getDynamicFields(instance) ?? <int>[];
      fields
        ..clear()
        ..addAll(fieldList);
      final keyMap = _dynamicKeys.putIfAbsent(instance, () => {});
      for (final f in fieldList) {
        final keys = await _client.getDynamicKeys(instance, f) ?? <int>[0];
        keyMap[f] = keys;
        for (final key in keys) {
          if (!forceRefresh && cache.containsKey(f * 256 + key)) continue;
          final entry = await _client.readBlockField(blockType, instance, f, key);
          if (entry != null) {
            cache[f * 256 + key] = entry;
          } else {
            cache.remove(f * 256 + key);
          }
        }
      }
      return;
    }

    // Script blocks (the Scripts range 0x3F4-0x3F7): keyed entries per category field. The Header (field 0) is
    // script metadata, not register content, so only the Input/Output/Variable/Constant
    // categories are exposed here.
    if (isScriptType(blockType)) {
      final keyMap = _scriptKeys.putIfAbsent(cacheKey, () => {});
      for (var f = ScriptField.input; f < block.meta.size; f++) {
        final keys = await _client.getBlockKeys(blockType, instance, f) ?? <int>[];
        keyMap[f] = keys;
        for (final key in keys) {
          if (!forceRefresh && cache.containsKey(f * 256 + key)) continue;
          final entry = await _client.readBlockField(blockType, instance, f, key);
          if (entry != null) {
            cache[f * 256 + key] = entry;
          } else {
            cache.remove(f * 256 + key);
          }
        }
      }
      return;
    }

    final fieldCount = block.meta.size;
    for (var f = 0; f < fieldCount; f++) {
      if (!forceRefresh && cache.containsKey(f)) continue;
      if (blockType == 0) {
        final keys = systemKeysForField(f);
        final fieldMap = <int, ({ValueInfo meta, List<int> value})>{};
        for (final key in keys) {
          final field = await _client.readField(f, key);
          if (field != null) {
            fieldMap[key] = field;
          }
        }
        if (fieldMap.isNotEmpty) {
          final primaryKey = keys.first;
          cache[f] = fieldMap[primaryKey]!;
          for (final entry in fieldMap.entries) {
            if (entry.key != primaryKey) {
              // Use offset 256 to avoid collision with primary field indices (0-255)
              cache[256 + f * 256 + entry.key] = entry.value;
            }
          }
        } else {
          cache.remove(f);
        }
      } else {
        final field = await _client.readBlockField(blockType, instance, f, 0xFF);
        if (field != null) {
          cache[f] = field;
        } else {
          cache.remove(f);
        }
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text('Register - ${idToString(widget.deviceId)}'),
        actions: [
          // Current/Backup view (docs Register.md appbar, right).
          TextButton.icon(
            onPressed: _busy
                ? null
                : () => _setViewMode(_backupMode
                    ? RegisterViewMode.current
                    : RegisterViewMode.backup),
            icon: Icon(_backupMode ? Icons.history : Icons.sensors, size: 18),
            label: Text(_backupMode ? 'Backup' : 'Current',
                style: const TextStyle(fontSize: 12)),
            style: TextButton.styleFrom(
              foregroundColor: _backupMode ? kOrange : null,
              visualDensity: VisualDensity.compact,
            ),
          ),
          if (!_backupMode)
            IconButton(
              tooltip: 'Save all to backup',
              icon: const Icon(Icons.save),
              onPressed: () async {
                final ok = await _withBusy(_saveAll);
                _snack(ok ? 'Saved' : 'Save failed');
              },
            )
          else
            IconButton(
              tooltip: 'Recall all from backup',
              icon: const Icon(Icons.restore),
              onPressed: () async {
                final ok = await _withBusy(_recallAll);
                _snack(ok ? 'Recalled' : 'Recall failed');
                if (ok) await _loadBackup();
              },
            ),
          if (widget.hasDynamicMemory && !_backupMode)
            IconButton(
              tooltip: _editMode ? 'Exit edit mode' : 'Edit dynamic blocks',
              icon: Icon(
                  _editMode ? Icons.check : Icons.edit_outlined,
                  color: _editMode ? kOrange : null),
              onPressed: () => setState(() => _editMode = !_editMode),
            ),
          if (_busy || _backupBusy)
            const Padding(
              padding: EdgeInsets.only(right: 12),
              child: SizedBox(
                width: 18,
                height: 18,
                child: CircularProgressIndicator(strokeWidth: 2),
              ),
            ),
          RefreshButton(
            onRefresh: _refreshAll,
            autoActive: autoRefreshActive,
            refreshing: _refreshing,
            error: _error != null,
            selectedInterval: selectedInterval,
            onSelectAuto: applyAuto,
          ),
        ],
      ),
      body: _buildBody(),
    );
  }

  Widget _buildBody() {
    final blocks = _blockMetas;
    if (blocks == null) {
      return Center(child: Text(_error ?? 'Loading...'));
    }
    if (blocks.isEmpty) {
      return const Center(child: Text('No blocks found'));
    }
    if (_backupMode) return _backupBody(blocks);
    return _liveBody(blocks);
  }

  /// The Backup view: the stored values, read-only, with a per-field recall.
  Widget _backupBody(List<({int type, int inst, ValueInfo meta, String name})?> blocks) {
    if (_backup == null && _backupBusy) {
      return const Center(child: CircularProgressIndicator());
    }
    if (_backupError != null) {
      return Center(child: Text('Could not read the backup: $_backupError'));
    }
    return RegisterBackupView(
      backup: _backup ?? DeviceBackup.empty,
      blocks: blocks,
      busy: _busy,
      hasNetId: _hasNetId,
      onRecall: _recallBackupField,
    );
  }

  Widget _liveBody(List<({int type, int inst, ValueInfo meta, String name})?> blocks) {
    // Partition: static/system blocks keep their fixed order; dynamic blocks are a
    // separate section (reorderable in edit mode).
    final fixed = <int>[];
    final dynamic = <int>[];
    for (var i = 0; i < blocks.length; i++) {
      final b = blocks[i];
      // A None-typed block is a dynamic tombstone slot (static blocks are never None).
      if (b != null && isDynamicType(b.meta.type)) {
        dynamic.add(i);
      } else {
        fixed.add(i);
      }
    }

    final children = <Widget>[
      for (final i in fixed) _blockCard(context, i, blocks[i]),
    ];

    if (dynamic.isNotEmpty || (_editMode && widget.hasDynamicMemory)) {
      if (_editMode) {
        final dynCards = <Widget>[
          for (final (si, i) in dynamic.indexed)
            _blockCard(context, i, blocks[i],
                cardKey: ValueKey('dyn-$i'), dragIndex: si),
        ];
        children.add(Padding(
          padding: const EdgeInsets.only(top: 8, bottom: 8),
          child: Divider(height: 1),
        ));
        children.add(ReorderableListView(
          header: const Padding(
            padding: EdgeInsets.only(top: 6, bottom: 6),
            child: Text('Dynamic blocks',
                style: TextStyle(fontWeight: FontWeight.w600))),
          footer: ListTile(
            leading: const Icon(Icons.add_box_outlined),
            title: const Text('Add dynamic block'),
            onTap: _createBlock,
          ),
          shrinkWrap: true,
          padding: const EdgeInsets.fromLTRB(10, 4, 10, 12),
          buildDefaultDragHandles: false,
          children: dynCards,
          onReorderItem: (oldIndex, newIndex) =>
              _reorderBlocks(dynamic, oldIndex, newIndex),
        ));
      } else {
        for (final i in dynamic) {
          final b = blocks[i];
          if (b != null && isDynamicType(b.meta.type)) {
            children.add(_blockCard(context, i, b));
          }
        }
      }
    }

    return SingleChildScrollView(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(10, 10, 10, 24),
        child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch, children: children),
      ),
    );
  }

  /// Applies a drag-reorder of a block's fields.
  Future<void> _reorderFields(({int type, int inst, ValueInfo meta, String name})? block,
      List<int> fields, int oldIndex, int newIndex) async {
    if (block == null || oldIndex == newIndex) return;
    final order = fields.toList();
    final moved = order.removeAt(oldIndex);
    order.insert(newIndex, moved);
    final dynBlock =
        DynBlock(index: block.inst, meta: block.meta, name: block.name);
    final ok = await _client.reorderDynamicFields(dynBlock, order);
    _snack(ok ? 'Fields reordered' : 'Reorder failed');
    await _refreshAll();
  }

  /// Applies a drag-reorder of the dynamic block section (oldIndex/newIndex are
  /// positions in `dynamic`), then rebuilds the registry in the new order.
  Future<void> _reorderBlocks(List<int> dynamic, int oldIndex, int newIndex) async {
    if (oldIndex == newIndex) return;
    final order = dynamic.toList();
    final moved = order.removeAt(oldIndex);
    order.insert(newIndex, moved);
    // Recreate only the LIVE blocks in the dragged order (tombstones compact to the end).
    final liveOrder = order
        .where((i) => isDynamicType(_blockMetas?[i]?.meta.type ?? -1))
        .map((i) => _blockMetas![i]!.inst)
        .toList();
    final ok = await _client.reorderDynamicBlocks(liveOrder);
    _snack(ok ? 'Blocks reordered' : 'Reorder failed');
    await _refreshAll();
  }

  /// Moves a dynamic block to a chosen registry index (dialogue).
  Future<void> _moveBlock(({int type, int inst, ValueInfo meta, String name})? block) async {
    if (block == null || !mounted) return;
    final controller = TextEditingController(text: '${block.inst}');
    final target = await showDialog<int>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Move block to index'),
        content: TextField(
          controller: controller,
          autofocus: true,
          keyboardType: TextInputType.number,
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(
            onPressed: () {
              final v = int.tryParse(controller.text.trim());
              Navigator.pop(context, v);
            },
            child: const Text('OK'),
          ),
        ],
      ),
    );
    if (target == null || target < 0 || target == block.inst) return;
    final ok = await _client.moveDynamicBlockTo(block.inst, target);
    _snack(ok ? 'Block moved' : 'Move failed');
    await _refreshAll();
  }

  String _scriptFieldName(int field) => switch (field) {
        ScriptField.header => 'Header',
        ScriptField.input => 'Input',
        ScriptField.output => 'Output',
        _ => 'Field $field',
      };

  Future<void> _editScriptEntry(int blockType, int inst, int field, int key,
      ({ValueInfo meta, List<int> value}) entry,
      ({int type, int inst, ValueInfo meta, String name})? block) async {
    final next = await showValueEditor(context, entry.meta.dataType, entry.value);
    if (next == null || !mounted) return;
    // Declare the actual value length: sending the field's declared Size with a shorter
    // value (a trimmed String) makes the device copy stale payload bytes. A shorter String
    // is null-padded by the firmware.
    final meta = ValueInfo(
        type: entry.meta.type, flags: entry.meta.flags, size: next.length, key: entry.meta.key);
    final ok = await _client.writeBlockField(blockType, inst, field, key, meta, next);
    _snack(ok != null ? 'Written' : 'Write failed');
    if (block != null) {
      await _loadBlockFields(blockType, inst, block, forceRefresh: true);
      if (mounted) setState(() {});
    }
  }

}
