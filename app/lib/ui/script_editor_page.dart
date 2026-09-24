/// Script editor (Docs/App/Service views/Script.md).
///
/// Works on the stored `SCR_XX` file, so both loaded and available scripts can be edited.
/// Covers: controls/state (live), inputs (type/style/limits/default), outputs
/// (type/name + live values), variables (add/remove, type, live values), constants
/// (value + name), and instructions (per-line/per-symbol editing with context
/// recommendations and a validity check). The appbar uploads the edited file and, for a
/// loaded script, applies it live by reloading.
library;

import 'package:flutter/material.dart';

import '../core/script_client.dart';
import '../core/script_draft.dart';
import '../core/script_file.dart';
import '../core/script_instructions.dart';
import '../core/storage_client.dart';
import '../core/types.dart';
import 'script_instruction_picker.dart';
import 'script_symbol_picker.dart';
import 'script_value_dialog.dart';
import 'script_widgets.dart';
import 'theme.dart';
import 'value_editor.dart';
import 'widgets.dart';

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

  final _statusText = <ScriptValueCategory, List<String>>{};

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
      final values = <ScriptValueCategory, List<String>>{};

      // I/O live values come from the Register (the only script content it exposes).
      for (final cat in const [ScriptValueCategory.input, ScriptValueCategory.output]) {
        final keys = await _client.enumerateKeys(widget.fileId, cat.field);
        final list = <String>[];
        for (final key in keys) {
          final e = await _client.readEntry(widget.fileId, cat.field, key);
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
        values[ScriptValueCategory.variable] = varList;
        values[ScriptValueCategory.constant] =
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
                _valueSection(draft, ScriptValueCategory.input),
                _valueSection(draft, ScriptValueCategory.output),
                _valueSection(draft, ScriptValueCategory.variable),
                _valueSection(draft, ScriptValueCategory.constant),
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

  Widget _valueSection(ScriptDraft draft, ScriptValueCategory category) {
    final list = draft.listOf(category);
    final live = _statusText[category];
    final title = category.pluralLabel;
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

  Widget _valueRow(ScriptValueCategory category, int index, ScriptDraftValue v, List<String>? live) {
    final liveText = live != null && index < live.length ? live[index] : null;
    return ListTile(
      dense: true,
      contentPadding: EdgeInsets.zero,
      title: Text(v.name.isEmpty ? '#$index' : v.name),
      subtitle: Text(
        '${dataTypeLabel(v.type)} · ${v.size}B'
        '${category == ScriptValueCategory.input ? ' · ${ScriptUiType.label(v.spec.uiType)}' : ''}',
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

  void _addValue(ScriptValueCategory category) {
    final draft = _draft!;
    final list = draft.listOf(category);
    if (list.length >= 255) return;
    list.add(ScriptDraftValue(type: DataType.number, size: 4));
    _touch();
    _editValue(category, list.length - 1);
  }

  void _removeValue(ScriptValueCategory category, int index) {
    final draft = _draft!;
    final list = draft.listOf(category);
    if (index < 0 || index >= list.length) return;
    setState(() {
      list.removeAt(index);
      _dirty = true;
      // Removing a value invalidates symbol references, so drop the program's symbols
      // that pointed at or beyond the removed index to keep the draft consistent.
      _dropSymbolReferences(category, index);
    });
  }

  void _dropSymbolReferences(ScriptValueCategory category, int removed) {
    final draft = _draft!;
    final type = category.symbolType;
    bool bad(ScriptSymbol s) => s.type == type && s.value >= removed;
    for (final line in draft.lines) {
      line.destinations.removeWhere(bad);
      line.operands.removeWhere(bad);
    }
  }

  // ---------------------------------------------------------------------------
  // Value edit dialog
  // ---------------------------------------------------------------------------

  Future<void> _editValue(ScriptValueCategory category, int index) async {
    final draft = _draft!;
    final list = draft.listOf(category);
    if (index >= list.length) return;
    final updated = await showDialog<ScriptDraftValue>(
      context: context,
      builder: (_) => ScriptValueDialog(
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
    final depths = _blockDepths(draft.lines);
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
          const SizedBox(height: 4),
          _legend(),
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
              itemBuilder: (context, i) => _lineEditor(draft, i, depths[i]),
            ),
        ]),
      ),
    );
  }

  /// Nesting depth of each instruction line (inside If/While blocks) for indentation.
  static List<int> _blockDepths(List<ScriptLine> lines) {
    final depths = <int>[];
    var depth = 0;
    for (final line in lines) {
      final d = line.def;
      final isEnd = d != null && d.category == catFlow && d.op == 2;
      if (isEnd && depth > 0) depth--;
      depths.add(depth);
      final isOpen = d != null && d.category == catFlow && (d.op == 0 || d.op == 1);
      if (isOpen) depth++;
    }
    return depths;
  }

  /// One line rendered as a readable Destination-Instruction-Operand row. Tapping a symbol
  /// changes it, tapping the instruction re-picks it, and the drag handle reorders lines.
  /// Adding destinations/operands is limited by the selected instruction. [depth] is the
  /// block nesting depth (the line is indented like formatted code).
  Widget _lineEditor(ScriptDraft draft, int index, int depth) {
    final line = draft.lines[index];
    final def = line.def;
    // The instruction counter is a line index, so the active line is a direct match.
    final active = widget.loaded && _instructionCounter == index;
    final canAddDestination =
        def != null && line.destinations.length < def.maxDestinations;
    final canAddOperand = def != null && line.operands.length < def.maxOperands;

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

    Widget addButton(String tooltip, VoidCallback onPressed) => IconButton(
          visualDensity: VisualDensity.compact,
          tooltip: tooltip,
          icon: const Icon(Icons.add_circle_outline, size: 18),
          onPressed: onPressed,
        );

    return Container(
      key: ObjectKey(line),
      margin: const EdgeInsets.symmetric(vertical: 4),
      padding: const EdgeInsets.symmetric(vertical: 4),
      decoration: BoxDecoration(
        color: active ? kOrange.withAlpha(28) : Colors.white.withAlpha(8),
        borderRadius: BorderRadius.circular(6),
        border: Border.all(color: active ? kOrange : Colors.white24),
      ),
      child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
        ReorderableDragStartListener(
          index: index,
          child: const Padding(
            padding: EdgeInsets.symmetric(horizontal: 4),
            child: Icon(Icons.drag_indicator, size: 18, color: Colors.white38),
          ),
        ),
        // Indent block contents (like formatted code).
        if (depth > 0) ...[
          Container(
              width: 2,
              height: 22,
              margin: const EdgeInsets.only(right: 6),
              color: Colors.white24),
          SizedBox(width: depth * 12.0),
        ],
        Expanded(
          // Wrap: a long line folds onto the next row instead of scrolling off-screen.
          child: Wrap(
            spacing: 4,
            runSpacing: 4,
            crossAxisAlignment: WrapCrossAlignment.end,
            children: [
              Text('${index + 1}',
                  style: const TextStyle(color: Colors.white38, fontSize: 11)),
              const SizedBox(width: 2),
              // Destinations, then the instruction, then the operands (with role hints).
              for (var i = 0; i < line.destinations.length; i++)
                _symbolChip(draft, line.destinations, i,
                    destination: true, def: def, role: def?.destinationRole),
              if (canAddDestination) addButton('Add destination', addDestination),
              Padding(
                padding: const EdgeInsets.symmetric(horizontal: 2),
                child: ActionChip(
                  avatar: const Icon(Icons.tune, size: 16, color: kOrange),
                  label: Text(_instructionLabel(line.instruction),
                      style: const TextStyle(
                          fontSize: 12, fontWeight: FontWeight.w600, color: kOrange)),
                  backgroundColor: kOrange.withAlpha(30),
                  side: BorderSide(color: kOrange.withAlpha(120)),
                  visualDensity: VisualDensity.compact,
                  onPressed: () => _changeInstruction(line),
                ),
              ),
              for (var i = 0; i < line.operands.length; i++)
                _symbolChip(draft, line.operands, i,
                    destination: false, def: def, role: def?.operandRole(i)),
              if (canAddOperand) addButton('Add operand', addOperand),
            ],
          ),
        ),
        if (active)
          const Padding(padding: EdgeInsets.only(right: 4), child: ChipLabel('ACTIVE')),
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

  /// Category colour for a script symbol: I/O, variable, constant or predefine.
  Color _symbolColor(ScriptSymbol s) => switch (s.type) {
        symInput => const Color(0xFFFFD54F), // amber - inputs
        symOutput => const Color(0xFFF06292), // pink - outputs
        symVariable => const Color(0xFF64B5F6), // blue - variables
        symConstant => const Color(0xFFBA68C8), // purple - constants
        symPredefine => const Color(0xFF81C784), // green - predefines
        _ => Colors.white70,
      };

  /// A colour-coded symbol chip. Drag it onto another chip to reorder it within its list;
  /// tap to change, X to delete. [role] is a short hint (e.g. "min", "condition").
  Widget _symbolChip(ScriptDraft draft, List<ScriptSymbol> list, int i,
      {required bool destination, ScriptInstructionDef? def, String? role}) {
    final s = list[i];
    final color = _symbolColor(s);

    Widget chip({bool dragging = false}) => InputChip(
          label: Text(_symbolLabel(draft, s, full: false),
              style: TextStyle(fontSize: 12, color: color)),
          backgroundColor: color.withAlpha(dragging ? 70 : 28),
          side: BorderSide(color: color.withAlpha(dragging ? 255 : 110)),
          visualDensity: VisualDensity.compact,
          tooltip: role == null ? null : '$role: ${_symbolLabel(draft, s)}',
          onPressed: () => _changeSymbol(draft, list, i,
              destination: destination, def: def, operandIndex: destination ? null : i),
          onDeleted: () => setState(() {
            list.removeAt(i);
            _dirty = true;
          }),
        );

    final body = DragTarget<int>(
      onWillAcceptWithDetails: (d) => d.data != i,
      onAcceptWithDetails: (d) => setState(() {
        final sym = list.removeAt(d.data);
        list.insert(i, sym);
        _dirty = true;
      }),
      builder: (context, candidate, rejected) => Draggable<int>(
        data: i,
        feedback: Material(
          color: Colors.transparent,
          child: ConstrainedBox(
              constraints: const BoxConstraints(maxWidth: 260),
              child: chip(dragging: true)),
        ),
        childWhenDragging: Opacity(opacity: 0.3, child: chip()),
        child: chip(),
      ),
    );

    if (role == null) return body;
    return Column(mainAxisSize: MainAxisSize.min, children: [
      Text(role, style: const TextStyle(fontSize: 8, color: Colors.white38)),
      body,
    ]);
  }

  /// Colour legend for the instruction card.
  Widget _legend() => Wrap(spacing: 10, runSpacing: 4, children: [
        _legendDot('Input', const Color(0xFFFFD54F)),
        _legendDot('Output', const Color(0xFFF06292)),
        _legendDot('Variable', const Color(0xFF64B5F6)),
        _legendDot('Constant', const Color(0xFFBA68C8)),
        _legendDot('Predefine', const Color(0xFF81C784)),
        _legendDot('Instruction', kOrange),
      ]);

  Widget _legendDot(String label, Color color) =>
      Row(mainAxisSize: MainAxisSize.min, children: [
        Container(
            width: 8,
            height: 8,
            decoration: BoxDecoration(color: color, shape: BoxShape.circle)),
        const SizedBox(width: 4),
        Text(label, style: const TextStyle(fontSize: 10, color: Colors.white54)),
      ]);

  String _instructionLabel(ScriptSymbol s) {
    final def = scriptInstructionFor(s);
    if (def == null) return 'Instruction ${s.value}';
    return '${ScriptInstructionDef.categoryName(def.category)} · ${def.label}';
  }

  Future<void> _changeInstruction(ScriptLine line) async {
    final def = await showDialog<ScriptInstructionDef>(
      context: context,
      builder: (_) => ScriptInstructionPicker(
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

  /// Human label for a symbol. [full] adds the category prefix (`In `, `Out `, …) and the
  /// full predefine wording; the on-line chip uses the compact form.
  String _symbolLabel(ScriptDraft draft, ScriptSymbol s, {bool full = true}) {
    switch (s.type) {
      case symInput:
        return _tagged('In', _named(draft.inputs, s.value), full);
      case symOutput:
        return _tagged('Out', _named(draft.outputs, s.value), full);
      case symVariable:
        return _tagged('Var', _named(draft.variables, s.value), full);
      case symConstant:
        return _tagged('Const', _named(draft.constants, s.value), full);
      case symPredefine:
        return _predefineLabel(s.subtype, s.value, full: full);
      default:
        return '?';
    }
  }

  static String _tagged(String tag, String name, bool full) => full ? '$tag $name' : name;

  /// Predefine label; [full] keeps the `Kind:` prefix (the line chip drops it).
  String _predefineLabel(int subtype, int value, {bool full = true}) {
    switch (subtype) {
      case preState:
        return _tagged('State', ScriptState.label(value), full);
      case preType:
        return _tagged('Type', dataTypeLabel(DataType.fromValue(value)), full);
      case preBool:
        return _tagged('Bool', value != 0 ? 'true' : 'false', full);
      case preChar: {
        final ch = value >= 32 && value < 127
            ? String.fromCharCode(value)
            : '\\x${value.toRadixString(16)}';
        return full ? "Char: '$ch'" : ch;
      }
      case preMathOp: {
        final op = scriptPredefineMathOps.firstWhere((e) => e.$1 == value,
            orElse: () => (value, '?'));
        return full ? '${_mathOpSymbol(value)} ${op.$2}' : _mathOpSymbol(value);
      }
      case preNumber:
        return full
            ? 'Number: ${((value >= 32768 ? value - 65536 : value) / 256).toStringAsFixed(3)}'
            : _numberLiteral(value);
      case preIndex:
      default:
        return _tagged('Index', '$value', full);
    }
  }

  /// Decimal rendering of a Q8.8 number literal.
  static String _numberLiteral(int value) {
    final q = value >= 32768 ? value - 65536 : value;
    final d = q / 256.0;
    return d == d.roundToDouble() ? '${d.toInt()}' : '$d';
  }

  /// Short symbol for a `Math op` predefine (used in expression labels).
  static String _mathOpSymbol(int op) => switch (op) {
        0 => '+',
        1 => '−',
        2 => '×',
        3 => '÷',
        4 => '%',
        5 => '^',
        6 => 'AND',
        7 => 'OR',
        8 => 'XOR',
        9 => 'NOT',
        12 => '==',
        13 => '!=',
        14 => '<',
        15 => '<=',
        16 => '>',
        17 => '>=',
        18 => '(',
        19 => ')',
        20 => 'dot',
        21 => 'cross',
        22 => 'size',
        23 => 'transpose',
        _ => '?'
      };

  String _named(List<ScriptDraftValue> list, int index) =>
      index < list.length ? (list[index].name.isNotEmpty ? list[index].name : '#$index') : '?$index';

  Future<ScriptSymbol?> _pickSymbol(ScriptDraft draft,
      {required bool destination, ScriptInstructionDef? def, int? operandIndex}) {
    return showDialog<ScriptSymbol>(
      context: context,
      builder: (_) => ScriptSymbolPicker(
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
