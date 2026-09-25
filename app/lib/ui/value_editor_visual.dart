part of 'value_editor.dart';

/// Colour: RGBA byte order on the wire (Data Formats.md). Editor offers an RGBA hex
/// field, HSVA sliders, a live preview and presets.
Future<List<int>?> _editColour(BuildContext context, List<int> current) {  int r = current.length >= 4 ? current[0] : 255;
  int g = current.length >= 4 ? current[1] : 255;
  int b = current.length >= 4 ? current[2] : 255;
  int a = current.length >= 4 ? current[3] : 255;
  final controller = TextEditingController(
      text: [r, g, b, a].map((v) => v.toRadixString(16).padLeft(2, '0')).join());

  String hex() => [r, g, b, a].map((v) => v.toRadixString(16).padLeft(2, '0')).join();
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
    final hsv = HSVColor.fromAHSV(a / 255, sliderH, sliderS / 100, sliderV / 100);
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

  return showDialog<List<int>>(
    context: context,
    builder: (context) => StatefulBuilder(
      builder: (context, setState) => AlertDialog(
        title: const Text('Colour'),
        content: DialogBody(
          maxWidth: 320,
          child: Column(mainAxisSize: MainAxisSize.min, children: [
            // Live preview.
            Container(
              height: 44,
              width: double.infinity,
              decoration: BoxDecoration(
                  color: preview(),
                  border: Border.all(color: Colors.white24),
                  borderRadius: BorderRadius.circular(8)),
            ),
            const SizedBox(height: 8),
            // RGBA hex entry.
            TextField(
              controller: controller,
              maxLength: 8,
              decoration: const InputDecoration(hintText: 'RRGGBBAA', labelText: 'RGBA hex'),
              onChanged: (text) {
                final clean = text.replaceAll(RegExp(r'[^0-9a-fA-F]'), '');
                if (clean.length == 8) {
                  r = int.parse(clean.substring(0, 2), radix: 16);
                  g = int.parse(clean.substring(2, 4), radix: 16);
                  b = int.parse(clean.substring(4, 6), radix: 16);
                  a = int.parse(clean.substring(6, 8), radix: 16);
                  syncHsv();
                }
                setState(() {});
              },
            ),
            const SizedBox(height: 4),
            // HSVA sliders.
            Row(children: [
              const SizedBox(width: 34, child: Text('H', style: TextStyle(fontSize: 12))),
              Expanded(
                child: Slider(
                  value: sliderH, min: 0, max: 360, divisions: 360,
                  label: '${sliderH.round()}°',
                  onChanged: (v) => setState(() { sliderH = v; applyHsv(); }),
                ),
              ),
              SizedBox(width: 34, child: Text('${sliderH.round()}', textAlign: TextAlign.right, style: const TextStyle(fontSize: 11))),
            ]),
            Row(children: [
              const SizedBox(width: 34, child: Text('S', style: TextStyle(fontSize: 12))),
              Expanded(
                child: Slider(
                  value: sliderS, min: 0, max: 100, divisions: 100,
                  label: '${sliderS.round()}%',
                  onChanged: (v) => setState(() { sliderS = v; applyHsv(); }),
                ),
              ),
              SizedBox(width: 34, child: Text('${sliderS.round()}', textAlign: TextAlign.right, style: const TextStyle(fontSize: 11))),
            ]),
            Row(children: [
              const SizedBox(width: 34, child: Text('V', style: TextStyle(fontSize: 12))),
              Expanded(
                child: Slider(
                  value: sliderV, min: 0, max: 100, divisions: 100,
                  label: '${sliderV.round()}%',
                  onChanged: (v) => setState(() { sliderV = v; applyHsv(); }),
                ),
              ),
              SizedBox(width: 34, child: Text('${sliderV.round()}', textAlign: TextAlign.right, style: const TextStyle(fontSize: 11))),
            ]),
            Row(children: [
              const SizedBox(width: 34, child: Text('A', style: TextStyle(fontSize: 12))),
              Expanded(
                child: Slider(
                  value: a.toDouble(), min: 0, max: 255, divisions: 255,
                  label: '$a',
                  onChanged: (v) => setState(() { a = v.round(); syncHsv(); }),
                ),
              ),
              SizedBox(width: 34, child: Text('$a', textAlign: TextAlign.right, style: const TextStyle(fontSize: 11))),
            ]),
            const SizedBox(height: 6),
            Wrap(
              spacing: 8,
              children: [
                for (final entry in presets.entries)
                  InkWell(
                    onTap: () {
                      final v = entry.value;
                      r = v[0]; g = v[1]; b = v[2]; a = v[3];
                      controller.text = hex();
                      syncHsv();
                      setState(() {});
                    },
                    child: Container(
                        width: 28,
                        height: 28,
                        decoration: BoxDecoration(
                            color: Color.fromARGB(
                                entry.value[3], entry.value[0], entry.value[1], entry.value[2]),
                            border: Border.all(color: Colors.white24),
                            borderRadius: BorderRadius.circular(6))),
                  ),
              ],
            ),
          ]),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(
              onPressed: () => Navigator.pop(context, [r, g, b, a]),
              child: const Text('OK')),
        ],
      ),
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
Future<List<int>?> _editTransformMatrix(BuildContext context, List<int> current) {
  final t = Transform23()..fromMatrix(current);

  // Raw cell controllers (a, b, tx, c, d, ty).
  final raw = List.generate(6, (i) => TextEditingController(text: _num3(t.toCells()[i])));

  // Transform controllers + mirror flags.
  final tc = <String, TextEditingController>{};
  bool mirrorX = t.mirrorX, mirrorY = t.mirrorY;

  void syncTc() {
    // raw -> transform.
    final cells = raw.map((c) => double.tryParse(c.text)).toList();
    if (cells.contains(null)) return;
    t.fromCells(cells.cast<double>());
    mirrorX = t.mirrorX;
    mirrorY = t.mirrorY;
    tc['ox']!.text = _num3(t.offsetX);
    tc['oy']!.text = _num3(t.offsetY);
    tc['rot']!.text = _num3(t.rotation);
    tc['sx']!.text = _num3(t.scaleX);
    tc['sy']!.text = _num3(t.scaleY);
    tc['skew']!.text = _num3(t.skew);
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
      raw[i].text = _num3(cells[i]);
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
      raw[i].text = _num3(cells[i]);
    }
    syncTc();
  }

tc['ox'] = TextEditingController(text: _num3(t.offsetX));
  tc['oy'] = TextEditingController(text: _num3(t.offsetY));
  tc['rot'] = TextEditingController(text: _num3(t.rotation));
  tc['sx'] = TextEditingController(text: _num3(t.scaleX));
  tc['sy'] = TextEditingController(text: _num3(t.scaleY));
  tc['skew'] = TextEditingController(text: _num3(t.skew));

  List<int> build() {
    final cells = raw.map((c) => double.tryParse(c.text)).toList();
    if (!cells.contains(null)) {
      t.fromCells(cells.cast<double>());
    }
    return t.toMatrix();
  }

  Widget row(String label, String key) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 2),
        child: Row(children: [
          SizedBox(width: 100, child: Text(label, style: const TextStyle(fontSize: 12))),
          Expanded(
            child: TextField(
              controller: tc[key]!,
              keyboardType: const TextInputType.numberWithOptions(signed: true, decimal: true),
              onChanged: (_) => syncRaw(),
            ),
          ),
        ]),
      );

  const cellNames = ['a', 'b', 'tx', 'c', 'd', 'ty'];

  var rawMode = false;

  return showDialog<List<int>>(
    context: context,
    builder: (context) => StatefulBuilder(
      builder: (context, setState) => AlertDialog(
          title: const Text('Matrix (2x3)'),
          content: DialogBody(
            maxWidth: 320,
            child: Column(mainAxisSize: MainAxisSize.min, children: [
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
              const SizedBox(height: 10),
              if (!rawMode) ...[
                row('Offset X', 'ox'),
                row('Offset Y', 'oy'),
                row('Rotation °', 'rot'),
                row('Scale X', 'sx'),
                row('Scale Y', 'sy'),
                row('Skew °', 'skew'),
                Row(children: [
                  Checkbox(
                      value: mirrorX,
                      onChanged: (v) => setState(() {
                        mirrorX = v ?? false;
                        syncRaw();
                      })),
                  const Text('Mirror X', style: TextStyle(fontSize: 12)),
                  const SizedBox(width: 24),
                  Checkbox(
                      value: mirrorY,
                      onChanged: (v) => setState(() {
                        mirrorY = v ?? false;
                        syncRaw();
                      })),
                  const Text('Mirror Y', style: TextStyle(fontSize: 12)),
                ]),
              ] else ...[
                for (var r = 0; r < 2; r++)
                  Row(children: [
                    for (var c = 0; c < 3; c++)
                      Expanded(
                        child: Padding(
                          padding: const EdgeInsets.all(3),
                          child: TextField(
                            controller: raw[r * 3 + c],
                            keyboardType: const TextInputType.numberWithOptions(signed: true, decimal: true),
                            textAlign: TextAlign.center,
                            decoration: InputDecoration(labelText: cellNames[r * 3 + c]),
                            onChanged: (_) => syncFromRaw(),
                          ),
                        ),
                      ),
                  ]),
              ],
            ]),
          ),
          actions: [
            TextButton(onPressed: () => setState(reset), child: const Text('Reset')),
            TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
            FilledButton(onPressed: () => Navigator.pop(context, build()), child: const Text('OK')),
          ],
        ),
      ),
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
  return showDialog<List<int>>(
    context: context,
    builder: (context) => AlertDialog(
      title: Text(hex ? '$title (dec/hex)' : title),
      content: TextField(
        controller: controller,
        autofocus: true,
        keyboardType: TextInputType.number,
      ),
      actions: [
        TextButton(
            onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(
          onPressed: () {
            final text = controller.text.trim().toLowerCase();
            var value = text.startsWith('0x')
                ? int.tryParse(text.substring(2), radix: 16)
                : int.tryParse(text);
            if (value == null && hex) {
              value = int.tryParse(text.replaceAll(RegExp(r'[^0-9a-f]'), ''), radix: 16);
            }
            if (value == null) return;
            if (!signed && value < 0) return;
            Navigator.pop(context,
                uint32ToBytes(signed ? value & 0xFFFFFFFF : value));
          },
          child: const Text('OK'),
        ),
      ],
    ),
  );
}

Future<List<int>?> _editNetAddr(BuildContext context, List<int> current) {
  final currentId =
      current.length >= 2 ? current[0] | (current[1] << 8) : 0;
  final controller = TextEditingController(text: idToString(currentId));
  return showDialog<List<int>>(
    context: context,
    builder: (context) => AlertDialog(
      title: const Text('Net address (net:device)'),
      content: TextField(controller: controller, autofocus: true),
      actions: [
        TextButton(
            onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(
          onPressed: () {
            final parts = controller.text.split(':');
            final net = int.tryParse(parts[0], radix: 16);
            final dev = parts.length > 1
                ? int.tryParse(parts[1], radix: 16)
                : null;
            if (net == null || dev == null || net < 0 || dev < 0) return;
            final id = ((net & 0x3F) << 10) | (dev & 0x3FF);
            Navigator.pop(context, [id & 0xFF, (id >> 8) & 0xFF]);
          },
          child: const Text('OK'),
        ),
      ],
    ),
  );
}

Future<List<int>?> _editString(
    BuildContext context, String title, String current,
    {int maxChars = 23}) {
  final controller = TextEditingController(text: current.trimRight());
  return showDialog<List<int>>(
    context: context,
    builder: (context) => AlertDialog(
      title: Text(title),
      content: TextField(
        controller: controller,
        autofocus: true,
        maxLength: maxChars,
      ),
      actions: [
        TextButton(
            onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(
            onPressed: () => Navigator.pop(
                context, controller.text.codeUnits.toList()),
            child: const Text('OK')),
      ],
    ),
  );
}

Future<List<int>?> _editHex(
    BuildContext context, String title, List<int> current) {
  final controller = TextEditingController(
      text: current.map((int b) => b.toRadixString(16).padLeft(2, '0')).join());
  return showDialog<List<int>>(
    context: context,
    builder: (context) => AlertDialog(
      title: Text('$title (hex)'),
      content: TextField(controller: controller, autofocus: true),
      actions: [
        TextButton(
            onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
        FilledButton(
          onPressed: () {
            final clean = controller.text.replaceAll(RegExp(r'[^0-9a-fA-F]'), '');
            if (clean.isEmpty || clean.length.isOdd) return;
            Navigator.pop(context, [
              for (var i = 0; i < clean.length; i += 2)
                int.parse(clean.substring(i, i + 2), radix: 16)
            ]);
          },
          child: const Text('OK'),
        ),
      ],
    ),
  );
}

// ---------------------------------------------------------------------------
// Small formatting helpers
// ---------------------------------------------------------------------------

String _hex(List<int> bytes) =>
    bytes.map((int b) => b.toRadixString(16).padLeft(2, '0')).join(' ');

String _num3(double v) =>
    v == v.roundToDouble() ? v.toStringAsFixed(0) : v.toStringAsFixed(3);
