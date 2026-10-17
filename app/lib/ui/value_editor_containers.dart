part of 'value_editor.dart';

// ---------------------------------------------------------------------------
// Shared editor blocks
// ---------------------------------------------------------------------------

/// A count stepper: a label, a decrement, the centred numeric field and an
/// increment - the one implementation behind every size control in the family
/// (a vector's length, a matrix's rows and columns). The caller owns what a new
/// count means, so the clamps and the resize stay with the data they resize.
class _SizeStepper extends StatelessWidget {
  const _SizeStepper({
    required this.label,
    required this.controller,
    required this.onText,
    required this.onStep,
  });

  /// The label printed before the field (`Size`, `Rows`, `Cols`).
  final String label;

  /// The field's text; the caller keeps it in sync with the count.
  final TextEditingController controller;

  /// The text as typed, for a caller that re-reads the count from it.
  final ValueChanged<String> onText;

  /// A one-step change from the `-`/`+` buttons (the delta is -1 or 1).
  final ValueChanged<int> onStep;

  /// A dense numeric field: wide enough for the two digits a count reaches,
  /// narrow enough that both buttons stay beside it.
  static const double _fieldWidth = 44;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Text('$label: ', style: Theme.of(context).textTheme.labelSmall),
        IconButton(
          icon: const Icon(Icons.remove_circle_outline, size: IconSize.lg),
          onPressed: () => onStep(-1),
        ),
        SizedBox(
          width: _fieldWidth,
          child: TextField(
            controller: controller,
            keyboardType: TextInputType.number,
            textAlign: TextAlign.center,
            decoration: const InputDecoration(),
            onChanged: onText,
          ),
        ),
        IconButton(
          icon: const Icon(Icons.add_circle_outline, size: IconSize.lg),
          onPressed: () => onStep(1),
        ),
      ],
    );
  }
}

/// A `rows` x `cols` grid of numeric cells: the one implementation behind the
/// matrix editor and the transform editor's raw mode. [labels] names each cell
/// (the transform prints the matrix element name); [onCellChanged] fires on
/// every keystroke for an editor that re-derives its other view from the cells.
class _NumberGrid extends StatelessWidget {
  const _NumberGrid({
    required this.rows,
    required this.cols,
    required this.controllers,
    this.labels,
    this.onCellChanged,
    this.signed = false,
  });

  final int rows;
  final int cols;

  /// One controller per cell, row-major.
  final List<TextEditingController> controllers;

  /// Cell labels, row-major; null for an unlabelled grid.
  final List<String>? labels;

  /// Fires after any cell is edited.
  final VoidCallback? onCellChanged;

  /// Accept a minus sign and a decimal point in the cell keyboard.
  final bool signed;

  @override
  Widget build(BuildContext context) {
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        for (var r = 0; r < rows; r++)
          Row(
            children: [
              for (var c = 0; c < cols; c++)
                Expanded(
                  child: Padding(
                    padding: const EdgeInsets.all(Spacing.xs),
                    child: TextField(
                      controller: controllers[r * cols + c],
                      keyboardType: signed
                          ? const TextInputType.numberWithOptions(
                              signed: true,
                              decimal: true,
                            )
                          : TextInputType.number,
                      textAlign: TextAlign.center,
                      decoration: labels == null
                          ? null
                          : InputDecoration(labelText: labels![r * cols + c]),
                      onChanged: onCellChanged == null
                          ? null
                          : (_) => onCellChanged!(),
                    ),
                  ),
                ),
            ],
          ),
      ],
    );
  }
}

/// Vector (N Numbers): one numeric field per component, with a size picker so the
/// length can be chosen/edited (Docs/Data Formats.md: size-flexible).
Future<List<int>?> _editVector(
  BuildContext context,
  FieldInfo? info,
  List<int> current,
) {
  var n = current.length >= 4 ? (current.length ~/ 4) : 3;
  if (n < 1) {
    n = 1;
  }
  final hasValue = current.length >= n * 4;
  final controllers = List.generate(
    n,
    (i) => TextEditingController(
      text: hasValue ? numberFromBytes(current, i * 4).toString() : '0.0',
    ),
  );
  final sizeCtrl = TextEditingController(text: '$n');
  final unit = info?.unit;

  void resize(int newN) {
    if (newN < 1) {
      newN = 1;
    }
    if (newN > 32) {
      newN = 32;
    }
    n = newN;
    while (controllers.length < n) {
      controllers.add(TextEditingController(text: '0.0'));
    }
    while (controllers.length > n) {
      controllers.removeLast();
    }
    sizeCtrl.text = '$n';
  }

  return showTamuDialog<List<int>>(
    context,
    title: unit == null ? (info?.name ?? 'Vector') : '${info!.name} [$unit]',
    body: StatefulBuilder(
      builder: (context, setState) => Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          _SizeStepper(
            label: 'Size',
            controller: sizeCtrl,
            onText: (text) => setState(() => resize(int.tryParse(text) ?? n)),
            onStep: (delta) => setState(() => resize(n + delta)),
          ),
          for (var i = 0; i < n; i++)
            Padding(
              padding: const EdgeInsets.symmetric(vertical: Spacing.xs),
              child: TextField(
                controller: controllers[i],
                keyboardType: TextInputType.number,
                decoration: InputDecoration(labelText: i < 3 ? 'XYZ'[i] : '$i'),
              ),
            ),
        ],
      ),
    ),
    actions: tamuDialogActions(
      onCancel: () => Navigator.pop(context),
      onConfirm: () {
        final values = controllers.map((c) => double.tryParse(c.text)).toList();
        if (values.any((v) => v == null)) {
          return;
        }
        final out = BytesBuilder();
        for (final v in values) {
          out.add(numberToBytes(v!));
        }
        Navigator.pop(context, out.toBytes());
      },
    ),
  );
}

/// Matrix (Rows x Cols of Number): grid editor with the header (h, w) preserved, and
/// rows/cols size pickers.
Future<List<int>?> _editMatrix(
  BuildContext context,
  FieldInfo? info,
  List<int> current,
) {
  var h = current.length >= 2 ? current[0] | (current[1] << 8) : 0;
  var w = current.length >= 4 ? current[2] | (current[3] << 8) : 0;
  if (h == 0 || w == 0 || h * w * 4 + 4 > current.length || h > 6 || w > 6) {
    if (current.isEmpty) {
      // Brand-new entry: offer a fresh identity-like 3x3 of zeros.
      h = w = 3;
      current = [
        h & 0xFF,
        h >> 8,
        w & 0xFF,
        w >> 8,
        for (var i = 0; i < h * w * 4; i++) 0,
      ];
    } else {
      return _editHex(context, 'Matrix ${h}x$w', current);
    }
  }
  if (h < 1) {
    h = 1;
  }
  if (w < 1) {
    w = 1;
  }
  final rowsCtrl = TextEditingController(text: '$h');
  final colsCtrl = TextEditingController(text: '$w');
  final controllers = List.generate(
    h * w,
    (i) => TextEditingController(
      text: numberFromBytes(current, 4 + i * 4).toString(),
    ),
  );

  void resize(int newH, int newW) {
    if (newH < 1) {
      newH = 1;
    }
    if (newW < 1) {
      newW = 1;
    }
    if (newH > 6) {
      newH = 6;
    }
    if (newW > 6) {
      newW = 6;
    }
    h = newH;
    w = newW;
    while (controllers.length < h * w) {
      controllers.add(TextEditingController(text: '0.0'));
    }
    while (controllers.length > h * w) {
      controllers.removeLast();
    }
    rowsCtrl.text = '$h';
    colsCtrl.text = '$w';
  }

  return showTamuDialog<List<int>>(
    context,
    title: info?.name ?? 'Matrix ${h}x$w',
    maxWidth: 320,
    body: StatefulBuilder(
      builder: (context, setState) {
        return Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            _SizeStepper(
              label: 'Rows',
              controller: rowsCtrl,
              onText: (text) =>
                  setState(() => resize(int.tryParse(text) ?? 0, w)),
              onStep: (delta) => setState(() => resize(h + delta, w)),
            ),
            _SizeStepper(
              label: 'Cols',
              controller: colsCtrl,
              onText: (text) =>
                  setState(() => resize(h, int.tryParse(text) ?? 0)),
              onStep: (delta) => setState(() => resize(h, w + delta)),
            ),
            const SizedBox(height: Spacing.sm),
            _NumberGrid(rows: h, cols: w, controllers: controllers),
          ],
        );
      },
    ),
    actions: tamuDialogActions(
      onCancel: () => Navigator.pop(context),
      onConfirm: () {
        final values = controllers.map((c) => double.tryParse(c.text)).toList();
        if (values.any((v) => v == null)) {
          return;
        }
        final out = BytesBuilder()..add([h & 0xFF, h >> 8, w & 0xFF, w >> 8]);
        for (final v in values) {
          out.add(numberToBytes(v!));
        }
        Navigator.pop(context, out.toBytes());
      },
    ),
  );
}

/// BlockInfo: a 32-bit register pointer (type 10 | instance 6 | field 8 | key 8) - the
/// same layout the Register service uses (Docs/Data Formats.md). The editor picks the
/// block type and the instance/field/key, showing the registry field name where known.
Future<List<int>?> _editBlockInfo(BuildContext context, List<int> current) {
  var bi = current.length >= 4 ? uint32FromBytes(current) : 0;
  var type = (bi >> 22) & 0x3FF;
  final inst = (bi >> 16) & 0x3F;
  final field = (bi >> 8) & 0xFF;
  final key = bi & 0xFF;
  final instC = TextEditingController(text: '$inst');
  final fieldC = TextEditingController(text: '$field');
  final keyC = TextEditingController(text: '$key');

  final types = <(int, String)>[
    (systemBlockTypeValue, 'System'),
    for (final t in BlockType.values)
      if (t != BlockType.none &&
          t != BlockType.undefined &&
          t != BlockType.deleted &&
          // App-only markers with no wire type: not addressable.
          t != BlockType.script &&
          t != BlockType.dynamic)
        (t.value, t.label),
  ];
  final typeLabels = {for (final (value, label) in types) value: label};

  return showTamuDialog<List<int>>(
    context,
    title: 'BlockInfo',
    // The shared dialog body (400): the dropdown's widest label ('Resistive
    // Measure') does not fit the 360 this editor used to bound itself to.
    body: StatefulBuilder(
      builder: (context, setState) {
        final name = blockInfoFor(
          BlockType.fromValue(type),
        )?.field(field)?.name;
        return Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            DropdownButtonFormField<int>(
              initialValue: type,
              decoration: const InputDecoration(labelText: 'Block'),
              items: dialogValueItems(
                types.map((e) => e.$1),
                type,
                (value) => typeLabels[value] ?? blockTypeLabel(value),
              ),
              onChanged: (v) => setState(() {
                if (v != null) {
                  type = v;
                }
              }),
            ),
            const SizedBox(height: Spacing.md),
            Row(
              children: [
                Expanded(
                  child: TextField(
                    controller: instC,
                    keyboardType: TextInputType.number,
                    decoration: const InputDecoration(labelText: 'Instance'),
                  ),
                ),
                const SizedBox(width: Spacing.md),
                Expanded(
                  child: TextField(
                    controller: fieldC,
                    keyboardType: TextInputType.number,
                    decoration: const InputDecoration(labelText: 'Field'),
                  ),
                ),
                const SizedBox(width: Spacing.md),
                Expanded(
                  child: TextField(
                    controller: keyC,
                    keyboardType: TextInputType.number,
                    decoration: const InputDecoration(labelText: 'Key'),
                  ),
                ),
              ],
            ),
            if (name != null)
              Padding(
                padding: const EdgeInsets.only(top: Spacing.md),
                child: Align(
                  alignment: Alignment.centerLeft,
                  child: Text(
                    'Field: $name',
                    style: Theme.of(
                      context,
                    ).textTheme.labelSmall?.copyWith(color: kTextMuted),
                  ),
                ),
              ),
          ],
        );
      },
    ),
    actions: tamuDialogActions(
      onCancel: () => Navigator.pop(context),
      onConfirm: () {
        final i = int.tryParse(instC.text) ?? 0;
        final f = int.tryParse(fieldC.text) ?? 0;
        final k = int.tryParse(keyC.text) ?? 0;
        Navigator.pop(context, uint32ToBytes(makeBlockInfo(type, i, f, k)));
      },
    ),
  );
}
