/// Shared script UI pieces (Docs/App/Service views/Script.md): state chips and the I/O
/// tables of a loaded script. Inputs are rendered using their UI specification (sliders,
/// toggles, buttons - docs: "formatted with the UI specifications").
///
/// Everything here reads the theme tokens and the shared widgets (phase 1c, batch
/// 2): a status dot, a section heading, a chunk of monospace text and the control
/// row are each defined once, for the Scripts page, the editor and the dialogs.
library;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../core/script_client.dart';
import '../core/script_fault.dart';
import '../core/script_file.dart';
import '../core/types.dart';
import 'formatters.dart';
import 'theme.dart';
import 'value_editor.dart';
import 'viewer_kit.dart';
import 'widgets.dart';

/// The status colour of a script state, in tokens.
Color scriptStateColor(int state) => switch (state) {
  ScriptState.running => kSuccess,
  ScriptState.paused => kWarning,
  ScriptState.waiting => kInfo,
  ScriptState.finished => kTextMuted,
  ScriptState.error => kError,
  _ => kTextDisabled,
};

/// The status dot of a script state: the System-live indicator the Scripts list
/// and the editor's control card both show.
class ScriptStateDot extends StatelessWidget {
  const ScriptStateDot(this.state, {super.key});

  final int state;

  @override
  Widget build(BuildContext context) {
    return Icon(Icons.circle, size: Spacing.lg, color: scriptStateColor(state));
  }
}

/// A decoded script fault, as the sentence the Scripts page and the editor show next to the
/// script's state, with a Copy that puts that sentence on the clipboard.
///
/// The text is the whole point: a report record's `instruction | error` pair prints as
/// `error 2 at cat 4 op 1` and says nothing. [ScriptFault.text] names the line, the instruction,
/// the register it addressed and the error.
class ScriptFaultRow extends StatelessWidget {
  const ScriptFaultRow(this.fault, {super.key, this.dense = false});

  final ScriptFault fault;

  /// The list-row size (the Scripts page card) rather than the editor card's.
  final bool dense;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final style =
        (dense ? theme.textTheme.bodySmall : theme.textTheme.bodyMedium)
            ?.copyWith(color: kError);
    return Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Expanded(child: SelectableText(fault.text, style: style)),
        IconButton(
          visualDensity: VisualDensity.compact,
          tooltip: 'Copy fault as text',
          icon: Icon(Icons.copy_all, size: dense ? IconSize.sm : IconSize.md),
          onPressed: () async {
            await Clipboard.setData(ClipboardData(text: fault.text));
            if (!context.mounted) return;
            showSnack(context, 'Fault copied');
          },
        ),
      ],
    );
  }
}

/// A script card's section heading: the accent [SectionLabel] with an optional
/// leading icon, an optional count and an optional trailing action (the "add"
/// button). The one heading shape the cluster's cards, dialogs and pickers use.
class ScriptCardHeading extends StatelessWidget {
  const ScriptCardHeading(
    this.title, {
    super.key,
    this.icon,
    this.count,
    this.action,
  });

  final String title;

  /// An icon before the label (the I/O sections carry the input/output mark).
  final IconData? icon;

  /// The item count shown after the label, e.g. `3` or `12 lines`.
  final String? count;

  /// Trailing content, normally the section's add button.
  final Widget? action;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Row(
      children: [
        if (icon != null) ...[
          Icon(icon, size: IconSize.sm, color: kOrange),
          const SizedBox(width: Spacing.sm),
        ],
        Expanded(child: SectionLabel(title)),
        if (count != null) ...[
          const SizedBox(width: Spacing.sm),
          Text(count!, style: theme.textTheme.labelSmall),
        ],
        ?action,
      ],
    );
  }
}

/// The Start/Pause/Stop/Restart control row shared by the Scripts list and the script editor.
class ScriptControlBar extends StatelessWidget {
  final int state;
  final VoidCallback onStart;
  final VoidCallback onPauseToggle;
  final VoidCallback onStop;
  final VoidCallback onRestart;

  /// Page-specific trailing buttons (Unload/Open editor, Move to line, ...).
  final List<Widget> extra;

  const ScriptControlBar({
    super.key,
    required this.state,
    required this.onStart,
    required this.onPauseToggle,
    required this.onStop,
    required this.onRestart,
    this.extra = const [],
  });

  @override
  Widget build(BuildContext context) {
    Widget control(IconData icon, String label, VoidCallback onPressed) =>
        OutlinedButton.icon(
          onPressed: onPressed,
          icon: Icon(icon, size: IconSize.md),
          label: Text(label),
        );
    return Wrap(
      spacing: Spacing.md,
      children: [
        control(Icons.play_arrow, 'Start', onStart),
        control(
          state == ScriptState.paused ? Icons.play_arrow : Icons.pause,
          state == ScriptState.paused ? 'Continue' : 'Pause',
          onPauseToggle,
        ),
        control(Icons.stop, 'Stop', onStop),
        control(Icons.restart_alt, 'Restart', onRestart),
        ...extra,
      ],
    );
  }
}

/// The "Function library" property switch, shared by the script create dialog and the
/// editor's function card.
class FunctionLibrarySwitch extends StatelessWidget {
  final bool value;
  final ValueChanged<bool> onChanged;

  const FunctionLibrarySwitch({
    super.key,
    required this.value,
    required this.onChanged,
  });

  @override
  Widget build(BuildContext context) {
    return SwitchListTile(
      dense: true,
      contentPadding: EdgeInsets.zero,
      title: const Text('Function library'),
      subtitle: Text(
        'The file is a function library, not a script',
        style: Theme.of(context).textTheme.labelSmall,
      ),
      value: value,
      onChanged: onChanged,
    );
  }
}

/// One category (inputs or outputs) of a loaded script's Register entries. Inputs are
/// editable, outputs are read-only.
class ScriptIoSection extends StatefulWidget {
  final ScriptClient client;
  final int slot;
  final int field;
  final String title;
  final IconData icon;
  final bool editable;

  /// Per-input UI specifications (inputs only), read from the script file.
  final List<ScriptInputSpec>? specs;

  /// Per-input display names (inputs only), from the script file's UI info.
  final List<String> names;

  /// Bump to force a re-read (the parent's refresh tick).
  final int revision;
  final VoidCallback? onChanged;

  const ScriptIoSection({
    super.key,
    required this.client,
    required this.slot,
    required this.field,
    required this.title,
    required this.icon,
    required this.editable,
    required this.revision,
    this.specs,
    this.names = const [],
    this.onChanged,
  });

  @override
  State<ScriptIoSection> createState() => _ScriptIoSectionState();
}

class _ScriptIoSectionState extends State<ScriptIoSection> {
  List<ScriptEntry?> _entries = [];
  bool _loaded = false;

  @override
  void initState() {
    super.initState();
    _load();
  }

  @override
  void didUpdateWidget(covariant ScriptIoSection oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.revision != widget.revision ||
        oldWidget.slot != widget.slot) {
      _load();
    }
  }

  Future<void> _load() async {
    final keys = await widget.client.enumerateKeys(widget.slot, widget.field);
    final entries = <ScriptEntry?>[];
    for (final key in keys) {
      entries.add(
        await widget.client.readEntry(widget.slot, widget.field, key),
      );
    }
    if (!mounted) return;
    setState(() {
      _entries = entries;
      _loaded = true;
    });
  }

  Future<void> _edit(int key, ScriptEntry entry) async {
    final next = await showValueEditor(
      context,
      entry.meta.dataType,
      entry.value,
    );
    if (next == null || !mounted) return;
    // Declare the actual value length (a shorter String is space-padded by the firmware);
    // the field's declared Size would make the device copy stale payload bytes.
    final meta = ValueInfo(
      type: entry.meta.type,
      flags: entry.meta.flags,
      size: next.length,
      key: entry.meta.key,
    );
    final ok = await widget.client.writeEntry(
      widget.slot,
      widget.field,
      key,
      meta,
      next,
    );
    if (!mounted) return;
    showSnack(context, ok ? '${widget.title} $key updated' : 'Write failed');
    widget.onChanged?.call();
  }

  @override
  Widget build(BuildContext context) {
    if (!_loaded || _entries.isEmpty) return const SizedBox.shrink();
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(
            Spacing.xl,
            Spacing.lg,
            Spacing.xl,
            Spacing.xs,
          ),
          child: ScriptCardHeading(
            widget.title,
            icon: widget.icon,
            count: '${_entries.length}',
          ),
        ),
        for (var i = 0; i < _entries.length; i++)
          if (_entries[i] != null) _row(i, _entries[i]!),
      ],
    );
  }

  ScriptInputSpec? _specFor(int index) {
    final specs = widget.specs;
    if (widget.field != ScriptField.input ||
        specs == null ||
        index >= specs.length) {
      return null;
    }
    return specs[index];
  }

  /// The input's display name (falls back to the generic "Input N").
  String _nameFor(int index) =>
      index < widget.names.length && widget.names[index].isNotEmpty
      ? widget.names[index]
      : '${widget.title} $index';

  Widget _row(int index, ScriptEntry entry) {
    final spec = _specFor(index);
    if (widget.editable && spec != null) {
      final isSlider =
          spec.uiType == ScriptUiType.slider &&
          spec.max > spec.min &&
          entry.meta.dataType == DataType.number;
      final isToggle =
          spec.uiType == ScriptUiType.toggle &&
          entry.meta.dataType == DataType.bool_;
      final isButton =
          spec.uiType == ScriptUiType.button &&
          entry.meta.dataType == DataType.bool_;
      // A custom enum (dropdown, or auto with labels) is rendered by the control too.
      final isEnum =
          entry.meta.dataType == DataType.enum_ &&
          (spec.uiType == ScriptUiType.dropdown ||
              (spec.uiType == ScriptUiType.auto && spec.options.isNotEmpty));
      if (isSlider || isToggle || isButton || isEnum) {
        return ScriptInputControl(
          client: widget.client,
          slot: widget.slot,
          inputIndex: index,
          entry: entry,
          spec: spec,
          title: _nameFor(index),
          onChanged: widget.onChanged,
        );
      }
    }
    final label = formatValue(entry.meta.dataType, entry.value);
    final theme = Theme.of(context);
    return ListTile(
      dense: true,
      contentPadding: const EdgeInsets.symmetric(horizontal: Spacing.xxl),
      title: Text(_nameFor(index)),
      subtitle: Text(
        dataTypeLabel(entry.meta.dataType),
        style: theme.textTheme.labelSmall,
      ),
      trailing: MonoText(label, muted: true),
      onTap: widget.editable ? () => _edit(index, entry) : null,
    );
  }
}

/// An input rendered per its UI specification: a slider, a toggle or a momentary button.
class ScriptInputControl extends StatefulWidget {
  final ScriptClient client;
  final int slot;
  final int inputIndex;
  final ScriptEntry entry;
  final ScriptInputSpec spec;
  final String title;
  final VoidCallback? onChanged;

  const ScriptInputControl({
    super.key,
    required this.client,
    required this.slot,
    required this.inputIndex,
    required this.entry,
    required this.spec,
    required this.title,
    this.onChanged,
  });

  @override
  State<ScriptInputControl> createState() => _ScriptInputControlState();
}

class _ScriptInputControlState extends State<ScriptInputControl> {
  double _value = 0;
  bool _dragging = false;

  @override
  void initState() {
    super.initState();
    _syncFromEntry();
  }

  @override
  void didUpdateWidget(covariant ScriptInputControl oldWidget) {
    super.didUpdateWidget(oldWidget);
    // Re-sync with the device value on refresh, but not while the user is dragging.
    if (!_dragging) _syncFromEntry();
  }

  void _syncFromEntry() {
    final v = widget.entry.value;
    _value = v.length >= 4
        ? numberFromBytes(v)
        : (v.isNotEmpty ? v.first.toDouble() : 0);
  }

  bool get _bool =>
      widget.entry.value.isNotEmpty && widget.entry.value.first != 0;

  Future<void> _write(List<int> bytes) async {
    final ok = await widget.client.writeEntry(
      widget.slot,
      ScriptField.input,
      widget.inputIndex,
      widget.entry.meta,
      bytes,
    );
    if (!mounted) return;
    if (!ok) showSnack(context, 'Write failed');
    widget.onChanged?.call();
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    switch (widget.spec.uiType) {
      case ScriptUiType.slider:
        final min = widget.spec.min;
        final max = widget.spec.max;
        final step = widget.spec.step > 0
            ? widget.spec.step
            : (max - min) / 100;
        final divisions = ((max - min) / step).round().clamp(1, 1000);
        return Padding(
          padding: const EdgeInsets.symmetric(horizontal: Spacing.xxl),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                children: [
                  Expanded(
                    child: Text(
                      widget.title,
                      style: theme.textTheme.bodyMedium,
                    ),
                  ),
                  MonoText(_value.toStringAsFixed(2), muted: true),
                ],
              ),
              Slider(
                value: _value.clamp(min, max),
                min: min,
                max: max,
                divisions: divisions,
                onChangeStart: (_) => _dragging = true,
                onChanged: (v) => setState(() => _value = v),
                onChangeEnd: (v) {
                  _dragging = false;
                  _write(numberToBytes(v));
                },
              ),
            ],
          ),
        );
      case ScriptUiType.toggle:
        return SwitchListTile(
          dense: true,
          contentPadding: const EdgeInsets.symmetric(horizontal: Spacing.xxl),
          title: Text(widget.title),
          value: _bool,
          onChanged: (v) => _write([v ? 1 : 0]),
        );
      case ScriptUiType.button:
        return ListTile(
          dense: true,
          contentPadding: const EdgeInsets.symmetric(horizontal: Spacing.xxl),
          title: Text(widget.title),
          trailing: FilledButton(
            onPressed: () => _write([1]),
            child: const Text('Press'),
          ),
        );
      case ScriptUiType.dropdown:
      case ScriptUiType.auto:
        // "Custom enum": named choices over an integer value (the stored value stays the
        // plain index - the labels live in the script's UI info). `auto` with labels is
        // treated as a dropdown so a re-saved script keeps working.
        final options = widget.spec.options;
        if (options.isEmpty) {
          if (widget.spec.uiType == ScriptUiType.auto) {
            return ListTile(
              dense: true,
              contentPadding: const EdgeInsets.symmetric(
                horizontal: Spacing.xxl,
              ),
              title: Text(widget.title),
              trailing: Text(
                _value.toStringAsFixed(0),
                style: theme.textTheme.labelMedium,
              ),
            );
          }
          return const SizedBox.shrink();
        }
        final index = _value.round().clamp(0, options.length - 1);
        return ListTile(
          dense: true,
          contentPadding: const EdgeInsets.symmetric(horizontal: Spacing.xxl),
          title: Text(widget.title),
          trailing: DropdownButton<int>(
            value: index,
            items: [
              for (var i = 0; i < options.length; i++)
                DropdownMenuItem<int>(value: i, child: Text(options[i])),
            ],
            onChanged: (v) {
              if (v != null) _write(_enumBytes(v));
            },
          ),
        );
      default:
        return const SizedBox.shrink();
    }
  }

  /// Encodes an enum choice with the input's declared type (1 byte enum/bool, else 4).
  List<int> _enumBytes(int index) {
    final t = widget.entry.meta.dataType;
    if (t == DataType.enum_ || t == DataType.bool_) return [index & 0xFF];
    return numberToBytes(index.toDouble());
  }
}
