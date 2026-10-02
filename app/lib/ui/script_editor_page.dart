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

part 'script_editor_view.dart';
part 'script_editor_line.dart';

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

  /// State update used by the part-file extensions (setState is @protected).
  void _rebuild(VoidCallback fn) => setState(fn);

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
      final st = await _client.readState(widget.fileId);
      final internal = await _client.readInternalState(widget.fileId);
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
        _state = st?.state ?? ScriptState.stopped;
        _instructionCounter = internal?.instructionCounter ?? 0;
        _errorCode = st?.error ?? 0;
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
    final ok = await _client.load(widget.fileId, widget.fileId);
    if (!mounted) return;
    showSnack(context, ok ? 'Reloaded' : 'Reload failed');
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
