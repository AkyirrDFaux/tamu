/// Script service UI (Docs/App/Service views/Script.md): lists the loaded and stored
/// (available) scripts, shows state, controls execution, and expands into the script's
/// inputs/outputs/variables/constants. The editor is opened per entry.
library;

import 'package:flutter/material.dart';

import '../core/script_client.dart';
import '../core/script_file.dart';
import '../core/storage_client.dart';
import 'script_editor_page.dart';
import 'script_create_dialog.dart';
import 'script_widgets.dart';
import 'widgets.dart';

class _LoadedScript {
  final int slot;
  final String name;
  final int state;
  final int instructionCounter;
  final List<ScriptInputSpec> inputSpecs;

  const _LoadedScript({
    required this.slot,
    required this.name,
    required this.state,
    required this.instructionCounter,
    this.inputSpecs = const [],
  });

  String get fileName =>
      'SCR_${slot.toRadixString(16).toUpperCase().padLeft(2, '0')}';
}

class ScriptsPage extends StatefulWidget {
  final int deviceId;
  final String deviceName;

  const ScriptsPage({super.key, required this.deviceId, required this.deviceName});

  @override
  State<ScriptsPage> createState() => _ScriptsPageState();
}

class _ScriptsPageState extends State<ScriptsPage> with AutoRefreshMixin<ScriptsPage> {
  late final ScriptClient _client = ScriptClient(deviceId: widget.deviceId);
  late final StorageClient _storage = StorageClient(deviceId: widget.deviceId);

  bool _showLoaded = true;
  bool _refreshing = false;
  List<_LoadedScript> _loaded = [];
  List<FileRecord> _available = [];
  int _revision = 0;

  @override
  void initState() {
    super.initState();
    _refresh();
  }

  @override
  Future<void> onAutoRefresh() => _refresh();

  @override
  void onAutoRefreshStarted() => _refresh();

  Future<void> _refresh() async {
    if (_refreshing) return;
    setState(() => _refreshing = true);
    try {
      if (_showLoaded) {
        await _loadLoaded();
      } else {
        await _loadAvailable();
      }
    } finally {
      if (mounted) setState(() => _refreshing = false);
    }
  }

  Future<void> _loadLoaded() async {
    final slots = await _client.loadedScripts();
    final list = <_LoadedScript>[];
    for (final slot in slots) {
      final meta = await _client.readBlockMeta(slot);
      final state = await _client.readState(slot) ?? ScriptState.stopped;
      final internal = await _client.readInternalState(slot);
      // The input UI specifications live in the script file.
      var specs = const <ScriptInputSpec>[];
      final fileName = 'SCR_${slot.toRadixString(16).toUpperCase().padLeft(2, '0')}';
      final bytes = await _storage.readFile(fileName);
      if (bytes != null) {
        try {
          specs = ScriptFileData.parse(bytes).inputSpecs;
        } on FormatException {
          specs = const [];
        }
      }
      list.add(_LoadedScript(
        slot: slot,
        name: (meta?.name.isNotEmpty ?? false) ? meta!.name : 'Script $slot',
        state: state,
        instructionCounter: internal?.instructionCounter ?? 0,
        inputSpecs: specs,
      ));
    }
    if (!mounted) return;
    setState(() {
      _loaded = list;
      _revision++;
    });
  }

  Future<void> _loadAvailable() async {
    final table = await _storage.readFileTable() ?? const <FileRecord>[];
    if (!mounted) return;
    setState(() {
      _available = table
          .where((f) => normalizeFileName(f.name).startsWith('SCR_'))
          .toList();
      _revision++;
    });
  }

  void _setMode(bool loaded) {
    setState(() => _showLoaded = loaded);
    _refresh();
  }

  Future<void> _control(_LoadedScript s, int state, {bool reset = false}) async {
    if (reset) await _client.moveToInstruction(s.slot, 0);
    final ok = await _client.setState(s.slot, state);
    if (!mounted) return;
    showSnack(context, ok ? '${s.name}: ${ScriptState.label(state)}' : 'Command failed');
    await _loadLoaded();
  }

  Future<void> _unload(_LoadedScript s) async {
    final ok = await _client.unload(s.slot);
    if (!mounted) return;
    showSnack(context, ok ? 'Unloaded ${s.name}' : 'Unload failed');
    await _loadLoaded();
  }

  Future<void> _loadFile(FileRecord file) async {
    final name = normalizeFileName(file.name);
    final id = int.tryParse(name.substring(4), radix: 16);
    if (id == null) {
      showSnack(context, 'Bad script file name');
      return;
    }
    final loaded = await _client.load(id);
    if (!mounted) return;
    showSnack(context, loaded == null ? 'Load failed' : 'Loaded $name');
    await _loadLoaded();
  }

  Future<void> _createScript() async {
    final table = await _storage.readFileTable() ?? const <FileRecord>[];
    final used = <int>{};
    for (final f in table) {
      final n = normalizeFileName(f.name);
      if (!n.startsWith('SCR_')) continue;
      final id = int.tryParse(n.substring(4), radix: 16);
      if (id != null) used.add(id);
    }
    final free = [for (var i = 0; i < maxScripts; i++) if (!used.contains(i)) i];
    if (!mounted) return;
    if (free.isEmpty) {
      showSnack(context, 'No free script slot');
      return;
    }
    final result = await showNewScriptDialog(context, freeSlots: free);
    if (result == null || !mounted) return;

    // Write the file only - the new script is intentionally NOT loaded.
    final image = ScriptFileBuilder(
      properties: result.properties,
      inputs: result.inputs,
      outputs: result.outputs,
      variables: result.variables,
      constants: result.constants,
      functionName: result.name,
    ).build();
    final fileName =
        'SCR_${result.slot.toRadixString(16).toUpperCase().padLeft(2, '0')}';
    final ok = await _storage.writeFile(fileName, image);
    if (!mounted) return;
    showSnack(context, ok ? 'Created $fileName (not loaded)' : 'Create failed');
    setState(() => _showLoaded = false);
    await _loadAvailable();
  }

  Future<void> _deleteFile(FileRecord file) async {    final name = normalizeFileName(file.name);
    if (!await confirmDialog(context,
        title: 'Delete script',
        body: 'Delete $name from the device?',
        confirmLabel: 'Delete')) {
      return;
    }
    final ok = await _storage.deleteFile(name);
    if (!mounted) return;
    showSnack(context, ok ? 'Deleted $name' : 'Delete failed');
    await _loadAvailable();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text('Scripts - ${widget.deviceName}'),
        actions: [
          RefreshButton(
            onRefresh: _refresh,
            autoActive: autoRefreshActive,
            refreshing: _refreshing,
            error: false,
            selectedInterval: selectedInterval,
            onSelectAuto: applyAuto,
          ),
        ],
      ),
      body: Column(
        children: [
          Padding(
            padding: const EdgeInsets.all(12),
            child: SegmentedButton<bool>(
              segments: const [
                ButtonSegment(value: true, label: Text('Loaded'), icon: Icon(Icons.play_circle_outline)),
                ButtonSegment(value: false, label: Text('Available'), icon: Icon(Icons.folder_outlined)),
              ],
              selected: {_showLoaded},
              onSelectionChanged: (s) => _setMode(s.first),
            ),
          ),
          Expanded(child: _showLoaded ? _buildLoaded() : _buildAvailable()),
        ],
      ),
      floatingActionButton: FloatingActionButton.extended(
        onPressed: _createScript,
        icon: const Icon(Icons.add),
        label: const Text('New script'),
      ),
    );
  }

  Widget _buildLoaded() {
    if (_loaded.isEmpty) {
      return const Center(
        child: Padding(
          padding: EdgeInsets.all(24),
          child: Text(
            'No scripts loaded.\nSwitch to "Available" and load a stored SCR_XX file.',
            style: TextStyle(color: Colors.white54),
            textAlign: TextAlign.center,
          ),
        ),
      );
    }
    return ListView.builder(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
      itemCount: _loaded.length,
      itemBuilder: (context, i) => _buildLoadedCard(_loaded[i]),
    );
  }

  Widget _buildLoadedCard(_LoadedScript s) {
    return Card(
      margin: const EdgeInsets.symmetric(vertical: 4),
      child: ExpansionTile(
        leading: Icon(Icons.circle, size: 12, color: scriptStateColor(s.state)),
        title: Text(s.name),
        subtitle: Text('${s.fileName} · ${ScriptState.label(s.state)}'
            '${s.instructionCounter > 0 ? ' · IC ${s.instructionCounter}' : ''}'),
        childrenPadding: const EdgeInsets.only(bottom: 8),
        children: [
          Wrap(
            spacing: 8,
            children: [
              OutlinedButton.icon(
                onPressed: () => _control(s, ScriptState.running),
                icon: const Icon(Icons.play_arrow, size: 18),
                label: const Text('Start'),
              ),
              OutlinedButton.icon(
                onPressed: () => _control(
                    s, s.state == ScriptState.paused ? ScriptState.running : ScriptState.paused),
                icon: Icon(s.state == ScriptState.paused ? Icons.play_arrow : Icons.pause, size: 18),
                label: Text(s.state == ScriptState.paused ? 'Continue' : 'Pause'),
              ),
              OutlinedButton.icon(
                onPressed: () => _control(s, ScriptState.stopped, reset: true),
                icon: const Icon(Icons.stop, size: 18),
                label: const Text('Stop'),
              ),
              OutlinedButton.icon(
                onPressed: () => _control(s, ScriptState.running, reset: true),
                icon: const Icon(Icons.restart_alt, size: 18),
                label: const Text('Restart'),
              ),
              OutlinedButton.icon(
                onPressed: () => _unload(s),
                icon: const Icon(Icons.eject, size: 18),
                label: const Text('Unload'),
              ),
              TextButton.icon(
                onPressed: () => _openEditor(s.slot, s.name, true),
                icon: const Icon(Icons.edit_note, size: 18),
                label: const Text('Open editor'),
              ),
            ],
          ),
          ScriptIoSection(
            client: _client,
            slot: s.slot,
            field: ScriptField.input,
            title: 'Input',
            icon: Icons.login,
            editable: true,
            revision: _revision,
            specs: s.inputSpecs,
            onChanged: _loadLoaded,
          ),
          ScriptIoSection(
            client: _client,
            slot: s.slot,
            field: ScriptField.output,
            title: 'Output',
            icon: Icons.logout,
            editable: false,
            revision: _revision,
          ),
        ],
      ),
    );
  }

  Widget _buildAvailable() {
    if (_available.isEmpty) {
      return const Center(
        child: Padding(
          padding: EdgeInsets.all(24),
          child: Text(
            'No stored scripts.\nUse "New script" to create one, or upload a SCR_XX file.',
            style: TextStyle(color: Colors.white54),
            textAlign: TextAlign.center,
          ),
        ),
      );
    }
    return ListView.builder(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
      itemCount: _available.length,
      itemBuilder: (context, i) {
        final file = _available[i];
        final name = normalizeFileName(file.name);
        return Card(
          margin: const EdgeInsets.symmetric(vertical: 4),
          child: ListTile(
            leading: const Icon(Icons.description_outlined),
            title: Text(name),
            subtitle: Text('${file.size} bytes'),
            trailing: Row(mainAxisSize: MainAxisSize.min, children: [
              IconButton(
                tooltip: 'Open editor',
                icon: const Icon(Icons.edit_note),
                onPressed: () {
                  final id = int.tryParse(name.substring(4), radix: 16);
                  if (id != null) _openEditor(id, name, false);
                },
              ),
              TextButton.icon(
                onPressed: () => _loadFile(file),
                icon: const Icon(Icons.play_arrow, size: 18),
                label: const Text('Load'),
              ),
              IconButton(
                icon: const Icon(Icons.delete_outline, color: Colors.redAccent),
                onPressed: () => _deleteFile(file),
              ),
            ]),
          ),
        );
      },
    );
  }

  void _openEditor(int fileId, String name, bool loaded) {
    Navigator.of(context).push(MaterialPageRoute(
      builder: (_) => ScriptEditorPage(
        deviceId: widget.deviceId,
        fileId: fileId,
        name: name,
        loaded: loaded,
      ),
    ));
  }
}
