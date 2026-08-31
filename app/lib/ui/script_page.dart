import 'dart:async';

import 'package:flutter/material.dart';

import '../core/connection.dart';
import '../core/script_client.dart';
import '../core/script_file.dart';
import '../core/types.dart';
import 'script_editor_page.dart';
import 'script_input_widget.dart';
import 'theme.dart';
import 'value_editor.dart' show dataTypeLabel, formatValue;
import 'widgets.dart';

/// Script service view (Docs/App/Service views/Script.md): lists all available
/// scripts (name, input/output definitions, state) with inline start/pause/stop
/// controls. A row unfolds to a detailed view: the inputs rendered as their
/// interaction controls (button/switch/...) plus live output/variable values -
/// a script can be driven without opening the editor.
class ScriptPage extends StatefulWidget {
  final int deviceId;

  const ScriptPage({super.key, required this.deviceId});

  @override
  State<ScriptPage> createState() => _ScriptPageState();
}

class _ScriptPageState extends State<ScriptPage> with AutoRefreshMixin<ScriptPage> {
  late final ScriptClient _client = ScriptClient(deviceId: widget.deviceId);

  List<_ScriptEntry>? _entries;
  bool _refreshing = false;

  final Set<int> _expanded = {};
  final Map<int, int> _liveStates = {};
  final Map<int, Map<int, ({BlockMeta meta, List<int> value})>> _liveInputs = {};
  final Map<int, Map<int, ({BlockMeta meta, List<int> value})>> _liveOutputs = {};
  final Map<int, Map<int, ({BlockMeta meta, List<int> value})>> _liveVars = {};
  Timer? _liveTimer;
  bool _polling = false;

  @override
  void initState() {
    super.initState();
    _refresh();
    _liveTimer =
        Timer.periodic(const Duration(seconds: 1), (_) => _pollExpanded());
  }

  @override
  void dispose() {
    _liveTimer?.cancel();
    super.dispose();
  }

  @override
  Future<void> onAutoRefresh() => _refresh();

  void _snack(String message) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(message)));
  }

  Future<void> _refresh() async {
    if (!ConnectionManager.instance.isConnected || _refreshing) return;
    setState(() => _refreshing = true);
    final ids = await _client.scriptIds();
    final entries = <_ScriptEntry>[];
    for (final id in ids) {
      final name = await _client.readName(id);
      final state = await _client.readState(id);
      final bytes = await _client.readScriptFile(id);
      final file = bytes == null ? null : ScriptFileData.parse(bytes);
      entries.add(_ScriptEntry(
        id: id,
        name: (name == null || name.isEmpty) ? 'Script $id' : name,
        state: state ?? ScriptStateCode.stopped,
        file: file,
      ));
    }
    if (!mounted) return;
    setState(() {
      _refreshing = false;
      _entries = entries;
    });
  }

  /// Polls the state + live values of the expanded scripts (so the input controls
  /// and output/variable previews stay current without opening the editor).
  Future<void> _pollExpanded() async {
    if (_polling || _refreshing || !ConnectionManager.instance.isConnected) return;
    if (_expanded.isEmpty || _entries == null) return;
    _polling = true;
    try {
      for (final id in _expanded.toList()) {
        final entry = _entries!.where((e) => e.id == id).firstOrNull;
        if (entry == null) continue;
        final state = await _client.readState(id);
        final inputs = <int, ({BlockMeta meta, List<int> value})>{};
        final outputs = <int, ({BlockMeta meta, List<int> value})>{};
        final vars = <int, ({BlockMeta meta, List<int> value})>{};
        if (state != null && state != ScriptStateCode.stopped) {
          final nInputs = entry.file?.inputCount ?? 0;
          final nOutputs = entry.file?.outputCount ?? 0;
          final nVars = entry.file?.variableCount ?? 0;
          for (var i = 0; i < nInputs; i++) {
            final v = await _client.readInput(id, i);
            if (v != null) inputs[i] = v;
          }
          for (var i = 0; i < nOutputs; i++) {
            final v = await _client.readOutput(id, i);
            if (v != null) outputs[i] = v;
          }
          for (var i = 0; i < nVars; i++) {
            final v = await _client.readVariable(id, i);
            if (v != null) vars[i] = v;
          }
        }
        if (!mounted) return;
        setState(() {
          if (state != null) _liveStates[id] = state;
          if (state == ScriptStateCode.stopped) {
            _liveInputs.remove(id);
            _liveOutputs.remove(id);
            _liveVars.remove(id);
          } else {
            _liveInputs[id] = inputs;
            _liveOutputs[id] = outputs;
            _liveVars[id] = vars;
          }
        });
      }
    } finally {
      _polling = false;
    }
  }

  Future<void> _setState(_ScriptEntry entry, int state) async {
    if (await _client.setState(entry.id, state)) {
      _snack('${entry.name}: ${ScriptStateCode.label(state)}');
    } else {
      _snack('State change failed');
    }
    await _refresh();
  }

  Future<void> _writeInput(_ScriptEntry entry, int index, List<int> value) async {
    final input = entry.file?.inputs[index];
    if (input == null) return;
    final meta =
        BlockMeta(flagsAndType: input.dataType.value, size: value.length);
    final ok = await _client.writeInput(entry.id, index, meta, value);
    if (!ok) _snack('Input $index write failed');
  }

  Future<void> _create() async {
    final id = await _client.createScript();
    if (id == null) {
      _snack('Could not create a script (no free ID?)');
      return;
    }
    if (!mounted) return;
    await Navigator.of(context).push(MaterialPageRoute(
        builder: (_) => ScriptEditorPage(deviceId: widget.deviceId, scriptId: id)));
    await _refresh();
  }

  Future<void> _delete(_ScriptEntry entry) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Delete script'),
        content: Text('Delete "${entry.name}" (script ${entry.id})?'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
          FilledButton(
              onPressed: () => Navigator.pop(context, true), child: const Text('Delete')),
        ],
      ),
    );
    if (ok != true) return;
    if (await _client.deleteScript(entry.id)) {
      _snack('Script ${entry.id} deleted');
    } else {
      _snack('Delete failed');
    }
    setState(() => _expanded.remove(entry.id));
    await _refresh();
  }

  Future<void> _openEditor(_ScriptEntry entry) async {
    await Navigator.of(context).push(MaterialPageRoute(
        builder: (_) => ScriptEditorPage(deviceId: widget.deviceId, scriptId: entry.id)));
    await _refresh();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('Scripts'),
        actions: [
          IconButton(
            icon: const Icon(Icons.refresh),
            tooltip: 'Refresh',
            onPressed: _refresh,
          ),
          IconButton(
            icon: const Icon(Icons.add),
            tooltip: 'New script',
            onPressed: _create,
          ),
        ],
      ),
      body: _entries == null
          ? const Center(child: CircularProgressIndicator())
          : _entries!.isEmpty
              ? const Center(child: Text('No scripts yet - tap + to create one'))
              : RefreshIndicator(
                  onRefresh: _refresh,
                  child: ListView.separated(
                    padding: const EdgeInsets.fromLTRB(10, 10, 10, 24),
                    itemCount: _entries!.length,
                    separatorBuilder: (_, _) => const SizedBox(height: 6),
                    itemBuilder: (context, index) =>
                        _row(context, _entries![index]),
                  ),
                ),
    );
  }

  Widget _row(BuildContext context, _ScriptEntry entry) {
    final state = _liveStates[entry.id] ?? entry.state;
    final expanded = _expanded.contains(entry.id);
    return Card(
      color: kSurfaceAlt,
      margin: EdgeInsets.zero,
      clipBehavior: Clip.antiAlias,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          InkWell(
            onTap: () => setState(() {
              expanded ? _expanded.remove(entry.id) : _expanded.add(entry.id);
            }),
            child: Padding(
              padding: const EdgeInsets.fromLTRB(12, 6, 4, 6),
              child: Row(children: [
                Icon(Icons.menu_book_outlined, color: kOrange),
                const SizedBox(width: 10),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Row(children: [
                        Expanded(
                            child: Text(entry.name,
                                style: const TextStyle(
                                    fontWeight: FontWeight.w600))),
                        _stateBadge(state),
                      ]),
                      Text(
                        _defsLine(entry),
                        style: const TextStyle(
                            fontFamily: 'monospace', fontSize: 11),
                      ),
                    ],
                  ),
                ),
                IconButton(
                  icon: Icon(expanded
                      ? Icons.expand_less
                      : Icons.expand_more),
                  tooltip: expanded ? 'Collapse' : 'Show details',
                  onPressed: () => setState(() {
                    expanded
                        ? _expanded.remove(entry.id)
                        : _expanded.add(entry.id);
                  }),
                ),
                if (state == ScriptStateCode.running ||
                    state == ScriptStateCode.waiting)
                  IconButton(
                      icon: const Icon(Icons.pause, size: 20),
                      tooltip: 'Pause',
                      onPressed: () => _setState(entry, ScriptStateCode.paused))
                else
                  IconButton(
                      icon: const Icon(Icons.play_arrow, size: 20),
                      tooltip: 'Start',
                      onPressed: () => _setState(entry, ScriptStateCode.running)),
                if (state != ScriptStateCode.stopped)
                  IconButton(
                      icon: const Icon(Icons.stop, size: 20),
                      tooltip: 'Stop',
                      onPressed: () => _setState(entry, ScriptStateCode.stopped)),
                PopupMenuButton<String>(
                  onSelected: (action) {
                    switch (action) {
                      case 'edit':
                        _openEditor(entry);
                      case 'delete':
                        _delete(entry);
                    }
                  },
                  itemBuilder: (_) => const [
                    PopupMenuItem(value: 'edit', child: Text('Open editor')),
                    PopupMenuItem(value: 'delete', child: Text('Delete')),
                  ],
                ),
              ]),
            ),
          ),
          if (expanded) _detail(context, entry, state),
        ],
      ),
    );
  }

  String _defsLine(_ScriptEntry entry) {
    final file = entry.file;
    final buf = StringBuffer('Script ${entry.id}');
    if (file != null) {
      if (file.inputs.isNotEmpty) {
        buf.write('  ·  In: ${file.inputs.map((i) => dataTypeLabel(i.dataType)).join(', ')}');
      }
      if (file.outputNames.isNotEmpty) {
        buf.write('  ·  Out: ${file.outputNames.join(', ')}');
      }
    }
    return buf.toString();
  }

  Widget _detail(BuildContext context, _ScriptEntry entry, int state) {
    final file = entry.file;
    final inputs = _liveInputs[entry.id];
    final outputs = _liveOutputs[entry.id];
    final vars = _liveVars[entry.id];
    return Padding(
      padding: const EdgeInsets.fromLTRB(12, 0, 12, 12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Divider(height: 8),
          Text('INPUTS',
              style: TextStyle(color: kOrange, fontSize: 11, letterSpacing: 1)),
          if (file == null || file.inputs.isEmpty)
            const Padding(
              padding: EdgeInsets.symmetric(vertical: 4),
              child: Text('No inputs defined.',
                  style: TextStyle(fontSize: 12, color: Colors.white54)),
            )
          else
            for (var i = 0; i < file.inputs.length; i++)
              ScriptInputControl(
                index: i,
                input: file.inputs[i],
                liveValue: inputs?[i]?.value,
                onWrite: (value) => _writeInput(entry, i, value),
              ),
          if (file != null && (file.outputNames.isNotEmpty || file.variableNames.isNotEmpty))
            const Divider(height: 8),
          if (file != null && file.outputNames.isNotEmpty) ...[
            Text('OUTPUTS (live)',
                style:
                    TextStyle(color: kOrange, fontSize: 11, letterSpacing: 1)),
            for (var i = 0; i < file.outputNames.length; i++)
              _valueLine('Out$i: ${file.outputNames[i].isEmpty ? '(unnamed)' : file.outputNames[i]}',
                  outputs?[i]),
          ],
          if (file != null && file.variableNames.isNotEmpty) ...[
            const Divider(height: 8),
            Text('VARIABLES (live)',
                style:
                    TextStyle(color: kOrange, fontSize: 11, letterSpacing: 1)),
            for (var i = 0; i < file.variableNames.length; i++)
              _valueLine('Var$i: ${file.variableNames[i].isEmpty ? '(unnamed)' : file.variableNames[i]}',
                  vars?[i]),
          ],
          if (state == ScriptStateCode.stopped)
            const Padding(
              padding: EdgeInsets.only(top: 6),
              child: Text('Script is stopped - live values appear once it runs.',
                  style: TextStyle(fontSize: 11, color: Colors.white54)),
            ),
          const SizedBox(height: 6),
          OutlinedButton.icon(
            onPressed: () => _openEditor(entry),
            icon: const Icon(Icons.edit_outlined, size: 18),
            label: const Text('Open editor'),
          ),
        ],
      ),
    );
  }

  Widget _valueLine(String label,
      ({BlockMeta meta, List<int> value})? live) {
    final text = live == null || live.value.isEmpty
        ? '-'
        : formatValue(live.meta.dataType, live.value);
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 2),
      child: Row(children: [
        Expanded(
            child: Text(label,
                style: const TextStyle(
                    fontFamily: 'monospace', fontSize: 12))),
        Text(text,
            style: const TextStyle(
                fontFamily: 'monospace',
                fontSize: 12,
                color: Color(0xFF4CAF50))),
      ]),
    );
  }

  Color _stateColor(int state) => switch (state) {
        ScriptStateCode.running => const Color(0xFF4CAF50),
        ScriptStateCode.waiting => const Color(0xFFFFC107),
        ScriptStateCode.error => const Color(0xFFF44336),
        ScriptStateCode.finished => const Color(0xFF90A4AE),
        ScriptStateCode.paused => const Color(0xFFFFA726),
        _ => kOrange,
      };

  Widget _stateBadge(int state) {
    final color = _stateColor(state);
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 1),
      decoration: BoxDecoration(
        color: color.withAlpha(40),
        borderRadius: BorderRadius.circular(4),
      ),
      child: Text(ScriptStateCode.label(state),
          style: TextStyle(fontSize: 10, color: color)),
    );
  }
}

class _ScriptEntry {
  final int id;
  final String name;
  final int state;
  final ScriptFileData? file;

  const _ScriptEntry({
    required this.id,
    required this.name,
    required this.state,
    this.file,
  });
}