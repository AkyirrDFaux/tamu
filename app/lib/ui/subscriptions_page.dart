/// Subscriptions service UI page.
library;

import 'package:flutter/material.dart';
import 'package:tamuapp/core/block_registry.dart' show blockInfoFor;
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/subscription_client.dart';
import 'package:tamuapp/core/types.dart' show BlockType, BlockMeta, Capability, ProviderSubscription, RequesterSubscription, TriggerType;

class SubscriptionsPage extends StatefulWidget {
  final int deviceId;
  final String deviceName;

  const SubscriptionsPage({
    super.key,
    required this.deviceId,
    required this.deviceName,
  });

  @override
  State<SubscriptionsPage> createState() => _SubscriptionsPageState();
}

class _SubscriptionsPageState extends State<SubscriptionsPage> with SingleTickerProviderStateMixin {
  late final TabController _tabController;
  late final SubscriptionClient _client;
  late final RegisterClient _regClient;

  List<ProviderSubscription> _providerSubs = [];
  List<RequesterSubscription> _requesterSubs = [];
  bool _loading = true;
  String? _error;

  @override
  void initState() {
    super.initState();
    _tabController = TabController(length: 2, vsync: this);
    _client = SubscriptionClient(deviceId: widget.deviceId);
    _regClient = RegisterClient(deviceId: widget.deviceId);
    _client.startListening();
    _loadSubscriptions();
  }

  @override
  void dispose() {
    _client.stopListening();
    _tabController.dispose();
    _client.dispose();
    super.dispose();
  }

  Future<void> _loadSubscriptions() async {
    setState(() {
      _loading = true;
      _error = null;
    });
    try {
      final provider = await _client.getProviderSubscriptions();
      final requester = await _client.getRequesterSubscriptions();
      setState(() {
        _providerSubs = provider;
        _requesterSubs = requester;
        _loading = false;
      });
    } catch (e) {
      setState(() {
        _error = e.toString();
        _loading = false;
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Scaffold(
      appBar: AppBar(
        title: Text('Subscriptions - ${widget.deviceName}'),
        bottom: TabBar(
          controller: _tabController,
          labelColor: theme.colorScheme.onPrimary,
          unselectedLabelColor: theme.colorScheme.onPrimary.withValues(alpha: 0.7),
          indicatorColor: theme.colorScheme.onPrimary,
          tabs: const [
            Tab(text: 'Provider (incoming)'),
            Tab(text: 'Requester (outgoing)'),
          ],
        ),
        actions: [
          IconButton(
            icon: const Icon(Icons.save),
            tooltip: 'Save requester subscriptions',
            onPressed: _saveAllSubscriptions,
          ),
          IconButton(
            icon: const Icon(Icons.refresh),
            onPressed: _loadSubscriptions,
            tooltip: 'Refresh',
          ),
        ],
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : _error != null
              ? Center(child: Text('Error: $_error', style: TextStyle(color: theme.colorScheme.error)))
              : TabBarView(
                  controller: _tabController,
                  children: [
                    _buildProviderTab(),
                    _buildRequesterTab(),
                  ],
                ),
    );
  }

  Widget _buildProviderTab() {
    final db = DeviceDatabase.instance;
    final device = db.byId(widget.deviceId);
    final hasProviderCapability = device?.capabilities != null && (device!.capabilities & Capability.subscriptions) != 0;
    
    if (!hasProviderCapability) {
      return const Center(
        child: Padding(
          padding: EdgeInsets.all(16),
          child: Text(
            'This device does not support acting as a subscription provider.\n'
            'Provider subscriptions require available RAM for the provider table.',
            style: TextStyle(color: Colors.white54),
            textAlign: TextAlign.center,
          ),
        ),
      );
    }
    
    if (_providerSubs.isEmpty) {
      return const Center(child: Text('No provider subscriptions', style: TextStyle(color: Colors.white54)));
    }
    return ListView.builder(
      padding: const EdgeInsets.all(8),
      itemCount: _providerSubs.length,
      itemBuilder: (context, i) => _buildProviderTile(_providerSubs[i]),
    );
  }

  Widget _buildProviderTile(ProviderSubscription sub) {
    return Card(
      margin: const EdgeInsets.symmetric(vertical: 4),
      child: ExpansionTile(
        title: Text('Sub #${sub.index}'),
        subtitle: Text('Requester: ${sub.requesterAddr} → Source: ${_formatBlockInfo(sub.sourceReg)}'),
        children: [
          Padding(
            padding: const EdgeInsets.all(16),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                _detailRow('Trigger', sub.trigger.label),
                _detailRow('Period', '${sub.periodMs} ms'),
                _detailRow('Min Interval', '${sub.minTimeMs} ms'),
                _detailRow('Last Sent', '${sub.lastSentMs} ms ago'),
                _detailRow('Requester Addr', '${sub.requesterAddr}'),
                _detailRow('TRID', '0x${sub.trid.toRadixString(16).padLeft(4, '0')}'),
                _detailRow('Hash', '0x${sub.hash.toRadixString(16).padLeft(8, '0')}'),
              ],
            ),
          ),
        ],
      ),
    );
  }

Widget _buildRequesterTab() {
    final listChildren = <Widget>[
      Expanded(
        child: _requesterSubs.isEmpty
            ? const Center(child: Text('No requester subscriptions', style: TextStyle(color: Colors.white54)))
            : ListView.builder(
                padding: const EdgeInsets.all(8),
                itemCount: _requesterSubs.length,
                itemBuilder: (context, i) => _buildRequesterTile(_requesterSubs[i]),
              ),
      ),
      Padding(
        padding: const EdgeInsets.all(16),
        child: SizedBox(
          width: double.infinity,
          child: FilledButton(
            onPressed: _showAddSubscriptionDialog,
            style: FilledButton.styleFrom(
              padding: const EdgeInsets.symmetric(vertical: 14),
            ),
            child: Row(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                Icon(Icons.add, size: 20),
                SizedBox(width: 8),
                Text('Add Subscription'),
              ],
            ),
          ),
        ),
      ),
      const SizedBox(height: 8),
    ];
    return Column(children: listChildren);
  }

  Widget _buildRequesterTile(RequesterSubscription sub) {
    return Card(
      margin: const EdgeInsets.symmetric(vertical: 4),
      child: ExpansionTile(
        title: Text('Sub #${sub.index} (TRID: 0x${sub.trid.toRadixString(16).padLeft(4, '0')})'),
        subtitle: Text('Target: ${_formatBlockInfo(sub.targetReg)} ← Source: ${_formatBlockInfo(sub.sourceReg)}'),
        trailing: IconButton(
          icon: const Icon(Icons.delete, color: Colors.red),
          onPressed: () => _deleteRequesterSubscription(sub.index),
        ),
        children: [
          Padding(
            padding: const EdgeInsets.all(16),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                _detailRow('Provider', '${sub.providerAddr}'),
                _detailRow('Trigger', sub.trigger.label),
                _detailRow('Period', '${sub.periodMs} ms'),
                _detailRow('Min Interval', '${sub.minTimeMs} ms'),
                const SizedBox(height: 8),
                FilledButton(
                  onPressed: () => _showEditSubscriptionDialog(sub),
                  child: const Text('Edit'),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _detailRow(String label, String value) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 2),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          SizedBox(width: 120, child: Text(label, style: const TextStyle(color: Colors.white54))),
          Expanded(child: Text(value, style: const TextStyle(fontFamily: 'monospace'))),
        ],
      ),
    );
  }

  String _formatBlockInfo(int bi) {
    final type = (bi >> 22) & 0x3FF;
    final inst = (bi >> 16) & 0x3F;
    final field = (bi >> 8) & 0xFF;
    final key = bi & 0xFF;
    final typeLabel = BlockType.fromValue(type).label;
    return '$typeLabel[$inst].f$field.k$key';
  }

  void _showAddSubscriptionDialog() {
    showDialog(
      context: context,
      builder: (context) => _SubscriptionDialog(
        client: _client,
        regClient: _regClient,
        currentSubs: _requesterSubs,
        onSaved: _loadSubscriptions,
      ),
    );
  }

  void _showEditSubscriptionDialog(RequesterSubscription sub) {
    showDialog(
      context: context,
      builder: (context) => _SubscriptionDialog(
        client: _client,
        regClient: _regClient,
        currentSubs: _requesterSubs,
        existing: sub,
        onSaved: _loadSubscriptions,
      ),
    );
  }

  Future<void> _saveAllSubscriptions() async {
    final ok = await _client.saveRequesterSubscriptions();
    if (mounted) {
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
          content: Text(ok ? 'Requester subscriptions saved' : 'Failed to save subscriptions')));
    }
  }

  Future<void> _deleteRequesterSubscription(int index) async {
    final ok = await _client.setRequesterSubscription(index, entry: null);
    if (ok) {
      await _loadSubscriptions();
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Subscription deleted')));
      }
    } else {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Failed to delete subscription')));
      }
    }
  }
}

/// Block+Field picker model
/// A block (block type + instance) available for a subscription source/target.
class _BlockSelection {
  final int type;
  final int inst;
  final String name;
  final String label;

  const _BlockSelection({
    required this.type,
    required this.inst,
    required this.name,
    required this.label,
  });
}

/// A field within a [_BlockSelection]; `keyed` is true when the field's data type is a
/// keyed type (>= 0x100), so a key picker is offered.
class _FieldSelection {
  final int field;
  final BlockMeta meta;
  final String name;
  final bool keyed;

  const _FieldSelection({
    required this.field,
    required this.meta,
    required this.name,
    required this.keyed,
  });
}

/// Dialog for adding/editing requester subscriptions
class _SubscriptionDialog extends StatefulWidget {
  final SubscriptionClient client;
  final RegisterClient regClient;
  final List<RequesterSubscription> currentSubs;
  final RequesterSubscription? existing;
  final Future<void> Function() onSaved;

  const _SubscriptionDialog({
    required this.client,
    required this.regClient,
    required this.currentSubs,
    this.existing,
    required this.onSaved,
  });

  @override
  State<_SubscriptionDialog> createState() => _SubscriptionDialogState();
}

class _SubscriptionDialogState extends State<_SubscriptionDialog> {
  bool _loadingBlocks = true;
  String? _blockError;
  List<_BlockSelection> _targetBlocks = [];
  List<_BlockSelection> _sourceBlocks = [];
  List<_FieldSelection> _targetFields = [];
  List<_FieldSelection> _sourceFields = [];
  List<DeviceEntry> _devices = [];

  // Form fields
  _BlockSelection? _selectedTargetBlock;
  _FieldSelection? _selectedTargetField;
  int _targetKey = 0;
  _BlockSelection? _selectedSourceBlock;
  _FieldSelection? _selectedSourceField;
  int _sourceKey = 0;
  int? _selectedProviderAddr;
  TriggerType _trigger = TriggerType.periodic;
  int _periodMs = 1000;
  int _minTimeMs = 100;

  // Controllers for text fields
  final _periodController = TextEditingController();
  final _minTimeController = TextEditingController();

  @override
  void initState() {
    super.initState();
    _periodController.text = _periodMs.toString();
    _minTimeController.text = _minTimeMs.toString();
    _loadData();
    if (widget.existing != null) {
      _populateFromExisting(widget.existing!);
    }
  }

  @override
  void dispose() {
    _periodController.dispose();
    _minTimeController.dispose();
    super.dispose();
  }

  Future<void> _loadData() async {
    setState(() {
      _loadingBlocks = true;
      _blockError = null;
    });
    try {
      // Load the block list for both the target (local device) and the source
      // (provider device). Fields are fetched lazily once a block is picked.
      final targetBlocks = await _fetchBlocks(widget.client.deviceId);
      final providerAddr = widget.existing?.providerAddr ?? 1;
      final sourceBlocks = await _fetchBlocks(providerAddr);

      final db = DeviceDatabase.instance;
      await db.refreshNetwork();
      final devices = db.all.where((d) => d.id != widget.client.deviceId).toList();

      setState(() {
        _targetBlocks = targetBlocks;
        _sourceBlocks = sourceBlocks;
        _devices = devices;
        _loadingBlocks = false;

        if (_selectedProviderAddr == null && devices.isNotEmpty) {
          _selectedProviderAddr = devices.first.id;
        }

        // Pre-select the existing subscription's target/source once the blocks are known.
        final existing = widget.existing;
        if (existing != null) {
          _selectedTargetBlock = _matchBlock(_targetBlocks, existing.blockType, existing.blockInst);
          if (_selectedTargetBlock != null) {
            _targetKey = existing.blockKey;
            _loadFieldsForTarget(_selectedTargetBlock!, existing.blockField, existing.blockKey);
          }
          _selectedSourceBlock = _matchBlock(_sourceBlocks, existing.blockTypeS, existing.blockInstS);
          if (_selectedSourceBlock != null) {
            _sourceKey = existing.blockKeyS;
            _loadFieldsForSource(_selectedSourceBlock!, existing.blockFieldS, existing.blockKeyS);
          }
        }
      });
    } catch (e) {
      setState(() {
        _blockError = e.toString();
        _loadingBlocks = false;
      });
    }
  }

  static _BlockSelection? _matchBlock(List<_BlockSelection> blocks, int type, int inst) {
    for (final b in blocks) {
      if (b.type == type && b.inst == inst) return b;
    }
    return null;
  }

  Future<List<_BlockSelection>> _fetchBlocks(int deviceId) async {
    final regClient = RegisterClient(deviceId: deviceId);
    final blocks = await regClient.readBlocks();
    if (blocks == null) return [];
    final out = <_BlockSelection>[];
    for (final b in blocks) {
      if (b == null) continue;
      final blockType = BlockType.fromValue(b.type);
      final name = b.name.trim().isNotEmpty ? b.name : blockType.label;
      out.add(_BlockSelection(
        type: b.type,
        inst: b.inst,
        name: name,
        label: '$name [${b.inst}]',
      ));
    }
    return out;
  }

  /// Reads the fields of a block. Fails gracefully (empty list) when the field
  /// enumeration is unavailable so navigation never blocks.
  Future<List<_FieldSelection>> _fetchFields(int deviceId, _BlockSelection block) async {
    final regClient = RegisterClient(deviceId: deviceId);
    final fieldCount = await regClient.getFieldCount(block.type, block.inst);
    if (fieldCount == null) return [];

    final out = <_FieldSelection>[];
    for (var f = 0; f < fieldCount; f++) {
      final fieldResult = await regClient.readBlockField(block.type, block.inst, f, 0);
      if (fieldResult == null) continue;
      // Design-time field names when the block has a schema; "Field N" otherwise.
      final blockSchema = blockInfoFor(BlockType.fromValue(block.type));
      final fieldInfo = (blockSchema != null && f < blockSchema.fields.length)
          ? blockSchema.fields[f].name
          : null;
      out.add(_FieldSelection(
        field: f,
        meta: fieldResult.meta,
        name: fieldInfo ?? 'Field $f',
        keyed: fieldResult.meta.typeValue >= 0x100,
      ));
    }
    return out;
  }

  Future<void> _loadFieldsForTarget(_BlockSelection block, [int? preField, int? preKey]) async {
    setState(() { _selectedTargetBlock = block; _selectedTargetField = null; _targetFields = []; _targetKey = 0; });
    final fields = await _fetchFields(widget.client.deviceId, block);
    if (!mounted) return;
    setState(() {
      _targetFields = fields;
      if (preField != null) {
        for (final fl in fields) {
          if (fl.field == preField) { _selectedTargetField = fl; break; }
        }
      }
      if (preKey != null) _targetKey = preKey;
    });
  }

  Future<void> _loadFieldsForSource(_BlockSelection block, [int? preField, int? preKey]) async {
    setState(() { _selectedSourceBlock = block; _selectedSourceField = null; _sourceFields = []; _sourceKey = 0; });
    final fields = await _fetchFields(_selectedProviderAddr ?? 0, block);
    if (!mounted) return;
    setState(() {
      _sourceFields = fields;
      if (preField != null) {
        for (final fl in fields) {
          if (fl.field == preField) { _selectedSourceField = fl; break; }
        }
      }
      if (preKey != null) _sourceKey = preKey;
    });
  }

  static int _makeBlockInfo(int type, int inst, int field, int key) {
    return ((type & 0x3FF) << 22) | ((inst & 0x3F) << 16) | ((field & 0xFF) << 8) | (key & 0xFF);
  }

  void _populateFromExisting(RequesterSubscription sub) {
    _trigger = sub.trigger;
    _periodMs = sub.periodMs;
    _minTimeMs = sub.minTimeMs;
    _selectedProviderAddr = sub.providerAddr;

    _periodController.text = sub.periodMs.toString();
    _minTimeController.text = sub.minTimeMs.toString();
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final isEdit = widget.existing != null;

    return AlertDialog(
      title: Text(isEdit ? 'Edit Subscription' : 'Add Subscription'),
      content: SizedBox(
        width: 500,
        child: _loadingBlocks
            ? const Center(child: CircularProgressIndicator())
            : _blockError != null
                ? Text('Error loading blocks: $_blockError', style: TextStyle(color: theme.colorScheme.error))
                : SingleChildScrollView(
                    child: Column(
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        // Target Block -> Field -> Key (local)
                        _BlockPicker(
                          label: 'Target Block',
                          hint: 'Where to write received values',
                          blocks: _targetBlocks,
                          value: _selectedTargetBlock,
                          onChanged: (v) { if (v != null) { _loadFieldsForTarget(v); } },
                        ),
                        if (_selectedTargetBlock != null) ...[
                          const SizedBox(height: 12),
                          _FieldPicker(
                            label: 'Target Field',
                            fields: _targetFields,
                            value: _selectedTargetField,
                            onChanged: (v) => setState(() => _selectedTargetField = v),
                          ),
                          if (_selectedTargetField != null && _selectedTargetField!.keyed) ...[
                            const SizedBox(height: 12),
                            _KeyPicker(
                              label: 'Target Key',
                              value: _targetKey,
                              onChanged: (v) => setState(() => _targetKey = v),
                            ),
                          ],
                        ],
                        const SizedBox(height: 12),

                        // Provider Device
                        _ProviderPicker(
                          label: 'Provider Device',
                          hint: 'Device providing the data',
                          devices: _devices,
                          value: _selectedProviderAddr,
                          onChanged: (v) async {
                            setState(() {
                              _selectedProviderAddr = v;
                              _selectedSourceBlock = null;
                              _selectedSourceField = null;
                              _sourceFields = [];
                            });
                            if (v != null) {
                              final blocks = await _fetchBlocks(v);
                              if (!mounted) return;
                              setState(() => _sourceBlocks = blocks);
                            }
                          },
                        ),
                        const SizedBox(height: 12),

                        // Source Block -> Field -> Key (remote)
                        _BlockPicker(
                          label: 'Source Block',
                          hint: 'Which value to subscribe to',
                          blocks: _sourceBlocks,
                          value: _selectedSourceBlock,
                          onChanged: (v) { if (v != null) { _loadFieldsForSource(v); } },
                        ),
                        if (_selectedSourceBlock != null) ...[
                          const SizedBox(height: 12),
                          _FieldPicker(
                            label: 'Source Field',
                            fields: _sourceFields,
                            value: _selectedSourceField,
                            onChanged: (v) => setState(() => _selectedSourceField = v),
                          ),
                          if (_selectedSourceField != null && _selectedSourceField!.keyed) ...[
                            const SizedBox(height: 12),
                            _KeyPicker(
                              label: 'Source Key',
                              value: _sourceKey,
                              onChanged: (v) => setState(() => _sourceKey = v),
                            ),
                          ],
                        ],
                        const SizedBox(height: 12),

                        // Trigger Type
                        DropdownButtonFormField<TriggerType>(
                          initialValue: _trigger,
                          decoration: const InputDecoration(labelText: 'Trigger Type'),
                          items: TriggerType.values.map((t) => DropdownMenuItem(
                            value: t,
                            child: Text(t.label),
                          )).toList(),
                          onChanged: (v) => setState(() => _trigger = v!),
                        ),
                        const SizedBox(height: 12),

                        // Period (ms) - shown for periodic triggers
                        if (_needsPeriod())
                          _NumberField(
                            label: 'Period (ms)',
                            controller: _periodController,
                            onChanged: (v) => _periodMs = v,
                            min: 1,
                          ),
                        if (_needsPeriod()) const SizedBox(height: 12),

                        // Min interval (periodic) / retry interval (confirm).
                        // Docs/Services/Subscriptions.md: plain Periodic only needs a Period.
                        if (_needsInterval()) ...[
                          _NumberField(
                            label: _intervalLabel(),
                            controller: _minTimeController,
                            onChanged: (v) => _minTimeMs = v,
                            min: 0,
                          ),
                          const SizedBox(height: 12),
                        ],
                      ],
                    ),
                  ),
      ),
      actions: [
        TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(
          onPressed: _canSave() ? _save : null,
          child: Text(isEdit ? 'Save' : 'Add'),
        ),
      ],
    );
  }

  bool _needsPeriod() {
    return _trigger == TriggerType.periodic ||
           _trigger == TriggerType.onChangePeriodic;
  }

  // Docs: OnChange+Period uses a Minimum interval; OnChange+Confirm a Retry interval.
  bool _needsInterval() {
    return _trigger == TriggerType.onChangePeriodic ||
           _trigger == TriggerType.onChangeConfirm;
  }

  String _intervalLabel() {
    return _trigger == TriggerType.onChangeConfirm
        ? 'Retry Interval (ms)'
        : 'Min Interval (ms)';
  }

  bool _canSave() {
    if (_selectedTargetBlock == null || _selectedTargetField == null ||
        _selectedSourceBlock == null || _selectedSourceField == null ||
        _selectedProviderAddr == null) {
      return false;
    }
    if (_needsPeriod() && _periodMs <= 0) return false;
    if (_minTimeMs < 0) return false;
    return true;
  }

  Future<void> _save() async {
    final targetReg = _makeBlockInfo(_selectedTargetBlock!.type, _selectedTargetBlock!.inst,
        _selectedTargetField!.field, _targetKey);
    final sourceReg = _makeBlockInfo(_selectedSourceBlock!.type, _selectedSourceBlock!.inst,
        _selectedSourceField!.field, _sourceKey);
    final providerAddr = _selectedProviderAddr!;

    final index = widget.existing?.index ?? _findFreeIndex();
    if (index >= 16) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Max subscriptions reached')));
      return;
    }

    final trid = widget.existing?.trid ?? (0xFA00 + index);

    final entry = RequesterSubscription(
      index: index,
      targetReg: targetReg,
      sourceReg: sourceReg,
      providerAddr: providerAddr,
      trigger: _trigger,
      periodMs: _periodMs,
      minTimeMs: _minTimeMs,
      trid: trid,
    );

    final ok = await widget.client.setRequesterSubscription(index, entry: entry);
    if (ok) {
      await widget.onSaved();
      if (mounted) {
        Navigator.pop(context);
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text(isEdit ? 'Subscription updated' : 'Subscription added')),
        );
      }
    } else {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('Failed to save subscription')));
      }
    }
  }

  bool get isEdit => widget.existing != null;

  int _findFreeIndex() {
    int index = 0;
    while (index < 16 && widget.currentSubs.any((s) => s.index == index)) {
      index++;
    }
    return index;
  }
}

class _BlockPicker extends StatelessWidget {
  final String label;
  final String hint;
  final List<_BlockSelection> blocks;
  final _BlockSelection? value;
  final ValueChanged<_BlockSelection?> onChanged;

  const _BlockPicker({
    required this.label,
    required this.hint,
    required this.blocks,
    required this.value,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    if (blocks.isEmpty) {
      return InputDecorator(
        decoration: InputDecoration(
          labelText: label,
          hintText: hint,
          border: const OutlineInputBorder(),
        ),
        child: const Text('No blocks available'),
      );
    }

    return DropdownButtonFormField<_BlockSelection>(
      initialValue: value,
      decoration: InputDecoration(
        labelText: label,
        hintText: hint,
        border: const OutlineInputBorder(),
      ),
      isExpanded: true,
      items: blocks.map((opt) => DropdownMenuItem(
        value: opt,
        child: Text(opt.label, overflow: TextOverflow.ellipsis),
      )).toList(),
      onChanged: onChanged,
      menuMaxHeight: 300,
    );
  }
}

class _FieldPicker extends StatelessWidget {
  final String label;
  final List<_FieldSelection> fields;
  final _FieldSelection? value;
  final ValueChanged<_FieldSelection?> onChanged;

  const _FieldPicker({
    required this.label,
    required this.fields,
    required this.value,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    if (fields.isEmpty) {
      return const InputDecorator(
        decoration: InputDecoration(
          labelText: 'Field',
          hintText: 'No fields available',
          border: OutlineInputBorder(),
        ),
        child: Text('No fields available'),
      );
    }

    return DropdownButtonFormField<_FieldSelection>(
      initialValue: value,
      decoration: InputDecoration(
        labelText: label,
        border: const OutlineInputBorder(),
      ),
      isExpanded: true,
      items: fields.map((opt) => DropdownMenuItem(
        value: opt,
        child: Text('${opt.name}  (${opt.keyed ? "keyed" : opt.meta.dataType.name})', overflow: TextOverflow.ellipsis),
      )).toList(),
      onChanged: onChanged,
      menuMaxHeight: 300,
    );
  }
}

class _KeyPicker extends StatelessWidget {
  final String label;
  final int value;
  final ValueChanged<int> onChanged;

  static const keys = [0, 1, 2, 3, 4, 5, 6, 7];

  const _KeyPicker({
    required this.label,
    required this.value,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    return DropdownButtonFormField<int>(
      initialValue: value,
      decoration: InputDecoration(
        labelText: label,
        border: const OutlineInputBorder(),
      ),
      isExpanded: true,
      items: keys.map((k) => DropdownMenuItem(value: k, child: Text('Key $k'))).toList(),
      onChanged: (v) => onChanged(v!),
    );
  }
}

class _ProviderPicker extends StatelessWidget {
  final String label;
  final String hint;
  final List<DeviceEntry> devices;
  final int? value;
  final ValueChanged<int?> onChanged;

  const _ProviderPicker({
    required this.label,
    required this.hint,
    required this.devices,
    required this.value,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    if (devices.isEmpty) {
      return InputDecorator(
        decoration: InputDecoration(
          labelText: label,
          hintText: 'No other devices found',
          border: const OutlineInputBorder(),
        ),
        child: const Text('No other devices found'),
      );
    }

    return DropdownButtonFormField<int>(
      initialValue: value,
      decoration: InputDecoration(
        labelText: label,
        hintText: hint,
        border: const OutlineInputBorder(),
      ),
      isExpanded: true,
      items: devices.map((d) => DropdownMenuItem(
        value: d.id,
        child: Text('${d.displayName} (ID: ${d.id})', overflow: TextOverflow.ellipsis),
      )).toList(),
      onChanged: onChanged,
    );
  }
}

class _NumberField extends StatelessWidget {
  final String label;
  final TextEditingController controller;
  final ValueChanged<int> onChanged;
  final int min;

  const _NumberField({
    required this.label,
    required this.controller,
    required this.onChanged,
    this.min = 0,
  });

  @override
  Widget build(BuildContext context) {
    return TextFormField(
      controller: controller,
      decoration: InputDecoration(
        labelText: label,
        border: const OutlineInputBorder(),
      ),
      keyboardType: TextInputType.number,
      onChanged: (v) {
        final parsed = int.tryParse(v) ?? min;
        onChanged(parsed.clamp(min, 0x7FFFFFFF));
      },
      validator: (v) {
        final parsed = int.tryParse(v ?? '');
        if (parsed == null || parsed < min) return 'Enter a valid number (min $min)';
        return null;
      },
    );
  }
}