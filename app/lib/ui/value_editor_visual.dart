part of 'value_editor.dart';

/// The bytes a hex field's text encodes, or null when it carries no whole byte
/// pair. Separators and any other non-hex character are ignored, which is what
/// the hex fields have always accepted (`ab cd` reads as one byte).
List<int>? _hexBytes(String text) {
  final clean = text.replaceAll(RegExp(r'[^0-9a-fA-F]'), '');
  if (clean.isEmpty || clean.length.isOdd) {
    return null;
  }
  return [
    for (var i = 0; i < clean.length; i += 2)
      int.parse(clean.substring(i, i + 2), radix: 16),
  ];
}

/// Colour: RGBA byte order on the wire (Data Formats.md). Editor offers an RGBA hex
/// field, HSVA sliders, a live preview and presets.
Future<List<int>?> _editColour(BuildContext context, List<int> current) {
  var r = current.length >= 4 ? current[0] : 255;
  var g = current.length >= 4 ? current[1] : 255;
  var b = current.length >= 4 ? current[2] : 255;
  var a = current.length >= 4 ? current[3] : 255;

  // The four bytes as `RRGGBBAA`: the hex field's text and its round trip.
  String hex() =>
      [r, g, b, a].map((v) => v.toRadixString(16).padLeft(2, '0')).join();

  final controller = TextEditingController(text: hex());
  Color preview() => Color.fromARGB((a & 0xFF), r & 0xFF, g & 0xFF, b & 0xFF);

  // HSVA slider state, derived from the current RGBA.
  var sliderH = 0.0, sliderS = 100.0, sliderV = 100.0;
  void syncHsv() {
    final hsv = HSVColor.fromColor(preview());
    sliderH = hsv.hue;
    sliderS = hsv.saturation * 100;
    sliderV = hsv.value * 100;
  }

  syncHsv();

  void applyHsv() {
    final hsv = HSVColor.fromAHSV(
      a / 255,
      sliderH,
      sliderS / 100,
      sliderV / 100,
    );
    final c = hsv.toColor();
    r = (c.r * 255).round();
    g = (c.g * 255).round();
    b = (c.b * 255).round();
    controller.text = hex();
  }

  const presets = {
    'Off': [0, 0, 0, 0],
    'White': [255, 255, 255, 255],
    'Red': [255, 0, 0, 255],
    'Green': [0, 255, 0, 255],
    'Blue': [0, 0, 255, 255],
    'Orange': [255, 128, 0, 255],
  };

  return showTamuDialog<List<int>>(
    context,
    title: 'Colour',
    maxWidth: 320,
    body: StatefulBuilder(
      builder: (context, setState) {
        final styles = Theme.of(context).textTheme;
        Widget channel(
          String label,
          double value,
          double max,
          int divisions,
          String readout,
          ValueChanged<double> onChanged,
        ) => Row(
          children: [
            SizedBox(width: 34, child: Text(label, style: styles.labelSmall)),
            Expanded(
              child: Slider(
                value: value,
                min: 0,
                max: max,
                divisions: divisions,
                label: readout,
                onChanged: onChanged,
              ),
            ),
            SizedBox(
              width: 34,
              child: Text(
                readout,
                textAlign: TextAlign.right,
                style: styles.labelSmall,
              ),
            ),
          ],
        );

        return Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            // Live preview.
            Container(
              height: 44,
              width: double.infinity,
              decoration: BoxDecoration(
                color: preview(),
                border: Border.all(color: kLine),
                borderRadius: Radii.control,
              ),
            ),
            const SizedBox(height: Spacing.md),
            // RGBA hex entry.
            TextField(
              controller: controller,
              maxLength: 8,
              decoration: const InputDecoration(
                hintText: 'RRGGBBAA',
                labelText: 'RGBA hex',
              ),
              onChanged: (text) {
                final bytes = _hexBytes(text);
                if (bytes != null && bytes.length == 4) {
                  r = bytes[0];
                  g = bytes[1];
                  b = bytes[2];
                  a = bytes[3];
                  syncHsv();
                }
                setState(() {});
              },
            ),
            const SizedBox(height: Spacing.xs),
            // HSVA sliders.
            channel('H', sliderH, 360, 360, '${sliderH.round()}°', (v) {
              setState(() {
                sliderH = v;
                applyHsv();
              });
            }),
            channel('S', sliderS, 100, 100, '${sliderS.round()}%', (v) {
              setState(() {
                sliderS = v;
                applyHsv();
              });
            }),
            channel('V', sliderV, 100, 100, '${sliderV.round()}%', (v) {
              setState(() {
                sliderV = v;
                applyHsv();
              });
            }),
            channel('A', a.toDouble(), 255, 255, '$a', (v) {
              setState(() {
                a = v.round();
                syncHsv();
              });
            }),
            const SizedBox(height: Spacing.sm),
            Wrap(
              spacing: Spacing.md,
              children: [
                for (final entry in presets.entries)
                  InkWell(
                    onTap: () {
                      final v = entry.value;
                      r = v[0];
                      g = v[1];
                      b = v[2];
                      a = v[3];
                      controller.text = hex();
                      syncHsv();
                      setState(() {});
                    },
                    child: Container(
                      width: 28,
                      height: 28,
                      decoration: BoxDecoration(
                        color: Color.fromARGB(
                          entry.value[3],
                          entry.value[0],
                          entry.value[1],
                          entry.value[2],
                        ),
                        border: Border.all(color: kLine),
                        borderRadius: Radii.tag,
                      ),
                    ),
                  ),
              ],
            ),
          ],
        );
      },
    ),
    actions: tamuDialogActions(
      onCancel: () => Navigator.pop(context),
      onConfirm: () => Navigator.pop(context, [r, g, b, a]),
    ),
  );
}

/// Matrix editor with two switchable modes for a 2x3 affine matrix [a b tx; c d ty]
/// (the render-block Position key):
///  - Transformation: Offset X/Y, Rotation, Scale X/Y, Skew, Mirror;
///  - Raw: the six matrix cells.
/// The transform mode reconstructs the matrix as:
///   [ cos.sx       cos.sx.tan(skew) - sin.sy    tx ]
///   [ sin.sx       sin.sx.tan(skew) + cos.sy    ty ]
Future<List<int>?> _editTransformMatrix(
  BuildContext context,
  List<int> current,
) {
  final t = Transform23()..fromMatrix(current);

  // Raw cell controllers (a, b, tx, c, d, ty).
  final raw = List.generate(
    6,
    (i) => TextEditingController(text: formatNumber3(t.toCells()[i])),
  );

  // Transform controllers + mirror flags.
  final tc = <String, TextEditingController>{};
  bool mirrorX = t.mirrorX, mirrorY = t.mirrorY;

  void syncTc() {
    // raw -> transform.
    final cells = raw.map((c) => double.tryParse(c.text)).toList();
    if (cells.contains(null)) {
      return;
    }
    t.fromCells(cells.cast<double>());
    mirrorX = t.mirrorX;
    mirrorY = t.mirrorY;
    tc['ox']!.text = formatNumber3(t.offsetX);
    tc['oy']!.text = formatNumber3(t.offsetY);
    tc['rot']!.text = formatNumber3(t.rotation);
    tc['sx']!.text = formatNumber3(t.scaleX);
    tc['sy']!.text = formatNumber3(t.scaleY);
    tc['skew']!.text = formatNumber3(t.skew);
  }

  void syncFromRaw() {
    syncTc();
  }

  void syncRaw() {
    // transform -> raw (called on every transform/mirror edit).
    t
      ..offsetX = double.tryParse(tc['ox']!.text) ?? t.offsetX
      ..offsetY = double.tryParse(tc['oy']!.text) ?? t.offsetY
      ..rotation = double.tryParse(tc['rot']!.text) ?? t.rotation
      ..scaleX = double.tryParse(tc['sx']!.text) ?? t.scaleX
      ..scaleY = double.tryParse(tc['sy']!.text) ?? t.scaleY
      ..skew = double.tryParse(tc['skew']!.text) ?? t.skew
      ..mirrorX = mirrorX
      ..mirrorY = mirrorY;
    final cells = t.toCells();
    for (var i = 0; i < 6; i++) {
      raw[i].text = formatNumber3(cells[i]);
    }
  }

  void reset() {
    t
      ..offsetX = 0
      ..offsetY = 0
      ..rotation = 0
      ..scaleX = 1
      ..scaleY = 1
      ..skew = 0
      ..mirrorX = false
      ..mirrorY = false;
    final cells = t.toCells();
    for (var i = 0; i < 6; i++) {
      raw[i].text = formatNumber3(cells[i]);
    }
    syncTc();
  }

  tc['ox'] = TextEditingController(text: formatNumber3(t.offsetX));
  tc['oy'] = TextEditingController(text: formatNumber3(t.offsetY));
  tc['rot'] = TextEditingController(text: formatNumber3(t.rotation));
  tc['sx'] = TextEditingController(text: formatNumber3(t.scaleX));
  tc['sy'] = TextEditingController(text: formatNumber3(t.scaleY));
  tc['skew'] = TextEditingController(text: formatNumber3(t.skew));

  List<int> build() {
    final cells = raw.map((c) => double.tryParse(c.text)).toList();
    if (!cells.contains(null)) {
      t.fromCells(cells.cast<double>());
    }
    return t.toMatrix();
  }

  const cellNames = ['a', 'b', 'tx', 'c', 'd', 'ty'];

  var rawMode = false;
  // The Reset action lives in the dialog's action row, outside the body's
  // StatefulBuilder, so it holds the setter to re-render the body it resets.
  late StateSetter rebuildBody;

  return showTamuDialog<List<int>>(
    context,
    title: 'Matrix (2x3)',
    maxWidth: 320,
    body: StatefulBuilder(
      builder: (context, setState) {
        rebuildBody = setState;
        final styles = Theme.of(context).textTheme;
        Widget row(String label, String key) => Padding(
          padding: const EdgeInsets.symmetric(vertical: Spacing.hairline),
          child: Row(
            children: [
              SizedBox(
                width: 100,
                child: Text(label, style: styles.labelSmall),
              ),
              Expanded(
                child: TextField(
                  controller: tc[key]!,
                  keyboardType: const TextInputType.numberWithOptions(
                    signed: true,
                    decimal: true,
                  ),
                  onChanged: (_) => syncRaw(),
                ),
              ),
            ],
          ),
        );

        return Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            SegmentedButton<bool>(
              segments: const [
                ButtonSegment(value: false, label: Text('Transform')),
                ButtonSegment(value: true, label: Text('Raw')),
              ],
              selected: {rawMode},
              onSelectionChanged: (sel) {
                final toRaw = sel.first;
                if (toRaw) {
                  syncTc();
                } else {
                  syncFromRaw();
                }
                setState(() => rawMode = toRaw);
              },
            ),
            const SizedBox(height: Spacing.md),
            if (!rawMode) ...[
              row('Offset X', 'ox'),
              row('Offset Y', 'oy'),
              row('Rotation °', 'rot'),
              row('Scale X', 'sx'),
              row('Scale Y', 'sy'),
              row('Skew °', 'skew'),
              Row(
                children: [
                  Checkbox(
                    value: mirrorX,
                    onChanged: (v) => setState(() {
                      mirrorX = v ?? false;
                      syncRaw();
                    }),
                  ),
                  Text('Mirror X', style: styles.labelSmall),
                  const SizedBox(width: Spacing.xxl),
                  Checkbox(
                    value: mirrorY,
                    onChanged: (v) => setState(() {
                      mirrorY = v ?? false;
                      syncRaw();
                    }),
                  ),
                  Text('Mirror Y', style: styles.labelSmall),
                ],
              ),
            ] else ...[
              _NumberGrid(
                rows: 2,
                cols: 3,
                controllers: raw,
                labels: cellNames,
                signed: true,
                onCellChanged: syncFromRaw,
              ),
            ],
          ],
        );
      },
    ),
    actions: [
      TextButton(
        onPressed: () => rebuildBody(reset),
        child: const Text('Reset'),
      ),
      ...tamuDialogActions(
        onCancel: () => Navigator.pop(context),
        onConfirm: () => Navigator.pop(context, build()),
      ),
    ],
  );
}

Future<List<int>?> _editInt(
  BuildContext context, {
  required String title,
  required bool signed,
  bool hex = false,
  int? initial,
}) {
  final controller = TextEditingController(text: initial?.toString() ?? '');
  return showTamuDialog<List<int>>(
    context,
    title: hex ? '$title (dec/hex)' : title,
    body: TextField(
      controller: controller,
      autofocus: true,
      keyboardType: TextInputType.number,
    ),
    actions: tamuDialogActions(
      onCancel: () => Navigator.pop(context),
      onConfirm: () {
        final text = controller.text.trim().toLowerCase();
        var value = text.startsWith('0x')
            ? int.tryParse(text.substring(2), radix: 16)
            : int.tryParse(text);
        if (value == null && hex) {
          value = int.tryParse(
            text.replaceAll(RegExp(r'[^0-9a-f]'), ''),
            radix: 16,
          );
        }
        if (value == null) {
          return;
        }
        if (!signed && value < 0) {
          return;
        }
        Navigator.pop(
          context,
          uint32ToBytes(signed ? value & 0xFFFFFFFF : value),
        );
      },
    ),
  );
}

Future<List<int>?> _editNetAddr(BuildContext context, List<int> current) {
  // The System NetID is a single byte; a full NetAddr is two (net.device). Preserve the
  // target size so a 1-byte field emits 1 byte.
  final oneByte = current.length == 1;
  final currentId = current.length >= 2
      ? current[0] | (current[1] << 8)
      : (oneByte ? current[0] : 0);
  final controller = TextEditingController(text: idToString(currentId));
  return showTamuDialog<List<int>>(
    context,
    title: oneByte ? 'Net ID' : 'Net address (net.device)',
    body: TextField(controller: controller, autofocus: true),
    actions: tamuDialogActions(
      onCancel: () => Navigator.pop(context),
      onConfirm: () {
        final id = idFromString(controller.text.trim());
        if (id == null) {
          return;
        }
        Navigator.pop(
          context,
          oneByte ? [id & 0xFF] : [id & 0xFF, (id >> 8) & 0xFF],
        );
      },
    ),
  );
}

Future<List<int>?> _editString(
  BuildContext context,
  String title,
  String current, {
  int maxChars = 23,
  bool secret = false,
}) {
  // A secret is never pre-filled: the field starts empty, so nothing the device holds (or
  // held) is echoed on screen, and an empty result means "unchanged".
  final controller = TextEditingController(
    text: secret ? '' : current.trimRight(),
  );
  return showTamuDialog<List<int>>(
    context,
    title: title,
    body: _SecretTextField(
      controller: controller,
      maxLength: maxChars,
      secret: secret,
    ),
    actions: tamuDialogActions(
      onCancel: () => Navigator.pop(context),
      onConfirm: () =>
          Navigator.pop(context, controller.text.codeUnits.toList()),
    ),
  );
}

/// The string editor's input: an ordinary field, or - for a secret - an obscured one with a
/// reveal toggle so the typist can check what was entered (the reveal dies with the dialog).
class _SecretTextField extends StatefulWidget {
  const _SecretTextField({
    required this.controller,
    required this.maxLength,
    required this.secret,
  });

  final TextEditingController controller;
  final int maxLength;
  final bool secret;

  @override
  State<_SecretTextField> createState() => _SecretTextFieldState();
}

class _SecretTextFieldState extends State<_SecretTextField> {
  bool _revealed = false;

  @override
  Widget build(BuildContext context) {
    return TextField(
      controller: widget.controller,
      autofocus: true,
      maxLength: widget.maxLength,
      obscureText: widget.secret && !_revealed,
      autocorrect: false,
      enableSuggestions: !widget.secret,
      decoration: widget.secret
          ? InputDecoration(
              hintText: 'Type the new value',
              suffixIcon: IconButton(
                tooltip: _revealed ? 'Hide' : 'Reveal',
                icon: Icon(
                  _revealed
                      ? Icons.visibility_off_outlined
                      : Icons.visibility_outlined,
                  size: IconSize.sm,
                ),
                onPressed: () => setState(() => _revealed = !_revealed),
              ),
            )
          : null,
    );
  }
}

Future<List<int>?> _editHex(
  BuildContext context,
  String title,
  List<int> current,
) {
  final controller = TextEditingController(
    text: current.map((int b) => b.toRadixString(16).padLeft(2, '0')).join(),
  );
  return showTamuDialog<List<int>>(
    context,
    title: '$title (hex)',
    body: TextField(controller: controller, autofocus: true),
    actions: tamuDialogActions(
      onCancel: () => Navigator.pop(context),
      onConfirm: () {
        final bytes = _hexBytes(controller.text);
        if (bytes == null) {
          return;
        }
        Navigator.pop(context, bytes);
      },
    ),
  );
}

// The shared number/hex formatters (formatNumber3, formatHexBytes) live in formatters.dart.
