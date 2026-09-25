part of 'value_editor.dart';

/// Vector (N Numbers): one numeric field per component, with a size picker so the
/// length can be chosen/edited (Docs/Data Formats.md: size-flexible).
Future<List<int>?> _editVector(
    BuildContext context, FieldInfo? info, List<int> current) {
  var n = current.length >= 4 ? (current.length ~/ 4) : 3;
  if (n < 1) n = 1;
  var hasValue = current.length >= n * 4;
  final controllers = List.generate(
      n,
      (i) => TextEditingController(
          text: hasValue
              ? numberFromBytes(current, i * 4).toString()
              : '0.0'));
  final sizeCtrl = TextEditingController(text: '$n');
  final unit = info?.unit;

  void resize(int newN) {
    if (newN < 1) newN = 1;
    if (newN > 32) newN = 32;
    n = newN;
    while (controllers.length < n) {
      controllers.add(TextEditingController(text: '0.0'));
    }
    while (controllers.length > n) {
      controllers.removeLast();
    }
    sizeCtrl.text = '$n';
    hasValue = false;
  }

  return showDialog<List<int>>(
    context: context,
    builder: (context) => StatefulBuilder(
      builder: (context, setState) => AlertDialog(
        title: Text(unit == null
            ? (info?.name ?? 'Vector')
            : '${info!.name} [$unit]'),
        content: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Row(children: [
              const Text('Size: ', style: TextStyle(fontSize: 12)),
              IconButton(
                icon: const Icon(Icons.remove_circle_outline, size: 20),
                onPressed: () => setState(() => resize(n - 1)),
              ),
              SizedBox(
                width: 44,
                child: TextField(
                  controller: sizeCtrl,
                  keyboardType: TextInputType.number,
                  textAlign: TextAlign.center,
                  decoration: const InputDecoration(isDense: true),
                  onChanged: (_) =>
                      setState(() => resize(int.tryParse(sizeCtrl.text) ?? n)),
                ),
              ),
              IconButton(
                icon: const Icon(Icons.add_circle_outline, size: 20),
                onPressed: () => setState(() => resize(n + 1)),
              ),
            ]),
            for (var i = 0; i < n; i++)
              Padding(
                padding: const EdgeInsets.symmetric(vertical: 4),
                child: TextField(
                  controller: controllers[i],
                  keyboardType: TextInputType.number,
                  decoration: InputDecoration(labelText: i < 3 ? 'XYZ'[i] : '$i'),
                ),
              ),
          ],
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(
            onPressed: () {
              final values =
                  controllers.map((c) => double.tryParse(c.text)).toList();
              if (values.any((v) => v == null)) return;
              final out = BytesBuilder();
              for (final v in values) {
                out.add(numberToBytes(v!));
              }
              Navigator.pop(context, out.toBytes());
            },
            child: const Text('OK'),
          ),
        ],
      ),
    ),
  );
}

/// Matrix (Rows x Cols of Number): grid editor with the header (h, w) preserved, and
/// rows/cols size pickers.
Future<List<int>?> _editMatrix(
    BuildContext context, FieldInfo? info, List<int> current) {
  var h = current.length >= 2 ? current[0] | (current[1] << 8) : 0;
  var w = current.length >= 4 ? current[2] | (current[3] << 8) : 0;
  if (h == 0 || w == 0 || h * w * 4 + 4 > current.length || h > 6 || w > 6) {
    if (current.isEmpty) {
      // Brand-new entry: offer a fresh identity-like 3x3 of zeros.
      h = w = 3;
      current = [h & 0xFF, h >> 8, w & 0xFF, w >> 8,
                 for (var i = 0; i < h * w * 4; i++) 0];
    } else {
      return _editHex(context, 'Matrix ${h}x$w', current);
    }
  }
  if (h < 1) h = 1;
  if (w < 1) w = 1;
  final rowsCtrl = TextEditingController(text: '$h');
  final colsCtrl = TextEditingController(text: '$w');
  final controllers = List.generate(
      h * w,
      (i) => TextEditingController(
          text: numberFromBytes(current, 4 + i * 4).toString()));

  void resize(int newH, int newW) {
    if (newH < 1) newH = 1;
    if (newW < 1) newW = 1;
    if (newH > 6) newH = 6;
    if (newW > 6) newW = 6;
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

  return showDialog<List<int>>(
    context: context,
    builder: (context) => StatefulBuilder(
      builder: (context, setState) {
        Widget sizeRow(String label, TextEditingController ctrl,
            void Function(int delta) bump) {
          return Row(children: [
            Text('$label: ', style: const TextStyle(fontSize: 12)),
            IconButton(
              icon: const Icon(Icons.remove_circle_outline, size: 20),
              onPressed: () => setState(() => bump(-1)),
            ),
            SizedBox(
              width: 40,
              child: TextField(
                controller: ctrl,
                keyboardType: TextInputType.number,
                textAlign: TextAlign.center,
                decoration: const InputDecoration(isDense: true),
                onChanged: (_) => setState(() {
                  final v = int.tryParse(ctrl.text) ?? 0;
                  if (label == 'Rows') {
                    resize(v, w);
                  } else {
                    resize(h, v);
                  }
                }),
              ),
            ),
            IconButton(
              icon: const Icon(Icons.add_circle_outline, size: 20),
              onPressed: () => setState(() => bump(1)),
            ),
          ]);
        }

        return AlertDialog(
          title: Text(info?.name ?? 'Matrix ${h}x$w'),
          content: DialogBody(
            maxWidth: 300,
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                sizeRow('Rows', rowsCtrl, (d) => resize(h + d, w)),
                sizeRow('Cols', colsCtrl, (d) => resize(h, w + d)),
                const SizedBox(height: 6),
                for (var r = 0; r < h; r++)
                  Row(children: [
                    for (var c = 0; c < w; c++)
                      Expanded(
                        child: Padding(
                          padding: const EdgeInsets.all(3),
                          child: TextField(
                            controller: controllers[r * w + c],
                            keyboardType: TextInputType.number,
                            textAlign: TextAlign.center,
                          ),
                        ),
                      ),
                  ]),
              ],
            ),
          ),
          actions: [
            TextButton(
                onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
            FilledButton(
              onPressed: () {
                final values =
                    controllers.map((c) => double.tryParse(c.text)).toList();
                if (values.any((v) => v == null)) return;
                final out = BytesBuilder()
                  ..add([h & 0xFF, h >> 8, w & 0xFF, w >> 8]);
                for (final v in values) {
                  out.add(numberToBytes(v!));
                }
                Navigator.pop(context, out.toBytes());
              },
              child: const Text('OK'),
            ),
          ],
        );
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
  var inst = (bi >> 16) & 0x3F;
  var field = (bi >> 8) & 0xFF;
  var key = bi & 0xFF;
  final instC = TextEditingController(text: '$inst');
  final fieldC = TextEditingController(text: '$field');
  final keyC = TextEditingController(text: '$key');

  final types = <(int, String)>[
    (systemBlockTypeValue, 'System'),
    for (final t in BlockType.values)
      if (t != BlockType.none &&
          t != BlockType.undefined &&
          t != BlockType.deleted &&
          t != BlockType.render)
        (t.value, t.label),
  ];
  if (!types.any((e) => e.$1 == type)) types.add((type, blockTypeLabel(type)));

  return showDialog<List<int>>(
    context: context,
    builder: (context) => StatefulBuilder(
      builder: (context, setState) {
        final name = blockInfoFor(BlockType.fromValue(type))?.field(field)?.name;
        return AlertDialog(
          title: const Text('BlockInfo'),
          content: DialogBody(
            maxWidth: 360,
            child: Column(mainAxisSize: MainAxisSize.min, children: [
              DropdownButtonFormField<int>(
                initialValue: type,
                decoration: const InputDecoration(labelText: 'Block'),
                items: [
                  for (final (v, l) in types)
                    DropdownMenuItem(value: v, child: Text(l)),
                ],
                onChanged: (v) => setState(() {
                  if (v != null) type = v;
                }),
              ),
              const SizedBox(height: 8),
              Row(children: [
                Expanded(
                    child: TextField(
                        controller: instC,
                        keyboardType: TextInputType.number,
                        decoration: const InputDecoration(labelText: 'Instance'))),
                const SizedBox(width: 8),
                Expanded(
                    child: TextField(
                        controller: fieldC,
                        keyboardType: TextInputType.number,
                        decoration: const InputDecoration(labelText: 'Field'))),
                const SizedBox(width: 8),
                Expanded(
                    child: TextField(
                        controller: keyC,
                        keyboardType: TextInputType.number,
                        decoration: const InputDecoration(labelText: 'Key'))),
              ]),
              if (name != null)
                Padding(
                  padding: const EdgeInsets.only(top: 10),
                  child: Align(
                    alignment: Alignment.centerLeft,
                    child: Text('Field: $name',
                        style: const TextStyle(color: Colors.white54, fontSize: 12)),
                  ),
                ),
            ]),
          ),
          actions: [
            TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
            FilledButton(
              onPressed: () {
                final i = int.tryParse(instC.text) ?? 0;
                final f = int.tryParse(fieldC.text) ?? 0;
                final k = int.tryParse(keyC.text) ?? 0;
                Navigator.pop(context, uint32ToBytes(makeBlockInfo(type, i, f, k)));
              },
              child: const Text('OK'),
            ),
          ],
        );
      },
    ),
  );
}

