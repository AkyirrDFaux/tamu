/// Script value editor (Docs/App/Service views/Script.md): name/type/UI-style/limits and,
/// for inputs and constants, the value bytes.
///
/// A caller of the one value-editing entry point: the value bytes are edited
/// through [showValueEditor] (the same [showTamuDialog]/[PickerShell] shell and
/// the same editors the register and script-entry callers use), and the dialog's
/// own chrome is the shared [TamuDialog]. Nothing here hand-rolls a dialog or a
/// field style.
library;

import 'dart:typed_data';

import 'package:flutter/material.dart';

import '../core/script_draft.dart';
import '../core/script_file.dart';
import '../core/types.dart';
import 'dialogs.dart';
import 'formatters.dart';
import 'theme.dart';
import 'value_editor.dart';

class ScriptValueDialog extends StatefulWidget {
  final ScriptValueCategory category;
  final ScriptDraftValue initial;
  final int index;
  final int deviceId;

  const ScriptValueDialog({
    super.key,
    required this.category,
    required this.initial,
    required this.index,
    required this.deviceId,
  });

  @override
  State<ScriptValueDialog> createState() => _ScriptValueDialogState();
}

class _ScriptValueDialogState extends State<ScriptValueDialog> {
  late final TextEditingController _name = TextEditingController(
    text: widget.initial.name,
  );
  late final TextEditingController _min = TextEditingController(
    text: _trim(widget.initial.spec.min),
  );
  late final TextEditingController _max = TextEditingController(
    text: _trim(widget.initial.spec.max),
  );
  late final TextEditingController _step = TextEditingController(
    text: _trim(widget.initial.spec.step),
  );

  /// The edited value. The type-change rules live in [ScriptDraftValue.setType].
  late final ScriptDraftValue _draft = ScriptDraftValue(
    name: widget.initial.name,
    type: widget.initial.type,
    size: widget.initial.size,
    value: widget.initial.value,
    spec: widget.initial.spec,
  );

  /// Custom enum option labels (the values stay the plain 0..N-1 indexes).
  late final List<TextEditingController> _options = [
    for (final o in widget.initial.spec.options) TextEditingController(text: o),
  ];

  bool get _isInput => widget.category == ScriptValueCategory.input;
  bool get _isConstant => widget.category == ScriptValueCategory.constant;

  /// Inputs carry a default and constants carry a value; outputs/variables have none.
  bool get _editableValue => _isInput || _isConstant;

  static String _trim(double v) =>
      v == v.roundToDouble() ? '${v.toInt()}' : '$v';

  @override
  void initState() {
    super.initState();
    // Value-bearing entries always present bytes; seed zeroed defaults when the file had none.
    if (_editableValue && _draft.value.isEmpty) {
      _draft.setValue(
        Uint8List.fromList(
          List<int>.filled(defaultSizeForType(_draft.type), 0),
        ),
      );
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
        _draft.setValue(
          Uint8List.fromList(List<int>.filled(defaultSizeForType(t), 0)),
        );
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

  /// Opens the one value-editing entry point for the current bytes. The device
  /// id lets a BlockInfo operand use the device-aware register picker, exactly as
  /// the register and script-entry callers do through the same entry point.
  Future<void> _editValue() async {
    final next = await showValueEditor(
      context,
      _draft.type,
      _draft.value,
      deviceId: widget.deviceId,
    );
    if (next == null || !mounted) return;
    setState(() => _draft.setValue(Uint8List.fromList(next)));
  }

  @override
  Widget build(BuildContext context) {
    final styles = Theme.of(context).textTheme;
    final uiStyles = uiStylesForType(_draft.type);
    final showLimits = _isInput && uiStyleSupportsLimits(_draft.spec.uiType);
    return TamuDialog(
      title: '${widget.category.label} ${widget.index}',
      maxWidth: 380,
      body: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          TextField(
            controller: _name,
            decoration: const InputDecoration(labelText: 'Name'),
          ),
          const SizedBox(height: Spacing.md),
          DropdownButtonFormField<DataType>(
            key: ValueKey('type-${_draft.type.name}'),
            initialValue: _draft.type,
            decoration: InputDecoration(
              labelText: 'Type',
              helperText: 'Size: ${defaultSizeForType(_draft.type)} bytes',
            ),
            items: [
              // Guard against a value type the picker no longer offers (a handler reading an
              // unknown type would otherwise assert inside DropdownButtonFormField).
              if (!scriptValueTypes.contains(_draft.type))
                DropdownMenuItem(
                  value: _draft.type,
                  child: Text(dataTypeLabel(_draft.type)),
                ),
              for (final t in scriptValueTypes)
                DropdownMenuItem(value: t, child: Text(dataTypeLabel(t))),
            ],
            onChanged: _changeType,
          ),
          if (_isInput) ...[
            const SizedBox(height: Spacing.md),
            DropdownButtonFormField<int>(
              initialValue: _draft.spec.uiType,
              decoration: const InputDecoration(labelText: 'UI style'),
              items: [
                if (!uiStyles.contains(_draft.spec.uiType))
                  DropdownMenuItem(
                    value: _draft.spec.uiType,
                    child: Text(ScriptUiType.label(_draft.spec.uiType)),
                  ),
                for (final t in uiStyles)
                  DropdownMenuItem(
                    value: t,
                    child: Text(ScriptUiType.label(t)),
                  ),
              ],
              onChanged: (t) => setState(
                () => _draft.spec = _draft.spec.copyWith(
                  uiType: t ?? ScriptUiType.auto,
                ),
              ),
            ),
            // Custom enum: name the values (the stored value stays the 0..N-1 index).
            if (_draft.type == DataType.enum_) ...[
              const SizedBox(height: Spacing.md),
              Row(
                children: [
                  Text('Enum values', style: styles.labelSmall),
                  const Spacer(),
                  TextButton(
                    onPressed: () =>
                        setState(() => _options.add(TextEditingController())),
                    child: const Text('Add value'),
                  ),
                ],
              ),
              for (var i = 0; i < _options.length; i++)
                Padding(
                  padding: const EdgeInsets.only(bottom: Spacing.sm),
                  child: Row(
                    children: [
                      SizedBox(
                        width: 26,
                        child: Text(
                          '$i',
                          style: styles.labelSmall?.copyWith(color: kTextMuted),
                        ),
                      ),
                      Expanded(
                        child: TextField(
                          controller: _options[i],
                          decoration: InputDecoration(
                            labelText: 'Value $i name',
                          ),
                        ),
                      ),
                      IconButton(
                        icon: const Icon(Icons.close, size: IconSize.md),
                        tooltip: 'Remove',
                        onPressed: () =>
                            setState(() => _options.removeAt(i).dispose()),
                      ),
                    ],
                  ),
                ),
            ],
          ],
          if (showLimits) ...[
            const SizedBox(height: Spacing.md),
            Row(
              children: [
                Expanded(
                  child: TextField(
                    controller: _min,
                    decoration: const InputDecoration(labelText: 'Min'),
                  ),
                ),
                const SizedBox(width: Spacing.md),
                Expanded(
                  child: TextField(
                    controller: _max,
                    decoration: const InputDecoration(labelText: 'Max'),
                  ),
                ),
                const SizedBox(width: Spacing.md),
                Expanded(
                  child: TextField(
                    controller: _step,
                    decoration: const InputDecoration(labelText: 'Step'),
                  ),
                ),
              ],
            ),
          ],
          if (_editableValue) ...[
            const SizedBox(height: Spacing.lg),
            Row(
              children: [
                Text(
                  'Value: ${formatValue(_draft.type, _draft.value)}',
                  style: const TextStyle(fontFamily: 'monospace'),
                ),
                const Spacer(),
                TextButton(
                  onPressed: _editValue,
                  child: const Text('Edit value'),
                ),
              ],
            ),
          ],
        ],
      ),
      actions: tamuDialogActions(
        onCancel: () => Navigator.pop(context),
        onConfirm: _submit,
      ),
    );
  }
}
