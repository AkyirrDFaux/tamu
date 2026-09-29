// The instruction line editor, symbol chips and legends.
//
// Part of script_editor_page.dart: an extension on its State so the editor keeps
// private access to the page's draft and state.

part of 'script_editor_page.dart';

extension on _ScriptEditorPageState {
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
      _rebuild(() {
        line.destinations.add(s);
        _dirty = true;
      });
    }

    Future<void> addOperand() async {
      final operandIndex = line.operands.length;
      final s = await _pickSymbol(draft,
          destination: false, def: def, operandIndex: operandIndex);
      if (s == null || !mounted) return;
      _rebuild(() {
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
          onPressed: () => _rebuild(() {
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
          onDeleted: () => _rebuild(() {
            list.removeAt(i);
            _dirty = true;
          }),
        );

    final body = DragTarget<int>(
      onWillAcceptWithDetails: (d) => d.data != i,
      onAcceptWithDetails: (d) => _rebuild(() {
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
    _rebuild(() {
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
    _rebuild(() {
      target[index] = s;
      _dirty = true;
    });
  }

  /// Human label for a symbol. [full] adds the category prefix (`In `, `Out `, …) and the
  /// full predefine wording; the on-line chip uses the compact form.
  String _symbolLabel(ScriptDraft draft, ScriptSymbol s, {bool full = true}) {
    switch (s.type) {
      case symInput:
        return _ScriptEditorPageState._tagged('In', _named(draft.inputs, s.value), full);
      case symOutput:
        return _ScriptEditorPageState._tagged('Out', _named(draft.outputs, s.value), full);
      case symVariable:
        return _ScriptEditorPageState._tagged('Var', _named(draft.variables, s.value), full);
      case symConstant:
        return _ScriptEditorPageState._tagged('Const', _named(draft.constants, s.value), full);
      case symPredefine:
        return _predefineLabel(s.subtype, s.value, full: full);
      default:
        return '?';
    }
  }
}
