// Edit / dialog / prompt actions for the Register page (dynamic blocks, entries,
// values, flags).
//
// Part of register_page.dart: an extension on its State so the dialogs keep private
// access to the page's client and caches.

part of 'register_page.dart';

extension on _RegisterPageState {
  Future<void> _editBlock(int blockIndex, ({int type, int inst, ValueInfo meta, String name})? block) async {
    if (block == null || !mounted) return;
    if (!isDynamicType(block.type)) {
      _snack('Only dynamic blocks can be renamed');
      return;
    }
    final result = await promptBlockName(context,
        title: 'Edit block', initialName: block.name);
    if (result == null || !mounted) return;
    final (name, _) = result;

    final ok = await _client.writeDynamicBlockMeta(
        DynBlock(index: block.inst, meta: block.meta, name: block.name), name, null);
    _snack(ok ? 'Block updated' : 'Update failed');
    await _refreshAll();
  }

  Future<void> _createBlock() async {
    if (!mounted) return;
    final result = await promptBlockName(context,
        title: 'New dynamic block', withIndex: true);
    if (result == null || !mounted) return;
    final (name, index) = result;

    final created = await _client.createDynamicBlock(name, index: index);
    _snack(created != null ? 'Block created' : 'Create failed');
    await _refreshAll();
  }

  Future<void> _deleteBlock(int blockIndex, ({int type, int inst, ValueInfo meta, String name})? block) async {
    if (block == null || !mounted) return;

    final ok = await _client.deleteDynamic(block: block.inst);
    _snack(ok ? 'Block deleted (save to free)' : 'Delete failed');
    await _refreshAll();
  }

  Future<void> _addEntry(int blockIndex, ({int type, int inst, ValueInfo meta, String name})? block) async {
    if (block == null || !mounted) return;
    final fields = _dynamicFields[block.inst] ?? <int>[];
    var firstFree = 0;
    while (fields.contains(firstFree)) {
      firstFree++;
    }
    final indexController = TextEditingController(text: '$firstFree');
    var selectedType = DataType.number;
    final result = await showDialog<(int, DataType)>(
      context: context,
      builder: (context) => StatefulBuilder(
        builder: (context, setState) => AlertDialog(
          title: const Text('Add field'),
          content: Column(mainAxisSize: MainAxisSize.min, children: [
            TextField(
              controller: indexController,
              autofocus: true,
              keyboardType: TextInputType.number,
              decoration: const InputDecoration(
                  labelText: 'Field index', helperText: '0..255, must be unused'),
            ),
            const SizedBox(height: 8),
            DropdownButtonFormField<DataType>(
              initialValue: selectedType,
              decoration: const InputDecoration(labelText: 'Type'),
              items: [
                for (final t in DataType.values)
                  if (t != DataType.deleted && t != DataType.none && t != DataType.undefined)
                    DropdownMenuItem(value: t, child: Text(dataTypeLabel(t))),
              ],
              onChanged: (t) => _rebuild(() => selectedType = t ?? DataType.number),
            ),
          ]),
          actions: [
            TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
            FilledButton(
              onPressed: () {
                final idx = int.tryParse(indexController.text.trim());
                if (idx == null) return;
                Navigator.pop(context, (idx, selectedType));
              },
              child: const Text('Next'),
            ),
          ],
        ),
      ),
    );
    if (result == null || !mounted) return;
    final (field, dataType) = result;
    if (field < 0 || field > 255) return;
    if (fields.contains(field)) {
      _snack('Field $field already exists');
      return;
    }
    // A dictionary field is created as an empty key-0 marker (no value).
    if (dataType == DataType.geometry || dataType == DataType.texture) {
      final seed = <int>[];
      final dynBlock = DynBlock(index: block.inst, meta: block.meta, name: block.name);
      final meta = ValueInfo(type: dataType.value, key: 0, size: 0);
      final confirmed =
          await _client.writeDynamicEntry(dynBlock, field, 0, meta, seed);
      _snack(confirmed != null ? 'Field added' : 'Add failed');
      await _refreshAll();
      return;
    }
    final seed = await showValueEditor(context, dataType, []);
    if (seed == null || !mounted) return;
    final dynBlock = DynBlock(index: block.inst, meta: block.meta, name: block.name);
    final meta = ValueInfo(type: dataType.value, key: 0, size: seed.length);
    final confirmed = await _client.writeDynamicEntry(dynBlock, field, 0, meta, seed);
    _snack(confirmed != null ? 'Field added' : 'Add failed');
    await _refreshAll();
  }

  Future<void> _changeType(int blockType, int inst,
      ({int type, int inst, ValueInfo meta, String name})? block, int fieldIndex) async {
    if (block == null || !mounted) return;
    final dataType = await _pickDataType();
    if (dataType == null || !mounted) return;
    final seed = await showValueEditor(context, dataType, []);
    if (seed == null || !mounted) return;
    final cache = _fieldCache[(blockType << 8) | inst];
    final field = cache?[fieldIndex];
    if (field == null) return;
    
    final dynBlock = DynBlock(index: block.inst, meta: block.meta, name: block.name);
    final dynField = DynField(index: fieldIndex, meta: field.meta, value: field.value);
    final confirmed = await _client.writeDynamicField(dynBlock, dynField, seed, newType: dataType);
    _snack(confirmed != null ? 'Type changed' : 'Change failed');
    await _refreshAll();
  }

  Future<void> _deleteEntry(int blockType, int inst,
      ({int type, int inst, ValueInfo meta, String name})? block, int fieldIndex) async {
    if (block == null || !mounted) return;
    final cache = _fieldCache[(blockType << 8) | inst];
    final field = cache?[fieldIndex];
    if (field == null) return;
    
    final dynBlock = DynBlock(index: block.inst, meta: block.meta, name: block.name);
    final dynField = DynField(index: fieldIndex, meta: field.meta, value: field.value);
    final confirmed = await _client.writeDynamicField(dynBlock, dynField, [], newType: DataType.deleted);
    _snack(confirmed != null ? 'Entry deleted (save to free)' : 'Delete failed');
    await _refreshAll();
  }

  Future<DataType?> _pickDataType({DataType? selected}) {
    return showDialog<DataType>(
      context: context,
      builder: (context) => SimpleDialog(
        title: const Text('Entry data type'),
        children: [
          for (final t in DataType.values)
            if (t != DataType.deleted && t != DataType.none && t != DataType.undefined)
              SimpleDialogOption(
                onPressed: () => Navigator.pop(context, t),
                child: Row(children: [
                  if (t == selected)
                    const Icon(Icons.check, size: 16, color: Colors.white38),
                  if (t != selected) const SizedBox(width: 20),
                  Expanded(child: Text(dataTypeLabel(t))),
                ]),
              ),
        ],
      ),
    );
  }

  Future<void> _editValue(int blockType, int inst, ({int type, int inst, ValueInfo meta, String name})? block, int fieldIndex) async {
    if (block == null || !mounted) return;
    final cacheKey = (blockType << 8) | inst;
    final cache = _fieldCache[cacheKey];
    final field = cache?[fieldIndex];
    if (field == null || field.meta.readOnly) return;
    if (!mounted) return;

    final isSystemField = blockType == 0 && inst == 0;
    final blockInfo = blockInfoFor(BlockType.fromValue(block.meta.type));
    // The System Name field is 16 bytes; cap the editor accordingly.
    final fieldInfo = isSystemField
        ? (fieldIndex == 6 ? const FieldInfo('Name', maxChars: 16) : null)
        : blockInfo?.field(fieldIndex);

    final newValue = await showValueEditor(
        context, field.meta.dataType, field.value,
        info: fieldInfo);
    if (newValue == null) return;

    final key = (blockType == 0) ? systemKeysForField(fieldIndex).first : (isDynamicType(blockType) ? 0 : 0xFF);
    final meta = ValueInfo(type: field.meta.type, flags: field.meta.flags, size: newValue.length, key: key);
    final confirmed = await _client.writeBlockField(blockType, inst, fieldIndex, key, meta, newValue);
    _snack(confirmed != null ? 'Value written' : 'Write failed');
    if (confirmed != null) {
      final field = await _client.readBlockField(blockType, inst, fieldIndex, key);
      if (field != null) {
        final cache = _fieldCache[cacheKey];
        if (cache != null) cache[fieldIndex] = field;
      }
      if (mounted) _rebuild(() {});
    }
  }

  /// Edits one (field, key) entry of a dynamic block.
  Future<void> _editDynamicEntry(int blockType, int inst,
      ({int type, int inst, ValueInfo meta, String name})? block, int fieldIndex, int key) async {
    if (block == null || !mounted) return;
    final cache = _fieldCache[(blockType << 8) | inst];
    final entry = cache?[fieldIndex * 256 + key];
    if (entry == null || entry.meta.readOnly) return;
    final head = cache?[fieldIndex * 256 + 0];
    final dictType =
        (head != null && isRenderDictType(head.meta.type)) ? head.meta.type : entry.meta.type;
    final info = isRenderDictType(dictType) ? renderKeyFieldInfo(dictType, key) : null;
    final newValue =
        await showValueEditor(context, entry.meta.dataType, entry.value, info: info);
    if (newValue == null) return;
    final dynBlock = DynBlock(index: inst, meta: block.meta, name: block.name);
    final dynField = DynField(index: fieldIndex, meta: entry.meta, value: entry.value);
    final confirmed =
        await _client.writeDynamicField(dynBlock, dynField, newValue, key: key);
    _snack(confirmed != null ? 'Value written' : 'Write failed');
    await _refreshAll();
  }

  /// Deletes one (field, key) entry of a dynamic block.
  Future<void> _deleteDynamicEntry(int blockType, int inst,
      ({int type, int inst, ValueInfo meta, String name})? block, int fieldIndex, int key) async {
    if (block == null || !mounted) return;
    final ok = await _client.deleteDynamic(block: inst, field: fieldIndex, key: key);
    _snack(ok ? 'Entry deleted' : 'Delete failed');
    await _refreshAll();
  }

  /// Deletes an entire field (every (field, key) entry).
  Future<void> _deleteField(int blockType, int inst,
      ({int type, int inst, ValueInfo meta, String name})? block, int fieldIndex) async {
    if (block == null || !mounted) return;
    final ok = await _client.deleteDynamic(block: inst, field: fieldIndex);
    _snack(ok ? 'Field deleted' : 'Delete failed');
    await _refreshAll();
  }

  /// Re-numbers a field to a chosen index (dialogue; pre-filled with the current).
  Future<void> _changeFieldIndex(int blockType, int inst,
      ({int type, int inst, ValueInfo meta, String name})? block, int fieldIndex) async {
    if (block == null || !mounted) return;
    final controller = TextEditingController(text: '$fieldIndex');
    final newField = await showDialog<int>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Change field index'),
        content: TextField(
          controller: controller,
          autofocus: true,
          keyboardType: TextInputType.number,
          decoration: const InputDecoration(helperText: '0..255, must be unused'),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
          FilledButton(
            onPressed: () {
              final v = int.tryParse(controller.text.trim());
              Navigator.pop(context, v);
            },
            child: const Text('OK'),
          ),
        ],
      ),
    );
    if (newField == null || newField < 0 || newField > 255 || newField == fieldIndex) return;
    final dynBlock = DynBlock(index: inst, meta: block.meta, name: block.name);
    final ok = await _client.setDynamicFieldIndex(dynBlock, fieldIndex, newField);
    _snack(ok ? 'Field moved' : 'Move failed (index already used?)');
    await _refreshAll();
  }

  /// Edits the Read-only / Persistent flags of one (field, key) entry.
  Future<void> _editEntryFlags(int blockType, int inst,
      ({int type, int inst, ValueInfo meta, String name})? block, int fieldIndex, int key) async {
    if (block == null || !mounted) return;
    final cache = _fieldCache[(blockType << 8) | inst];
    final entry = cache?[fieldIndex * 256 + key];
    if (entry == null) return;
    var ro = entry.meta.readOnly;
    var per = entry.meta.persistent;
    final result = await showDialog<List<bool>>(
      context: context,
      builder: (context) => StatefulBuilder(
        builder: (context, setState) => AlertDialog(
          title: Text('Entry flags (field $fieldIndex)'),
          content: Column(mainAxisSize: MainAxisSize.min, children: [
            SwitchListTile(
              title: const Text('Read-only'),
              subtitle: const Text('Blocks value edits from the app'),
              value: ro,
              onChanged: (v) => _rebuild(() => ro = v),
            ),
            SwitchListTile(
              title: const Text('Persistent'),
              subtitle: const Text('Saved to the block DV file on Save; survives reboot'),
              value: per,
              onChanged: (v) => _rebuild(() => per = v),
            ),
          ]),
          actions: [
            TextButton(
                onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
            FilledButton(
                onPressed: () => Navigator.pop(context, [ro, per]),
                child: const Text('OK')),
          ],
        ),
      ),
    );
    if (result == null || !mounted) return;
    final dynBlock = DynBlock(index: inst, meta: block.meta, name: block.name);
    final ok = await _client.setDynamicEntryFlags(
        dynBlock, fieldIndex, key, readOnly: result[0], persistent: result[1]);
    _snack(ok != null ? 'Flags updated' : 'Update failed');
    await _refreshAll();
  }

  /// RO/P flag suffix for a dynamic entry's subtitle.
  String _flagsSuffix(ValueInfo meta) {
    final f = <String>[];
    if (meta.readOnly) f.add('RO');
    if (meta.persistent) f.add('P');
    return f.isEmpty ? '' : ' · ${f.join(' · ')}';
  }

  /// Prompts for a key number to add/change in a field. For dictionaries the meaningful
  /// keys of the current shape/effect are offered as a selector, but manual numeric input
  /// (0..255) is always allowed - including keys beyond the dictionary specification.
  Future<int?> _promptKey({
    required String title,
    required int startKey,
    required bool isDict,
    required int dictType,
    required int selector,
  }) async {
    final controller = TextEditingController(text: '$startKey');
    List<int> meaningful;
    if (isDict) {
      final ks = dictType == geometryDictType
          ? geometryKeysForShape(selector)
          : textureKeysForType(selector);
      meaningful = ks.toList()..sort();
    } else {
      meaningful = <int>[];
    }
    int? chosenDropdown;
    return showDialog<int>(
      context: context,
      builder: (context) => StatefulBuilder(
        builder: (context, setState) => AlertDialog(
          title: Text(title),
          content: Column(mainAxisSize: MainAxisSize.min, children: [
            if (meaningful.isNotEmpty) ...[
              DropdownButtonFormField<int?>(
                initialValue: chosenDropdown,
                decoration: const InputDecoration(labelText: 'Dictionary keys'),
                hint: Text(controller.text == '$startKey'
                    ? 'Pick (or type below)'
                    : 'Type below'),
                items: [
                  for (final k in meaningful)
                    DropdownMenuItem(
                      value: k,
                      child: Text('$k · ${renderDictKeyName(dictType, k)}'),
                    ),
                ],
                onChanged: (v) => _rebuild(() {
                  if (v != null) {
                    chosenDropdown = v;
                    controller.text = '$v';
                  }
                }),
              ),
              const SizedBox(height: 8),
            ],
            TextField(
              controller: controller,
              autofocus: true,
              keyboardType: TextInputType.number,
              decoration: const InputDecoration(
                  labelText: 'Key number',
                  helperText: '0..255 - any value allowed'),
            ),
          ]),
          actions: [
            TextButton(
                onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
            FilledButton(
              onPressed: () {
                final v = int.tryParse(controller.text.trim());
                if (v == null || v < 0 || v > 255) return;
                Navigator.pop(context, v);
              },
              child: const Text('OK'),
            ),
          ],
        ),
      ),
    );
  }

  /// Adds a key to a dynamic block's field: asks which key (dict selector + manual
  /// numeric, always), then the type and a value.
  Future<void> _addDynamicEntry(int blockType, int inst,
      ({int type, int inst, ValueInfo meta, String name})? block, int fieldIndex) async {
    if (block == null || !mounted) return;
    final keys = _dynamicKeys[inst]?[fieldIndex] ?? <int>[];
    final cache = _fieldCache[(blockType << 8) | inst];
    final head = cache?[fieldIndex * 256 + 0];
    final dictType =
        (head != null && isRenderDictType(head.meta.type)) ? head.meta.type : 0;
    final isDict = isRenderDictType(dictType);
    final selector = cache?[fieldIndex * 256 + 1]?.value.first ?? 0;

    var startKey = 0;
    while (keys.contains(startKey)) {
      startKey++;
    }

    final newKey = await _promptKey(
      title: 'Add key to field $fieldIndex',
      startKey: startKey,
      isDict: isDict,
      dictType: dictType,
      selector: selector,
    );
    if (newKey == null || !mounted) return;
    if (keys.contains(newKey)) {
      _snack('Key $newKey already exists in this field');
      return;
    }

    final dataType = await _pickDataType(selected: DataType.number);
    if (dataType == null || !mounted) return;
    final seed = await showValueEditor(context, dataType, []);
    if (seed == null || !mounted) return;
    final dynBlock = DynBlock(index: inst, meta: block.meta, name: block.name);
    final meta = ValueInfo(type: dataType.value, key: newKey, size: seed.length);
    final confirmed =
        await _client.writeDynamicEntry(dynBlock, fieldIndex, newKey, meta, seed);
    _snack(confirmed != null ? 'Entry added' : 'Add failed');
    await _refreshAll();
  }

  /// Changes one entry's data type (writes a fresh value of the new type).
  Future<void> _changeDynamicType(int blockType, int inst,
      ({int type, int inst, ValueInfo meta, String name})? block, int fieldIndex, int key) async {
    if (block == null || !mounted) return;
    final cache = _fieldCache[(blockType << 8) | inst];
    final entry = cache?[fieldIndex * 256 + key];
    if (entry == null) return;
    final dataType = await _pickDataType(selected: entry.meta.dataType);
    if (dataType == null || !mounted) return;
    final seed = await showValueEditor(context, dataType, []);
    if (seed == null || !mounted) return;
    final dynBlock = DynBlock(index: inst, meta: block.meta, name: block.name);
    final dynField = DynField(index: fieldIndex, meta: entry.meta, value: entry.value);
    final confirmed = await _client.writeDynamicField(
        dynBlock, dynField, seed, newType: dataType, key: key);
    _snack(confirmed != null ? 'Type changed' : 'Change failed');
    await _refreshAll();
  }

  /// Re-keys one entry (moves it to a different key byte within the field). Uses the
  /// dict-aware key selector + always allows manual numeric input.
  Future<void> _changeDynamicKey(int blockType, int inst,
      ({int type, int inst, ValueInfo meta, String name})? block, int fieldIndex, int key) async {
    if (block == null || !mounted) return;
    final cache = _fieldCache[(blockType << 8) | inst];
    final entry = cache?[fieldIndex * 256 + key];
    if (entry == null) return;
    final keys = _dynamicKeys[inst]?[fieldIndex] ?? <int>[];
    final head = cache?[fieldIndex * 256 + 0];
    final dictType =
        (head != null && isRenderDictType(head.meta.type)) ? head.meta.type : 0;
    final isDict = isRenderDictType(dictType);
    final selector = cache?[fieldIndex * 256 + 1]?.value.first ?? 0;

    final newKey = await _promptKey(
      title: 'Change key (was $key)',
      startKey: key,
      isDict: isDict,
      dictType: dictType,
      selector: selector,
    );
    if (newKey == null || newKey == key) return;
    if (newKey < 0 || newKey > 255) return;
    if (keys.contains(newKey)) {
      _snack('Key $newKey already exists in this field');
      return;
    }
    final dynBlock = DynBlock(index: inst, meta: block.meta, name: block.name);
    final written = await _client.writeDynamicEntry(
        dynBlock, fieldIndex, newKey, entry.meta, entry.value);
    if (written != null) {
      await _client.deleteDynamic(block: inst, field: fieldIndex, key: key);
    }
    _snack(written != null ? 'Key changed' : 'Change failed');
    await _refreshAll();
  }

  /// Formats a dynamic (field, key) entry with enum labels where known.
  String _formatDynamicValue(
      ({ValueInfo meta, List<int> value}) e, bool isDict, int dictType, int key) {
    if (e.meta.dataType == DataType.enum_ && e.value.isNotEmpty) {
      final enums = isDict ? renderKeyFieldInfo(dictType, key).enumValues : null;
      final label = enums?[e.value[0]];
      if (label != null) return label;
    }
    return formatValue(e.meta.dataType, e.value);
  }

}
