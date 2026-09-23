/// "New script" dialog (Docs/App/Service views/Script.md): defines a fresh `SCR_XX` file's
/// function name, properties, and its input/output/variable/constant skeleton. The file is
/// only written to storage - it is never loaded automatically.
library;

import 'package:flutter/material.dart';

import '../core/script_file.dart';
import '../core/types.dart';
import 'theme.dart';
import 'widgets.dart';

/// The result of the create-script dialog.
class NewScriptResult {
  final String name;
  final int slot;
  final int properties;
  final List<ScriptValueInfo> inputs;
  final List<ScriptValueInfo> outputs;
  final List<ScriptValueInfo> variables;
  final List<ScriptValueInfo> constants;

  const NewScriptResult({
    required this.name,
    required this.slot,
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
  return showDialog<NewScriptResult>(
    context: context,
    builder: (_) => _NewScriptDialog(freeSlots: freeSlots),
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
  late int _slot = widget.freeSlots.first;
  bool _loadOnBoot = false;
  bool _runOnLoad = false;

  final _inputs = <_EntryDraft>[];
  final _outputs = <_EntryDraft>[];
  final _variables = <_EntryDraft>[];
  final _constants = <_EntryDraft>[];

  @override
  void dispose() {
    _nameController.dispose();
    super.dispose();
  }

  String _slotName(int id) =>
      'SCR_${id.toRadixString(16).toUpperCase().padLeft(2, '0')}';

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

  List<ScriptValueInfo> _collect(List<_EntryDraft> drafts) =>
      [for (final e in drafts) ScriptValueInfo(type: e.type, size: defaultSizeForType(e.type))];

  void _submit() {
    var properties = 0;
    if (_loadOnBoot) properties |= ScriptProperties.loadOnBoot;
    if (_runOnLoad) properties |= ScriptProperties.runOnLoad;
    Navigator.pop(
      context,
      NewScriptResult(
        name: _nameController.text.trim(),
        slot: _slot,
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
    return AlertDialog(
      scrollable: true,
      title: const Text('New script'),
      content: DialogBody(
        maxWidth: 420,
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            TextField(
              controller: _nameController,
              autofocus: true,
              maxLength: 23,
              decoration: const InputDecoration(
                  labelText: 'Function name',
                  helperText: 'Shown in the app and on the script block'),
            ),
            const SizedBox(height: 8),
            DropdownButtonFormField<int>(
              initialValue: _slot,
              decoration: const InputDecoration(labelText: 'File slot'),
              items: [
                for (final id in widget.freeSlots)
                  DropdownMenuItem(value: id, child: Text(_slotName(id))),
              ],
              onChanged: (v) => setState(() => _slot = v ?? _slot),
            ),
            const SizedBox(height: 4),
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
            const Divider(),
            _section('Inputs', _inputs),
            _section('Outputs', _outputs),
            _section('Variables', _variables),
            _section('Constants', _constants),
            const SizedBox(height: 8),
            const Text(
              'The script is written to storage only and is not loaded.',
              style: TextStyle(color: Colors.white54, fontSize: 12),
            ),
          ],
        ),
      ),
      actions: [
        TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(onPressed: _submit, child: const Text('Create')),
      ],
    );
  }

  Widget _section(String title, List<_EntryDraft> list) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Padding(
          padding: const EdgeInsets.only(top: 8),
          child: Row(children: [
            Text(title, style: const TextStyle(color: kOrange, fontWeight: FontWeight.w600)),
            const Spacer(),
            IconButton(
              visualDensity: VisualDensity.compact,
              icon: const Icon(Icons.add, size: 18),
              tooltip: 'Add $title',
              onPressed: () => _addEntry(list, list.length),
            ),
          ]),
        ),
        for (var i = 0; i < list.length; i++) _entryRow(i, list),
      ],
    );
  }

  Widget _entryRow(int index, List<_EntryDraft> list) {
    final e = list[index];
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 2),
      child: Row(children: [
        const SizedBox(width: 24, child: Text('#', style: TextStyle(color: Colors.white38))),
        Text('$index', style: const TextStyle(color: Colors.white54)),
        const SizedBox(width: 8),
        Expanded(
          child: DropdownButtonFormField<DataType>(
            initialValue: e.type,
            isDense: true,
            decoration: const InputDecoration(labelText: 'Type'),
            items: [
              for (final t in scriptValueTypes)
                DropdownMenuItem(value: t, child: Text(_typeLabel(t))),
            ],
            onChanged: (t) => _changeType(e, t),
          ),
        ),
        const SizedBox(width: 8),
        Text('${defaultSizeForType(e.type)}B',
            style: const TextStyle(color: Colors.white38, fontSize: 12)),
        IconButton(
          visualDensity: VisualDensity.compact,
          icon: const Icon(Icons.remove_circle_outline, size: 18, color: Colors.redAccent),
          onPressed: () => _removeEntry(list, index),
        ),
      ]),
    );
  }

  String _typeLabel(DataType t) => switch (t) {
        DataType.bool_ => 'Bool',
        DataType.integer => 'Index',
        DataType.number => 'Number',
        DataType.enum_ => 'Enum',
        DataType.colour => 'Colour',
        DataType.vector => 'Vector',
        DataType.matrix => 'Matrix',
        DataType.string => 'String',
        DataType.filename => 'Filename',
        DataType.uint32 => 'Uint32',
        _ => t.name,
      };
}
