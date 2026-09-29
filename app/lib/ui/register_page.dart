import 'dart:async';

import 'package:flutter/material.dart';

import '../core/block_registry.dart';
import '../core/connection.dart';
import '../core/device_backup.dart';
import '../core/device_db.dart';
import '../core/register_client.dart';
import '../core/render_dict.dart' show geometryDictType, geometryKeysForShape, isRenderDictType, renderDictKeyName, renderKeyFieldInfo, textureKeysForType;
import '../core/script_file.dart' show ScriptField;
import '../core/storage_client.dart' show FileRecord, StorageClient, normalizeFileName;
import '../core/types.dart';
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
  List<({int type, int inst, BlockMeta meta, String name})?>? _blockMetas;
  final Map<int, Map<int, ({BlockMeta meta, List<int> value})?>> _fieldCache = {};
  final Map<int, List<int>> _dynamicFields = {};
  final Map<int, Map<int, List<int>>> _dynamicKeys = {};

  /// Script blocks (0x3FE): cacheKey -> field -> keys/entity indexes.
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
  /// the device's STATLOG / DT_ / DV_ files).
  RegisterViewMode _viewMode = RegisterViewMode.current;
  DeviceBackup? _backup;
  bool _backupBusy = false;
  String? _backupError;

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
      final blocks = await _client.readBlocks();
      if (!mounted) return;
      if (blocks == null) {
        setState(() => _error = 'Device did not respond');
        return;
      }
      _error = null;
      _ticksSinceTopology = 0;
      // Preserve cache for blocks that still exist
      final newMetas = <({int type, int inst, BlockMeta meta, String name})?>[];
      final oldCache = Map<int, Map<int, ({BlockMeta meta, List<int> value})?>>.from(_fieldCache);
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

  /// Reads and decodes the device's backup files: `STATLOG` for the static/System fields
  /// and `DT_`/`DV_` for each dynamic slot (docs Register.md "Save").
  Future<void> _loadBackup() async {
    if (_backupBusy) return;
    setState(() {
      _backupBusy = true;
      _backupError = null;
    });
    try {
      final store = StorageClient(deviceId: widget.deviceId);
      final table = await store.readFileTable();
      List<int>? statlog;
      final tables = <int, List<int>>{};
      final values = <int, List<int>>{};
      for (final f in table ?? const <FileRecord>[]) {
        final name = normalizeFileName(f.name).toUpperCase();
        if (name == 'STATLOG') {
          statlog = await store.readFile(f.name, size: f.size);
        } else if (name.startsWith('DT_') || name.startsWith('DV_')) {
          final slot = int.tryParse(name.substring(3).trim(), radix: 16);
          if (slot == null) continue;
          final bytes = await store.readFile(f.name, size: f.size);
          if (bytes == null) continue;
          (name.startsWith('DT_') ? tables : values)[slot] = bytes;
        }
      }
      // STATLOG addresses blocks by their static-registry index, so the registry must be
      // the same ordered list the decoder expects: every non-System block, in page order.
      final registry = <({int type, int inst})>[
        for (final b in _blockMetas ?? const [])
          if (b != null && b.type != 0) (type: b.type, inst: b.inst),
      ];
      final dynamic = <int, ({List<int> table, List<int> values})>{
        for (final slot in tables.keys)
          slot: (table: tables[slot]!, values: values[slot] ?? const []),
      };
      final decoded = DeviceBackup.decode(
          statlog: statlog, staticRegistry: registry, dynamic: dynamic);
      if (!mounted) return;
      setState(() => _backup = decoded);
    } catch (e) {
      if (!mounted) return;
      setState(() => _backupError = '$e');
    } finally {
      if (mounted) setState(() => _backupBusy = false);
    }
  }

  /// Recalls one stored field into RAM (CID 4 at the field's BlockInfo).
  Future<void> _recallBackupField(int blockType, int inst, int field, int key) async {
    final ok = await _withBusy(() async {
      final reply =
          await _client.request(4, payload: blockInfoBytes(blockType, inst, field, key));
      return reply != null && reply.isNotEmpty && reply[0] == 0;
    });
    _snack(ok ? 'Recalled' : 'Recall failed');
    if (ok && mounted) {
      // The live cache no longer matches RAM; drop the recalled entry.
      setState(() => _fieldCache[(blockType << 8) | inst]?.remove(field));
    }
  }

  /// Persists every block to its backup (docs Register.md "Save"): the dynamic
  /// registry, the System block (Name/NetID) and each static block's persistent
  /// fields. Each target is a separate request so failures are reported accurately.
  Future<bool> _saveAll() async {
    final c = _client;
    // The dynamic registry only exists on devices advertising dynamic memory (the DAS
    // has none, so the request would report failure and mark the whole save as failed).
    // Check both the widget flag and the live capability report to stay correct even if
    // the page was opened before capabilities were loaded.
    final dev = DeviceDatabase.instance.byId(widget.deviceId);
    final hasDyn = widget.hasDynamicMemory ||
        (dev != null && dev.capabilities & Capability.dynamicMemory != 0);
    bool ok = true;
    if (hasDyn) {
      ok &= await _requestStatus(c, 3, const [0xFF, 0xFF, 0xFF, 0xFF]); // dynamic registry
    }
    ok &= await _requestStatus(c, 3, blockInfoBytes(0, 0, 0xFF, 0)); // system Name/NetID
    for (final b in _blockMetas ?? const []) {
      if (b == null || b.type == 0 || b.type == 0x3FF) continue;
      ok &= await _requestStatus(c, 3, blockInfoBytes(b.type, b.inst, 0xFF, 0));
    }
    return ok;
  }

  Future<bool> _recallAll() async {
    final c = _client;
    bool ok = true;
    if (widget.hasDynamicMemory) {
      ok &= await _requestStatus(c, 4, const [0xFF, 0xFF, 0xFF, 0xFF]);
    }
    ok &= await _requestStatus(c, 4, blockInfoBytes(0, 0, 0xFF, 0));
    for (final b in _blockMetas ?? const []) {
      if (b == null || b.type == 0 || b.type == 0x3FF) continue;
      ok &= await _requestStatus(c, 4, blockInfoBytes(b.type, b.inst, 0xFF, 0));
    }
    return ok;
  }

  Future<bool> _requestStatus(RegisterClient c, int cid, List<int> payload) async {
    // The dynamic registry save/recall (CID 3/4, inst 0x3F) writes per-block files and
    // runs the 64-slot cleanup - allow it more than the default request timeout.
    final reply = await c.request(cid, payload: payload,
        timeout: const Duration(seconds: 25));
    return reply != null && reply.isNotEmpty && reply[0] == 0;
  }

  /// Saves one persistent field to its backup (docs System Memory view: "Saveable
  /// values show a Save button"). System/static fields are addressed by BlockInfo.
  Future<void> _saveField(int blockType, int inst,
      ({int type, int inst, BlockMeta meta, String name})? block, int fieldIndex) async {
    final bi = blockInfoBytes(blockType == 0 ? 0 : blockType, inst, fieldIndex, 0);
    final ok = await _withBusy(() async {
      final reply = await _client.request(3, payload: bi);
      return reply != null && reply.isNotEmpty && reply[0] == 0;
    });
    _snack(ok ? 'Saved' : 'Save failed');
  }

  Future<void> _recallField(int blockType, int inst,
      ({int type, int inst, BlockMeta meta, String name})? block, int fieldIndex) async {
    final bi = blockInfoBytes(blockType == 0 ? 0 : blockType, inst, fieldIndex, 0);
    final ok = await _withBusy(() async {
      final reply = await _client.request(4, payload: bi);
      return reply != null && reply.isNotEmpty && reply[0] == 0;
    });
    _snack(ok ? 'Recalled' : 'Recall failed');
    if (ok) {
      await _loadBlockFields(blockType, inst, block, forceRefresh: true);
      if (mounted) setState(() {});
    }
  }

  Future<void> _loadBlockFields(int blockType, int instance, ({int type, int inst, BlockMeta meta, String name})? block, {bool forceRefresh = false}) async {
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
    if (blockType == BlockType.dynamic.value) {
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
          final entry = await _client.readBlockField(BlockType.dynamic.value, instance, f, key);
          if (entry != null) {
            cache[f * 256 + key] = entry;
          } else {
            cache.remove(f * 256 + key);
          }
        }
      }
      return;
    }

    // Script blocks (0x3FE): keyed entries per category field. The Header (field 0) is
    // script metadata, not register content, so only the Input/Output/Variable/Constant
    // categories are exposed here.
    if (blockType == BlockType.script.value) {
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
        final fieldMap = <int, ({BlockMeta meta, List<int> value})>{};
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
  Widget _backupBody(List<({int type, int inst, BlockMeta meta, String name})?> blocks) {
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
      onRecall: _recallBackupField,
    );
  }

  Widget _liveBody(List<({int type, int inst, BlockMeta meta, String name})?> blocks) {
    // Partition: static/system blocks keep their fixed order; dynamic blocks are a
    // separate section (reorderable in edit mode).
    final fixed = <int>[];
    final dynamic = <int>[];
    for (var i = 0; i < blocks.length; i++) {
      final b = blocks[i];
      // A None-typed block is a dynamic tombstone slot (static blocks are never None).
      if (b != null && b.meta.typeValue == BlockType.dynamic.value) {
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
          if (blocks[i]?.meta.typeValue == BlockType.dynamic.value) {
            children.add(_blockCard(context, i, blocks[i]));
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
  Future<void> _reorderFields(({int type, int inst, BlockMeta meta, String name})? block,
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
        .where((i) => _blockMetas?[i]?.meta.typeValue == BlockType.dynamic.value)
        .map((i) => _blockMetas![i]!.inst)
        .toList();
    final ok = await _client.reorderDynamicBlocks(liveOrder);
    _snack(ok ? 'Blocks reordered' : 'Reorder failed');
    await _refreshAll();
  }

  /// Moves a dynamic block to a chosen registry index (dialogue).
  Future<void> _moveBlock(({int type, int inst, BlockMeta meta, String name})? block) async {
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
      ({BlockMeta meta, List<int> value}) entry,
      ({int type, int inst, BlockMeta meta, String name})? block) async {
    final next = await showValueEditor(context, entry.meta.dataType, entry.value);
    if (next == null || !mounted) return;
    // Declare the actual value length: sending the field's declared Size with a shorter
    // value (a trimmed String) makes the device copy stale payload bytes. A shorter String
    // is space-padded by the firmware.
    final meta = BlockMeta(
        flagsAndType: entry.meta.flagsAndType, size: next.length, key: entry.meta.key);
    final ok = await _client.writeBlockField(blockType, inst, field, key, meta, next);
    _snack(ok != null ? 'Written' : 'Write failed');
    if (block != null) {
      await _loadBlockFields(blockType, inst, block, forceRefresh: true);
      if (mounted) setState(() {});
    }
  }

}
