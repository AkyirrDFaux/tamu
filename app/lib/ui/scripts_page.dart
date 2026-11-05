/// Script service UI (Docs/App/Service views/Script.md): lists the loaded and stored
/// (available) scripts, shows state, controls execution, and expands into the script's
/// inputs/outputs/variables/constants. The editor is opened per entry.
library;

import 'package:flutter/material.dart';

import '../core/script_client.dart';
import '../core/script_fault.dart';
import '../core/script_file.dart';
import '../core/storage_client.dart';
import 'script_editor_page.dart';
import 'script_create_dialog.dart';
import 'script_widgets.dart';
import 'theme.dart';
import 'widgets.dart';

class _LoadedScript {
  final int slot;
  final int fileId;
  final String name;
  final int state;
  final int instructionCounter;
  final List<ScriptInputSpec> inputSpecs;
  final List<String> inputNames;

  /// The decoded fault of this slot, or null when it is not in error (Docs/Services/Script.md
  /// "Error Model"). Built from the CID 3 report against the stored file's own program.
  final ScriptFault? fault;

  /// Why this slot's file could not be read, in the reader's own words - null when it read. A
  /// failed or partial read is shown on the row, never as an empty input list.
  final String? fileError;

  const _LoadedScript({
    required this.slot,
    required this.fileId,
    required this.name,
    required this.state,
    required this.instructionCounter,
    this.inputSpecs = const [],
    this.inputNames = const [],
    this.fault,
    this.fileError,
  });

  String get fileName => scriptFileName(fileId);
}

class ScriptsPage extends StatefulWidget {
  final int deviceId;
  final String deviceName;

  const ScriptsPage({
    super.key,
    required this.deviceId,
    required this.deviceName,
  });

  @override
  State<ScriptsPage> createState() => _ScriptsPageState();
}

class _ScriptsPageState extends State<ScriptsPage>
    with AutoRefreshMixin<ScriptsPage> {
  late final ScriptClient _client = ScriptClient(deviceId: widget.deviceId);
  late final StorageClient _storage = StorageClient(deviceId: widget.deviceId);

  bool _showLoaded = true;
  bool _refreshing = false;

  /// Why the visible mode has no list, in the reader's own words - null when it read. A failure
  /// is NEVER rendered as "there is nothing here" (Akyirr, 2026-11-05): both modes show this
  /// sentence and a Try again instead of an empty state.
  String? _error;

  List<_LoadedScript> _loaded = [];

  /// The whole-table read the Available view is showing: the records, an absent table (a device
  /// with no filesystem - legitimately empty) or the failure to say out loud.
  FileTableRead? _table;
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
    setState(() {
      _refreshing = true;
      _error = null;
    });
    try {
      if (_showLoaded) {
        await _loadLoaded();
      } else {
        await _loadAvailable();
      }
    } catch (e) {
      if (mounted) setState(() => _error = '$e');
    } finally {
      if (mounted) setState(() => _refreshing = false);
    }
  }

  /// Resolves the loaded scripts from CID 0's streamed slot map (Stage 4): the map gives both
  /// which slots are occupied and the device's actual slot count, so nothing assumes 256. The
  /// slot is the addressable register instance; the file id is independent of it.
  ///
  /// NULL when the device did not answer: [ScriptClient.readLoadedSlotMap] keeps "no reply" and
  /// "an empty slot map" apart, and this page must too - an unanswered slot map rendered as
  /// "No scripts loaded" is a failure shown as device state.
  Future<List<({int slot, int fileId, String metaName})>?>
  _probeLoaded() async {
    final map = await _client.readLoadedSlotMap();
    if (map == null) return null;
    final out = <({int slot, int fileId, String metaName})>[];
    for (final entry in map.entries) {
      final meta = await _client.readBlockMeta(entry.key);
      out.add((
        slot: entry.key,
        fileId: entry.value,
        metaName: meta?.name ?? '',
      ));
    }
    return out;
  }

  Future<void> _loadLoaded() async {
    final probed = await _probeLoaded();
    if (!mounted) return;
    if (probed == null) {
      setState(() {
        _loaded = const [];
        _error =
            'The device did not answer the slot map request (Script CID 0), so '
            'its loaded slots are unknown. This is not "no scripts loaded".';
      });
      return;
    }
    final list = <_LoadedScript>[];
    for (final p in probed) {
      final report = await _client.readState(p.slot);
      // The input UI specifications and function name live in the script file.
      var specs = const <ScriptInputSpec>[];
      var names = const <String>[];
      var display = p.metaName;
      ScriptFault? fault;
      String? fileError;
      // The same tolerant whole-file reader the Storage page uses: whole bytes, an absent file,
      // or a failed/partial read. A file that could not be read is said out loud on the row
      // instead of rendering as "this script has no inputs".
      final read = await _storage.readFileWhole(scriptFileName(p.fileId));
      if (read.ok) {
        try {
          final parsed = ScriptFileData.parse(read.bytes!);
          specs = parsed.inputSpecs;
          names = parsed.inputNames;
          if (parsed.functionName.isNotEmpty) display = parsed.functionName;
          // The record names the instruction and the error, never the operand: the register
          // the faulted instruction addressed is read back from the file's own program.
          fault = fileFault(report, parsed);
        } on FormatException {
          specs = const [];
          names = const [];
        }
      } else if (read.absent) {
        // The slot names a file the device does not hold: that IS a device state, and the row
        // says so rather than pretending the script has no inputs.
        fileError = '${scriptFileName(p.fileId)}: ${read.detail}';
      } else {
        fileError = read.detail;
      }
      // A file that is missing or malformed still decodes the instruction and the error.
      fault ??= report == null || !report.hasError
          ? null
          : ScriptFault.fromReport(report);
      list.add(
        _LoadedScript(
          slot: p.slot,
          fileId: p.fileId,
          name: display.isNotEmpty ? display : 'Script ${p.slot}',
          state: report?.state ?? ScriptState.stopped,
          instructionCounter: report?.line ?? 0,
          inputSpecs: specs,
          inputNames: names,
          fault: fault,
          fileError: fileError,
        ),
      );
    }
    if (!mounted) return;
    setState(() {
      _loaded = list;
      _error = null;
      _revision++;
    });
  }

  Future<void> _loadAvailable() async {
    // The ONE shared whole-table reader (StorageClient.readFileTableResult): a failed read comes
    // back with its reason instead of as a list, and an absent table is flagged as such.
    final table = await _storage.readFileTableResult();
    if (!mounted) return;
    setState(() {
      _table = table;
      _error = table.ok || table.absent ? null : table.detail;
    });
  }

  /// The stored SCR_ files of the last whole-table read (empty for an absent or unread table -
  /// the builders check [_table] / [_error] first).
  List<FileRecord> get _available => (_table?.records ?? const <FileRecord>[])
      .where((f) => normalizeFileName(f.name).toUpperCase().startsWith('SCR_'))
      .toList();

  void _setMode(bool loaded) {
    setState(() => _showLoaded = loaded);
    _refresh();
  }

  Future<void> _control(
    _LoadedScript s,
    int action, {
    bool reset = false,
  }) async {
    if (reset) await _client.moveToInstruction(s.slot, 0);
    final ok = await _client.setAction(s.slot, action);
    if (!mounted) return;
    showSnack(
      context,
      ok ? '${s.name}: ${ScriptAction.label(action)}' : 'Command failed',
    );
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
    final id = scriptFileId(name);
    if (id == null) {
      showSnack(context, 'Bad script file name');
      return;
    }
    // The file id and the loaded slot are independent: pick the lowest free slot from CID 0's
    // slot map (the device reports which slots are empty).
    final map = await _client.slotMap();
    var free = -1;
    for (var i = 0; i < map.length; i++) {
      if (map[i] == scriptSlotEmpty) {
        free = i;
        break;
      }
    }
    if (!mounted) return;
    if (free < 0) {
      showSnack(context, 'No free script slot');
      return;
    }
    final loaded = await _client.load(id, free);
    if (!mounted) return;
    showSnack(context, loaded ? 'Loaded $name into slot $free' : 'Load failed');
    await _loadLoaded();
  }

  Future<void> _createScript() async {
    // A failed read is not "nothing is used": choosing an id from an unread table could overwrite
    // a file the device does hold, so the create is refused with the reason instead.
    final table = await _storage.readFileTableResult();
    if (!mounted) return;
    if (!table.ok) {
      showSnack(context, table.detail ?? 'Could not read the file table');
      return;
    }
    final used = <int>{};
    for (final f in table.recordsOrEmpty) {
      final id = scriptFileId(f.name);
      if (id != null) used.add(id);
    }
    final free = [
      for (var i = 0; i < maxScriptFiles; i++)
        if (!used.contains(i)) i,
    ];
    if (!mounted) return;
    if (free.isEmpty) {
      showSnack(context, 'No free script id');
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
    final fileName = scriptFileName(result.fileId);
    final ok = await _storage.writeFile(fileName, image);
    if (!mounted) return;
    showSnack(context, ok ? 'Created $fileName (not loaded)' : 'Create failed');
    setState(() => _showLoaded = false);
    await _loadAvailable();
  }

  Future<void> _deleteFile(FileRecord file) async {
    final name = normalizeFileName(file.name);
    if (!await confirmDialog(
      context,
      title: 'Delete script',
      body: 'Delete $name from the device?',
      confirmLabel: 'Delete',
    )) {
      return;
    }
    final ok = await _storage.deleteFile(name);
    if (!mounted) return;
    showSnack(context, ok ? 'Deleted $name' : 'Delete failed');
    await _loadAvailable();
  }

  @override
  Widget build(BuildContext context) {
    return TamuPage(
      title: 'Scripts - ${widget.deviceName}',
      actions: [
        PageActions.autoRefresh(
          control: this,
          onRefresh: _refresh,
          refreshing: _refreshing,
          error: _error != null,
        ),
      ],
      floatingActionButton: FloatingActionButton.extended(
        onPressed: _createScript,
        icon: const Icon(Icons.add),
        label: const Text('New script'),
      ),
      child: Column(
        children: [
          Padding(
            padding: Insets.page,
            child: SegmentedButton<bool>(
              segments: const [
                ButtonSegment(
                  value: true,
                  label: Text('Loaded'),
                  icon: Icon(Icons.play_circle_outline),
                ),
                ButtonSegment(
                  value: false,
                  label: Text('Available'),
                  icon: Icon(Icons.folder_outlined),
                ),
              ],
              selected: {_showLoaded},
              onSelectionChanged: (s) => _setMode(s.first),
            ),
          ),
          Expanded(child: _showLoaded ? _buildLoaded() : _buildAvailable()),
        ],
      ),
    );
  }

  Widget _buildLoaded() {
    // A failure is not an empty list: the slot map read (or a slot's file read) failing says so
    // and offers the retry that re-asks the device.
    final error = _error;
    if (error != null) {
      return StateMessage.error(
        message: 'The loaded scripts could not be read',
        details: error,
        action: FilledButton(
          onPressed: _refresh,
          child: const Text('Try again'),
        ),
      );
    }
    if (_loaded.isEmpty) {
      return const StateMessage.empty(
        message: 'No scripts loaded',
        details: 'Switch to "Available" and load a stored SCR_XXX file.',
      );
    }
    return ListView.separated(
      padding: Insets.row,
      itemCount: _loaded.length,
      separatorBuilder: (context, index) => const SizedBox(height: Spacing.md),
      itemBuilder: (context, i) => _buildLoadedCard(_loaded[i]),
    );
  }

  Widget _buildLoadedCard(_LoadedScript s) {
    final theme = Theme.of(context);
    return Card(
      clipBehavior: Clip.antiAlias,
      child: ExpansionTile(
        leading: ScriptStateDot(s.state),
        title: Text(s.name, style: theme.textTheme.titleSmall),
        subtitle: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              '${s.fileName} · ${ScriptState.label(s.state)}'
              '${s.instructionCounter > 0 ? ' · IC ${s.instructionCounter}' : ''}',
              style: theme.textTheme.bodySmall,
            ),
            // The fault sits directly under the state it belongs to: a state of Error without
            // the decoded sentence is the "error 2 at cat 4 op 1" the owner reported.
            if (s.fault case final fault?) ScriptFaultRow(fault, dense: true),
            // A file that could not be read is said here, on the row, in the reader's words -
            // the inputs below are then unknown rather than empty.
            if (s.fileError case final fileError?)
              Padding(
                padding: const EdgeInsets.only(top: Spacing.xs),
                child: Text(
                  fileError,
                  style: theme.textTheme.bodySmall?.copyWith(color: kError),
                ),
              ),
          ],
        ),
        childrenPadding: const EdgeInsets.only(bottom: Spacing.md),
        children: [
          ScriptControlBar(
            state: s.state,
            onStart: () => _control(s, ScriptAction.start),
            onPauseToggle: () => _control(
              s,
              s.state == ScriptState.paused
                  ? ScriptAction.resume
                  : ScriptAction.pause,
            ),
            onStop: () => _control(s, ScriptAction.stop, reset: true),
            onRestart: () => _control(s, ScriptAction.start, reset: true),
            extra: [
              OutlinedButton.icon(
                onPressed: () => _unload(s),
                icon: const Icon(Icons.eject, size: IconSize.md),
                label: const Text('Unload'),
              ),
              TextButton.icon(
                onPressed: () =>
                    _openEditor(s.fileId, s.name, true, slot: s.slot),
                icon: const Icon(Icons.edit_note, size: IconSize.md),
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
            names: s.inputNames,
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
    // Three cases, told apart (Akyirr, 2026-11-05): the read failed / came back partial (error +
    // the reason + a retry), the device holds no file table (a legitimate empty list), or the
    // table arrived and simply has no SCR_XXX record in it.
    final error = _error;
    if (error != null) {
      return StateMessage.error(
        message: 'The stored script list could not be read',
        details: error,
        action: FilledButton(
          onPressed: _refresh,
          child: const Text('Try again'),
        ),
      );
    }
    final table = _table;
    if (table == null) return const StateMessage.loading();
    if (table.absent) {
      return const StateMessage.empty(
        message: 'No stored scripts',
        details:
            'The device holds no file table, so it has no stored SCR_XXX files.',
      );
    }
    final available = _available;
    if (available.isEmpty) {
      return const StateMessage.empty(
        message: 'No stored scripts',
        details: 'Use "New script" to create one, or upload a SCR_XXX file.',
      );
    }
    return ListView.separated(
      padding: Insets.row,
      itemCount: available.length,
      separatorBuilder: (context, index) => const SizedBox(height: Spacing.md),
      itemBuilder: (context, i) {
        final file = available[i];
        final name = normalizeFileName(file.name);
        return Card(
          child: ListTile(
            leading: const Icon(Icons.description_outlined),
            title: Text(name),
            subtitle: Text('${file.size} bytes'),
            trailing: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                IconButton(
                  tooltip: 'Open editor',
                  icon: const Icon(Icons.edit_note),
                  onPressed: () {
                    final id = scriptFileId(name);
                    if (id != null) _openEditor(id, name, false);
                  },
                ),
                TextButton.icon(
                  onPressed: () => _loadFile(file),
                  icon: const Icon(Icons.play_arrow, size: IconSize.md),
                  label: const Text('Load'),
                ),
                IconButton(
                  icon: const Icon(Icons.delete_outline, color: kError),
                  onPressed: () => _deleteFile(file),
                ),
              ],
            ),
          ),
        );
      },
    );
  }

  void _openEditor(int fileId, String name, bool loaded, {int? slot}) {
    Navigator.of(context).push(
      MaterialPageRoute(
        builder: (_) => ScriptEditorPage(
          deviceId: widget.deviceId,
          fileId: fileId,
          name: name,
          loaded: loaded,
          slot: slot,
        ),
      ),
    );
  }
}
