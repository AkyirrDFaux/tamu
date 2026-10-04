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
  /// The edited value. The type-change rules live in [ScriptDraftValue.setType].
  late final ScriptDraftValue _draft = ScriptDraftValue(
    name: widget.initial.name,
    type: widget.initial.type,
    size: widget.initial.size,
    value: widget.initial.value,
    spec: widget.initial.spec,
  );
  /// Custom enum option labels (the values stay the plain 0..N-1 indexes).
  late final List<TextEditingController> _options =
      [for (final o in widget.initial.spec.options) TextEditingController(text: o)];

  bool get _isInput => widget.category == ScriptValueCategory.input;
  bool get _isConstant => widget.category == ScriptValueCategory.constant;

  /// Inputs carry a default and constants carry a value; outputs/variables have none.
  bool get _editableValue => _isInput || _isConstant;

  static String _trim(double v) => v == v.roundToDouble() ? '${v.toInt()}' : '$v';

  @override
  void initState() {
    super.initState();
    // Value-bearing entries always present bytes; seed zeroed defaults when the file had none.
    if (_editableValue && _draft.value.isEmpty) {
      _draft.setValue(Uint8List.fromList(List<int>.filled(defaultSizeForType(_draft.type), 0)));
    }
  }

  @override
  void dispose() {
    _name.dispose();
    for (final c in _options) {
      c.dispose();
    }
    _min.dispose();
    _max.dispose();
    _step.dispose();
    super.dispose();
  }

  void _changeType(DataType? t) {
    if (t == null) return;
    setState(() {
      // [ScriptDraftValue.setType] owns the type-change rules (size + UI-style reset).
      _draft.setType(t);
      if (_editableValue) {
        _draft.setValue(Uint8List.fromList(List<int>.filled(defaultSizeForType(t), 0)));
      }
    });
  }

  void _submit() {
    final spec = _isInput
        ? ScriptInputSpec(
            uiType: _draft.spec.uiType,
            min: double.tryParse(_min.text.trim()) ?? 0,
            max: double.tryParse(_max.text.trim()) ?? 0,
            step: double.tryParse(_step.text.trim()) ?? 0,
            options: [
              for (final c in _options)
                if (c.text.trim().isNotEmpty) c.text.trim(),
            ],
          )
        : const ScriptInputSpec();
    Navigator.pop(
      context,
      ScriptDraftValue(
        name: _name.text.trim(),
        type: _draft.type,
        // Value-less entries keep the declared size from the file.
        size: _editableValue ? null : widget.initial.size,
        value: _editableValue ? Uint8List.fromList(_draft.value) : null,
        spec: spec,
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final styles = uiStylesForType(_draft.type);
    final showLimits = _isInput && uiStyleSupportsLimits(_draft.spec.uiType);
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
            key: ValueKey('type-${_draft.type.name}'),
            initialValue: _draft.type,
            decoration: InputDecoration(
                labelText: 'Type', helperText: 'Size: ${defaultSizeForType(_draft.type)} bytes'),
            items: [
              // Guard against a value type the picker no longer offers (a handler reading an
              // unknown type would otherwise assert inside DropdownButtonFormField).
              if (!scriptValueTypes.contains(_draft.type))
                DropdownMenuItem(value: _draft.type, child: Text(dataTypeLabel(_draft.type))),
              for (final t in scriptValueTypes)
                DropdownMenuItem(value: t, child: Text(dataTypeLabel(t))),
            ],
            onChanged: _changeType,
          ),
          if (_isInput) ...[
            const SizedBox(height: 8),
            DropdownButtonFormField<int>(
              initialValue: _draft.spec.uiType,
              decoration: const InputDecoration(labelText: 'UI style'),
              items: [
                if (!styles.contains(_draft.spec.uiType))
                  DropdownMenuItem(
                      value: _draft.spec.uiType, child: Text(ScriptUiType.label(_draft.spec.uiType))),
                for (final t in styles)
                  DropdownMenuItem(value: t, child: Text(ScriptUiType.label(t))),
              ],
              onChanged: (t) => setState(() =>
                  _draft.spec = _draft.spec.copyWith(uiType: t ?? ScriptUiType.auto)),
            ),
            // Custom enum: name the values (the stored value stays the 0..N-1 index).
            if (_draft.type == DataType.enum_) ...[
              const SizedBox(height: 10),
              Row(children: [
                const Text('Enum values',
                    style: TextStyle(fontSize: 12, color: Colors.white70)),
                const Spacer(),
                TextButton(
                  onPressed: () => setState(() => _options.add(TextEditingController())),
                  child: const Text('Add value'),
                ),
              ]),
              for (var i = 0; i < _options.length; i++)
                Padding(
                  padding: const EdgeInsets.only(bottom: 6),
                  child: Row(children: [
                    SizedBox(
                        width: 26,
                        child: Text('$i',
                            style: const TextStyle(color: Colors.white54, fontSize: 12))),
                    Expanded(
                      child: TextField(
                        controller: _options[i],
                        decoration: InputDecoration(
                            labelText: 'Value $i name', isDense: true),
                      ),
                    ),
                    IconButton(
                      icon: const Icon(Icons.close, size: 18),
                      tooltip: 'Remove',
                      onPressed: () => setState(() => _options.removeAt(i).dispose()),
                    ),
                  ]),
                ),
            ],
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
              Text('Value: ${formatValue(_draft.type, _draft.value)}',
                  style: const TextStyle(fontFamily: 'monospace')),
              const Spacer(),
              TextButton(
                onPressed: () async {
                  // BlockInfo uses the tiered block/field/key picker (device-aware).
                  final next = _draft.type == DataType.blockInfo
                      ? await pickBlockInfo(context, widget.deviceId, current: _draft.value)
                      : await showValueEditor(context, _draft.type, _draft.value);
                  if (next == null || !mounted) return;
                  setState(() => _draft.setValue(Uint8List.fromList(next)));
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
