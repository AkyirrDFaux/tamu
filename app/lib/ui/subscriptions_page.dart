/// Subscriptions service UI page.
library;

import 'package:flutter/material.dart';
import 'package:tamuapp/core/block_registry.dart' show FieldInfo, blockInfoFor;
import 'package:tamuapp/core/device_db.dart';
import 'package:tamuapp/core/register_client.dart';
import 'package:tamuapp/core/subscription_client.dart';
import 'package:tamuapp/core/types.dart' show BlockType, BlockMeta, Capability, DataType, int32FromBytes, int32ToBytes, ProviderSubscription, RequesterSubscription, TriggerType, Tlvf;
import 'package:tamuapp/ui/value_editor.dart' show dataTypeLabel;

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
                _detailRow('Counter', '${sub.counter}'),
                _detailRow('Last Sent', '${sub.lastSentMs} ms ago'),
                _detailRow('Requester Addr', '${sub.requesterAddr}'),
                if (sub.tolerance.isNotEmpty)
                  _detailRow('Tolerance', _formatTolerance(sub.tolerance)),
                if (sub.lastValue.isNotEmpty) ...[
                  const Divider(),
                  Text('Last Value:', style: TextStyle(color: Colors.orange)),
                  _detailRow('TLFV', _formatTlvf(sub.lastValue)),
                ],
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
                _detailRow('Counter', '${sub.counter}'),
                if (sub.tolerance.isNotEmpty)
                  _detailRow('Tolerance', _formatTolerance(sub.tolerance)),
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

  String _formatTolerance(List<int> tol) {
    if (tol.isEmpty) return '-';
    final dt = DataType.fromValue(tol[0]);
    if (tol.length >= 5) {
      final val = int32FromBytes(tol, 1);
      return '${dataTypeLabel(dt)}: $val';
    }
    return tol.map((b) => '0x${b.toRadixString(16).padLeft(2, '0')}').join(' ');
  }

  String _formatTlvf(List<int> tlvf) {
    final t = Tlvf.fromBytes(tlvf);
    return t?.format() ?? tlvf.map((b) => '0x${b.toRadixString(16).padLeft(2, '0')}').join(' ');
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
class _BlockFieldOption {
  final int blockInfo;
  final String label;
  final String blockName;
  final int type;
  final int inst;
  final int field;
  final BlockMeta meta;
  final FieldInfo? fieldInfo;

  const _BlockFieldOption({
    required this.blockInfo,
    required this.label,
    required this.blockName,
    required this.type,
    required this.inst,
    required this.field,
    required this.meta,
    this.fieldInfo,
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
  List<_BlockFieldOption> _targetOptions = [];
  List<_BlockFieldOption> _sourceOptions = [];
  List<DeviceEntry> _devices = [];

  // Form fields
  _BlockFieldOption? _selectedTarget;
  _BlockFieldOption? _selectedSource;
  int? _selectedProviderAddr;
  TriggerType _trigger = TriggerType.periodic;
  int _periodMs = 1000;
  int _minTimeMs = 100;
  int _counter = 0;
  int? _toleranceValue;

  // Controllers for text fields
  final _periodController = TextEditingController();
  final _minTimeController = TextEditingController();
  final _counterController = TextEditingController();
  final _toleranceController = TextEditingController();

  @override
  void initState() {
    super.initState();
    _periodController.text = _periodMs.toString();
    _minTimeController.text = _minTimeMs.toString();
    _counterController.text = _counter.toString();
    _loadData();
    if (widget.existing != null) {
      _populateFromExisting(widget.existing!);
    }
  }

  @override
  void dispose() {
    _periodController.dispose();
    _minTimeController.dispose();
    _counterController.dispose();
    _toleranceController.dispose();
    super.dispose();
  }

  Future<void> _loadData() async {
    setState(() {
      _loadingBlocks = true;
      _blockError = null;
    });
    try {
      // Load available blocks for target (local device) and source (provider device)
      final targetBlocks = await _fetchAllBlockFields(widget.client.deviceId);
      final providerAddr = widget.existing?.providerAddr ?? 1;
      final sourceBlocks = await _fetchAllBlockFields(providerAddr);

      // Load available devices for provider selection
      final db = DeviceDatabase.instance;
      await db.refreshNetwork();
      final devices = db.all.where((d) => d.id != widget.client.deviceId).toList();

      setState(() {
        _targetOptions = targetBlocks;
        _sourceOptions = sourceBlocks;
        _devices = devices;
        _loadingBlocks = false;

        // Set default provider
        if (_selectedProviderAddr == null && devices.isNotEmpty) {
          _selectedProviderAddr = devices.first.id;
        }
      });
    } catch (e) {
      setState(() {
        _blockError = e.toString();
        _loadingBlocks = false;
      });
    }
  }

  Future<List<_BlockFieldOption>> _fetchAllBlockFields(int deviceId) async {
    final regClient = RegisterClient(deviceId: deviceId);
    final blocks = await regClient.readBlocks();
    if (blocks == null) return [];

    final options = <_BlockFieldOption>[];
    for (final b in blocks) {
      if (b == null) continue;
      final fieldCount = await regClient.getFieldCount(b.type, b.inst);
      if (fieldCount == null) continue;
      final blockType = BlockType.fromValue(b.type);
      final blockInfo = blockInfoFor(blockType);
      for (var f = 0; f < fieldCount; f++) {
        final fieldResult = await regClient.readBlockField(b.type, b.inst, f, 0);
        if (fieldResult != null) {
          final bi = ((b.type & 0x3FF) << 22) | ((b.inst & 0x3F) << 16) | ((f & 0xFF) << 8) | 0;
          final fieldInfo = blockInfo?.field(f);
          final fieldName = fieldInfo?.name ?? 'Field $f';
          options.add(_BlockFieldOption(
            blockInfo: bi,
            label: '${b.name.isNotEmpty ? b.name : blockType.label}[${b.inst}].$fieldName',
            blockName: b.name.isNotEmpty ? b.name : blockType.label,
            type: b.type,
            inst: b.inst,
            field: f,
            meta: fieldResult.meta,
            fieldInfo: fieldInfo,
          ));
        }
      }
    }
    return options;
  }

  void _populateFromExisting(RequesterSubscription sub) {
    // We can't easily populate pickers without loading blocks first,
    // so we'll set the raw values and update pickers after loading
    _trigger = sub.trigger;
    _periodMs = sub.periodMs;
    _minTimeMs = sub.minTimeMs;
    _counter = sub.counter;
    _selectedProviderAddr = sub.providerAddr;

    _periodController.text = sub.periodMs.toString();
    _minTimeController.text = sub.minTimeMs.toString();
    _counterController.text = sub.counter.toString();

    if (sub.tolerance.length >= 5) {
      _toleranceValue = int32FromBytes(sub.tolerance, 1);
      _toleranceController.text = _toleranceValue.toString();
    }
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
                        // Target Block (local)
                        _BlockFieldPicker(
                          label: 'Target Block (Local)',
                          hint: 'Where to write received values',
                          options: _targetOptions,
                          value: _selectedTarget,
                          onChanged: (v) => setState(() => _selectedTarget = v),
                        ),
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
                              _selectedSource = null;
                              _sourceOptions = [];
                            });
                            if (v != null) {
                              final blocks = await _fetchAllBlockFields(v);
                              setState(() => _sourceOptions = blocks);
                            }
                          },
                        ),
                        const SizedBox(height: 12),

                        // Source Block (remote)
                        _BlockFieldPicker(
                          label: 'Source Block (Remote)',
                          hint: 'Which value to subscribe to',
                          options: _sourceOptions,
                          value: _selectedSource,
                          onChanged: (v) => setState(() => _selectedSource = v),
                        ),
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

                        // Min interval (periodic triggers) / retry interval (confirm + edge triggers).
                        // Docs/Services/Subscriptions.md lists these per trigger type; plain
                        // Periodic only needs a Period, so the field is hidden there.
                        if (_needsInterval()) ...[
                          _NumberField(
                            label: _intervalLabel(),
                            controller: _minTimeController,
                            onChanged: (v) => _minTimeMs = v,
                            min: 0,
                          ),
                          const SizedBox(height: 12),
                        ],

                        // Counter - shown for edge triggers
                        if (_needsCounter()) ...[
                          _NumberField(
                            label: 'Counter (initial)',
                            controller: _counterController,
                            onChanged: (v) => _counter = v,
                            min: 0,
                          ),
                          const SizedBox(height: 12),
                        ],

                        // Tolerance - shown for delta triggers
                        if (_needsTolerance()) ...[
                          _NumberField(
                            label: 'Tolerance (int32)',
                            controller: _toleranceController,
                            onChanged: (v) => _toleranceValue = v,
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
           _trigger == TriggerType.onChangePeriodic ||
           _trigger == TriggerType.deltaPeriodic;
  }

  // Docs: Minimum interval is a Periodic-family setting (OnChange+Period, Delta+Period);
  // Confirm and edge triggers use a Retry interval instead. Plain Periodic needs neither.
  bool _needsInterval() {
    return _trigger == TriggerType.onChangePeriodic ||
           _trigger == TriggerType.onChangeConfirm ||
           _trigger == TriggerType.edgeRise ||
           _trigger == TriggerType.edgeFall ||
           _trigger == TriggerType.deltaPeriodic ||
           _trigger == TriggerType.deltaConfirm;
  }

  String _intervalLabel() {
    return _needsConfirmOrEdge()
        ? 'Retry Interval (ms)'
        : 'Min Interval (ms)';
  }

  bool _needsConfirmOrEdge() {
    return _trigger == TriggerType.onChangeConfirm ||
           _trigger == TriggerType.edgeRise ||
           _trigger == TriggerType.edgeFall ||
           _trigger == TriggerType.deltaConfirm;
  }

  bool _needsCounter() {
    return _trigger == TriggerType.edgeRise || _trigger == TriggerType.edgeFall;
  }

  bool _needsTolerance() {
    return _trigger == TriggerType.deltaPeriodic || _trigger == TriggerType.deltaConfirm;
  }

  bool _canSave() {
    if (_selectedTarget == null || _selectedSource == null || _selectedProviderAddr == null) return false;
    if (_needsPeriod() && _periodMs <= 0) return false;
    if (_minTimeMs < 0) return false;
    return true;
  }

  Future<void> _save() async {
    final targetReg = _selectedTarget!.blockInfo;
    final sourceReg = _selectedSource!.blockInfo;
    final providerAddr = _selectedProviderAddr!;

    List<int> tolerance = [];
    if (_needsTolerance() && _toleranceValue != null) {
      tolerance = int32ToBytes(_toleranceValue!);
    }

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
      counter: _counter,
      tolerance: tolerance,
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

class _BlockFieldPicker extends StatelessWidget {
  final String label;
  final String hint;
  final List<_BlockFieldOption> options;
  final _BlockFieldOption? value;
  final ValueChanged<_BlockFieldOption?> onChanged;

  const _BlockFieldPicker({
    required this.label,
    required this.hint,
    required this.options,
    required this.value,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    if (options.isEmpty) {
      return InputDecorator(
        decoration: InputDecoration(
          labelText: label,
          hintText: 'No blocks available',
          border: const OutlineInputBorder(),
        ),
        child: const Text('No blocks available'),
      );
    }

    return DropdownButtonFormField<_BlockFieldOption>(
      initialValue: value,
      decoration: InputDecoration(
        labelText: label,
        hintText: hint,
        border: const OutlineInputBorder(),
      ),
      isExpanded: true,
      items: options.map((opt) => DropdownMenuItem(
        value: opt,
        child: Text(opt.label, overflow: TextOverflow.ellipsis),
      )).toList(),
      onChanged: onChanged,
      menuMaxHeight: 300,
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