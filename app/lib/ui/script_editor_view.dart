// The script editor's cards (controls, values, functions, errors).
//
// Part of script_editor_page.dart: an extension on its State so the cards keep
// private access to the page's draft and state.

part of 'script_editor_page.dart';

extension on _ScriptEditorPageState {
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
    final ok = await _client.moveToInstruction(_slot, line);
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
    _rebuild(() {
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
    _rebuild(() {
      list[index] = updated;
      _dirty = true;
    });
  }

  // ---------------------------------------------------------------------------
  // Instructions
  // ---------------------------------------------------------------------------

  Widget _instructionsCard(ScriptDraft draft) {
    final depths = _ScriptEditorPageState._blockDepths(draft.lines);
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
              onPressed: () => _rebuild(() {
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
              onReorderItem: (oldIndex, newIndex) => _rebuild(() {
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

}
