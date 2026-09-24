/// Context-aware script symbol picker (Docs/App/Service views/Script.md: "recommendations
/// ... based on context, per symbol category split, variable/constant creation shortcuts").
library;

import 'dart:typed_data';

import 'package:flutter/material.dart';

import '../core/script_draft.dart';
import '../core/script_file.dart';
import '../core/script_instructions.dart';
import '../core/types.dart';
import 'block_info_picker.dart';
import 'theme.dart';
import 'value_editor.dart';
import 'widgets.dart';

class ScriptSymbolPicker extends StatefulWidget {
  final ScriptDraft draft;
  final bool destination;

  /// The instruction the symbol is being added to (drives the allowed kinds/counts).
  final ScriptInstructionDef? def;
  final int? operandIndex;
  final int deviceId;
  final String Function(ScriptDraft, ScriptSymbol) nameOf;

  const ScriptSymbolPicker({
    super.key,
    required this.draft,
    required this.destination,
    this.def,
    this.operandIndex,
    required this.deviceId,
    required this.nameOf,
  });

  @override
  State<ScriptSymbolPicker> createState() => _ScriptSymbolPickerState();
}

class _ScriptSymbolPickerState extends State<ScriptSymbolPicker> {
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

  /// True when the destination being picked must be an integer (Index/Uint32).
  bool get _integerDestination =>
      widget.destination && widget.def?.integerDestination == true;

  /// Filters a candidate symbol by the current instruction's expectations.
  bool _allowed(ScriptDraft draft, ScriptSymbol s) {
    if (widget.destination) {
      if (s.type != symVariable && s.type != symOutput) return false;
      if (_integerDestination) {
        final t = draft.valueTypeOf(s);
        return t != null && integerDestinationTypes.contains(t.value);
      }
      return true;
    }
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

  /// True for Select's boolean condition (If/While/Wait until are expressions, so their
  /// `expression` branch handles them before this is consulted).
  bool get _conditionOperand {
    final def = widget.def;
    return def != null &&
        !widget.destination &&
        def.category == catLogic &&
        def.op == 12 &&
        widget.operandIndex == 0;
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
    bool isInteger(DataType t) => integerDestinationTypes.contains(t.value);
    bool numeric(DataType t) => scriptTypeIsNumeric(t.value);

    if (widget.def?.expression == true) {
      // An expression: values plus a few common operators.
      addVarsWhere(any);
      addConstsWhere(any);
      candidates.add(ScriptSymbol.predefine(preMathOp, 0)); // +
      candidates.add(ScriptSymbol.predefine(preMathOp, 6)); // AND
      candidates.add(ScriptSymbol.predefine(preMathOp, 16)); // >
      candidates.add(ScriptSymbol.predefine(preBool, 1));
    } else if (widget.destination) {
      if (_integerDestination) {
        addVarsWhere(isInteger);
        for (var i = 0; i < draft.outputs.length; i++) {
          if (isInteger(draft.outputs[i].type)) candidates.add(ScriptSymbol.output(i));
        }
      } else {
        addVarsWhere(any); // writable first
        for (var i = 0; i < draft.outputs.length; i++) {
          candidates.add(ScriptSymbol.output(i));
        }
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
      if (_integerDestination) {
        return 'Destination: an integer variable or output (Index/Uint32)';
      }
      return 'Destination: a variable or output';
    }
    if (_constantOnly) return 'Register address (a 4-byte BlockInfo constant)';
    if (_addressOperand) return 'Device address (Id)';
    if (_targetOperand) return 'Target line index';
    if (widget.def?.expression == true) {
      return 'Expression: values and operators (+ − × ÷ % ^, AND OR XOR NOT, comparisons, parentheses)';
    }
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
    if (widget.def?.expression == true) {
      // Infix expression: values plus inline operators / parentheses.
      return [
        ('Values', filter([
          for (var i = 0; i < draft.inputs.length; i++) ScriptSymbol.input(i),
          for (var i = 0; i < draft.variables.length; i++) ScriptSymbol.variable(i),
          for (var i = 0; i < draft.constants.length; i++) ScriptSymbol.constant(i),
        ])),
        ('Operators', [
          for (final (op, _) in scriptPredefineMathOps)
            if (expressionOps.contains(op)) ScriptSymbol.predefine(preMathOp, op),
        ]),
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
              builder: (_) => ScriptPredefineValueDialog(subtype: subtype),
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
                  context: context, builder: (_) => const ScriptLiteralDialog());
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

/// Chooses a predefine's value for its subtype (Docs predefine subtypes: State, Type,
/// Index, Char, Math op, Bool).
class ScriptPredefineValueDialog extends StatefulWidget {
  final int subtype;

  const ScriptPredefineValueDialog({super.key, required this.subtype});

  @override
  State<ScriptPredefineValueDialog> createState() => _ScriptPredefineValueDialogState();
}

class _ScriptPredefineValueDialogState extends State<ScriptPredefineValueDialog> {
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
class ScriptLiteralDialog extends StatefulWidget {
  const ScriptLiteralDialog({super.key});

  @override
  State<ScriptLiteralDialog> createState() => _ScriptLiteralDialogState();
}

class _ScriptLiteralDialogState extends State<ScriptLiteralDialog> {
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
