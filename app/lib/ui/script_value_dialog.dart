/// Script value editor (Docs/App/Service views/Script.md): name/type/UI-style/limits and,
/// for inputs and constants, the value bytes.
library;

import 'dart:typed_data';

import 'package:flutter/material.dart';

import '../core/script_draft.dart';
import '../core/script_file.dart';
import '../core/types.dart';
import 'block_info_picker.dart';
import 'value_editor.dart';
import 'widgets.dart';

class ScriptValueDialog extends StatefulWidget {
  final ScriptValueCategory category;
  final ScriptDraftValue initial;
  final int index;
  final int deviceId;

  const ScriptValueDialog(
      {super.key,
      required this.category,
      required this.initial,
      required this.index,
      required this.deviceId});

  @override
  State<ScriptValueDialog> createState() => _ScriptValueDialogState();
}

class _ScriptValueDialogState extends State<ScriptValueDialog> {
  late final TextEditingController _name = TextEditingController(text: widget.initial.name);
  late final TextEditingController _min = TextEditingController(text: _trim(widget.initial.spec.min));
  late final TextEditingController _max = TextEditingController(text: _trim(widget.initial.spec.max));
  late final TextEditingController _step = TextEditingController(text: _trim(widget.initial.spec.step));
  late DataType _type = widget.initial.type;
  late int _uiType = widget.initial.spec.uiType;
  late List<int> _value = widget.initial.value.isNotEmpty
      ? List<int>.from(widget.initial.value)
      : (_editableValue ? List<int>.filled(defaultSizeForType(widget.initial.type), 0) : <int>[]);

  bool get _isInput => widget.category == ScriptValueCategory.input;
  bool get _isConstant => widget.category == ScriptValueCategory.constant;

  /// Inputs carry a default and constants carry a value; outputs/variables have none.
  bool get _editableValue => _isInput || _isConstant;

  static String _trim(double v) => v == v.roundToDouble() ? '${v.toInt()}' : '$v';

  @override
  void dispose() {
    _name.dispose();
    _min.dispose();
    _max.dispose();
    _step.dispose();
    super.dispose();
  }

  void _changeType(DataType? t) {
    if (t == null) return;
    setState(() {
      _type = t;
      // Size follows the type (fixed types) or the value (value-bearing entries).
      _value = List<int>.filled(defaultSizeForType(t), 0);
      if (!uiStylesForType(t).contains(_uiType)) _uiType = ScriptUiType.auto;
    });
  }

  void _submit() {
    final spec = _isInput
        ? ScriptInputSpec(
            uiType: _uiType,
            min: double.tryParse(_min.text.trim()) ?? 0,
            max: double.tryParse(_max.text.trim()) ?? 0,
            step: double.tryParse(_step.text.trim()) ?? 0,
          )
        : const ScriptInputSpec();
    Navigator.pop(
      context,
      ScriptDraftValue(
        name: _name.text.trim(),
        type: _type,
        // Value-less entries keep the declared size from the file.
        size: _editableValue ? null : widget.initial.size,
        value: _editableValue ? Uint8List.fromList(_value) : null,
        spec: spec,
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final styles = uiStylesForType(_type);
    final showLimits = _isInput && uiStyleSupportsLimits(_uiType);
    return AlertDialog(
      scrollable: true,
      title: Text('${widget.category.label} ${widget.index}'),
      content: DialogBody(
        maxWidth: 380,
        child: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
          TextField(
            controller: _name,
            decoration: const InputDecoration(labelText: 'Name'),
          ),
          const SizedBox(height: 8),
          DropdownButtonFormField<DataType>(
            key: ValueKey('type-${_type.name}'),
            initialValue: _type,
            decoration: InputDecoration(
                labelText: 'Type', helperText: 'Size: ${defaultSizeForType(_type)} bytes'),
            items: [
              // Guard against a value type the picker no longer offers (a handler reading an
              // unknown type would otherwise assert inside DropdownButtonFormField).
              if (!scriptValueTypes.contains(_type))
                DropdownMenuItem(value: _type, child: Text(dataTypeLabel(_type))),
              for (final t in scriptValueTypes)
                DropdownMenuItem(value: t, child: Text(dataTypeLabel(t))),
            ],
            onChanged: _changeType,
          ),
          if (_isInput) ...[
            const SizedBox(height: 8),
            DropdownButtonFormField<int>(
              initialValue: _uiType,
              decoration: const InputDecoration(labelText: 'UI style'),
              items: [
                if (!styles.contains(_uiType))
                  DropdownMenuItem(value: _uiType, child: Text(ScriptUiType.label(_uiType))),
                for (final t in styles)
                  DropdownMenuItem(value: t, child: Text(ScriptUiType.label(t))),
              ],
              onChanged: (t) => setState(() => _uiType = t ?? ScriptUiType.auto),
            ),
          ],
          if (showLimits) ...[
            const SizedBox(height: 8),
            Row(children: [
              Expanded(child: TextField(controller: _min, decoration: const InputDecoration(labelText: 'Min'))),
              const SizedBox(width: 8),
              Expanded(child: TextField(controller: _max, decoration: const InputDecoration(labelText: 'Max'))),
              const SizedBox(width: 8),
              Expanded(child: TextField(controller: _step, decoration: const InputDecoration(labelText: 'Step'))),
            ]),
          ],
          if (_editableValue) ...[
            const SizedBox(height: 12),
            Row(children: [
              Text('Value: ${formatValue(_type, _value)}',
                  style: const TextStyle(fontFamily: 'monospace')),
              const Spacer(),
              TextButton(
                onPressed: () async {
                  // BlockInfo uses the tiered block/field/key picker (device-aware).
                  final next = _type == DataType.blockInfo
                      ? await pickBlockInfo(context, widget.deviceId, current: _value)
                      : await showValueEditor(context, _type, _value);
                  if (next == null || !mounted) return;
                  setState(() => _value = next);
                },
                child: const Text('Edit value'),
              ),
            ]),
          ],
        ]),
      ),
      actions: [
        TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(onPressed: _submit, child: const Text('OK')),
      ],
    );
  }
}
