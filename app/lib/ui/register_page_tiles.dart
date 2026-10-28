// Field/block tiles for the Register page.
//
// Part of register_page.dart: an extension on its State so the tiles keep private
// access to the page's caches while staying out of the main file.
//
// Presentation comes from `theme.dart` (tokens), `widgets.dart` and `viewer_kit.dart`
// (shared widgets) only: no colour, text size, gap or radius of its own, and every
// display string comes through `formatters.dart`.

part of 'register_page.dart';

extension on _RegisterPageState {
  /// Renders one category field of a loaded script (Header/Input/Output/Variable/
  /// Constant) as a keyed list. Only `ScriptField.input` is editable; the header, outputs,
  /// variables and constants are read-only (variables are edited through the script file,
  /// not the Register view).
  Widget _scriptFieldTile(
    BuildContext context,
    int blockType,
    int inst,
    int cacheKey,
    int fieldIndex,
    ({int type, int inst, ValueInfo meta, String name})? block,
    Key? cardKey,
  ) {
    // The Header category is script metadata and is intentionally not part of the
    // Register view.
    if (fieldIndex == ScriptField.header) return const SizedBox.shrink();
    final theme = Theme.of(context);
    final cache = _fieldCache[cacheKey];
    final keys = _scriptKeys[cacheKey]?[fieldIndex] ?? <int>[];
    final name = scriptFieldLabel(fieldIndex);
    if (keys.isEmpty) {
      return ListTile(
        key: cardKey,
        dense: true,
        title: Text(
          name,
          style: theme.textTheme.labelSmall?.copyWith(color: kTextMuted),
        ),
        subtitle: Text(
          'None',
          style: theme.textTheme.labelSmall?.copyWith(color: kTextDisabled),
        ),
      );
    }
    final editableField = fieldIndex == ScriptField.input;
    return Column(
      children: [
        for (final key in keys)
          _scriptEntryTile(
            context,
            blockType,
            inst,
            fieldIndex,
            key,
            cache?[fieldIndex * 256 + key],
            name,
            editableField,
            block,
            key == keys.first ? cardKey : null,
          ),
      ],
    );
  }

  /// One loaded script entry: the field's keyed value, editable when the category is
  /// Input and the entry is not read-only.
  Widget _scriptEntryTile(
    BuildContext context,
    int blockType,
    int inst,
    int fieldIndex,
    int key,
    ({ValueInfo meta, List<int> value})? entry,
    String name,
    bool editableField,
    ({int type, int inst, ValueInfo meta, String name})? block,
    Key? cardKey,
  ) {
    final theme = Theme.of(context);
    if (entry == null) {
      return ListTile(
        key: cardKey,
        dense: true,
        leading: const SizedBox(
          width: IconSize.sm,
          height: IconSize.sm,
          child: CircularProgressIndicator(),
        ),
        title: Text(
          '$name $key',
          style: theme.textTheme.labelSmall?.copyWith(color: kTextDisabled),
        ),
      );
    }
    final editable = editableField && !entry.meta.readOnly;
    return ListTile(
      key: cardKey,
      dense: true,
      title: Row(
        children: [
          SizedBox(
            width: _labelColumnWidth,
            child: Text(
              '$name $key',
              style: theme.textTheme.labelSmall?.copyWith(color: kTextMuted),
            ),
          ),
          Expanded(
            child: MonoText(formatValue(entry.meta.dataType, entry.value)),
          ),
        ],
      ),
      subtitle: Text(
        dataTypeLabel(entry.meta.dataType) +
            (entry.meta.readOnly ? ' · RO' : ''),
        style: theme.textTheme.labelSmall,
      ),
      trailing: editable
          ? const Icon(Icons.edit, size: IconSize.sm, color: kTextDisabled)
          : null,
      onTap: editable
          ? () =>
                _editScriptEntry(blockType, inst, fieldIndex, key, entry, block)
          : null,
    );
  }

  Widget _blockCard(
    BuildContext context,
    int blockIndex,
    ({int type, int inst, ValueInfo meta, String name})? block, {
    Key? cardKey,
    int? dragIndex,
  }) {
    final theme = Theme.of(context);
    if (block == null) {
      return Card(
        child: ListTile(
          leading: const Icon(Icons.memory, color: kOrange),
          title: Text('System Block', style: theme.textTheme.titleSmall),
          subtitle: const Text('Loading...'),
        ),
      );
    }

    final isExpanded = _expanded.contains(blockIndex);
    final isSystem = block.type == 0 && block.inst == 0;
    final isDynamic = isDynamicType(block.type);
    if (isHiddenRegisterSlot(block.type, block.meta)) {
      // Dynamic tombstone slots are hidden (they carry no block). The System block reports
      // meta type 0x00 too, so it is excluded by the slot type (see isHiddenRegisterSlot).
      return const SizedBox.shrink();
    }

    final folderIcon = Icon(
      isExpanded ? Icons.folder_open : Icons.folder,
      color: kOrange,
    );
    final dragHandle = dragIndex == null
        ? null
        : ReorderableDragStartListener(
            index: dragIndex,
            child: const Icon(Icons.drag_handle, color: kTextDisabled),
          );

    return Card(
      key: cardKey,
      clipBehavior: Clip.antiAlias,
      child: Column(
        children: [
          ListTile(
            leading: dragHandle == null
                ? folderIcon
                : Row(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      dragHandle,
                      const SizedBox(width: Spacing.md),
                      folderIcon,
                    ],
                  ),
            title: Row(
              children: [
                Expanded(
                  child: Text(
                    block.name.isNotEmpty
                        ? block.name
                        : (isSystem
                              ? 'System'
                              : (isDynamic
                                    ? 'Dynamic #${block.inst}'
                                    : 'Block #${block.inst}')),
                    style: theme.textTheme.titleSmall,
                  ),
                ),
                TamuChip(
                  isSystem ? 'System' : '#${block.inst}',
                  tone: ChipTone.subtle,
                ),
              ],
            ),
            subtitle: TamuChipRow(
              chips: [
                TamuChip(
                  isSystem ? 'System' : blockTypeLabel(block.meta.type),
                  tone: ChipTone.accent,
                ),
                for (final flag in ValueFlags.describe(block.meta.flags))
                  TamuChip(
                    flag,
                    tone: flag == 'RO' ? ChipTone.accent : ChipTone.subtle,
                  ),
                if (!isScriptType(block.meta.type))
                  TamuChip('${block.meta.size} fields', tone: ChipTone.subtle),
              ],
            ),
            trailing: isDynamicType(block.meta.type)
                ? PopupMenuButton<String>(
                    tooltip: 'Block actions',
                    onSelected: (action) {
                      if (action == 'edit') {
                        _editBlock(blockIndex, block);
                      } else if (action == 'add') {
                        _addEntry(blockIndex, block);
                      } else if (action == 'move') {
                        _moveBlock(block);
                      } else if (action == 'delete') {
                        _deleteBlock(blockIndex, block);
                      }
                    },
                    itemBuilder: (_) => [
                      const PopupMenuItem(value: 'edit', child: Text('Rename')),
                      const PopupMenuItem(
                        value: 'add',
                        child: Text('Add field'),
                      ),
                      if (_editMode)
                        const PopupMenuItem(
                          value: 'move',
                          child: Text('Move to index...'),
                        ),
                      const PopupMenuItem(
                        value: 'delete',
                        child: Text('Delete block'),
                      ),
                    ],
                  )
                : null,
            onTap: () async {
              _rebuild(() {
                isExpanded
                    ? _expanded.remove(blockIndex)
                    : _expanded.add(blockIndex);
              });
              if (_expanded.contains(blockIndex)) {
                final block = _blockMetas?[blockIndex];
                if (block != null) {
                  await _loadBlockFields(
                    block.type,
                    block.inst,
                    block,
                    forceRefresh: true,
                  );
                }
                if (mounted) _rebuild(() {});
              }
            },
          ),
          if (isExpanded)
            Material(
              color: kSurface,
              child: Column(
                children: [
                  const Divider(),
                  if (block.meta.size > 0) ...[
                    if (_editMode && isDynamic)
                      ReorderableListView(
                        shrinkWrap: true,
                        buildDefaultDragHandles: false,
                        children: [
                          for (final (si, f)
                              in (_dynamicFields[block.inst] ?? <int>[])
                                  .indexed)
                            _fieldTile(
                              context,
                              block.type,
                              block.inst,
                              block,
                              f,
                              cardKey: ValueKey('fld-$f'),
                              dragIndex: si,
                            ),
                        ],
                        onReorderItem: (oldIndex, newIndex) => _reorderFields(
                          block,
                          _dynamicFields[block.inst] ?? <int>[],
                          oldIndex,
                          newIndex,
                        ),
                      )
                    else if (isDynamic)
                      for (final f in (_dynamicFields[block.inst] ?? <int>[]))
                        _fieldTile(context, block.type, block.inst, block, f)
                    else
                      for (var f = 0; f < block.meta.size; f++)
                        _fieldTile(context, block.type, block.inst, block, f),
                  ],
                ],
              ),
            ),
        ],
      ),
    );
  }

  Widget _fieldTile(
    BuildContext context,
    int blockType,
    int inst,
    ({int type, int inst, ValueInfo meta, String name})? block,
    int fieldIndex, {
    Key? cardKey,
    int? dragIndex,
  }) {
    final theme = Theme.of(context);
    final cacheKey = (blockType << 8) | inst;
    final cache = _fieldCache[cacheKey];
    final field = cache?[fieldIndex];
    final isSystem = blockType == 0 && inst == 0;

    // Loaded scripts (0x3F4-0x3F7): keyed entries per category field.
    if (isScriptType(blockType)) {
      return _scriptFieldTile(
        context,
        blockType,
        inst,
        cacheKey,
        fieldIndex,
        block,
        cardKey,
      );
    }

    // Dynamic blocks: flat (field, key) entries. A field is a DICTIONARY when its
    // key-0 entry is a Geometry/Texture marker; otherwise key 0 is the field's plain
    // value. Key 0 is never shown as a row - it's the value/marker itself.
    if (isDynamicType(blockType)) {
      final keys = _dynamicKeys[inst]?[fieldIndex] ?? <int>[0];
      final entries = <(int, ({ValueInfo meta, List<int> value})?)>[
        for (final k in keys) (k, cache?[fieldIndex * 256 + k]),
      ];
      if (entries.every((e) => e.$2 == null)) {
        return ListTile(
          key: cardKey,
          dense: true,
          leading: const SizedBox(
            width: IconSize.sm,
            height: IconSize.sm,
            child: CircularProgressIndicator(),
          ),
          title: Text(
            '...',
            style: theme.textTheme.labelSmall?.copyWith(color: kTextDisabled),
          ),
        );
      }
      final head = entries.firstOrNull?.$2;
      final isDict = head != null && isRenderDictType(head.meta.type);
      final dictType = head?.meta.type ?? 0;
      final extraKeys = <({int key, ({ValueInfo meta, List<int> value}) e})>[];
      for (final (k, e) in entries) {
        if (k > 0 && e != null) extraKeys.add((key: k, e: e));
      }

      // Plain field (key 0 only, not a dictionary): a single editable value tile.
      if (!isDict && extraKeys.isEmpty && head != null) {
        return ListTile(
          key: cardKey,
          dense: true,
          leading: dragIndex == null
              ? null
              : ReorderableDragStartListener(
                  index: dragIndex,
                  child: const Icon(Icons.drag_handle, color: kTextDisabled),
                ),
          title: Row(
            children: [
              SizedBox(
                width: _labelColumnWidth,
                child: Text(
                  'Field $fieldIndex',
                  style: theme.textTheme.labelSmall?.copyWith(
                    color: kTextMuted,
                  ),
                ),
              ),
              if (head.meta.dataType == DataType.colour &&
                  head.value.length >= 4)
                ColourSwatch(head.value),
              Expanded(child: MonoText(formatDynamicValue(head, false, 0, 0))),
            ],
          ),
          subtitle: Text(
            dataTypeLabel(head.meta.dataType) + flagsSuffix(head.meta),
            style: theme.textTheme.labelSmall,
          ),
          trailing: _editMode
              ? PopupMenuButton<String>(
                  icon: const Icon(Icons.more_vert, size: IconSize.sm),
                  onSelected: (action) {
                    if (action == 'edit' && !head.meta.readOnly) {
                      _editDynamicEntry(blockType, inst, block, fieldIndex, 0);
                    } else if (action == 'type' && !head.meta.readOnly) {
                      _changeDynamicType(blockType, inst, block, fieldIndex, 0);
                    } else if (action == 'fidx' && !head.meta.readOnly) {
                      _changeFieldIndex(blockType, inst, block, fieldIndex);
                    } else if (action == 'flags') {
                      _editEntryFlags(blockType, inst, block, fieldIndex, 0);
                    } else if (action == 'delete') {
                      _deleteField(blockType, inst, block, fieldIndex);
                    } else if (action == 'addkey' && !head.meta.readOnly) {
                      _addDynamicEntry(blockType, inst, block, fieldIndex);
                    }
                  },
                  itemBuilder: (_) => [
                    if (!head.meta.readOnly) ...[
                      const PopupMenuItem(
                        value: 'edit',
                        child: Text('Edit value'),
                      ),
                      const PopupMenuItem(
                        value: 'type',
                        child: Text('Change type'),
                      ),
                      const PopupMenuItem(
                        value: 'fidx',
                        child: Text('Change field index'),
                      ),
                      const PopupMenuItem(
                        value: 'addkey',
                        child: Text('Add key'),
                      ),
                    ],
                    const PopupMenuItem(
                      value: 'flags',
                      child: Text('Edit flags'),
                    ),
                    const PopupMenuItem(
                      value: 'delete',
                      child: Text('Delete field'),
                    ),
                  ],
                )
              : null,
          onTap: !head.meta.readOnly
              ? () => _editDynamicEntry(blockType, inst, block, fieldIndex, 0)
              : null,
        );
      }

      // Dictionary (or a field with extra keys): expand the key>0 rows; key 0 (the
      // marker/value) is shown compactly in the header but never as a row.
      return ExpansionTile(
        key: cardKey,
        dense: true,
        leading: dragIndex == null
            ? null
            : ReorderableDragStartListener(
                index: dragIndex,
                child: const Icon(Icons.drag_handle, color: kTextDisabled),
              ),
        title: Row(
          children: [
            SizedBox(
              width: _labelColumnWidth,
              child: Text(
                isDict
                    ? (dictType == geometryDictType ? 'Geometry' : 'Texture')
                    : 'Field $fieldIndex',
                style: theme.textTheme.labelSmall?.copyWith(color: kTextMuted),
              ),
            ),
            Expanded(child: MonoText('[${extraKeys.length} keys]')),
            if (_editMode) ...[
              IconButton(
                tooltip: 'Add key to field $fieldIndex',
                icon: const Icon(Icons.add, size: IconSize.md),
                onPressed: () =>
                    _addDynamicEntry(blockType, inst, block, fieldIndex),
              ),
              PopupMenuButton<String>(
                tooltip: 'Field actions',
                icon: const Icon(Icons.more_vert, size: IconSize.sm),
                onSelected: (action) {
                  if (action == 'type' && head != null && !head.meta.readOnly) {
                    _changeDynamicType(blockType, inst, block, fieldIndex, 0);
                  } else if (action == 'fidx') {
                    _changeFieldIndex(blockType, inst, block, fieldIndex);
                  } else if (action == 'delete') {
                    _deleteField(blockType, inst, block, fieldIndex);
                  }
                },
                itemBuilder: (_) => [
                  if (head != null && !head.meta.readOnly)
                    const PopupMenuItem(
                      value: 'type',
                      child: Text('Change type'),
                    ),
                  const PopupMenuItem(
                    value: 'fidx',
                    child: Text('Change field index'),
                  ),
                  const PopupMenuItem(
                    value: 'delete',
                    child: Text('Delete field'),
                  ),
                ],
              ),
            ],
          ],
        ),
        children: <Widget>[
          for (final ek in extraKeys)
            ListTile(
              dense: true,
              contentPadding: Insets.memberRow,
              title: Row(
                children: [
                  SizedBox(
                    width: _keyLabelWidth,
                    child: Text(
                      isDict
                          ? '${ek.key} · ${renderDictKeyName(dictType, ek.key)}'
                          : 'key ${ek.key}',
                      style: theme.textTheme.labelSmall?.copyWith(
                        color: kTextMuted,
                      ),
                    ),
                  ),
                  if (ek.e.meta.dataType == DataType.colour &&
                      ek.e.value.length >= 4)
                    ColourSwatch(ek.e.value),
                  Expanded(
                    child: MonoText(
                      formatDynamicValue(ek.e, isDict, dictType, ek.key),
                    ),
                  ),
                ],
              ),
              subtitle: Text(
                dataTypeLabel(ek.e.meta.dataType) + flagsSuffix(ek.e.meta),
                style: theme.textTheme.labelSmall,
              ),
              trailing: PopupMenuButton<String>(
                icon: const Icon(Icons.more_vert, size: IconSize.sm),
                onSelected: (action) {
                  if (action == 'edit' && !ek.e.meta.readOnly) {
                    _editDynamicEntry(
                      blockType,
                      inst,
                      block,
                      fieldIndex,
                      ek.key,
                    );
                  } else if (action == 'type' && !ek.e.meta.readOnly) {
                    _changeDynamicType(
                      blockType,
                      inst,
                      block,
                      fieldIndex,
                      ek.key,
                    );
                  } else if (action == 'key' && !ek.e.meta.readOnly) {
                    _changeDynamicKey(
                      blockType,
                      inst,
                      block,
                      fieldIndex,
                      ek.key,
                    );
                  } else if (action == 'flags') {
                    _editEntryFlags(blockType, inst, block, fieldIndex, ek.key);
                  } else if (action == 'delete' && !ek.e.meta.readOnly) {
                    _deleteDynamicEntry(
                      blockType,
                      inst,
                      block,
                      fieldIndex,
                      ek.key,
                    );
                  }
                },
                itemBuilder: (_) => [
                  if (!ek.e.meta.readOnly) ...[
                    const PopupMenuItem(
                      value: 'edit',
                      child: Text('Edit value'),
                    ),
                    if (_editMode) ...[
                      const PopupMenuItem(
                        value: 'type',
                        child: Text('Change type'),
                      ),
                      const PopupMenuItem(
                        value: 'key',
                        child: Text('Change key'),
                      ),
                    ],
                  ],
                  if (_editMode)
                    const PopupMenuItem(
                      value: 'flags',
                      child: Text('Edit flags'),
                    ),
                  if (!ek.e.meta.readOnly)
                    const PopupMenuItem(
                      value: 'delete',
                      child: Text('Delete entry'),
                    ),
                ],
              ),
              onTap: !ek.e.meta.readOnly
                  ? () => _editDynamicEntry(
                      blockType,
                      inst,
                      block,
                      fieldIndex,
                      ek.key,
                    )
                  : null,
            ),
        ],
      );
    }

    if (field == null) {
      return ListTile(
        dense: true,
        leading: const SizedBox(
          width: IconSize.sm,
          height: IconSize.sm,
          child: CircularProgressIndicator(),
        ),
        title: Text(
          '...',
          style: theme.textTheme.labelSmall?.copyWith(color: kTextDisabled),
        ),
      );
    }

    final primaryFlags = ValueFlags.describe(field.meta.flags);
    final isSystemField = isSystem;

    // Field display: resolves the registry's per-type field metadata for enum labels and
    // the DAS ResistiveMeasure custom view (Sensor Type at field 1, Measured Value at
    // field 3 with a sensor-derived unit + fuzzy lux level for the LDR).
    String displayValue() {
      if (isSystemField) return formatValue(field.meta.dataType, field.value);
      final blockInfo = blockInfoFor(BlockType.fromValue(blockType));
      final fieldInfo = blockInfo?.field(fieldIndex);

      // A secret field (the WiFi password) has no display path: the row prints the mask, and
      // an empty value means the device does not read it back rather than "the password is
      // empty".
      if (fieldInfo?.secret == true) {
        return field.value.isEmpty ? '-' : kSecretMask;
      }

      if (blockType == BlockType.resistiveMeasure.value) {
        final sensorRaw = cache?[1];
        final sensor = (sensorRaw != null && sensorRaw.value.isNotEmpty)
            ? sensorRaw.value[0]
            : -1;
        if (fieldIndex == 1) {
          return sensor >= 0
              ? sensorTypeLabel(sensor)
              : formatValue(field.meta.dataType, field.value);
        }
        // Measured Value is field 3 (0 Sampling Rate, 1 Sensor Type, 2 Filter
        // Coefficient, 3 Measured Value, 4 Current Range).
        if (fieldIndex == 3 && sensor >= 0 && field.value.length >= 4) {
          final numVal = numberFromBytes(field.value);
          final unit = sensorUnits[sensor] ?? '';
          var text = formatValue(DataType.number, field.value);
          if (unit.isNotEmpty) text += ' $unit';
          if (sensor == 3) text += ' · ${luxLevel(numVal)}';
          return text;
        }
      }

      // Enum fields with known option labels (button edges, Acc&Gyr ODR/ranges, sensor
      // type): show the label instead of the raw index.
      final enumLabels = fieldInfo?.enumValues;
      if (enumLabels != null && field.value.isNotEmpty) {
        final raw = field.value[0];
        return enumLabels[raw] ?? 'Enum $raw';
      }
      return formatValue(field.meta.dataType, field.value);
    }

    // For system block, collect all keys for this field
    List<({int key, ({ValueInfo meta, List<int> value})? field})> keyedEntries =
        [];
    if (isSystemField && cache != null) {
      final keys = systemKeysForField(fieldIndex);
      for (final key in keys) {
        // Use offset 256 to match _loadBlockFields storage
        final extraField = cache[256 + fieldIndex * 256 + key];
        if (extraField != null) {
          keyedEntries.add((key: key, field: extraField));
        }
      }
      if (!keyedEntries.any(
        (e) => e.key == systemKeysForField(fieldIndex).first,
      )) {
        keyedEntries.insert(0, (
          key: systemKeysForField(fieldIndex).first,
          field: field,
        ));
      }
    }

    if (isSystemField && keyedEntries.length > 1) {
      return ExpansionTile(
        dense: true,
        title: Row(
          children: [
            SizedBox(
              width: _labelColumnWidth,
              child: Text(
                systemFieldName(fieldIndex),
                style: theme.textTheme.labelSmall?.copyWith(color: kTextMuted),
              ),
            ),
            Expanded(child: MonoText('[${keyedEntries.length} fields]')),
          ],
        ),
        children: keyedEntries.map((entry) {
          final key = entry.key;
          final f = entry.field;
          if (f == null) return const SizedBox.shrink();
          return ListTile(
            dense: true,
            contentPadding: Insets.memberRow,
            title: Row(
              children: [
                SizedBox(
                  width: _memberLabelWidth,
                  child: Text(
                    systemStructMemberName(fieldIndex, key),
                    style: theme.textTheme.labelSmall?.copyWith(
                      color: kTextMuted,
                    ),
                  ),
                ),
                Expanded(
                  child: MonoText(
                    formatSystemValue(
                      f.meta.dataType,
                      f.value,
                      fieldIndex,
                      key,
                    ),
                  ),
                ),
                TamuChipRow(
                  spacing: Spacing.xs,
                  chips: [
                    for (final flag in primaryFlags)
                      TamuChip(
                        flag,
                        tone: flag == 'RO' ? ChipTone.accent : ChipTone.subtle,
                      ),
                  ],
                ),
              ],
            ),
            subtitle: Text(
              '${dataTypeLabel(f.meta.dataType)} '
              '[member=${systemStructMemberName(fieldIndex, key)}]',
              style: theme.textTheme.labelSmall,
            ),
            trailing: f.meta.readOnly
                ? null
                : PopupMenuButton<String>(
                    icon: const Icon(Icons.more_vert, size: IconSize.sm),
                    onSelected: (action) {
                      if (action == 'edit') {
                        _editValue(
                          blockType,
                          inst,
                          block,
                          fieldIndex,
                          memberKey: key,
                        );
                      } else if (action == 'save') {
                        _saveField(blockType, inst, block, fieldIndex);
                      } else if (action == 'recall') {
                        _recallField(blockType, inst, block, fieldIndex);
                      }
                    },
                    itemBuilder: (_) => [
                      const PopupMenuItem(
                        value: 'edit',
                        child: Text('Edit value'),
                      ),
                      if (f.meta.persistent) ...[
                        const PopupMenuItem(
                          value: 'save',
                          child: Text('Save to backup'),
                        ),
                        const PopupMenuItem(
                          value: 'recall',
                          child: Text('Recall from backup'),
                        ),
                      ],
                    ],
                  ),
          );
        }).toList(),
      );
    }

    final fieldInfo = isSystemField
        ? null
        : blockInfoFor(
            BlockType.fromValue(block?.meta.type ?? 0),
          )?.field(fieldIndex);

    return ListTile(
      dense: true,
      contentPadding: Insets.nestedRow,
      title: Row(
        children: [
          SizedBox(
            width: _labelColumnWidth,
            child: Text(
              isSystemField
                  ? systemFieldName(fieldIndex)
                  : (fieldInfo?.name ?? 'Field $fieldIndex'),
              style: theme.textTheme.labelSmall?.copyWith(color: kTextMuted),
            ),
          ),
          Expanded(
            child: MonoText(
              isSystemField
                  ? formatSystemValue(
                      field.meta.dataType,
                      field.value,
                      fieldIndex,
                      systemKeysForField(fieldIndex).first,
                    )
                  : displayValue(),
            ),
          ),
          TamuChipRow(
            spacing: Spacing.xs,
            chips: [
              for (final flag in ValueFlags.describe(field.meta.flags))
                TamuChip(
                  flag,
                  tone: flag == 'RO' ? ChipTone.accent : ChipTone.subtle,
                ),
            ],
          ),
        ],
      ),
      subtitle: Text(
        '${dataTypeLabel(field.meta.dataType)}'
        '${!isSystemField && fieldInfo?.unit != null ? ' [${fieldInfo!.unit}]' : ''}',
        style: theme.textTheme.labelSmall,
      ),
      trailing: field.meta.readOnly
          ? null
          : PopupMenuButton<String>(
              icon: const Icon(Icons.more_vert, size: IconSize.md),
              tooltip: 'Field actions',
              onSelected: (action) {
                if (action == 'edit') {
                  _editValue(blockType, inst, block, fieldIndex);
                } else if (action == 'save') {
                  _saveField(blockType, inst, block, fieldIndex);
                } else if (action == 'recall') {
                  _recallField(blockType, inst, block, fieldIndex);
                }
              },
              itemBuilder: (_) => [
                const PopupMenuItem(value: 'edit', child: Text('Edit value')),
                if (field.meta.persistent) ...[
                  const PopupMenuItem(
                    value: 'save',
                    child: Text('Save to backup'),
                  ),
                  const PopupMenuItem(
                    value: 'recall',
                    child: Text('Recall from backup'),
                  ),
                ],
              ],
            ),
      onTap: (!field.meta.readOnly)
          ? () => _editValue(blockType, inst, block, fieldIndex)
          : null,
    );
  }
}
