/// Script editor (Docs/App/Service views/Script.md).
///
/// Works on the stored `SCR_XX` file, so both loaded and available scripts can be edited.
/// Covers: controls/state (live), inputs (type/style/limits/default), outputs
/// (type/name + live values), variables (add/remove, type, live values), constants
/// (value + name), and instructions (per-line/per-symbol editing with context
/// recommendations and a validity check). The appbar uploads the edited file and, for a
/// loaded script, applies it live by reloading.
library;

import 'dart:typed_data';

import 'package:flutter/material.dart';

import '../core/script_client.dart';
import '../core/script_draft.dart';
import '../core/script_file.dart';
import '../core/script_instructions.dart';
import '../core/storage_client.dart';
import '../core/types.dart';
import 'block_info_picker.dart';
import 'script_widgets.dart';
import 'theme.dart';
import 'value_editor.dart';
import 'widgets.dart';

enum _Category { input, output, variable, constant }

class ScriptEditorPage extends StatefulWidget {
  final int deviceId;
  final int fileId;
  final String name;
  final bool loaded;

  const ScriptEditorPage({
    super.key,
    required this.deviceId,
    required this.fileId,
    required this.name,
    required this.loaded,
  });

  @override
  State<ScriptEditorPage> createState() => _ScriptEditorPageState();
}

class _ScriptEditorPageState extends State<ScriptEditorPage>
    with AutoRefreshMixin<ScriptEditorPage> {
  late final ScriptClient _client = ScriptClient(deviceId: widget.deviceId);
  late final StorageClient _storage = StorageClient(deviceId: widget.deviceId);

  ScriptDraft? _draft;
  bool _loading = true;
  String? _loadError;
  bool _busy = false;
  bool _dirty = false;
  List<String>? _errors;
  int _state = ScriptState.stopped;
  int _instructionCounter = 0;
  int _errorCode = 0;

  final _statusText = <_Category, List<String>>{};

  String get _fileName =>
      'SCR_${widget.fileId.toRadixString(16).toUpperCase().padLeft(2, '0')}';

  @override
  void initState() {
    super.initState();
    _load();
  }

  @override
  Future<void> onAutoRefresh() => _refresh();

  @override
  void onAutoRefreshStarted() => _refresh();

  Future<void> _load() async {
    setState(() {
      _loading = true;
      _loadError = null;
    });
    try {
      final bytes = await _storage.readFile(_fileName);
      final draft = bytes == null
          ? ScriptDraft(functionName: widget.name)
          : ScriptDraft.fromFile(ScriptFileData.parse(bytes));
      if (!mounted) return;
      setState(() {
        _draft = draft;
        _loading = false;
      });
      await _refresh();
    } catch (e) {
      // A device read can fail/time out (busy bus) or the file can be malformed: show a
      // retry instead of an endless spinner.
      if (!mounted) return;
      setState(() {
        _loading = false;
        _loadError = '$e';
      });
    }
  }

  Widget _loadErrorBody() => Center(
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Column(mainAxisSize: MainAxisSize.min, children: [
            const Icon(Icons.error_outline, color: Colors.redAccent, size: 36),
            const SizedBox(height: 8),
            Text('Could not load $_fileName',
                style: const TextStyle(fontWeight: FontWeight.w600)),
            const SizedBox(height: 4),
            Text(_loadError ?? '',
                style: const TextStyle(color: Colors.white54, fontSize: 12),
                textAlign: TextAlign.center),
            const SizedBox(height: 12),
            FilledButton.icon(
                onPressed: _load, icon: const Icon(Icons.refresh), label: const Text('Retry')),
          ]),
        ),
      );

  Future<void> _refresh() async {
    final draft = _draft;
    if (widget.loaded) {
      final state = await _client.readState(widget.fileId);
      final internal = await _client.readInternalState(widget.fileId);
      final error = await _client.readError(widget.fileId);
      final values = <_Category, List<String>>{};

      // I/O live values come from the Register (the only script content it exposes).
      for (final cat in const [_Category.input, _Category.output]) {
        final keys = await _client.enumerateKeys(widget.fileId, _fieldOf(cat));
        final list = <String>[];
        for (final key in keys) {
          final e = await _client.readEntry(widget.fileId, _fieldOf(cat), key);
          list.add(e == null ? '-' : formatValue(e.meta.dataType, e.value));
        }
        values[cat] = list;
      }

      // Variables live in the script RAM (CID 5); constants are file data.
      if (draft != null) {
        final ram = internal?.variables ?? const <int>[];
        final varList = <String>[];
        var off = 0;
        for (final v in draft.variables) {
          final size = v.size;
          final end = off + size;
          varList.add(end <= ram.length ? formatValue(v.type, ram.sublist(off, end)) : '-');
          off += (size + 3) & ~3;
        }
        values[_Category.variable] = varList;
        values[_Category.constant] =
            [for (final c in draft.constants) formatValue(c.type, c.value)];
      }

      if (!mounted) return;
      setState(() {
        _state = state ?? ScriptState.stopped;
        _instructionCounter = internal?.instructionCounter ?? 0;
        _errorCode = error ?? 0;
        _statusText..clear()..addAll(values);
      });
    } else {
      if (!mounted) return;
      setState(() {
        _statusText.clear();
      });
    }
  }

  static int _fieldOf(_Category c) => switch (c) {
        _Category.input => ScriptField.input,
        _Category.output => ScriptField.output,
        _Category.variable => ScriptField.variable,
        _Category.constant => ScriptField.constant,
      };

  static List<ScriptDraftValue> _listOf(ScriptDraft d, _Category c) => switch (c) {
        _Category.input => d.inputs,
        _Category.output => d.outputs,
        _Category.variable => d.variables,
        _Category.constant => d.constants,
      };

  void _touch() => setState(() => _dirty = true);

  // ---------------------------------------------------------------------------
  // Persistence / validity
  // ---------------------------------------------------------------------------

  Future<void> _save() async {
    final draft = _draft;
    if (draft == null) return;
    setState(() => _busy = true);
    final ok = await _storage.writeFile(_fileName, draft.toImage());
    if (!mounted) return;
    setState(() {
      _busy = false;
      if (ok) _dirty = false;
    });
    showSnack(context, ok ? 'Uploaded $_fileName' : 'Upload failed');
    if (ok && widget.loaded && mounted) {
      final reload = await confirmDialog(context,
          title: 'Apply live',
          body: 'Reload the script to apply the changes on the device?',
          confirmLabel: 'Reload');
      if (reload) await _reloadLive();
    }
  }

  Future<void> _reloadLive() async {
    await _client.unload(widget.fileId);
    final id = await _client.load(widget.fileId);
    if (!mounted) return;
    showSnack(context, id == null ? 'Reload failed' : 'Reloaded');
    await _refresh();
  }

  Future<void> _unloadScript() async {
    final ok = await _client.unload(widget.fileId);
    if (!mounted) return;
    showSnack(context, ok ? 'Unloaded' : 'Unload failed');
    if (ok) Navigator.of(context).pop();
  }

  void _check() {
    final draft = _draft;
    if (draft == null) return;
    final errors = validateScriptLines(draft.lines, draft.validationContext);
    setState(() => _errors = errors);
    showSnack(context, errors.isEmpty ? 'Script is valid' : '${errors.length} problem(s) found');
  }

  Future<void> _control(int state, {bool reset = false}) async {
    if (reset) await _client.moveToInstruction(widget.fileId, 0);
    final ok = await _client.setState(widget.fileId, state);
    if (!mounted) return;
    showSnack(context, ok ? ScriptState.label(state) : 'Command failed');
    await _refresh();
  }

  // ---------------------------------------------------------------------------
  // Build
  // ---------------------------------------------------------------------------

  @override
  Widget build(BuildContext context) {
    final draft = _draft;
    return Scaffold(
      appBar: AppBar(
        title: Text('Edit ${draft?.functionName.isNotEmpty == true ? draft!.functionName : widget.name}'),
        actions: [
          IconButton(
            tooltip: 'Check validity',
            icon: const Icon(Icons.rule),
            onPressed: draft == null ? null : _check,
          ),
          IconButton(
            tooltip: 'Upload (write the file)',
            icon: const Icon(Icons.upload_file),
            onPressed: draft == null || _busy ? null : _save,
          ),
          if (widget.loaded)
            IconButton(
              tooltip: 'Update (reload live)',
              icon: const Icon(Icons.sync),
              onPressed: _busy ? null : _reloadLive,
            ),
          if (widget.loaded)
            IconButton(
              tooltip: 'Unload',
              icon: const Icon(Icons.eject),
              onPressed: _busy ? null : _unloadScript,
            ),
          RefreshButton(
            onRefresh: _refresh,
            autoActive: autoRefreshActive,
            refreshing: false,
            error: false,
            selectedInterval: selectedInterval,
            onSelectAuto: applyAuto,
          ),
        ],
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : _loadError != null
              ? _loadErrorBody()
              : draft == null
                  ? const Center(child: CircularProgressIndicator())
                  : ListView(
              padding: const EdgeInsets.only(bottom: 24),
              children: [
                if (widget.loaded) _controlsCard(),
                if (!widget.loaded) _storedNotice(),
                _functionCard(draft),
                _valueSection(draft, _Category.input),
                _valueSection(draft, _Category.output),
                _valueSection(draft, _Category.variable),
                _valueSection(draft, _Category.constant),
                _instructionsCard(draft),
                _validityCard(),
              ],
            ),
      floatingActionButton: _dirty
          ? FloatingActionButton.extended(
              onPressed: _busy ? null : _save,
              icon: const Icon(Icons.upload),
              label: const Text('Upload'),
            )
          : null,
    );
  }

  Widget _controlsCard() {
    return Card(
      margin: const EdgeInsets.fromLTRB(8, 8, 8, 4),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Row(children: [
            Icon(Icons.circle, size: 12, color: scriptStateColor(_state)),
            const SizedBox(width: 8),
            Text(ScriptState.label(_state), style: const TextStyle(fontWeight: FontWeight.w600)),
            const Spacer(),
            Text('IC $_instructionCounter', style: const TextStyle(color: Colors.white54)),
          ]),
          if (_errorCode != 0)
            Padding(
              padding: const EdgeInsets.only(top: 6),
              child: Text('Error: ${_errorLabel(_errorCode)}',
                  style: const TextStyle(color: Colors.redAccent)),
            ),
          const SizedBox(height: 10),
          Wrap(spacing: 8, children: [
            OutlinedButton.icon(
                onPressed: () => _control(ScriptState.running),
                icon: const Icon(Icons.play_arrow, size: 18),
                label: const Text('Start')),
            OutlinedButton.icon(
                onPressed: () => _control(
                    _state == ScriptState.paused ? ScriptState.running : ScriptState.paused),
                icon: Icon(_state == ScriptState.paused ? Icons.play_arrow : Icons.pause, size: 18),
                label: Text(_state == ScriptState.paused ? 'Continue' : 'Pause')),
            OutlinedButton.icon(
                onPressed: () => _control(ScriptState.stopped, reset: true),
                icon: const Icon(Icons.stop, size: 18),
                label: const Text('Stop')),
            OutlinedButton.icon(
                onPressed: () => _control(ScriptState.running, reset: true),
                icon: const Icon(Icons.restart_alt, size: 18),
                label: const Text('Restart')),
            OutlinedButton.icon(
                onPressed: _moveToLine,
                icon: const Icon(Icons.alt_route, size: 18),
                label: const Text('Move to line')),
          ]),
        ]),
      ),
    );
  }

  String _errorLabel(int code) => switch (code) {
        0 => 'None',
        1 => 'Unknown opcode',
        2 => 'Type mismatch',
        3 => 'Bad operand',
        4 => 'Out of bounds',
        5 => 'Call stack overflow',
        6 => 'Register access failed',
        7 => 'Confirmation timeout',
        8 => 'Not implemented',
        _ => 'Error $code',
      };

  Future<void> _moveToLine() async {
    final controller = TextEditingController(text: '$_instructionCounter');
    final line = await showDialog<int>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Move to line'),
        content: TextField(
          controller: controller,
          autofocus: true,
          keyboardType: TextInputType.number,
          decoration: const InputDecoration(labelText: 'Line index'),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(
            onPressed: () => Navigator.pop(context, int.tryParse(controller.text.trim())),
            child: const Text('OK'),
          ),
        ],
      ),
    );
    if (line == null || !mounted) return;
    final ok = await _client.moveToInstruction(widget.fileId, line);
    if (!mounted) return;
    showSnack(context, ok ? 'Moved to line $line' : 'Move failed');
    await _refresh();
  }

  Widget _storedNotice() {
    return Card(
      margin: const EdgeInsets.fromLTRB(8, 8, 8, 4),
      child: const ListTile(
        leading: Icon(Icons.info_outline, color: kOrange),
        title: Text('Stored script'),
        subtitle: Text('Not loaded. Edit and upload; load it from the Scripts page to run.'),
      ),
    );
  }

  Widget _functionCard(ScriptDraft draft) {
    return Card(
      margin: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          const Text('Function', style: TextStyle(color: kOrange, fontWeight: FontWeight.w600)),
          const SizedBox(height: 8),
          TextFormField(
            initialValue: draft.functionName,
            maxLength: 23,
            decoration: const InputDecoration(labelText: 'Function name'),
            onChanged: (v) {
              draft.functionName = v;
              _touch();
            },
          ),
          SwitchListTile(
            dense: true,
            contentPadding: EdgeInsets.zero,
            title: const Text('Load on boot'),
            value: draft.properties & ScriptProperties.loadOnBoot != 0,
            onChanged: (v) {
              draft.properties = v
                  ? draft.properties | ScriptProperties.loadOnBoot
                  : draft.properties & ~ScriptProperties.loadOnBoot;
              _touch();
            },
          ),
          SwitchListTile(
            dense: true,
            contentPadding: EdgeInsets.zero,
            title: const Text('Run on load'),
            value: draft.properties & ScriptProperties.runOnLoad != 0,
            onChanged: (v) {
              draft.properties = v
                  ? draft.properties | ScriptProperties.runOnLoad
                  : draft.properties & ~ScriptProperties.runOnLoad;
              _touch();
            },
          ),
        ]),
      ),
    );
  }

  Widget _valueSection(ScriptDraft draft, _Category category) {
    final list = _listOf(draft, category);
    final live = _statusText[category];
    final title = switch (category) {
      _Category.input => 'Inputs',
      _Category.output => 'Outputs',
      _Category.variable => 'Variables',
      _Category.constant => 'Constants',
    };
    return Card(
      margin: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Row(children: [
            Text(title, style: const TextStyle(color: kOrange, fontWeight: FontWeight.w600)),
            const SizedBox(width: 6),
            Text('(${list.length})', style: const TextStyle(color: Colors.white54, fontSize: 12)),
            const Spacer(),
            IconButton(
              visualDensity: VisualDensity.compact,
              icon: const Icon(Icons.add, size: 20),
              tooltip: 'Add $title',
              onPressed: () => _addValue(category),
            ),
          ]),
          for (var i = 0; i < list.length; i++) _valueRow(category, i, list[i], live),
        ]),
      ),
    );
  }

  Widget _valueRow(_Category category, int index, ScriptDraftValue v, List<String>? live) {
    final liveText = live != null && index < live.length ? live[index] : null;
    return ListTile(
      dense: true,
      contentPadding: EdgeInsets.zero,
      title: Text(v.name.isEmpty ? '#$index' : v.name),
      subtitle: Text(
        '${dataTypeLabel(v.type)} · ${v.size}B'
        '${category == _Category.input ? ' · ${ScriptUiType.label(v.spec.uiType)}' : ''}',
        style: const TextStyle(fontSize: 11, color: Colors.white54),
      ),
      trailing: Row(mainAxisSize: MainAxisSize.min, children: [
        if (liveText != null)
          Text(liveText, style: const TextStyle(fontFamily: 'monospace', color: Colors.white70)),
        IconButton(
          visualDensity: VisualDensity.compact,
          icon: const Icon(Icons.edit, size: 18),
          onPressed: () => _editValue(category, index),
        ),
        IconButton(
          visualDensity: VisualDensity.compact,
          icon: const Icon(Icons.remove_circle_outline, size: 18, color: Colors.redAccent),
          onPressed: () => _removeValue(category, index),
        ),
      ]),
      onTap: () => _editValue(category, index),
    );
  }

  void _addValue(_Category category) {
    final draft = _draft!;
    final list = _listOf(draft, category);
    if (list.length >= 255) return;
    list.add(ScriptDraftValue(type: DataType.number, size: 4));
    _touch();
    _editValue(category, list.length - 1);
  }

  void _removeValue(_Category category, int index) {
    final draft = _draft!;
    final list = _listOf(draft, category);
    if (index < 0 || index >= list.length) return;
    setState(() {
      list.removeAt(index);
      _dirty = true;
      // Removing a value invalidates symbol references, so drop the program's symbols
      // that pointed at or beyond the removed index to keep the draft consistent.
      _dropSymbolReferences(category, index);
    });
  }

  void _dropSymbolReferences(_Category category, int removed) {
    final draft = _draft!;
    final type = switch (category) {
      _Category.input => symInput,
      _Category.output => symOutput,
      _Category.variable => symVariable,
      _Category.constant => symConstant,
    };
    bool bad(ScriptSymbol s) => s.type == type && s.value >= removed;
    for (final line in draft.lines) {
      line.destinations.removeWhere(bad);
      line.operands.removeWhere(bad);
    }
  }

  // ---------------------------------------------------------------------------
  // Value edit dialog
  // ---------------------------------------------------------------------------

  Future<void> _editValue(_Category category, int index) async {
    final draft = _draft!;
    final list = _listOf(draft, category);
    if (index >= list.length) return;
    final updated = await showDialog<ScriptDraftValue>(
      context: context,
      builder: (_) => _ValueDialog(
        category: category,
        initial: list[index],
        index: index,
        deviceId: widget.deviceId,
      ),
    );
    if (updated == null || !mounted) return;
    setState(() {
      list[index] = updated;
      _dirty = true;
    });
  }

  // ---------------------------------------------------------------------------
  // Instructions
  // ---------------------------------------------------------------------------

  Widget _instructionsCard(ScriptDraft draft) {
    return Card(
      margin: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Row(children: [
            const Text('Instructions',
                style: TextStyle(color: kOrange, fontWeight: FontWeight.w600)),
            const SizedBox(width: 6),
            Text('(${draft.lines.length} lines)',
                style: const TextStyle(color: Colors.white54, fontSize: 12)),
            const Spacer(),
            IconButton(
              visualDensity: VisualDensity.compact,
              icon: const Icon(Icons.add, size: 20),
              tooltip: 'Add line',
              onPressed: () => setState(() {
                draft.lines.add(ScriptLine(instruction: ScriptSymbol.instruction(catMath, 0)));
                _dirty = true;
              }),
            ),
          ]),
          if (draft.lines.isEmpty)
            const Padding(
              padding: EdgeInsets.symmetric(vertical: 8),
              child: Text('No instructions yet. Add a line to start the program.',
                  style: TextStyle(color: Colors.white54)),
            )
          else
            ReorderableListView.builder(
              shrinkWrap: true,
              physics: const NeverScrollableScrollPhysics(),
              buildDefaultDragHandles: false,
              itemCount: draft.lines.length,
              onReorderItem: (oldIndex, newIndex) => setState(() {
                final line = draft.lines.removeAt(oldIndex);
                draft.lines.insert(newIndex, line);
                _dirty = true;
              }),
              itemBuilder: (context, i) => _lineEditor(draft, i),
            ),
        ]),
      ),
    );
  }

  /// One line rendered as a readable Destination-Instruction-Operand row. Tapping a symbol
  /// changes it, tapping the instruction re-picks it, and the drag handle reorders lines.
  /// Adding destinations/operands is limited by the selected instruction.
  Widget _lineEditor(ScriptDraft draft, int index) {
    final line = draft.lines[index];
    final def = line.def;
    // The instruction counter is a line index, so the active line is a direct match.
    final active = widget.loaded && _instructionCounter == index;
    final canAddDestination =
        def != null && line.destinations.length < def.maxDestinations;
    final canAddOperand = def != null && line.operands.length < def.maxOperands;

    Widget symbolChip(ScriptSymbol s, VoidCallback onTap, VoidCallback onDelete) => Padding(
          padding: const EdgeInsets.symmetric(horizontal: 2),
          child: InputChip(
            label: Text(_symbolLabel(draft, s), style: const TextStyle(fontSize: 12)),
            visualDensity: VisualDensity.compact,
            onPressed: onTap,
            onDeleted: onDelete,
          ),
        );

    Future<void> addDestination() async {
      final s = await _pickSymbol(draft, destination: true, def: def, operandIndex: 0);
      if (s == null || !mounted) return;
      setState(() {
        line.destinations.add(s);
        _dirty = true;
      });
    }

    Future<void> addOperand() async {
      final operandIndex = line.operands.length;
      final s = await _pickSymbol(draft,
          destination: false, def: def, operandIndex: operandIndex);
      if (s == null || !mounted) return;
      setState(() {
        line.operands.add(s);
        _dirty = true;
      });
    }

    return Container(
      key: ObjectKey(line),
      margin: const EdgeInsets.symmetric(vertical: 4),
      padding: const EdgeInsets.symmetric(vertical: 2),
      decoration: BoxDecoration(
        color: active ? kOrange.withAlpha(28) : Colors.white.withAlpha(8),
        borderRadius: BorderRadius.circular(6),
        border: Border.all(color: active ? kOrange : Colors.white24),
      ),
      child: Row(children: [
        ReorderableDragStartListener(
          index: index,
          child: const Padding(
            padding: EdgeInsets.symmetric(horizontal: 4),
            child: Icon(Icons.drag_indicator, size: 18, color: Colors.white38),
          ),
        ),
        Expanded(
          child: SingleChildScrollView(
            scrollDirection: Axis.horizontal,
            child: Row(children: [
              const SizedBox(width: 4),
              Text('${index + 1}', style: const TextStyle(color: Colors.white38, fontSize: 11)),
              const SizedBox(width: 6),
              // Destinations... then the add-destination affordance, before the instruction.
              for (var i = 0; i < line.destinations.length; i++)
                symbolChip(
                  line.destinations[i],
                  () => _changeSymbol(draft, line.destinations, i,
                      destination: true, def: def),
                  () => setState(() {
                    line.destinations.removeAt(i);
                    _dirty = true;
                  }),
                ),
              if (canAddDestination)
                IconButton(
                  visualDensity: VisualDensity.compact,
                  tooltip: 'Add destination',
                  icon: const Icon(Icons.add_circle_outline, size: 18),
                  onPressed: addDestination,
                ),
              Padding(
                padding: const EdgeInsets.symmetric(horizontal: 4),
                child: ActionChip(
                  avatar: const Icon(Icons.tune, size: 16),
                  label: Text(_instructionLabel(line.instruction),
                      style: const TextStyle(fontSize: 12, fontWeight: FontWeight.w600)),
                  onPressed: () => _changeInstruction(line),
                ),
              ),
              for (var i = 0; i < line.operands.length; i++)
                symbolChip(
                  line.operands[i],
                  () => _changeSymbol(draft, line.operands, i,
                      destination: false, def: def, operandIndex: i),
                  () => setState(() {
                    line.operands.removeAt(i);
                    _dirty = true;
                  }),
                ),
              if (canAddOperand)
                IconButton(
                  visualDensity: VisualDensity.compact,
                  tooltip: 'Add operand',
                  icon: const Icon(Icons.add_circle_outline, size: 18),
                  onPressed: addOperand,
                ),
              const SizedBox(width: 4),
            ]),
          ),
        ),
        if (active) const Padding(padding: EdgeInsets.only(right: 4), child: ChipLabel('ACTIVE')),
        IconButton(
          visualDensity: VisualDensity.compact,
          icon: const Icon(Icons.delete_outline, size: 18, color: Colors.redAccent),
          onPressed: () => setState(() {
            draft.lines.removeAt(index);
            _dirty = true;
          }),
        ),
      ]),
    );
  }

  String _instructionLabel(ScriptSymbol s) {
    final def = scriptInstructionFor(s);
    if (def == null) return 'Instruction ${s.value}';
    return '${ScriptInstructionDef.categoryName(def.category)} · ${def.label}';
  }

  Future<void> _changeInstruction(ScriptLine line) async {
    final def = await showDialog<ScriptInstructionDef>(
      context: context,
      builder: (_) => _InstructionPicker(
        current: line.def,
        hasDestination: line.destinations.isNotEmpty,
      ),
    );
    if (def == null || !mounted) return;
    setState(() {
      line.instruction = def.symbol();
      // Trim symbols the new instruction cannot take.
      if (line.destinations.length > def.maxDestinations) {
        line.destinations.removeRange(def.maxDestinations, line.destinations.length);
      }
      if (line.operands.length > def.maxOperands) {
        line.operands.removeRange(def.maxOperands, line.operands.length);
      }
      _dirty = true;
    });
  }

  Future<void> _changeSymbol(ScriptDraft draft, List<ScriptSymbol> target, int index,
      {required bool destination, ScriptInstructionDef? def, int? operandIndex}) async {
    final s = await _pickSymbol(draft,
        destination: destination, def: def, operandIndex: operandIndex);
    if (s == null || !mounted) return;
    setState(() {
      target[index] = s;
      _dirty = true;
    });
  }

  String _symbolLabel(ScriptDraft draft, ScriptSymbol s) {
    switch (s.type) {
      case symInput:
        return 'In ${_named(draft.inputs, s.value)}';
      case symOutput:
        return 'Out ${_named(draft.outputs, s.value)}';
      case symVariable:
        return 'Var ${_named(draft.variables, s.value)}';
      case symConstant:
        return 'Const ${_named(draft.constants, s.value)}';
      case symPredefine:
        return _predefineLabel(s.subtype, s.value);
      default:
        return '?';
    }
  }

  String _predefineLabel(int subtype, int value) {
    switch (subtype) {
      case preState:
        return 'State: ${ScriptState.label(value)}';
      case preType:
        return 'Type: ${dataTypeLabel(DataType.fromValue(value))}';
      case preBool:
        return 'Bool: ${value != 0 ? 'true' : 'false'}';
      case preChar:
        return "Char: '${value >= 32 && value < 127 ? String.fromCharCode(value) : '\\x${value.toRadixString(16)}'}'";
      case preMathOp:
        final op = scriptPredefineMathOps.firstWhere((e) => e.$1 == value,
            orElse: () => (value, '?'));
        return 'Op: ${op.$2}';
      case preNumber: {
        final q = value >= 32768 ? value - 65536 : value;
        return 'Number: ${(q / 256).toStringAsFixed(3)}';
      }
      case preIndex:
      default:
        return 'Index: $value';
    }
  }

  String _named(List<ScriptDraftValue> list, int index) =>
      index < list.length ? (list[index].name.isNotEmpty ? list[index].name : '#$index') : '?$index';

  Future<ScriptSymbol?> _pickSymbol(ScriptDraft draft,
      {required bool destination, ScriptInstructionDef? def, int? operandIndex}) {
    return showDialog<ScriptSymbol>(
      context: context,
      builder: (_) => _SymbolPicker(
        draft: draft,
        destination: destination,
        def: def,
        operandIndex: operandIndex,
        deviceId: widget.deviceId,
        nameOf: _symbolLabel,
      ),
    );
  }

  // ---------------------------------------------------------------------------
  // Validity
  // ---------------------------------------------------------------------------

  Widget _validityCard() {
    final errors = _errors;
    if (errors == null) return const SizedBox.shrink();
    return Card(
      margin: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Text(errors.isEmpty ? 'Valid' : '${errors.length} problem(s)',
              style: TextStyle(
                  color: errors.isEmpty ? Colors.greenAccent : Colors.redAccent,
                  fontWeight: FontWeight.w600)),
          for (final e in errors)
            Padding(
              padding: const EdgeInsets.only(top: 4),
              child: Text('• $e', style: const TextStyle(color: Colors.white70, fontSize: 12)),
            ),
        ]),
      ),
    );
  }
}

/// Per-value edit dialog: name, type, size, and the category-specific extras (UI spec +
/// default for inputs, default value for outputs/variables, editable value for constants).
class _ValueDialog extends StatefulWidget {
  final _Category category;
  final ScriptDraftValue initial;
  final int index;
  final int deviceId;

  const _ValueDialog(
      {required this.category, required this.initial, required this.index, required this.deviceId});

  @override
  State<_ValueDialog> createState() => _ValueDialogState();
}

class _ValueDialogState extends State<_ValueDialog> {
  late final TextEditingController _name = TextEditingController(text: widget.initial.name);
  late final TextEditingController _min = TextEditingController(text: _trim(widget.initial.spec.min));
  late final TextEditingController _max = TextEditingController(text: _trim(widget.initial.spec.max));
  late final TextEditingController _step = TextEditingController(text: _trim(widget.initial.spec.step));
  late DataType _type = widget.initial.type;
  late int _uiType = widget.initial.spec.uiType;
  late List<int> _value = widget.initial.value.isNotEmpty
      ? List<int>.from(widget.initial.value)
      : (_editableValue ? List<int>.filled(defaultSizeForType(widget.initial.type), 0) : <int>[]);

  bool get _isInput => widget.category == _Category.input;
  bool get _isConstant => widget.category == _Category.constant;

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
      title: Text('${_categoryTitle(widget.category)} ${widget.index}'),
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

  static String _categoryTitle(_Category c) => switch (c) {
        _Category.input => 'Input',
        _Category.output => 'Output',
        _Category.variable => 'Variable',
        _Category.constant => 'Constant',
      };
}

/// Context-aware symbol picker (Docs: "recommendations ... based on context, per symbol
/// category split, variable/constant creation shortcuts").
class _SymbolPicker extends StatefulWidget {
  final ScriptDraft draft;
  final bool destination;

  /// The instruction the symbol is being added to (drives the allowed kinds/counts).
  final ScriptInstructionDef? def;
  final int? operandIndex;
  final int deviceId;
  final String Function(ScriptDraft, ScriptSymbol) nameOf;

  const _SymbolPicker({
    required this.draft,
    required this.destination,
    this.def,
    this.operandIndex,
    required this.deviceId,
    required this.nameOf,
  });

  @override
  State<_SymbolPicker> createState() => _SymbolPickerState();
}

class _SymbolPickerState extends State<_SymbolPicker> {
  int? _group;

  /// The instruction's constant-operand position (a 4-byte BlockInfo).
  bool get _constantOnly {
    final idx = widget.def?.constantIndex ?? -1;
    return idx >= 0 && idx == widget.operandIndex;
  }

  /// True when the operand being picked is the instruction's device address.
  bool get _addressOperand {
    final idx = widget.def?.addressIndex ?? -1;
    return idx >= 0 && idx == widget.operandIndex;
  }

  /// Filters a candidate symbol by the current instruction's expectations.
  bool _allowed(ScriptDraft draft, ScriptSymbol s) {
    if (widget.destination) return s.type == symVariable || s.type == symOutput;
    if (_constantOnly) return s.type == symConstant;
    if (_addressOperand) {
      final t = draft.valueTypeOf(s);
      if (t == null) return s.type == symPredefine && s.subtype == preIndex;
      return t == DataType.id ||
          t == DataType.uint32 ||
          t == DataType.integer ||
          t == DataType.number;
    }
    final def = widget.def;
    if (def == null || !def.numeric) return true;
    final t = draft.valueTypeOf(s);
    if (t == null) {
      return s.type == symPredefine &&
          const {preIndex, preChar, preBool, preMathOp, preNumber}.contains(s.subtype);
    }
    return scriptTypeIsNumeric(t.value);
  }

  /// True when this operand is expected to be a numeric value.
  bool get _numericOperand =>
      widget.def?.numeric == true && !_constantOnly && !_addressOperand;

  /// True when the instruction expects a boolean condition at this position.
  bool get _conditionOperand {
    final def = widget.def;
    if (def == null || widget.destination) return false;
    if (def.category == catFlow && (def.op == 0 || def.op == 1)) return true; // If/While
    if (def.category == catTime && def.op == 1) return true; // Wait until
    if (def.category == catLogic && def.op == 12 && widget.operandIndex == 0) return true; // Select
    return false;
  }

  /// True when the instruction expects a line target (Jump/Call).
  bool get _targetOperand {
    final def = widget.def;
    return def != null && !widget.destination && def.category == catFlow && (def.op == 3 || def.op == 4);
  }

  /// Instruction/position-aware recommendations shown before the category groups.
  List<ScriptSymbol> _recommendations(ScriptDraft draft) {
    final candidates = <ScriptSymbol>[];
    void addInputsWhere(bool Function(DataType) test) {
      for (var i = 0; i < draft.inputs.length; i++) {
        if (test(draft.inputs[i].type)) candidates.add(ScriptSymbol.input(i));
      }
    }

    void addVarsWhere(bool Function(DataType) test) {
      for (var i = 0; i < draft.variables.length; i++) {
        if (test(draft.variables[i].type)) candidates.add(ScriptSymbol.variable(i));
      }
    }

    void addConstsWhere(bool Function(DataType) test) {
      for (var i = 0; i < draft.constants.length; i++) {
        if (test(draft.constants[i].type)) candidates.add(ScriptSymbol.constant(i));
      }
    }

    bool any(DataType _) => true;
    bool isId(DataType t) => t == DataType.id;
    bool isBool(DataType t) => t == DataType.bool_;
    bool numeric(DataType t) => scriptTypeIsNumeric(t.value);

    if (widget.destination) {
      addVarsWhere(any); // writable first
      for (var i = 0; i < draft.outputs.length; i++) {
        candidates.add(ScriptSymbol.output(i));
      }
    } else if (_constantOnly) {
      addConstsWhere(any);
    } else if (_addressOperand) {
      addInputsWhere(isId);
      addVarsWhere(isId);
      addConstsWhere(isId);
      addInputsWhere(numeric);
      addConstsWhere(numeric);
    } else if (_targetOperand) {
      candidates.add(ScriptSymbol.predefine(preIndex, 0));
      candidates.add(ScriptSymbol.predefine(preIndex, 1));
      addVarsWhere(numeric);
      addConstsWhere(numeric);
    } else if (_conditionOperand) {
      addVarsWhere(isBool);
      addInputsWhere(isBool);
      candidates.add(ScriptSymbol.predefine(preBool, 1));
      candidates.add(ScriptSymbol.predefine(preBool, 0));
    } else if (_numericOperand) {
      addConstsWhere(numeric);
      addVarsWhere(numeric);
      addInputsWhere(numeric);
      // Common inline literals (no named constant needed).
      candidates.add(ScriptSymbol.predefine(preIndex, 0));
      candidates.add(ScriptSymbol.predefine(preIndex, 1));
      candidates.add(ScriptSymbol.predefine(preIndex, 2));
      candidates.add(ScriptSymbol.predefine(preNumber, 128)); // 0.5
    } else {
      addConstsWhere(any);
      addVarsWhere(any);
      addInputsWhere(any);
      candidates.add(ScriptSymbol.predefine(preIndex, 0));
    }
    return candidates.where((s) => _allowed(draft, s)).take(6).toList();
  }

  /// A short description of what this position expects.
  String? get _hint {
    if (widget.destination) {
      if (widget.def != null && widget.def!.maxDestinations == 0) {
        return 'This instruction takes no destination';
      }
      return 'Destination: a variable or output';
    }
    if (_constantOnly) return 'Register address (a 4-byte BlockInfo constant)';
    if (_addressOperand) return 'Device address (Id)';
    if (_targetOperand) return 'Target line index';
    if (_conditionOperand) return 'Boolean condition';
    if (_numericOperand) return 'Numeric value';
    return null;
  }

  List<(String, List<ScriptSymbol>)> _groups(ScriptDraft draft) {
    List<ScriptSymbol> filter(List<ScriptSymbol> xs) =>
        xs.where((s) => _allowed(draft, s)).toList();
    if (widget.destination) {
      return [
        ('Variables', filter([for (var i = 0; i < draft.variables.length; i++) ScriptSymbol.variable(i)])),
        ('Outputs', filter([for (var i = 0; i < draft.outputs.length; i++) ScriptSymbol.output(i)])),
      ];
    }
    if (_constantOnly) {
      return [
        ('Constants', filter([for (var i = 0; i < draft.constants.length; i++) ScriptSymbol.constant(i)])),
      ];
    }
    return [
      ('Inputs', filter([for (var i = 0; i < draft.inputs.length; i++) ScriptSymbol.input(i)])),
      ('Variables', filter([for (var i = 0; i < draft.variables.length; i++) ScriptSymbol.variable(i)])),
      ('Constants', filter([for (var i = 0; i < draft.constants.length; i++) ScriptSymbol.constant(i)])),
      ('Predefines', const []),
    ];
  }

  /// Predefine subtypes allowed by the current instruction.
  List<(int, String)> get _predefineSubtypes {
    final def = widget.def;
    if (def != null && def.numeric) {
      return scriptPredefineSubtypes
          .where((e) => const {preIndex, preChar, preBool, preMathOp, preNumber}.contains(e.$1))
          .toList();
    }
    return scriptPredefineSubtypes;
  }

  @override
  Widget build(BuildContext context) {
    final draft = widget.draft;
    final groups = _groups(draft);
    final group = _group;
    return AlertDialog(
      title: Row(children: [
        if (group != null)
          IconButton(
            visualDensity: VisualDensity.compact,
            icon: const Icon(Icons.arrow_back, size: 18),
            tooltip: 'Back to groups',
            onPressed: () => setState(() => _group = null),
          ),
        Expanded(
          child: Text(group == null
              ? (widget.destination ? 'Pick destination' : 'Pick operand')
              : groups[group].$1),
        ),
      ]),
      content: DialogBody(
        maxWidth: 360,
        height: 380,
        child: Column(children: [
          if (_hint != null)
            Padding(
              padding: const EdgeInsets.only(bottom: 4),
              child: Text(_hint!, style: const TextStyle(color: kOrange, fontSize: 12)),
            ),
          Expanded(
            child: group == null
                ? _menu(draft, groups)
                : groups[group].$1 == 'Predefines'
                    ? _predefineList()
                    : _groupList(draft, groups[group].$2),
          ),
        ]),
      ),
      actions: [
        TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
      ],
    );
  }

  Widget _menu(ScriptDraft draft, List<(String, List<ScriptSymbol>)> groups) {
    final recs = _recommendations(draft);
    return ListView(children: [
      if (recs.isNotEmpty) ...[
        _header('Recommended'),
        for (final s in recs) _symbolTile(draft, s),
      ],
      _header('Groups'),
      for (var i = 0; i < groups.length; i++)
        ListTile(
          dense: true,
          leading: const Icon(Icons.folder_outlined, size: 18),
          title: Text(groups[i].$1),
          trailing: Text('${groups[i].$2.length}',
              style: const TextStyle(color: Colors.white38, fontSize: 12)),
          onTap: () => setState(() => _group = i),
        ),
      const Divider(),
      ..._creationTiles(draft),
    ]);
  }

  Widget _groupList(ScriptDraft draft, List<ScriptSymbol> symbols) {
    return ListView(children: [
      if (symbols.isEmpty)
        const Padding(
          padding: EdgeInsets.all(12),
          child: Text('None', style: TextStyle(color: Colors.white38)),
        ),
      for (final s in symbols) _symbolTile(draft, s),
      const Divider(),
      ..._creationTiles(draft),
    ]);
  }

  /// Predefine editor entry point: pick the subtype, then its value.
  Widget _predefineList() {
    return ListView(children: [
      for (final (subtype, name) in _predefineSubtypes)
        ListTile(
          dense: true,
          title: Text(name),
          trailing: const Icon(Icons.chevron_right, size: 18),
          onTap: () async {
            final s = await showDialog<ScriptSymbol>(
              context: context,
              builder: (_) => _PredefineValueDialog(subtype: subtype),
            );
            if (s == null || !mounted) return;
            Navigator.pop(context, s);
          },
        ),
    ]);
  }

  List<Widget> _creationTiles(ScriptDraft draft) => [
        if (!_constantOnly)
          ListTile(
            dense: true,
            leading: const Icon(Icons.add, size: 18),
            title: const Text('New variable'),
            onTap: () {
              draft.variables.add(ScriptDraftValue(type: DataType.number));
              Navigator.pop(context, ScriptSymbol.variable(draft.variables.length - 1));
            },
          ),
        if (_constantOnly)
          ListTile(
            dense: true,
            leading: const Icon(Icons.add, size: 18),
            title: const Text('New BlockInfo'),
            subtitle: const Text('Register target (block/field/key)',
                style: TextStyle(fontSize: 11, color: Colors.white54)),
            onTap: () async {
              final bytes = await pickBlockInfo(context, widget.deviceId);
              if (bytes == null || !mounted) return;
              draft.constants.add(ScriptDraftValue(
                  name: 'Target',
                  type: DataType.blockInfo,
                  value: Uint8List.fromList(bytes)));
              Navigator.pop(context, ScriptSymbol.constant(draft.constants.length - 1));
            },
          )
        else if (!widget.destination)
          ListTile(
            dense: true,
            leading: const Icon(Icons.add, size: 18),
            title: const Text('New constant'),
            onTap: () {
              draft.constants.add(ScriptDraftValue(type: DataType.number));
              Navigator.pop(context, ScriptSymbol.constant(draft.constants.length - 1));
            },
          ),
        if (_numericOperand)
          ListTile(
            dense: true,
            leading: const Icon(Icons.numbers, size: 18),
            title: const Text('Literal…'),
            subtitle: const Text('Inline number (no constant needed)',
                style: TextStyle(fontSize: 11, color: Colors.white54)),
            onTap: () async {
              final s = await showDialog<ScriptSymbol>(
                  context: context, builder: (_) => const _LiteralDialog());
              if (s == null || !mounted) return;
              Navigator.pop(context, s);
            },
          ),
      ];

  Widget _header(String text) => Padding(
        padding: const EdgeInsets.only(top: 8, bottom: 2),
        child: Text(text, style: const TextStyle(color: kOrange, fontWeight: FontWeight.w600)),
      );

  Widget _symbolTile(ScriptDraft draft, ScriptSymbol s) => ListTile(
        dense: true,
        title: Text(widget.nameOf(draft, s)),
        onTap: () => Navigator.pop(context, s),
      );
}

/// Instruction picker with the same recommendation-first, grouped layout.
class _InstructionPicker extends StatefulWidget {
  final ScriptInstructionDef? current;
  final bool hasDestination;

  const _InstructionPicker({this.current, this.hasDestination = false});

  @override
  State<_InstructionPicker> createState() => _InstructionPickerState();
}

class _InstructionPickerState extends State<_InstructionPicker> {
  int? _category;

  List<(String, int, List<ScriptInstructionDef>)> _groups() {
    final groups = <(String, int, List<ScriptInstructionDef>)>[];
    for (final def in scriptInstructions) {
      if (groups.isNotEmpty && groups.last.$2 == def.category) {
        groups.last.$3.add(def);
      } else {
        groups.add((ScriptInstructionDef.categoryName(def.category), def.category, [def]));
      }
    }
    return groups;
  }

  List<ScriptInstructionDef> _recommendations() {
    final recs = <ScriptInstructionDef>[];
    void add(int cat, int op) {
      for (final d in scriptInstructions) {
        if (d.category == cat && d.op == op && !recs.contains(d)) recs.add(d);
      }
    }

    // Context: with a destination the line produces a value; without one it is control
    // flow / timing / a service action.
    if (widget.hasDestination) {
      add(catMath, 0); // Set
      add(catMath, 1); // Add
      add(catLogic, 6); // Compare =
      add(catTime, 2); // Get time
      add(catService, 1); // Register read
      add(catCompose, 1); // Extract
    } else {
      add(catFlow, 0); // If
      add(catFlow, 1); // While
      add(catTime, 1); // Wait until
      add(catService, 2); // Register write
      add(catService, 4); // Nop
      add(catFlow, 6); // Halt
    }
    final current = widget.current;
    if (current != null && !recs.contains(current)) recs.insert(0, current);
    return recs;
  }

  @override
  Widget build(BuildContext context) {
    final groups = _groups();
    final category = _category;
    return AlertDialog(
      title: Row(children: [
        if (category != null)
          IconButton(
            visualDensity: VisualDensity.compact,
            icon: const Icon(Icons.arrow_back, size: 18),
            tooltip: 'Back to categories',
            onPressed: () => setState(() => _category = null),
          ),
        Expanded(child: Text(category == null ? 'Pick instruction' : ScriptInstructionDef.categoryName(category))),
      ]),
      content: DialogBody(
        maxWidth: 380,
        height: 400,
        child: category == null
            ? ListView(children: [
                const Padding(
                  padding: EdgeInsets.only(top: 8, bottom: 2),
                  child: Text('Recommended',
                      style: TextStyle(color: kOrange, fontWeight: FontWeight.w600)),
                ),
                for (final def in _recommendations()) _tile(def),
                const Padding(
                  padding: EdgeInsets.only(top: 8, bottom: 2),
                  child: Text('Groups',
                      style: TextStyle(color: kOrange, fontWeight: FontWeight.w600)),
                ),
                for (var i = 0; i < groups.length; i++)
                  ListTile(
                    dense: true,
                    leading: const Icon(Icons.folder_outlined, size: 18),
                    title: Text(groups[i].$1),
                    trailing: Text('${groups[i].$3.length}',
                        style: const TextStyle(color: Colors.white38, fontSize: 12)),
                    onTap: () => setState(() => _category = groups[i].$2),
                  ),
              ])
            : ListView(children: [
                for (final group in groups)
                  if (group.$2 == category) for (final def in group.$3) _tile(def),
              ]),
      ),
      actions: [
        TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
      ],
    );
  }

  Widget _tile(ScriptInstructionDef def) => ListTile(
        dense: true,
        title: Text(def.label),
        subtitle: Text('${ScriptInstructionDef.categoryName(def.category)}'
            '${def.destination ? ' · destination' : ''}'
            ' · ${def.minOperands}..${def.maxOperands} operands',
            style: const TextStyle(fontSize: 11, color: Colors.white54)),
        onTap: () => Navigator.pop(context, def),
      );
}

/// Chooses a predefine's value for its subtype (Docs predefine subtypes: State, Type,
/// Index, Char, Math op, Bool).
class _PredefineValueDialog extends StatefulWidget {
  final int subtype;

  const _PredefineValueDialog({required this.subtype});

  @override
  State<_PredefineValueDialog> createState() => _PredefineValueDialogState();
}

class _PredefineValueDialogState extends State<_PredefineValueDialog> {
  final _text = TextEditingController();

  @override
  void dispose() {
    _text.dispose();
    super.dispose();
  }

  void _pickValue(int value) =>
      Navigator.pop(context, ScriptSymbol.predefine(widget.subtype, value));

  @override
  Widget build(BuildContext context) {
    final subtype = widget.subtype;
    final Widget body = switch (subtype) {
      preState => _list([for (var s = 0; s <= 5; s++) (s, ScriptState.label(s))]),
      preType => _list([for (final t in scriptValueTypes) (t.value, dataTypeLabel(t))]),
      preBool => _list(const [(0, 'false'), (1, 'true')]),
      preMathOp => _list(scriptPredefineMathOps),
      preIndex => _numberEntry(max: 0xFFFF, hint: '0..65535'),
      preChar => _charEntry(),
      preNumber => _numberLiteralEntry(),
      _ => const SizedBox.shrink(),
    };
    return AlertDialog(
      title: Text('Predefine: ${ScriptSymbol.predefineName(subtype)}'),
      content: DialogBody(maxWidth: 320, height: 360, child: body),
      actions: [
        TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
      ],
    );
  }

  Widget _list(List<(int, String)> items) => ListView(
        children: [
          for (final (value, label) in items)
            ListTile(dense: true, title: Text(label), onTap: () => _pickValue(value)),
        ],
      );

  Widget _numberEntry({required int max, String? hint}) {
    return Column(mainAxisSize: MainAxisSize.min, children: [
      TextField(
        controller: _text,
        autofocus: true,
        keyboardType: TextInputType.number,
        decoration: InputDecoration(labelText: 'Value', helperText: hint),
      ),
      const SizedBox(height: 12),
      FilledButton(
        onPressed: () {
          final v = int.tryParse(_text.text.trim());
          if (v == null || v < 0 || v > max) return;
          _pickValue(v);
        },
        child: const Text('OK'),
      ),
    ]);
  }

  Widget _charEntry() {
    return Column(mainAxisSize: MainAxisSize.min, children: [
      TextField(
        controller: _text,
        autofocus: true,
        maxLength: 1,
        decoration: const InputDecoration(labelText: 'Character'),
      ),
      const SizedBox(height: 12),
      FilledButton(
        onPressed: () {
          if (_text.text.isEmpty) return;
          _pickValue(_text.text.codeUnitAt(0) & 0xFF);
        },
        child: const Text('OK'),
      ),
    ]);
  }

  /// Number literal: a decimal value encoded as a 16-bit Q8.8 (step 1/256, range ±128).
  Widget _numberLiteralEntry() {
    return Column(mainAxisSize: MainAxisSize.min, children: [
      TextField(
        controller: _text,
        autofocus: true,
        keyboardType: const TextInputType.numberWithOptions(decimal: true, signed: true),
        decoration: const InputDecoration(
            labelText: 'Value', helperText: '-128..127.99 (e.g. 0.5, 2.25)'),
      ),
      const SizedBox(height: 12),
      FilledButton(
        onPressed: () {
          final v = double.tryParse(_text.text.trim().replaceAll(',', '.'));
          if (v == null) return;
          final q = (v * 256).round();
          if (q < -32768 || q > 32767) return;
          _pickValue(q & 0xFFFF);
        },
        child: const Text('OK'),
      ),
    ]);
  }
}

/// Inline numeric literal picker: an integer in 0..65535 becomes an `Index` predefine,
/// anything else a Q8.8 `Number` predefine.
class _LiteralDialog extends StatefulWidget {
  const _LiteralDialog();

  @override
  State<_LiteralDialog> createState() => _LiteralDialogState();
}

class _LiteralDialogState extends State<_LiteralDialog> {
  final _text = TextEditingController();

  @override
  void dispose() {
    _text.dispose();
    super.dispose();
  }

  void _submit() {
    final v = double.tryParse(_text.text.trim().replaceAll(',', '.'));
    if (v == null) return;
    if (v == v.roundToDouble() && v >= 0 && v <= 65535) {
      Navigator.pop(context, ScriptSymbol.predefine(preIndex, v.toInt()));
      return;
    }
    final q = (v * 256).round();
    if (q < -32768 || q > 32767) return;
    Navigator.pop(context, ScriptSymbol.predefine(preNumber, q & 0xFFFF));
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: const Text('Literal'),
      content: DialogBody(
        maxWidth: 320,
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          TextField(
            controller: _text,
            autofocus: true,
            keyboardType: const TextInputType.numberWithOptions(decimal: true, signed: true),
            decoration: const InputDecoration(
                labelText: 'Value', helperText: 'Integer 0..65535, or a fraction like 0.5'),
            onSubmitted: (_) => _submit(),
          ),
        ]),
      ),
      actions: [
        TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(onPressed: _submit, child: const Text('OK')),
      ],
    );
  }
}
