import 'dart:async';

import 'package:flutter/material.dart';

import '../core/script_asm.dart';
import '../core/script_client.dart';
import '../core/script_file.dart';
import '../core/types.dart';
import 'script_input_widget.dart';
import 'theme.dart';
import 'value_editor.dart' show dataTypeLabel, formatValue, showValueEditor;
import 'widgets.dart' show AutoRefreshMixin;

/// Script editor (Docs/App/Service views/Script.md).
///
/// - Inputs are presented as a dictionary: each input carries a dict key, the
///   expected type, a default value and an interaction style (button/switch/...).
/// - Instructions are built symbol-by-symbol (never raw text): every line is a row
///   of pickable chips ([Output] [Op] [Operands...]). The opcode picker shows a
///   searchable, grouped list with hints and a "recommended" section (not limiting).
/// - Constants/variables/outputs/inputs referenced by an instruction are
///   auto-declared so the user never manages indexes by hand.
class ScriptEditorPage extends StatefulWidget {
  final int deviceId;
  final int scriptId;

  const ScriptEditorPage(
      {super.key, required this.deviceId, required this.scriptId});

  @override
  State<ScriptEditorPage> createState() => _ScriptEditorPageState();
}

class _ScriptEditorPageState extends State<ScriptEditorPage>
    with AutoRefreshMixin<ScriptEditorPage> {
  late final ScriptClient _client = ScriptClient(deviceId: widget.deviceId);

  int _state = ScriptStateCode.stopped;
  bool _busy = true;
  final TextEditingController _nameCtrl = TextEditingController();

  List<ScriptInput> _inputs = [];
  List<String> _outputNames = [];
  List<String> _variableNames = [];
  List<ScriptConstant> _constants = [];

  /// One entry per instruction line; each line is its symbols WITHOUT the EndLine.
  List<List<ScriptSymbol>> _lines = [];

  /// Returns true if any instruction references the given output index as either
  /// an output destination or an operand.
  bool _outputOrVarReferenced(ScriptSymbolType type, int index) {
    for (final line in _lines) {
      if (line.isEmpty) continue;
      if (line.first.type == type && line.first.value == index) return true;
      for (var i = 1; i < line.length; i++) {
        if (line[i].type == type && line[i].value == index) return true;
      }
    }
    return false;
  }

  // Live-run state polled while the editor is open (state, current instruction and
  // the real input/variable/output values).
  Timer? _liveTimer;
  bool _polling = false;
  int? _currentInstruction;
  final Map<int, ({BlockMeta meta, List<int> value})> _liveInputs = {};
  final Map<int, ({BlockMeta meta, List<int> value})> _liveVars = {};
  final Map<int, ({BlockMeta meta, List<int> value})> _liveOutputs = {};

  @override
  void initState() {
    super.initState();
    _load();
    _liveTimer =
        Timer.periodic(const Duration(seconds: 1), (_) => _pollLive());
  }

  @override
  void dispose() {
    _liveTimer?.cancel();
    _nameCtrl.dispose();
    super.dispose();
  }

  @override
  Future<void> onAutoRefresh() async {
    await _refreshState();
  }

  Future<void> _load() async {
    setState(() => _busy = true);
    final bytes = await _client.readScriptFile(widget.scriptId);
    if (!mounted) return;
    setState(() {
      _busy = false;
      // A freshly created script has no file yet (Create only reserves the ID) and a
      // corrupt file is overwritten on save - either way start from a blank script.
      if (bytes == null) {
        _resetToBlank();
        return;
      }
      final file = ScriptFileData.parse(bytes);
      if (file == null) {
        _resetToBlank();
        return;
      }
      _nameCtrl.text = file.name;
      _inputs = List.of(file.inputs);
      _outputNames = List.of(file.outputNames);
      _variableNames = List.of(file.variableNames);
      _constants = List.of(file.constants);
      _lines = splitLines(file.instructions)
          .map((l) => _symbolsOf(l))
          .toList();
    });
    await _refreshState();
  }

  void _resetToBlank() {
    _nameCtrl.text = '';
    _inputs = [];
    _outputNames = [];
    _variableNames = [];
    _constants = [];
    _lines = [
      [const ScriptSymbol(ScriptSymbolType.instruction, 23, 0)] // END
    ];
  }

  /// Extracts a line's symbols (output + op + inputs, or the lone flow terminator).
  List<ScriptSymbol> _symbolsOf(ScriptLine line) {
    if (line.degenerate) {
      return line.op == null
          ? []
          : [ScriptSymbol(ScriptSymbolType.instruction, line.op!.value, 0)];
    }
    return [
      if (line.output != null) line.output!,
      if (line.op != null)
        ScriptSymbol(ScriptSymbolType.instruction, line.op!.value, 0),
      ...line.input,
    ];
  }

  Future<void> _refreshState() async {
    final state = await _client.readState(widget.scriptId);
    if (!mounted || state == null) return;
    setState(() => _state = state);
  }

  /// Polls the running script: state, the current instruction (to highlight in the
  /// instruction editor) and the real input/variable/output values (preview).
  Future<void> _pollLive() async {
    if (_polling || _busy || !mounted) return;
    _polling = true;
    try {
      final id = widget.scriptId;
      final state = await _client.readState(id);
      if (state == null) return;
      final instruction = await _client.getCurrentInstruction(id);

      if (state == ScriptStateCode.stopped) {
        // No instance: nothing to preview.
        if (!mounted) return;
        setState(() {
          _state = state;
          _currentInstruction = null;
          _liveInputs.clear();
          _liveVars.clear();
          _liveOutputs.clear();
        });
        return;
      }

      final inputs = <int, ({BlockMeta meta, List<int> value})>{};
      final vars = <int, ({BlockMeta meta, List<int> value})>{};
      final outs = <int, ({BlockMeta meta, List<int> value})>{};
      for (var i = 0; i < _inputs.length; i++) {
        final v = await _client.readInput(id, i);
        if (v != null) inputs[i] = v;
      }
      for (var i = 0; i < _variableNames.length; i++) {
        final v = await _client.readVariable(id, i);
        if (v != null) vars[i] = v;
      }
      for (var i = 0; i < _outputNames.length; i++) {
        final v = await _client.readOutput(id, i);
        if (v != null) outs[i] = v;
      }
      if (!mounted) return;
      setState(() {
        _state = state;
        _currentInstruction = instruction;
        _liveInputs
          ..clear()
          ..addAll(inputs);
        _liveVars
          ..clear()
          ..addAll(vars);
        _liveOutputs
          ..clear()
          ..addAll(outs);
      });
    } finally {
      _polling = false;
    }
  }

  void _snack(String message) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(message)));
  }

  // --- state controls -------------------------------------------------------

  Future<void> _control(int state) async {
    final ok = await _client.setState(widget.scriptId, state);
    _snack(ok ? 'State: ${ScriptStateCode.label(state)}' : 'State change failed');
    await _refreshState();
  }

  // --- auto-compile ---------------------------------------------------------

  /// Declares any In/Out/Var/Const referenced by an instruction that is not yet
  /// declared (Docs: "constants and variables are automatically compiled in").
  void _ensureCounts() {
    var needInputs = _inputs.length;
    var needOutputs = _outputNames.length;
    var needVars = _variableNames.length;
    var needConsts = _constants.length;
    for (final line in _lines) {
      for (final s in line) {
        switch (s.type) {
          case ScriptSymbolType.input:
            if (s.value >= needInputs) needInputs = s.value + 1;
          case ScriptSymbolType.output:
            if (s.value >= needOutputs) needOutputs = s.value + 1;
          case ScriptSymbolType.variable:
            if (s.value >= needVars) needVars = s.value + 1;
          case ScriptSymbolType.constant:
            if (s.value >= needConsts) needConsts = s.value + 1;
          default:
            break;
        }
      }
    }
    var nextKey =
        _inputs.fold<int>(0, (max, e) => e.key > max ? e.key : max);
    while (_inputs.length < needInputs) {
      _inputs.add(ScriptInput(
          key: ++nextKey,
          flagsAndType: DataType.number.value,
          defaultValue: [0, 0, 0, 0]));
    }
    while (_outputNames.length < needOutputs) {
      _outputNames.add('out${_outputNames.length}');
    }
    while (_variableNames.length < needVars) {
      _variableNames.add('var${_variableNames.length}');
    }
    while (_constants.length < needConsts) {
      _constants.add(
          ScriptConstant(flagsAndType: DataType.number.value, value: [0, 0, 0, 0]));
    }
  }

  // --- saving ---------------------------------------------------------------

  Future<void> _save() async {
    _ensureCounts();
    final symbols = <ScriptSymbol>[];
    for (final line in _lines) {
      if (line.isEmpty) continue;
      symbols.addAll(line);
      symbols.add(const ScriptSymbol(ScriptSymbolType.endLine, 0, 0));
    }

    final validation = validateProgram(symbols,
        inputCount: _inputs.length,
        outputCount: _outputNames.length,
        variableCount: _variableNames.length,
        constantCount: _constants.length);
    if (!validation.ok) {
      _snack(validation.errors.first);
      return;
    }

    final file = ScriptFileData(
      name: _nameCtrl.text.trim(),
      inputs: _inputs,
      outputNames: _outputNames,
      variableNames: _variableNames,
      constants: _constants,
      instructions: symbols,
    );
    final bytes = file.toBytes();
    final ok = await _client.writeScriptFile(widget.scriptId, bytes);
    _snack(ok ? 'Saved (${bytes.length} B)' : 'Save failed');
    if (ok) await _load();
  }

  // --- symbol picking -------------------------------------------------------

  List<String> get _inputItems => [
        for (var i = 0; i < _inputs.length; i++)
          'In$i · ${dataTypeLabel(_inputs[i].dataType)}',
      ];
  List<String> get _outputItems => [
        for (var i = 0; i < _outputNames.length; i++)
          'Out$i${_outputNames[i].isEmpty ? '' : ' · ${_outputNames[i]}'}',
      ];
  List<String> get _variableItems => [
        for (var i = 0; i < _variableNames.length; i++)
          'Var$i${_variableNames[i].isEmpty ? '' : ' · ${_variableNames[i]}'}',
      ];
  List<String> get _constantItems => [
        for (var i = 0; i < _constants.length; i++)
          'Const$i · ${dataTypeLabel(_constants[i].dataType)} · ${formatValue(_constants[i].dataType, _constants[i].value)}',
      ];

  Future<void> _pickOutput(int lineIndex) async {
    final line = _lines[lineIndex];
    final current = line.isNotEmpty && line.first.type != ScriptSymbolType.instruction
        ? line.first
        : const ScriptSymbol(ScriptSymbolType.variable, 0, 0);
    final picked = await showDialog<ScriptSymbol>(
      context: context,
      builder: (_) => _SymbolPicker(
        title: 'Output target',
        current: current,
        allowInput: false,
        allowOutput: true,
        allowVariable: true,
        allowConstant: false,
        allowPredefine: false,
        inputItems: const [],
        outputItems: _outputItems,
        variableItems: _variableItems,
        constantItems: const [],
      ),
    );
    if (picked == null || !mounted) return;
    setState(() {
      if (line.isEmpty || line.first.type == ScriptSymbolType.instruction) {
        line.insert(0, picked);
      } else {
        line[0] = picked;
      }
      _ensureCounts();
    });
  }

  Future<void> _pickOp(int lineIndex) async {
    final line = _lines[lineIndex];
    final current = line.isNotEmpty
        ? (line.first.type == ScriptSymbolType.instruction
            ? line.first.subtype
            : (line.length > 1 ? line[1].subtype : -1))
        : -1;
    final picked = await showDialog<int>(
      context: context,
      builder: (_) => _OpPicker(current: current),
    );
    if (picked == null || !mounted) return;
    setState(() {
      final opSymbol = ScriptSymbol(ScriptSymbolType.instruction, picked, 0);
      final isTerminator = picked == ScriptOpcode.end.value ||
          picked == ScriptOpcode.endIf.value ||
          picked == ScriptOpcode.endWhile.value;
      if (line.isEmpty || line.first.type == ScriptSymbolType.instruction) {
        // Degenerate (or empty) line.
        if (isTerminator) {
          line
            ..clear()
            ..add(opSymbol);
        } else {
          line
            ..clear()
            ..add(const ScriptSymbol(ScriptSymbolType.variable, 0, 0))
            ..add(opSymbol);
        }
      } else {
        if (isTerminator) {
          line
            ..clear()
            ..add(opSymbol);
        } else if (line.length == 1) {
          line.add(opSymbol);
        } else {
          line[1] = opSymbol;
        }
      }
      _ensureCounts();
    });
  }

  Future<void> _editOperand(int lineIndex, int operandIndex) async {
    final line = _lines[lineIndex];
    if (line.length < operandIndex + 1) return;
    final picked = await _pickOperandSymbol(line[operandIndex]);
    if (picked == null || !mounted) return;
    setState(() => line[operandIndex] = picked);
  }

  Future<void> _addOperand(int lineIndex) async {
    final picked = await _pickOperandSymbol(null);
    if (picked == null || !mounted) return;
    setState(() {
      _lines[lineIndex].add(picked);
      _ensureCounts();
    });
  }

  Future<ScriptSymbol?> _pickOperandSymbol(ScriptSymbol? current) async {
    final picked = await showDialog<ScriptSymbol>(
      context: context,
      builder: (_) => _SymbolPicker(
        title: 'Operand',
        current: current,
        allowInput: true,
        allowOutput: true,
        allowVariable: true,
        allowConstant: true,
        allowPredefine: true,
        inputItems: _inputItems,
        outputItems: _outputItems,
        variableItems: _variableItems,
        constantItems: _constantItems,
      ),
    );
    if (picked != null) _ensureCounts();
    return picked;
  }

  // --- add line templates ---------------------------------------------------

  void _addLine() {
    setState(() => _lines.add([
          const ScriptSymbol(ScriptSymbolType.variable, 0, 0),
          const ScriptSymbol(ScriptSymbolType.instruction, 0, 0), // ADD
          const ScriptSymbol(ScriptSymbolType.input, 0, 0),
          const ScriptSymbol(ScriptSymbolType.constant, 0, 0),
        ]));
  }

  void _addTemplate(int index) {
    final templates = <String, List<ScriptSymbol>>{
      'ADD (Var = In + Const)': [
        ScriptSymbol(ScriptSymbolType.variable, 0, 0),
        ScriptSymbol(ScriptSymbolType.instruction, 0, 0),
        ScriptSymbol(ScriptSymbolType.input, 0, 0),
        ScriptSymbol(ScriptSymbolType.constant, 0, 0),
      ],
      'Compare (Out = Var == Value)': [
        ScriptSymbol(ScriptSymbolType.output, 0, 0),
        ScriptSymbol(ScriptSymbolType.instruction, 8, 0), // EQ
        ScriptSymbol(ScriptSymbolType.variable, 0, 0),
        ScriptSymbol(ScriptSymbolType.predefine, PredefineSubtype.index_.value, 0),
      ],
      'IF (condition)': [
        ScriptSymbol(ScriptSymbolType.variable, 0, 0),
        ScriptSymbol(ScriptSymbolType.instruction, 19, 0), // IF
        ScriptSymbol(ScriptSymbolType.input, 0, 0),
        ScriptSymbol(ScriptSymbolType.predefine, PredefineSubtype.bool_.value, 1),
      ],
      'END_IF': [
        ScriptSymbol(ScriptSymbolType.instruction, 21, 0),
      ],
      'WHILE (condition)': [
        ScriptSymbol(ScriptSymbolType.variable, 0, 0),
        ScriptSymbol(ScriptSymbolType.instruction, 20, 0), // WHILE
        ScriptSymbol(ScriptSymbolType.input, 0, 0),
        ScriptSymbol(ScriptSymbolType.predefine, PredefineSubtype.bool_.value, 1),
      ],
      'END_WHILE': [
        ScriptSymbol(ScriptSymbolType.instruction, 22, 0),
      ],
      'Delay (Var = DELAY ms)': [
        ScriptSymbol(ScriptSymbolType.variable, 0, 0),
        ScriptSymbol(ScriptSymbolType.instruction, 24, 0), // DELAY
        ScriptSymbol(ScriptSymbolType.predefine, PredefineSubtype.index_.value, 1000),
      ],
      'Get time (Var = GET_TIME)': [
        ScriptSymbol(ScriptSymbolType.variable, 0, 0),
        ScriptSymbol(ScriptSymbolType.instruction, 25, 0),
      ],
      'Mem write (Var = MEM_WRITE In0 addr)': [
        ScriptSymbol(ScriptSymbolType.variable, 0, 0),
        ScriptSymbol(ScriptSymbolType.instruction, 18, 0), // MEM_WRITE
        ScriptSymbol(ScriptSymbolType.input, 0, 0),
        ScriptSymbol(ScriptSymbolType.constant, 0, 0),
      ],
      'Mem read (Var = MEM_READ addr)': [
        ScriptSymbol(ScriptSymbolType.variable, 0, 0),
        ScriptSymbol(ScriptSymbolType.instruction, 17, 0), // MEM_READ
        ScriptSymbol(ScriptSymbolType.constant, 0, 0),
      ],
      'END': [
        ScriptSymbol(ScriptSymbolType.instruction, 23, 0),
      ],
      'Blank line': [],
    };
    final entries = templates.entries.toList();
    showModalBottomSheet<void>(
      context: context,
      builder: (context) => SafeArea(
        child: ListView(
          shrinkWrap: true,
          children: [
            for (final e in entries)
              ListTile(
                dense: true,
                title: Text(e.key),
                onTap: () {
                  Navigator.pop(context);
                  setState(() => _lines.insert(index, e.value));
                },
              ),
          ],
        ),
      ),
    );
  }

  // --- UI -------------------------------------------------------------------

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text(_nameCtrl.text.isEmpty ? 'Script ${widget.scriptId}' : _nameCtrl.text),
        actions: [
          _stateBadge(),
          IconButton(
              icon: const Icon(Icons.play_arrow),
              tooltip: 'Start',
              onPressed: () => _control(ScriptStateCode.running)),
          IconButton(
              icon: const Icon(Icons.pause),
              tooltip: 'Pause',
              onPressed: _state == ScriptStateCode.running
                  ? () => _control(ScriptStateCode.paused)
                  : null),
          IconButton(
              icon: const Icon(Icons.play_arrow_outlined),
              tooltip: 'Resume',
              onPressed: _state == ScriptStateCode.paused
                  ? () => _control(ScriptStateCode.running)
                  : null),
          IconButton(
              icon: const Icon(Icons.stop),
              tooltip: 'Stop',
              onPressed: () => _control(ScriptStateCode.stopped)),
          IconButton(
              icon: const Icon(Icons.save_outlined),
              tooltip: 'Save',
              onPressed: _busy ? null : _save),
        ],
      ),
      body: _busy
          ? const Center(child: CircularProgressIndicator())
          : _buildBody(),
    );
  }

  Widget _stateBadge() {
    final color = switch (_state) {
      ScriptStateCode.running => const Color(0xFF4CAF50),
      ScriptStateCode.waiting => const Color(0xFFFFC107),
      ScriptStateCode.error => const Color(0xFFF44336),
      ScriptStateCode.finished => const Color(0xFF90A4AE),
      ScriptStateCode.paused => const Color(0xFFFFA726),
      _ => kOrange,
    };
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 8),
      child: Center(
        child: Container(
          padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
          decoration: BoxDecoration(
              color: color.withAlpha(40), borderRadius: BorderRadius.circular(4)),
          child: Text(ScriptStateCode.label(_state),
              style: TextStyle(fontSize: 11, color: color)),
        ),
      ),
    );
  }

  Widget _buildBody() {
    return ListView(
      padding: const EdgeInsets.fromLTRB(10, 10, 10, 24),
      children: [
        _card('Name', [
          TextField(
            controller: _nameCtrl,
            onChanged: (_) => setState(() {}),
            decoration: const InputDecoration(
                labelText: 'Script name (max 16)',
                border: OutlineInputBorder()),
            maxLength: 16,
          ),
        ]),
        const SizedBox(height: 8),
        _card('Inputs (dictionary)', [
          for (var i = 0; i < _inputs.length; i++) _inputRow(i),
          TextButton.icon(
              onPressed: () => _addInput(),
              icon: const Icon(Icons.add),
              label: const Text('Add input')),
        ]),
        const SizedBox(height: 8),
        _card('Outputs (${_outputNames.length})', [
          for (var i = 0; i < _outputNames.length; i++) _outputRow(i),
          TextButton.icon(
              onPressed: () => _addName(_outputNames, 'output'),
              icon: const Icon(Icons.add),
              label: const Text('Add output')),
        ]),
        const SizedBox(height: 8),
        _card('Variables (${_variableNames.length})', [
          for (var i = 0; i < _variableNames.length; i++) _variableRow(i),
          TextButton.icon(
              onPressed: () => _addName(_variableNames, 'variable'),
              icon: const Icon(Icons.add),
              label: const Text('Add variable')),
        ]),
        const SizedBox(height: 8),
        _card('Constants (${_constants.length})', [
          for (var i = 0; i < _constants.length; i++) _constantRow(i),
          TextButton.icon(
              onPressed: () => _addConstant(),
              icon: const Icon(Icons.add),
              label: const Text('Add constant')),
        ]),
        const SizedBox(height: 8),
        // The instruction editor sits at the bottom of the page.
        _instructionsCard(),
      ],
    );
  }

  Widget _card(String title, List<Widget> children) {
    return Card(
      color: kSurfaceAlt,
      margin: EdgeInsets.zero,
      child: Padding(
        padding: const EdgeInsets.all(10),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text(title.toUpperCase(),
                style: TextStyle(color: kOrange, fontSize: 12, letterSpacing: 1)),
            const SizedBox(height: 6),
            ...children,
          ],
        ),
      ),
    );
  }

  // --- inputs (dictionary cards) --------------------------------------------

  Future<void> _addInput() async {
    final nextKey =
        _inputs.fold<int>(0, (max, e) => e.key > max ? e.key : max) + 1;
    final input = ScriptInput(
        key: nextKey,
        flagsAndType: DataType.number.value,
        defaultValue: [0, 0, 0, 0]);
    final edited = await showDialog<bool>(
      context: context,
      builder: (_) => _InputDialog(
          index: _inputs.length, input: input, initialKey: nextKey),
    );
    if (edited == true && mounted) {
      setState(() => _inputs.add(input));
    }
  }

  Widget _inputRow(int index) {
    final input = _inputs[index];
    final type = input.dataType;
    final live = _liveInputs[index];
    return Card(
      color: Colors.black.withAlpha(24),
      margin: const EdgeInsets.only(bottom: 6),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          ListTile(
            contentPadding: const EdgeInsets.fromLTRB(10, 0, 4, 0),
            dense: true,
            title: Text('Input $index · Key ${input.key}',
                style:
                    const TextStyle(fontWeight: FontWeight.w600, fontSize: 13)),
            subtitle: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text('Type: ${dataTypeLabel(type)} · Default: ${formatValue(type, input.defaultValue)}',
                    style: const TextStyle(fontFamily: 'monospace', fontSize: 12)),
                Text('Style: ${input.style.label}',
                    style: const TextStyle(fontSize: 11, color: Colors.white60)),
              ],
            ),
            trailing: Row(mainAxisSize: MainAxisSize.min, children: [
              IconButton(
                  icon: const Icon(Icons.edit_outlined, size: 18),
                  tooltip: 'Edit definition',
                  onPressed: () => _editInput(index)),
              IconButton(
                  icon: const Icon(Icons.delete_outline, size: 18),
                  onPressed: () => setState(() => _inputs.removeAt(index))),
            ]),
            onTap: () => _editInput(index),
          ),
          // The same live interaction control as on the script list, so an input
          // can be driven from the editor while the script runs.
          Padding(
            padding: const EdgeInsets.only(left: 10, right: 10, bottom: 6),
            child: ScriptInputControl(
              index: index,
              input: input,
              liveValue: live?.value,
              onWrite: (value) => _writeInputValue(index, value),
            ),
          ),
        ],
      ),
    );
  }

  /// Writes a live value to the script's input (wakes a Waiting script too). The
  /// write is fire-and-forget (the 1 s poll reconciles and echoes the value); the
  /// slider's `_pending` keeps the thumb where the user left it until then.
  Future<void> _writeInputValue(int index, List<int> value) async {
    final input = _inputs[index];
    final meta =
        BlockMeta(flagsAndType: input.dataType.value, size: value.length);
    final ok = await _client.writeInput(widget.scriptId, index, meta, value);
    if (!ok) _snack('Input write failed');
  }

  Future<void> _editInput(int index) async {
    final edited = await showDialog<bool>(
      context: context,
      builder: (_) => _InputDialog(
          index: index, input: _inputs[index], initialKey: _inputs[index].key),
    );
    if (edited == true && mounted) setState(() {});
  }

  // --- outputs / variables / constants --------------------------------------

  Widget _outputRow(int index) {
    final live = _liveOutputs[index];
    final name = _outputNames[index];
    return ListTile(
      contentPadding: EdgeInsets.zero,
      dense: true,
      leading: const Icon(Icons.output, size: 18),
      title: Text('Out$index: ${name.isEmpty ? '(unnamed)' : name}',
          style: const TextStyle(fontFamily: 'monospace')),
      subtitle: live != null &&
              live.meta.dataType != DataType.none &&
              live.value.isNotEmpty
          ? Text('Live: ${formatValue(live.meta.dataType, live.value)}',
              style: const TextStyle(
                  fontSize: 11, color: Color(0xFF4CAF50), fontFamily: 'monospace'))
          : null,
      trailing: Row(mainAxisSize: MainAxisSize.min, children: [
        IconButton(
            icon: const Icon(Icons.edit_outlined, size: 18),
            onPressed: () => _renameName(_outputNames, index)),
        IconButton(
            icon: const Icon(Icons.delete_outline, size: 18),
            tooltip: _outputOrVarReferenced(ScriptSymbolType.output, index)
                ? 'Remove instruction references first'
                : 'Remove output',
            onPressed: _outputOrVarReferenced(ScriptSymbolType.output, index)
                ? null
                : () => setState(() => _outputNames.removeAt(index))),
      ]),
    );
  }

  Widget _variableRow(int index) {
    final live = _liveVars[index];
    final name = _variableNames[index];
    return ListTile(
      contentPadding: EdgeInsets.zero,
      dense: true,
      leading: const Icon(Icons.data_object, size: 18),
      title: Text('Var$index: ${name.isEmpty ? '(unnamed)' : name}',
          style: const TextStyle(fontFamily: 'monospace')),
      subtitle: live != null &&
              live.meta.dataType != DataType.none &&
              live.value.isNotEmpty
          ? Text('Live: ${formatValue(live.meta.dataType, live.value)}',
              style: const TextStyle(
                  fontSize: 11, color: Color(0xFF4CAF50), fontFamily: 'monospace'))
          : null,
      trailing: Row(mainAxisSize: MainAxisSize.min, children: [
        IconButton(
            icon: const Icon(Icons.tune, size: 18),
            tooltip: 'Write live variable value',
            onPressed: () => _writeLiveVariable(index)),
        IconButton(
            icon: const Icon(Icons.edit_outlined, size: 18),
            onPressed: () => _renameName(_variableNames, index)),
        IconButton(
            icon: const Icon(Icons.delete_outline, size: 18),
            tooltip: _outputOrVarReferenced(ScriptSymbolType.variable, index)
                ? 'Remove instruction references first'
                : 'Remove variable',
            onPressed: _outputOrVarReferenced(ScriptSymbolType.variable, index)
                ? null
                : () => setState(() => _variableNames.removeAt(index))),
      ]),
    );
  }

  /// Debug-writes a running script's variable (CID 10).
  Future<void> _writeLiveVariable(int index) async {
    final live = _liveVars[index];
    final type = live?.meta.dataType ?? DataType.number;
    final value = await showValueEditor(context, type, live?.value ?? [0, 0, 0, 0]);
    if (value == null || !mounted) return;
    final meta = BlockMeta(flagsAndType: type.value, size: value.length);
    final ok = await _client.writeVariable(widget.scriptId, index, meta, value);
    _snack(ok ? 'Variable written' : 'Write failed');
    if (ok) await _pollLive();
  }

  Future<void> _addName(List<String> list, String kind) async {
    final controller = TextEditingController();
    final result = await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text('New $kind'),
        content: TextField(controller: controller, autofocus: true),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(
              onPressed: () => Navigator.pop(context, controller.text),
              child: const Text('Add')),
        ],
      ),
    );
    controller.dispose();
    if (result == null || !mounted) return;
    setState(() => list.add(result.trim()));
  }

  Future<void> _renameName(List<String> list, int index) async {
    final controller = TextEditingController(text: list[index]);
    final result = await showDialog<String>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Rename'),
        content: TextField(controller: controller, autofocus: true),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(
              onPressed: () => Navigator.pop(context, controller.text),
              child: const Text('OK')),
        ],
      ),
    );
    controller.dispose();
    if (result == null || !mounted) return;
    setState(() => list[index] = result.trim());
  }

  Widget _constantRow(int index) {
    final c = _constants[index];
    final type = DataType.fromValue(c.flagsAndType & 0x03FF);
    return ListTile(
      contentPadding: EdgeInsets.zero,
      dense: true,
      leading: const Icon(Icons.data_object, size: 18),
      title: Text(
          'Const$index: ${dataTypeLabel(type)} · ${formatValue(type, c.value)}',
          style: const TextStyle(fontFamily: 'monospace', fontSize: 13)),
      trailing: Row(mainAxisSize: MainAxisSize.min, children: [
        IconButton(
            icon: const Icon(Icons.edit_outlined, size: 18),
            onPressed: () => _editConstant(index)),
        IconButton(
            icon: const Icon(Icons.delete_outline, size: 18),
            onPressed: () => setState(() => _constants.removeAt(index))),
      ]),
    );
  }

  Future<void> _addConstant() async {
    final constant = ScriptConstant(
        flagsAndType: DataType.number.value, value: [0, 0, 0, 0]);
    final ok = await _editConstantValue(constant, 'New constant');
    if (ok && mounted) setState(() => _constants.add(constant));
  }

  Future<void> _editConstant(int index) async {
    final ok = await _editConstantValue(_constants[index], 'Edit constant');
    if (ok && mounted) setState(() {});
  }

  Future<bool> _editConstantValue(ScriptConstant c, String title) async {
    final type = await showDialog<DataType>(
      context: context,
      builder: (context) => _TypePicker(current: c.dataType),
    );
    if (type == null || !mounted) return false;
    final value = await showValueEditor(context, type, c.value);
    if (value == null || !mounted) return false;
    setState(() {
      c
        ..flagsAndType = type.value
        ..value = value;
    });
    return true;
  }

  // --- instructions (symbol-based builder) ----------------------------------

  Widget _instructionsCard() {
    return Card(
      color: kSurfaceAlt,
      margin: EdgeInsets.zero,
      child: Padding(
        padding: const EdgeInsets.all(10),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text('INSTRUCTIONS (${_lines.length})',
                style:
                    TextStyle(color: kOrange, fontSize: 12, letterSpacing: 1)),
            const SizedBox(height: 6),
            Text('Tap a chip to change it; indexes are declared automatically.',
                style: TextStyle(fontSize: 11, color: Colors.white54)),
            const SizedBox(height: 6),
            if (_lines.isEmpty)
              const Padding(
                padding: EdgeInsets.symmetric(vertical: 8),
                child: Text('No instructions yet.'),
              )
            else
              for (var i = 0; i < _lines.length; i++) _lineBuilder(i),
            const SizedBox(height: 4),
            Row(children: [
              Expanded(
                child: OutlinedButton.icon(
                    onPressed: _addLine,
                    icon: const Icon(Icons.add),
                    label: const Text('Add line')),
              ),
              const SizedBox(width: 8),
              OutlinedButton.icon(
                  onPressed: () => _addTemplate(_lines.length),
                  icon: const Icon(Icons.playlist_add),
                  label: const Text('Template')),
            ]),
          ],
        ),
      ),
    );
  }

  Widget _lineBuilder(int index) {
    final line = _lines[index];
    final degenerate =
        line.isNotEmpty && line.first.type == ScriptSymbolType.instruction;
    final opSubtype = degenerate
        ? line.first.subtype
        : (line.length > 1 ? line[1].subtype : null);
    final op = opSubtype == null ? null : ScriptOpcode.fromValue(opSubtype);
    final isCurrent = index == _currentInstruction;

    return Card(
      color: Colors.black.withAlpha(24),
      shape: isCurrent
          ? RoundedRectangleBorder(
              side: const BorderSide(color: kOrange, width: 2),
              borderRadius: BorderRadius.circular(8))
          : null,
      margin: const EdgeInsets.only(bottom: 6),
      child: Padding(
        padding: const EdgeInsets.fromLTRB(8, 6, 8, 6),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(children: [
              Text(isCurrent ? '▶ ${index + 1}' : '#${index + 1}',
                  style: TextStyle(
                      fontSize: 11,
                      color: isCurrent ? kOrange : Colors.white54,
                      fontWeight: isCurrent ? FontWeight.bold : FontWeight.normal)),
              const Spacer(),
              IconButton(
                  visualDensity: VisualDensity.compact,
                  icon: const Icon(Icons.copy_all, size: 16),
                  tooltip: 'Duplicate line',
                  onPressed: () => setState(() => _lines.insert(index + 1, List.of(line)))),
              IconButton(
                  visualDensity: VisualDensity.compact,
                  icon: const Icon(Icons.arrow_upward, size: 16),
                  onPressed: index == 0
                      ? null
                      : () => setState(() {
                            final tmp = _lines[index - 1];
                            _lines[index - 1] = line;
                            _lines[index] = tmp;
                          })),
              IconButton(
                  visualDensity: VisualDensity.compact,
                  icon: const Icon(Icons.arrow_downward, size: 16),
                  onPressed: index == _lines.length - 1
                      ? null
                      : () => setState(() {
                            final tmp = _lines[index + 1];
                            _lines[index + 1] = line;
                            _lines[index] = tmp;
                          })),
              IconButton(
                  visualDensity: VisualDensity.compact,
                  icon: const Icon(Icons.close, size: 16),
                  onPressed: () => setState(() => _lines.removeAt(index))),
            ]),
            Wrap(
              spacing: 6,
              runSpacing: 6,
              crossAxisAlignment: WrapCrossAlignment.center,
              children: [
                if (degenerate)
                  _chip(op?.label ?? '?', Colors.white70,
                      () => _pickOp(index), hint: op?.hint)
                else ...[
                  _chip(
                      _symbolChipLabel(line.first),
                      _symbolChipColor(line.first),
                      () => _pickOutput(index),
                      hint: 'Output target'),
                  if (line.length < 2)
                    _chip('op', kOrange, () => _pickOp(index), hint: 'Choose an instruction')
                  else
                    _chip(op?.label ?? '?', kOrange, () => _pickOp(index),
                        hint: op?.hint),
                  for (var oi = 2; oi < line.length; oi++)
                    _chip(_symbolChipLabel(line[oi]), _symbolChipColor(line[oi]),
                        () => _editOperand(index, oi),
                        hint: _operandHint(line[oi]), onDelete: () {
                      setState(() => line.removeAt(oi));
                    }),
                  _chip('+ operand', Colors.white24, () => _addOperand(index)),
                ],
              ],
            ),
            if (op != null)
              Padding(
                padding: const EdgeInsets.only(top: 4),
                child: Text(op.hint,
                    style: const TextStyle(fontSize: 11, color: Colors.white54)),
              ),
          ],
        ),
      ),
    );
  }

  String _symbolChipLabel(ScriptSymbol s) {
    switch (s.type) {
      case ScriptSymbolType.input:
        return 'In${s.value}';
      case ScriptSymbolType.output:
        if (_outputNames.length > s.value && _outputNames[s.value].isNotEmpty) {
          return _outputNames[s.value];
        }
        return 'Out${s.value}';
      case ScriptSymbolType.variable:
        if (_variableNames.length > s.value && _variableNames[s.value].isNotEmpty) {
          return _variableNames[s.value];
        }
        return 'Var${s.value}';
      case ScriptSymbolType.constant:
        return 'Const${s.value}';
      case ScriptSymbolType.predefine:
        return s.predefineText;
      default:
        return s.operandText;
    }
  }

  Color _symbolChipColor(ScriptSymbol s) => switch (s.type) {
        ScriptSymbolType.input => const Color(0xFF81C784),
        ScriptSymbolType.output => kOrange,
        ScriptSymbolType.variable => const Color(0xFF64B5F6),
        ScriptSymbolType.constant => const Color(0xFFBA68C8),
        ScriptSymbolType.predefine => const Color(0xFF4DD0E1),
        _ => Colors.white70,
      };

  String _operandHint(ScriptSymbol s) => switch (s.type) {
        ScriptSymbolType.input =>
          'Input ${s.value} (${_inputs.length > s.value ? dataTypeLabel(_inputs[s.value].dataType) : 'auto-declared'})',
        ScriptSymbolType.output =>
          'Output ${s.value}${_outputNames.length > s.value && _outputNames[s.value].isNotEmpty ? ' · ${_outputNames[s.value]}' : ''}',
        ScriptSymbolType.variable =>
          'Variable ${s.value}${_variableNames.length > s.value && _variableNames[s.value].isNotEmpty ? ' · ${_variableNames[s.value]}' : ''}',
        ScriptSymbolType.constant =>
          'Constant ${s.value} (${_constants.length > s.value ? dataTypeLabel(_constants[s.value].dataType) : 'auto-declared'})',
        ScriptSymbolType.predefine => 'Predefine literal',
        _ => '',
      };

  Widget _chip(String label, Color color, VoidCallback onTap,
      {String? hint, VoidCallback? onDelete}) {
    return Tooltip(
      message: hint ?? '',
      child: InputChip(
        label: Text(label, style: TextStyle(fontSize: 12, color: color)),
        visualDensity: VisualDensity.compact,
        backgroundColor: color.withAlpha(24),
        side: BorderSide(color: color.withAlpha(80)),
        onPressed: onTap,
        onDeleted: onDelete,
      ),
    );
  }
}

/// Picks an instruction: the recommended instructions are listed first, then category
/// buttons that open the smaller per-category lists. Search still finds anything.
class _OpPicker extends StatefulWidget {
  final int? current;
  const _OpPicker({this.current});

  @override
  State<_OpPicker> createState() => _OpPickerState();
}

class _OpPickerState extends State<_OpPicker> {
  String _query = '';
  OpCategory? _category;
  final TextEditingController _search = TextEditingController();

  @override
  void dispose() {
    _search.dispose();
    super.dispose();
  }

  List<ScriptOpcode> get _matching {
    final q = _query.trim().toLowerCase();
    if (q.isEmpty) return ScriptOpcode.values;
    return [
      for (final op in ScriptOpcode.values)
        if (op.label.toLowerCase().contains(q) || op.hint.toLowerCase().contains(q)) op
    ];
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: Text(_category == null ? 'Choose instruction' : _category!.label),
      content: SizedBox(
        width: 400,
        height: 430,
        child: Column(children: [
          TextField(
            controller: _search,
            autofocus: false,
            onChanged: (v) => setState(() => _query = v),
            decoration: const InputDecoration(
                hintText: 'Search (name or hint)', prefixIcon: Icon(Icons.search)),
          ),
          const SizedBox(height: 8),
          Expanded(child: _category == null ? _home() : _categoryList(_category!)),
        ]),
      ),
      actions: [
        if (_category != null)
          TextButton(
              onPressed: () => setState(() => _category = null),
              child: const Text('Categories')),
        TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
      ],
    );
  }

  /// First screen: recommended instructions + category buttons.
  Widget _home() {
    if (_query.trim().isNotEmpty) {
      final matching = _matching;
      return matching.isEmpty
          ? const Center(child: Text('No matching instructions'))
          : ListView(children: [for (final op in matching) _opTile(op)]);
    }
    return ListView(children: [
      const _SectionHeader('Recommended'),
      for (final op in ScriptOpcode.values)
        if (op.recommended) _opTile(op),
      const SizedBox(height: 4),
      const _SectionHeader('Categories'),
      Padding(
        padding: const EdgeInsets.symmetric(vertical: 4),
        child: Wrap(
          spacing: 8,
          runSpacing: 8,
          children: [
            for (final category in OpCategory.values)
              ActionChip(
                avatar: Icon(_categoryIcon(category), size: 16),
                label: Text(category.label),
                onPressed: () => setState(() => _category = category),
              ),
          ],
        ),
      ),
    ]);
  }

  /// A category's smaller list (recommended ops stay on the home screen).
  Widget _categoryList(OpCategory category) {
    final ops = [
      for (final op in ScriptOpcode.values)
        if (op.category == category && !op.recommended) op
    ];
    if (ops.isEmpty) {
      return const Center(child: Text('No extra instructions in this category'));
    }
    return ListView(children: [
      for (final op in ops) _opTile(op),
    ]);
  }

  IconData _categoryIcon(OpCategory category) => switch (category) {
        OpCategory.math => Icons.functions,
        OpCategory.logic => Icons.account_tree,
        OpCategory.compare => Icons.compare_arrows,
        OpCategory.compose => Icons.palette_outlined,
        OpCategory.memory => Icons.memory,
        OpCategory.flow => Icons.route,
        OpCategory.time => Icons.timer_outlined,
        OpCategory.state => Icons.pause_circle_outline,
        OpCategory.macro => Icons.call_split,
      };

  Widget _opTile(ScriptOpcode op) {
    final selected = op.value == widget.current;
    return ListTile(
      dense: true,
      selected: selected,
      leading: Icon(op.recommended ? Icons.star : Icons.settings, size: 16),
      title: Text(op.label, style: const TextStyle(fontFamily: 'monospace')),
      subtitle: Text(op.hint,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: const TextStyle(fontSize: 11, color: Colors.white54)),
      onTap: () => Navigator.pop(context, op.value),
    );
  }
}

class _SectionHeader extends StatelessWidget {
  final String text;
  const _SectionHeader(this.text);
  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.only(top: 8, bottom: 2),
      child: Text(text.toUpperCase(),
          style: TextStyle(color: kOrange, fontSize: 11, letterSpacing: 1)),
    );
  }
}

/// Picks a symbol (output or operand) by NAME: choose a category (Input/Output/
/// Variable/Constant or a Predefine literal), then one of the already-declared
/// entries (labelled by name/type/value) or "add new" - never a raw index.
class _SymbolPicker extends StatefulWidget {
  final String title;
  final ScriptSymbol? current;
  final bool allowInput;
  final bool allowOutput;
  final bool allowVariable;
  final bool allowConstant;
  final bool allowPredefine;
  final List<String> inputItems;
  final List<String> outputItems;
  final List<String> variableItems;
  final List<String> constantItems;

  const _SymbolPicker({
    required this.title,
    required this.current,
    required this.allowInput,
    required this.allowOutput,
    required this.allowVariable,
    required this.allowConstant,
    required this.allowPredefine,
    required this.inputItems,
    required this.outputItems,
    required this.variableItems,
    required this.constantItems,
  });

  @override
  State<_SymbolPicker> createState() => _SymbolPickerState();
}

class _SymbolPickerState extends State<_SymbolPicker> {
  late ScriptSymbolType _kind;
  late PredefineSubtype _preSub;
  late final TextEditingController _preValueCtrl;

  @override
  void initState() {
    super.initState();
    final current = widget.current;
    _kind = current?.type ?? _firstAllowed();
    if (!_kindAllowed(_kind)) _kind = _firstAllowed();
    _preSub = PredefineSubtype.index_;
    _preValueCtrl = TextEditingController(
        text: '${_kind == ScriptSymbolType.predefine ? current?.value ?? 0 : 0}');
  }

  @override
  void dispose() {
    _preValueCtrl.dispose();
    super.dispose();
  }

  bool _kindAllowed(ScriptSymbolType kind) => switch (kind) {
        ScriptSymbolType.input => widget.allowInput,
        ScriptSymbolType.output => widget.allowOutput,
        ScriptSymbolType.variable => widget.allowVariable,
        ScriptSymbolType.constant => widget.allowConstant,
        ScriptSymbolType.predefine => widget.allowPredefine,
        _ => false,
      };

  ScriptSymbolType _firstAllowed() {
    const order = [
      ScriptSymbolType.input,
      ScriptSymbolType.output,
      ScriptSymbolType.variable,
      ScriptSymbolType.constant,
      ScriptSymbolType.predefine,
    ];
    return order.firstWhere(_kindAllowed);
  }

  List<String> get _items => switch (_kind) {
        ScriptSymbolType.input => widget.inputItems,
        ScriptSymbolType.output => widget.outputItems,
        ScriptSymbolType.variable => widget.variableItems,
        ScriptSymbolType.constant => widget.constantItems,
        _ => const [],
      };

  String get _kindName => switch (_kind) {
        ScriptSymbolType.input => 'input',
        ScriptSymbolType.output => 'output',
        ScriptSymbolType.variable => 'variable',
        ScriptSymbolType.constant => 'constant',
        ScriptSymbolType.predefine => 'predefine',
        _ => 'symbol',
      };

  IconData get _kindIcon => switch (_kind) {
        ScriptSymbolType.input => Icons.input,
        ScriptSymbolType.output => Icons.output,
        ScriptSymbolType.variable => Icons.data_object,
        ScriptSymbolType.constant => Icons.lock_outline,
        _ => Icons.bolt,
      };

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: Text(widget.title),
      content: SingleChildScrollView(
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          // Category selector first: I/O/V/C (+ predefine).
          Wrap(
            spacing: 6,
            runSpacing: 6,
            children: [
              if (widget.allowInput)
                _kindChip(ScriptSymbolType.input, 'Inputs (I)'),
              if (widget.allowOutput)
                _kindChip(ScriptSymbolType.output, 'Outputs (O)'),
              if (widget.allowVariable)
                _kindChip(ScriptSymbolType.variable, 'Variables (V)'),
              if (widget.allowConstant)
                _kindChip(ScriptSymbolType.constant, 'Constants (C)'),
              if (widget.allowPredefine)
                _kindChip(ScriptSymbolType.predefine, 'Predefine'),
            ],
          ),
          const SizedBox(height: 10),
          if (_kind == ScriptSymbolType.predefine)
            Column(children: [
              DropdownButtonFormField<PredefineSubtype>(
                initialValue: _preSub,
                decoration: const InputDecoration(labelText: 'Predefine'),
                items: [
                  for (final s in PredefineSubtype.values)
                    DropdownMenuItem(
                        value: s,
                        child: Text(switch (s) {
                          PredefineSubtype.bool_ => 'Bool',
                          PredefineSubtype.char_ => 'Char',
                          PredefineSubtype.index_ => 'Index',
                          PredefineSubtype.state => 'State',
                          PredefineSubtype.type => 'Type',
                          PredefineSubtype.mathOp => 'Math op',
                        })),
                ],
                onChanged: (v) {
                  if (v == null) return;
                  setState(() => _preSub = v);
                },
              ),
              const SizedBox(height: 10),
              TextField(
                controller: _preValueCtrl,
                keyboardType: TextInputType.number,
                decoration: const InputDecoration(
                    labelText: 'Value (0-65535)', border: OutlineInputBorder()),
              ),
              const SizedBox(height: 10),
              SizedBox(
                width: double.infinity,
                child: FilledButton.icon(
                  onPressed: () {
                    final value = int.tryParse(_preValueCtrl.text);
                    if (value == null || value < 0 || value > 0xFFFF) return;
                    Navigator.pop(context,
                        ScriptSymbol(ScriptSymbolType.predefine, _preSub.value, value));
                  },
                  icon: const Icon(Icons.add),
                  label: const Text('Add predefine'),
                ),
              ),
            ])
          else
            SizedBox(
              width: 360,
              height: 300,
              child: ListView(
                children: [
                  if (_items.isEmpty)
                    Padding(
                      padding: const EdgeInsets.all(12),
                      child: Text('No ${_kindName}s yet - add one below.'),
                    )
                  else
                    for (var i = 0; i < _items.length; i++)
                      ListTile(
                        dense: true,
                        selected: widget.current?.type == _kind &&
                            widget.current?.value == i,
                        leading: Icon(_kindIcon, size: 18),
                        title: Text(_items[i],
                            style: const TextStyle(fontFamily: 'monospace')),
                        onTap: () => Navigator.pop(context, ScriptSymbol(_kind, 0, i)),
                      ),
                  ListTile(
                    dense: true,
                    leading: const Icon(Icons.add, size: 18, color: kOrange),
                    title: Text('Add new $_kindName',
                        style: TextStyle(color: kOrange)),
                    onTap: () =>
                        Navigator.pop(context, ScriptSymbol(_kind, 0, _items.length)),
                  ),
                ],
              ),
            ),
        ]),
      ),
      actions: [
        TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
      ],
    );
  }

  Widget _kindChip(ScriptSymbolType kind, String label) {
    final selected = _kind == kind;
    return ChoiceChip(
      label: Text(label,
          style: TextStyle(
              fontSize: 12, color: selected ? Colors.black : Colors.white70)),
      selected: selected,
      selectedColor: kOrange,
      onSelected: (_) => setState(() => _kind = kind),
    );
  }
}

/// Edits one input's dictionary entry: key, expected type, default value, style.
class _InputDialog extends StatefulWidget {
  final int index;
  final ScriptInput input;
  final int initialKey;

  const _InputDialog(
      {required this.index, required this.input, required this.initialKey});

  @override
  State<_InputDialog> createState() => _InputDialogState();
}

class _InputDialogState extends State<_InputDialog> {
  late final TextEditingController _keyCtrl;
  late DataType _type;
  late InputStyle _style;

  @override
  void initState() {
    super.initState();
    _keyCtrl = TextEditingController(text: '${widget.initialKey}');
    _type = widget.input.dataType;
    _style = widget.input.style;
  }

  @override
  void dispose() {
    _keyCtrl.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: Text('Input ${widget.index}'),
      content: SingleChildScrollView(
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          TextField(
            controller: _keyCtrl,
            keyboardType: TextInputType.number,
            decoration: const InputDecoration(
                labelText: 'Dictionary key (1-255)', border: OutlineInputBorder()),
          ),
          const SizedBox(height: 10),
          DropdownButtonFormField<DataType>(
            initialValue: _type,
            decoration: const InputDecoration(labelText: 'Expected type'),
            items: [
              for (final t in DataType.values)
                if (t != DataType.none && t != DataType.deleted)
                  DropdownMenuItem(value: t, child: Text(dataTypeLabel(t))),
            ],
            onChanged: (v) {
              if (v == null) return;
              setState(() {
                _type = v;
                // The expected type dictates the possible styles: clamp the current
                // selection back to a compatible one when the type changes.
                if (!InputStyle.allowedFor(_type).contains(_style)) {
                  _style = InputStyle.forType(_type);
                }
              });
            },
          ),
          const SizedBox(height: 10),
          ListTile(
            contentPadding: EdgeInsets.zero,
            title: const Text('Default value'),
            subtitle: Text(
                formatValue(_type, widget.input.defaultValue),
                style: const TextStyle(fontFamily: 'monospace')),
            trailing: const Icon(Icons.edit_outlined),
            onTap: () async {
              final value = await showValueEditor(context, _type, widget.input.defaultValue);
              if (value != null && mounted) {
                setState(() => widget.input.defaultValue = value);
              }
            },
          ),
          const SizedBox(height: 10),
          DropdownButtonFormField<InputStyle>(
            initialValue: _style,
            decoration: const InputDecoration(
                labelText: 'Interaction style (per type)'),
            items: [
              for (final s in InputStyle.allowedFor(_type))
                DropdownMenuItem(
                    value: s,
                    child: Text('${s.label}${s == InputStyle.automatic ? ' (${InputStyle.forType(_type).label})' : ''}')),
            ],
            onChanged: (v) {
              if (v == null) return;
              setState(() => _style = v);
            },
          ),
        ]),
      ),
      actions: [
        TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
        FilledButton(
            onPressed: () {
              final key = int.tryParse(_keyCtrl.text);
              if (key == null || key < 1 || key > 255) return;
              widget.input
                ..key = key
                ..flagsAndType = _type.value
                ..style = _style;
              Navigator.pop(context, true);
            },
            child: const Text('OK')),
      ],
    );
  }
}

/// Picks a data type for a constant.
class _TypePicker extends StatelessWidget {
  final DataType current;
  const _TypePicker({required this.current});

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: const Text('Constant type'),
      content: SizedBox(
        width: 260,
        child: ListView(
          shrinkWrap: true,
          children: [
            for (final t in DataType.values)
              if (t != DataType.none && t != DataType.deleted)
                ListTile(
                  dense: true,
                  title: Text(dataTypeLabel(t)),
                  trailing: t == current
                      ? const Icon(Icons.check, color: kOrange)
                      : null,
                  onTap: () => Navigator.pop(context, t),
                ),
          ],
        ),
      ),
      actions: [
        TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
      ],
    );
  }
}