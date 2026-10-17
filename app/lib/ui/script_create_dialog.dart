/// "New script" dialog (Docs/App/Service views/Script.md): defines a fresh `SCR_XXX` file's
/// function name, properties, and its input/output/variable/constant skeleton. The file is
/// only written to storage - it is never loaded automatically.
library;

import 'package:flutter/material.dart';

import '../core/script_file.dart';
import '../core/types.dart';
import 'dialogs.dart';
import 'theme.dart';
import 'script_widgets.dart';

/// The result of the create-script dialog.
class NewScriptResult {
  final String name;
  final int fileId;
  final int properties;
  final List<ValueInfo> inputs;
  final List<ValueInfo> outputs;
  final List<ValueInfo> variables;
  final List<ValueInfo> constants;

  const NewScriptResult({
    required this.name,
    required this.fileId,
    required this.properties,
    required this.inputs,
    required this.outputs,
    required this.variables,
    required this.constants,
  });
}

Future<NewScriptResult?> showNewScriptDialog(
  BuildContext context, {
  required List<int> freeSlots,
}) {
  return showTamuPicker<NewScriptResult>(
    context,
    _NewScriptDialog(freeSlots: freeSlots),
  );
}

class _EntryDraft {
  DataType type;
  _EntryDraft(this.type);
}

class _NewScriptDialog extends StatefulWidget {
  final List<int> freeSlots;

  const _NewScriptDialog({required this.freeSlots});

  @override
  State<_NewScriptDialog> createState() => _NewScriptDialogState();
}

class _NewScriptDialogState extends State<_NewScriptDialog> {
  final _nameController = TextEditingController();
  final _idController = TextEditingController();
  final _slotController = TextEditingController(text: '0');
  bool _loadOnBoot = false;
  bool _runOnLoad = false;
  bool _functionLibrary = false;

  final _inputs = <_EntryDraft>[];
  final _outputs = <_EntryDraft>[];
  final _variables = <_EntryDraft>[];
  final _constants = <_EntryDraft>[];

  @override
  void initState() {
    super.initState();
    // Default to the lowest free file id; the id space is SCR_XXX (4096), so a text field
    // beats a dropdown.
    _idController.text = widget.freeSlots.first
        .toRadixString(16)
        .toUpperCase()
        .padLeft(3, '0');
  }

  @override
  void dispose() {
    _nameController.dispose();
    _idController.dispose();
    _slotController.dispose();
    super.dispose();
  }

  /// The file id currently typed, or null if it is not a free, valid id.
  int? get _fileId {
    final v = int.tryParse(_idController.text.trim(), radix: 16);
    if (v == null || v < 0 || v >= maxScriptFiles) return null;
    return widget.freeSlots.contains(v) ? v : null;
  }

  void _addEntry(List<_EntryDraft> list, int index) {
    setState(() => list.insert(index, _EntryDraft(DataType.number)));
  }

  void _removeEntry(List<_EntryDraft> list, int index) {
    setState(() => list.removeAt(index));
  }

  void _changeType(_EntryDraft e, DataType? type) {
    if (type == null) return;
    setState(() => e.type = type);
  }

  List<ValueInfo> _collect(List<_EntryDraft> drafts) => [
    for (final e in drafts)
      ValueInfo(type: e.type.value, size: defaultSizeForType(e.type)),
  ];

  void _submit() {
    final id = _fileId;
    if (id == null) return;
    var properties = 0;
    if (_loadOnBoot) properties |= ScriptProperties.loadOnBoot;
    if (_runOnLoad) properties |= ScriptProperties.runOnLoad;
    if (_functionLibrary) properties |= ScriptProperties.functionLibrary;
    if (_loadOnBoot) {
      final slot = int.tryParse(_slotController.text.trim()) ?? 0;
      properties = scriptPropertiesWithSlot(properties, slot.clamp(0, 255));
    }
    Navigator.pop(
      context,
      NewScriptResult(
        name: _nameController.text.trim(),
        fileId: id,
        properties: properties,
        inputs: _collect(_inputs),
        outputs: _collect(_outputs),
        variables: _collect(_variables),
        constants: _collect(_constants),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return TamuDialog(
      title: 'New script',
      maxWidth: 420,
      body: SingleChildScrollView(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            TextField(
              controller: _nameController,
              autofocus: true,
              maxLength: 23,
              decoration: const InputDecoration(
                labelText: 'Function name',
                helperText: 'Shown in the app and on the script block',
              ),
            ),
            const SizedBox(height: Spacing.md),
            TextField(
              controller: _idController,
              decoration: InputDecoration(
                labelText: 'File id (hex)',
                errorText: _fileId == null
                    ? 'Not a free id'
                    : 'File ${scriptFileName(_fileId!)}',
              ),
              onChanged: (_) => setState(() {}),
            ),
            const SizedBox(height: Spacing.xs),
            SwitchListTile(
              dense: true,
              contentPadding: EdgeInsets.zero,
              title: const Text('Load on boot'),
              value: _loadOnBoot,
              onChanged: (v) => setState(() => _loadOnBoot = v),
            ),
            SwitchListTile(
              dense: true,
              contentPadding: EdgeInsets.zero,
              title: const Text('Run on load'),
              value: _runOnLoad,
              onChanged: (v) => setState(() => _runOnLoad = v),
            ),
            FunctionLibrarySwitch(
              value: _functionLibrary,
              onChanged: (v) => setState(() => _functionLibrary = v),
            ),
            if (_loadOnBoot)
              TextField(
                controller: _slotController,
                keyboardType: TextInputType.number,
                decoration: const InputDecoration(
                  labelText: 'Boot slot (0-255)',
                  helperText: 'The slot a load-on-boot script loads into',
                ),
              ),
            const Divider(),
            _section('Inputs', _inputs),
            _section('Outputs', _outputs),
            _section('Variables', _variables),
            _section('Constants', _constants),
            const SizedBox(height: Spacing.md),
            Text(
              'The script is written to storage only and is not loaded.',
              style: theme.textTheme.bodySmall,
            ),
          ],
        ),
      ),
      actions: tamuDialogActions(
        onCancel: () => Navigator.pop(context),
        onConfirm: _fileId == null ? null : _submit,
        confirmLabel: 'Create',
      ),
    );
  }

  Widget _section(String title, List<_EntryDraft> list) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Padding(
          padding: const EdgeInsets.only(top: Spacing.md),
          child: ScriptCardHeading(
            title,
            action: IconButton(
              visualDensity: VisualDensity.compact,
              icon: const Icon(Icons.add, size: IconSize.md),
              tooltip: 'Add $title',
              onPressed: () => _addEntry(list, list.length),
            ),
          ),
        ),
        for (var i = 0; i < list.length; i++) _entryRow(i, list),
      ],
    );
  }

  Widget _entryRow(int index, List<_EntryDraft> list) {
    final e = list[index];
    final theme = Theme.of(context);
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: Spacing.hairline),
      child: Row(
        children: [
          Text(
            '#$index',
            style: theme.textTheme.labelSmall?.copyWith(color: kTextMuted),
          ),
          const SizedBox(width: Spacing.md),
          Expanded(
            child: DropdownButtonFormField<DataType>(
              initialValue: e.type,
              isDense: true,
              decoration: const InputDecoration(labelText: 'Type'),
              items: [
                for (final t in scriptValueTypes)
                  DropdownMenuItem(value: t, child: Text(dataTypeWord(t))),
              ],
              onChanged: (t) => _changeType(e, t),
            ),
          ),
          const SizedBox(width: Spacing.md),
          Text(
            '${defaultSizeForType(e.type)}B',
            style: theme.textTheme.labelSmall?.copyWith(color: kTextDisabled),
          ),
          IconButton(
            visualDensity: VisualDensity.compact,
            icon: const Icon(
              Icons.remove_circle_outline,
              size: IconSize.md,
              color: kError,
            ),
            onPressed: () => _removeEntry(list, index),
          ),
        ],
      ),
    );
  }
}
