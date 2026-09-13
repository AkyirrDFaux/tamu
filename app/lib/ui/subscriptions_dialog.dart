/// Requester-subscription create/edit dialog and its block/field/key pickers
/// (extracted from subscriptions_page.dart).
library;

import 'package:flutter/material.dart';

import '../core/block_registry.dart' show blockInfoFor;
import '../core/device_db.dart';
import '../core/register_client.dart';
import '../core/subscription_client.dart';
import '../core/types.dart' show BlockType, BlockMeta, ProviderSubscription, RequesterSubscription, TriggerType, makeBlockInfo;
import 'widgets.dart' show showSnack;

/// Block+Field picker model
/// A block (block type + instance) available for a subscription source/target.
class BlockSelection {
  final int type;
  final int inst;
  final String name;
  final String label;

  const BlockSelection({
    required this.type,
    required this.inst,
    required this.name,
    required this.label,
  });
}

/// A field within a [BlockSelection]; `keyed` is true when the field's data type is a
/// keyed type (>= 0x100), so a key picker is offered.
class FieldSelection {
  final int field;
  final BlockMeta meta;
  final String name;
  final bool keyed;

  const FieldSelection({
    required this.field,
    required this.meta,
    required this.name,
    required this.keyed,
  });
}

/// Dialog for adding/editing requester subscriptions
class SubscriptionDialog extends StatefulWidget {
  final SubscriptionClient client;
  final RegisterClient regClient;
  final List<RequesterSubscription> currentSubs;
  final RequesterSubscription? existing;
  final Future<void> Function() onSaved;

  const SubscriptionDialog({
    required this.client,
    required this.regClient,
    required this.currentSubs,
    this.existing,
    required this.onSaved,
  });

  @override
  State<SubscriptionDialog> createState() => SubscriptionDialogState();
}

class SubscriptionDialogState extends State<SubscriptionDialog> {
  bool _loadingBlocks = true;
  String? _blockError;
  List<BlockSelection> _targetBlocks = [];
  List<BlockSelection> _sourceBlocks = [];
  List<FieldSelection> _targetFields = [];
  List<FieldSelection> _sourceFields = [];
  List<DeviceEntry> _devices = [];

  // Form fields
  BlockSelection? _selectedTargetBlock;
  FieldSelection? _selectedTargetField;
  int _targetKey = 0;
  BlockSelection? _selectedSourceBlock;
  FieldSelection? _selectedSourceField;
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
      // The provider picker lists every device except the local one. Resolve it up
      // front so the source block list always comes from the device the picker shows
      // (editing keeps the existing subscription's provider).
      final db = DeviceDatabase.instance;
      await db.refreshNetwork();
      final devices = db.all.where((d) => d.id != widget.client.deviceId).toList();

      final targetBlocks = await _fetchBlocks(widget.client.deviceId);
      final providerAddr =
          widget.existing?.providerAddr ?? (devices.isNotEmpty ? devices.first.id : 1);
      final sourceBlocks = await _fetchBlocks(providerAddr);

      setState(() {
        _targetBlocks = targetBlocks;
        _sourceBlocks = sourceBlocks;
        _devices = devices;
        _loadingBlocks = false;
        _selectedProviderAddr = providerAddr;

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

  static BlockSelection? _matchBlock(List<BlockSelection> blocks, int type, int inst) {
    for (final b in blocks) {
      if (b.type == type && b.inst == inst) return b;
    }
    return null;
  }

  Future<List<BlockSelection>> _fetchBlocks(int deviceId) async {
    final regClient = RegisterClient(deviceId: deviceId);
    final blocks = await regClient.readBlocks();
    if (blocks == null) return [];
    final out = <BlockSelection>[];
    for (final b in blocks) {
      if (b == null) continue;
      final blockType = BlockType.fromValue(b.type);
      final name = b.name.trim().isNotEmpty ? b.name : blockType.label;
      out.add(BlockSelection(
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
  Future<List<FieldSelection>> _fetchFields(int deviceId, BlockSelection block) async {
    final regClient = RegisterClient(deviceId: deviceId);
    final fieldCount = await regClient.getFieldCount(block.type, block.inst);
    if (fieldCount == null) return [];

    final out = <FieldSelection>[];
    for (var f = 0; f < fieldCount; f++) {
      final fieldResult = await regClient.readBlockField(block.type, block.inst, f, 0);
      if (fieldResult == null) continue;
      // Design-time field names when the block has a schema; "Field N" otherwise.
      final blockSchema = blockInfoFor(BlockType.fromValue(block.type));
      final fieldInfo = (blockSchema != null && f < blockSchema.fields.length)
          ? blockSchema.fields[f].name
          : null;
      out.add(FieldSelection(
        field: f,
        meta: fieldResult.meta,
        name: fieldInfo ?? 'Field $f',
        keyed: fieldResult.meta.typeValue >= 0x100,
      ));
    }
    return out;
  }

  Future<void> _loadFieldsForTarget(BlockSelection block, [int? preField, int? preKey]) async {
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

  Future<void> _loadFieldsForSource(BlockSelection block, [int? preField, int? preKey]) async {
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
                        BlockPicker(
                          label: 'Target Block',
                          hint: 'Where to write received values',
                          blocks: _targetBlocks,
                          value: _selectedTargetBlock,
                          onChanged: (v) { if (v != null) { _loadFieldsForTarget(v); } },
                        ),
                        if (_selectedTargetBlock != null) ...[
                          const SizedBox(height: 12),
                          FieldPicker(
                            label: 'Target Field',
                            fields: _targetFields,
                            value: _selectedTargetField,
                            onChanged: (v) => setState(() => _selectedTargetField = v),
                          ),
                          if (_selectedTargetField != null && _selectedTargetField!.keyed) ...[
                            const SizedBox(height: 12),
                            KeyPicker(
                              label: 'Target Key',
                              value: _targetKey,
                              onChanged: (v) => setState(() => _targetKey = v),
                            ),
                          ],
                        ],
                        const SizedBox(height: 12),

                        // Provider Device
                        ProviderPicker(
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
                        BlockPicker(
                          label: 'Source Block',
                          hint: 'Which value to subscribe to',
                          blocks: _sourceBlocks,
                          value: _selectedSourceBlock,
                          onChanged: (v) { if (v != null) { _loadFieldsForSource(v); } },
                        ),
                        if (_selectedSourceBlock != null) ...[
                          const SizedBox(height: 12),
                          FieldPicker(
                            label: 'Source Field',
                            fields: _sourceFields,
                            value: _selectedSourceField,
                            onChanged: (v) => setState(() => _selectedSourceField = v),
                          ),
                          if (_selectedSourceField != null && _selectedSourceField!.keyed) ...[
                            const SizedBox(height: 12),
                            KeyPicker(
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
                          NumberField(
                            label: 'Period (ms)',
                            controller: _periodController,
                            onChanged: (v) => _periodMs = v,
                            min: 1,
                          ),
                        if (_needsPeriod()) const SizedBox(height: 12),

                        // Min interval (periodic) / retry interval (confirm).
                        // Docs/Services/Subscriptions.md: plain Periodic only needs a Period.
                        if (_needsInterval()) ...[
                          NumberField(
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
    final targetReg = makeBlockInfo(_selectedTargetBlock!.type, _selectedTargetBlock!.inst,
        _selectedTargetField!.field, _targetKey);
    final sourceReg = makeBlockInfo(_selectedSourceBlock!.type, _selectedSourceBlock!.inst,
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

class BlockPicker extends StatelessWidget {
  final String label;
  final String hint;
  final List<BlockSelection> blocks;
  final BlockSelection? value;
  final ValueChanged<BlockSelection?> onChanged;

  const BlockPicker({
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

    return DropdownButtonFormField<BlockSelection>(
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

class FieldPicker extends StatelessWidget {
  final String label;
  final List<FieldSelection> fields;
  final FieldSelection? value;
  final ValueChanged<FieldSelection?> onChanged;

  const FieldPicker({
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

    return DropdownButtonFormField<FieldSelection>(
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

class KeyPicker extends StatelessWidget {
  final String label;
  final int value;
  final ValueChanged<int> onChanged;

  static const keys = [0, 1, 2, 3, 4, 5, 6, 7];

  const KeyPicker({
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

class ProviderPicker extends StatelessWidget {
  final String label;
  final String hint;
  final List<DeviceEntry> devices;
  final int? value;
  final ValueChanged<int?> onChanged;

  const ProviderPicker({
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

class NumberField extends StatelessWidget {
  final String label;
  final TextEditingController controller;
  final ValueChanged<int> onChanged;
  final int min;

  const NumberField({
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