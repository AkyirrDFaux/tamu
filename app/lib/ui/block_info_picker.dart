/// Script BlockInfo picker (register targets): a tiered block -> field -> key dialogue
/// reusing the Subscriptions dialog's pickers (Docs/Services/Script.md: register read/write
/// take a blockinfo operand).
library;

import 'package:flutter/material.dart';

import '../core/types.dart';
import 'subscriptions_dialog.dart'
    show BlockPicker, BlockSelection, FieldPicker, FieldSelection, KeyPicker,
        fetchBlockSelections, fetchFieldSelections;
import 'widgets.dart' show DialogBody;

/// Shows the tiered BlockInfo picker and returns the 4-byte value, or null when cancelled.
Future<List<int>?> pickBlockInfo(BuildContext context, int deviceId,
    {List<int> current = const []}) {
  return showDialog<List<int>>(
    context: context,
    builder: (_) => _BlockInfoDialog(deviceId: deviceId, current: current),
  );
}

class _BlockInfoDialog extends StatefulWidget {
  final int deviceId;
  final List<int> current;

  const _BlockInfoDialog({required this.deviceId, required this.current});

  @override
  State<_BlockInfoDialog> createState() => _BlockInfoDialogState();
}

class _BlockInfoDialogState extends State<_BlockInfoDialog> {
  bool _loading = true;
  String? _error;
  List<BlockSelection> _blocks = [];
  List<FieldSelection> _fields = [];
  BlockSelection? _block;
  FieldSelection? _field;
  int _key = 0;

  @override
  void initState() {
    super.initState();
    _load();
  }

  int get _bi => widget.current.length >= 4 ? uint32FromBytes(widget.current) : 0;

  Future<void> _load() async {
    try {
      final list = await fetchBlockSelections(widget.deviceId);
      final curType = (_bi >> 22) & 0x3FF;
      final curInst = (_bi >> 16) & 0x3F;
      final curField = (_bi >> 8) & 0xFF;
      final curKey = _bi & 0xFF;
      BlockSelection? sel;
      for (final b in list) {
        if (b.type == curType && b.inst == curInst) {
          sel = b;
          break;
        }
      }
      if (!mounted) return;
      setState(() {
        _blocks = list;
        _block = sel;
        _key = curKey;
        _loading = false;
      });
      if (sel != null) await _loadFields(sel, curField);
    } catch (e) {
      if (!mounted) return;
      setState(() {
        _error = '$e';
        _loading = false;
      });
    }
  }

  Future<void> _loadFields(BlockSelection block, [int? select]) async {
    final fields = await fetchFieldSelections(widget.deviceId, block);
    if (!mounted) return;
    setState(() {
      _fields = fields;
      _field = fields.where((e) => e.field == select).firstOrNull ?? fields.firstOrNull;
    });
  }

  @override
  Widget build(BuildContext context) {
    final body = _loading
        ? const SizedBox(
            height: 120, child: Center(child: CircularProgressIndicator()))
        : _error != null
            ? Text('Could not load blocks: $_error')
            : Column(mainAxisSize: MainAxisSize.min, children: [
                BlockPicker(
                  label: 'Block',
                  hint: 'Pick a block',
                  blocks: _blocks,
                  value: _block,
                  onChanged: (b) {
                    if (b == null) return;
                    setState(() {
                      _block = b;
                      _fields = [];
                      _field = null;
                    });
                    _loadFields(b);
                  },
                ),
                const SizedBox(height: 8),
                FieldPicker(
                  label: 'Field',
                  fields: _fields,
                  value: _field,
                  onChanged: (f) => setState(() => _field = f),
                ),
                const SizedBox(height: 8),
                KeyPicker(
                  label: 'Key',
                  value: _key > 7 ? 0 : _key,
                  onChanged: (k) => setState(() => _key = k),
                ),
              ]);
    return AlertDialog(
      title: const Text('BlockInfo'),
      content: DialogBody(maxWidth: 360, child: body),
      actions: [
        TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(
          onPressed: _block == null || _field == null
              ? null
              : () => Navigator.pop(
                  context,
                  uint32ToBytes(makeBlockInfo(
                      _block!.type, _block!.inst, _field!.field, _key))),
          child: const Text('OK'),
        ),
      ],
    );
  }
}
