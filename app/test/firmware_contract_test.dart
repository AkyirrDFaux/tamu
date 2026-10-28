import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tamuapp/core/ble_security.dart';
import 'package:tamuapp/core/block_registry.dart';
import 'package:tamuapp/core/render_dict.dart';
import 'package:tamuapp/core/script_errors.dart';
import 'package:tamuapp/core/script_file.dart';
import 'package:tamuapp/core/script_functions.dart';
import 'package:tamuapp/core/types.dart';
import 'package:tamuapp/core/wifi_settings.dart';
import 'package:tamuapp/core/script_instructions.dart';

/// Guards the app<->firmware script *encoding* contract.
///
/// The 4-byte symbol stream is a private convention implemented twice: the editor writes it
/// (`lib/core/script_instructions.dart`, `script_file.dart`) and the VM reads it
/// (`firmware/src/Core/Services/ScriptDefs.h`). Nothing else would notice a drift in a category,
/// an opcode or the header size - the file would parse, the values would mean something else.
/// This compares the two tables directly.
///
/// The firmware tree is read from disk, so the test skips cleanly when the app is checked out
/// or tested on its own.
Map<String, int> _firmwareDefines() {
  final candidates = [
    '../firmware/src/Core/Services/ScriptDefs.h',
    '../../firmware/src/Core/Services/ScriptDefs.h',
  ];
  for (final path in candidates) {
    final file = File(path);
    if (!file.existsSync()) continue;
    final defines = <String, int>{};
    final re = RegExp(
      r'^#define\s+(SCRIPT_\w+|MAX_SCRIPTS)\s+(0x[0-9A-Fa-f]+|-?\d+)[uUlL]*',
      multiLine: true,
    );
    for (final m in re.allMatches(file.readAsStringSync())) {
      final raw = m.group(2)!;
      defines[m.group(1)!] = raw.startsWith('0x')
          ? int.parse(raw.substring(2), radix: 16)
          : int.parse(raw);
    }
    return defines;
  }
  return const {};
}

/// Reads one enumerator body from a firmware header, as `name -> value`.
///
/// Handles both forms used in `Enums.h`: `enum class Name : uint16_t { ... }` and
/// `enum Name : uint16_t { ... }`.
Map<String, int> _firmwareEnum(String relativePath, String enumName) {
  final candidates = [
    '../firmware/$relativePath',
    '../../firmware/$relativePath',
  ];
  for (final path in candidates) {
    final file = File(path);
    if (!file.existsSync()) continue;
    final source = file.readAsStringSync();
    final header = RegExp(
      '\\benum\\s+(?:class\\s+)?$enumName\\b[^{]*\\{',
    ).firstMatch(source);
    if (header == null) continue;
    final open = header.end - 1;
    final close = source.indexOf('};', open);
    if (close < 0) continue;
    final body = source.substring(open + 1, close);
    final values = <String, int>{};
    final re = RegExp(r'(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)');
    for (final m in re.allMatches(body)) {
      final raw = m.group(2)!;
      values[m.group(1)!] = raw.startsWith('0x')
          ? int.parse(raw.substring(2), radix: 16)
          : int.parse(raw);
    }
    return values;
  }
  return const {};
}

/// Reads the field table of a firmware static block (`const BlockEntry Name[] = { ... }`).
///
/// The app's field labels are indexed by field number, so a firmware field the app table does
/// not carry renders as an unnamed "Field N" row on every surface that displays it. Parsing the
/// entry table (field index, offset, type, flags) keeps the two in step instead of trusting a
/// comment that nothing checks.
///
/// Returns null when the firmware tree is not checked out next to the app (the contract tests
/// then skip), and an empty list when the file exists but the table could not be parsed - that
/// is a broken guard, not an absent firmware, so it must not pass silently.
List<({int field, int offset, String type, String flags})>?
_firmwareBlockEntries(String relativePath, String tableName) {
  for (final path in [
    '../firmware/$relativePath',
    '../../firmware/$relativePath',
  ]) {
    final file = File(path);
    if (!file.existsSync()) continue;
    final source = file.readAsStringSync();
    final start = source.indexOf('$tableName[]');
    if (start < 0) return const [];
    final open = source.indexOf('{', start);
    final close = source.indexOf('\n};', open);
    if (open < 0 || close < 0) return const [];
    final out = <({int field, int offset, String type, String flags})>[];
    final re = RegExp(
      r'MakeFieldKey\((\d+),\s*(\d+)\),\s*(\d+),\s*\{\(uint16_t\)DataType::(\w+),\s*sizeof\([^)]*\),\s*([^}]*)\}',
    );
    for (final line in source.substring(open, close).split('\n')) {
      final m = re.firstMatch(line);
      if (m == null) continue;
      out.add((
        field: int.parse(m.group(1)!),
        offset: int.parse(m.group(3)!),
        type: m.group(4)!,
        flags: m.group(5)!.trim(),
      ));
    }
    return out;
  }
  return null;
}

/// The WiFi Settings table's `(field, type)` pairs, in table order.
///
/// [_firmwareBlockEntries] cannot read it: that parser expects one entry per line, a numeric
/// field index and a `sizeof(...)` size, while this table writes each entry over two lines,
/// names its field indexes with macros (`WIFI_SET_FIELD_ENABLED`) and declares its sizes as
/// macros too. WiFi Settings is a compile-in option, so the header is only there when the option
/// is enabled - null means "not checked out", `[]` means the file exists but the table was not
/// found (a broken guard, not an absent block).
List<({int field, String type})>? _wifiSettingsEntries() {
  for (final path in [
    '../firmware/src/Blocks/WifiSettings.h',
    '../../firmware/src/Blocks/WifiSettings.h',
  ]) {
    final file = File(path);
    if (!file.existsSync()) continue;
    final source = file.readAsStringSync();
    final defines = <String, int>{};
    for (final m in RegExp(
      r'^#define\s+(WIFI_SET_FIELD_\w+)\s+(\d+)',
      multiLine: true,
    ).allMatches(source)) {
      defines[m.group(1)!] = int.parse(m.group(2)!);
    }
    final start = source.indexOf('WifiSettings_Entries[]');
    if (start < 0) return const [];
    final open = source.indexOf('{', start);
    final close = source.indexOf('\n};', open);
    if (open < 0 || close < 0) return const [];
    final body = source.substring(open, close);
    // `{ MakeFieldKey(FIELD, key), offset,` ... `{ (uint16_t)DataType::Type, size, flags } }`
    final re = RegExp(
      r'MakeFieldKey\((\w+),\s*\d+\),\s*[^,]+,\s*\{\s*\(uint16_t\)DataType::(\w+)',
      dotAll: true,
    );
    return [
      for (final m in re.allMatches(body))
        if (defines[m.group(1)!] != null)
          (field: defines[m.group(1)!]!, type: m.group(2)!),
    ];
  }
  return null;
}

/// The BLE Settings table's `(field, type, size)` triples, in table order.
///
/// [_firmwareBlockEntries] cannot read this table either: each entry spans two lines, names its
/// field index with a macro (`BLE_SET_FIELD_PASSKEY`) and its size with another
/// (`BLE_PASSKEY_LEN`) or a `sizeof(...)`. The size matters here because the app pads a text
/// write to the device's declared width - a wrong number would write six bytes into a sixteen-byte
/// name. Returns null when the firmware tree is not checked out next to the app, and an empty list
/// when the file exists but the table could not be parsed.
List<({int field, String type, int size})>? _bleSettingsEntries() {
  for (final path in [
    '../firmware/src/Blocks/BleSettings.h',
    '../../firmware/src/Blocks/BleSettings.h',
  ]) {
    final file = File(path);
    if (!file.existsSync()) continue;
    final source = file.readAsStringSync();
    // The widths the table names live beside it, in its own header and in the policy header it
    // includes (BLE_PASSKEY_LEN).
    final defines = <String, int>{};
    final policy = File('${file.parent.path}/BleSecurityPolicy.h');
    final sources = [
      source,
      if (policy.existsSync()) policy.readAsStringSync(),
    ];
    for (final text in sources) {
      for (final m in RegExp(
        r'^#define\s+(BLE_\w+)\s+(\d+)\s*$',
        multiLine: true,
      ).allMatches(text)) {
        defines[m.group(1)!] = int.parse(m.group(2)!);
      }
    }
    final start = source.indexOf('BleSettings_Entries[]');
    if (start < 0) return const [];
    final open = source.indexOf('{', start);
    final close = source.indexOf('\n};', open);
    if (open < 0 || close < 0) return const [];
    final body = source.substring(open, close);
    // `{ MakeFieldKey(FIELD, key), offset,` ... `{ (uint16_t)DataType::Type, size, flags } }`
    final re = RegExp(
      r'MakeFieldKey\((BLE_SET_FIELD_\w+),\s*\d+\),.*?'
      r'\(uint16_t\)DataType::(\w+),\s*([^,]+),',
      dotAll: true,
    );
    final out = <({int field, String type, int size})>[];
    for (final m in re.allMatches(body)) {
      final field = defines[m.group(1)!];
      if (field == null) continue;
      final type = m.group(2)!;
      final sizeText = m.group(3)!.trim();
      final width =
          int.tryParse(sizeText) ??
          defines[sizeText] ??
          switch (sizeText) {
            'sizeof(uint32_t)' => 4,
            'sizeof(uint8_t)' => 1,
            _ => null,
          };
      if (width == null) continue;
      out.add((field: field, type: type, size: width));
    }
    return out;
  }
  return null;
}

/// The BLE securing budgets the firmware states (`constexpr uint32_t BleSecureBudget*Ms = N;` in
/// `src/Blocks/BleSecurityPolicy.h`), keyed by name.
///
/// The app restates both because the failure classifier judges a dropped link against them and has
/// no other evidence: a wrong figure would either miss the passkey band (5-30 s) or start calling
/// ordinary link losses pairing refusals. Returns null when the firmware tree is not checked out
/// next to the app, and an empty map when the file exists but nothing could be parsed.
Map<String, int>? _bleSecureBudgets() {
  for (final path in [
    '../firmware/src/Blocks/BleSecurityPolicy.h',
    '../../firmware/src/Blocks/BleSecurityPolicy.h',
  ]) {
    final file = File(path);
    if (!file.existsSync()) continue;
    final source = file.readAsStringSync();
    return {
      for (final m in RegExp(
        r'constexpr\s+uint32_t\s+(BleSecureBudget\w+Ms)\s*=\s*(\d+)\s*;',
      ).allMatches(source))
        m.group(1)!: int.parse(m.group(2)!),
    };
  }
  return null;
}

/// Compares an app enum (`name -> value`) against a firmware enum.
///
/// [alias] maps app names onto the firmware spelling where they differ; [firmwareOnly] lists
/// firmware members (aliases such as `Unknown`/`System`) that deliberately have no app entry;
/// [appOnly] lists app members that are not real firmware enum values (pseudo types). Every
/// name on either side must be accounted for, so a new enum member cannot slip through.
void _expectEnumMatches(
  Map<String, int> app,
  Map<String, int> firmware, {
  Map<String, String> alias = const {},
  Set<String> firmwareOnly = const {},
  Set<String> appOnly = const {},
}) {
  String norm(String s) => s.toLowerCase().replaceAll(RegExp('[^a-z0-9]'), '');
  final fwByName = {for (final e in firmware.entries) norm(e.key): e.value};
  final seen = <String>{};

  for (final entry in app.entries) {
    final target = norm(alias[entry.key] ?? entry.key);
    if (appOnly.contains(norm(entry.key))) continue;
    expect(
      fwByName.containsKey(target),
      isTrue,
      reason: 'firmware enum has no member for "${entry.key}"',
    );
    expect(fwByName[target], entry.value, reason: '"${entry.key}" value');
    seen.add(target);
  }

  for (final name in fwByName.keys) {
    if (seen.contains(name)) continue;
    expect(
      firmwareOnly.contains(name),
      isTrue,
      reason: 'app enum has no member for the firmware entry "$name"',
    );
  }
}

/// Compares an app value->label map against a firmware `Name = value` enum. Labels such as
/// "Double Parabola" and the firmware's `DoubleParabola` are the same name.
void _expectSameEnum(Map<int, String> app, Map<String, int> firmware) {
  String norm(String s) => s.toLowerCase().replaceAll(RegExp('[^a-z0-9]'), '');
  expect(firmware.length, app.length, reason: 'enum size differs');
  final byName = {for (final e in firmware.entries) norm(e.key): e.value};
  for (final entry in app.entries) {
    final key = norm(entry.value);
    expect(
      byName.containsKey(key),
      isTrue,
      reason: 'firmware has no enum member for "${entry.value}"',
    );
    expect(byName[key], entry.key, reason: '${entry.value} numbering');
  }
}

void main() {
  final fw = _firmwareDefines();
  final skipReason = fw.isEmpty
      ? 'firmware source not available next to the app'
      : null;

  group('script encoding contract (app vs firmware)', () {
    test('the symbol-type enum matches', () {
      expect(fw['SCRIPT_SYM_INSTRUCTION'], symInstruction);
      expect(fw['SCRIPT_SYM_INPUT'], symInput);
      expect(fw['SCRIPT_SYM_OUTPUT'], symOutput);
      expect(fw['SCRIPT_SYM_VARIABLE'], symVariable);
      expect(fw['SCRIPT_SYM_CONSTANT'], symConstant);
      expect(fw['SCRIPT_SYM_ENDLINE'], symEndline);
      expect(fw['SCRIPT_SYM_PREDEFINE'], symPredefine);
    });

    test('the predefine-subtype enum matches', () {
      expect(fw['SCRIPT_PRE_STATE'], preState);
      expect(fw['SCRIPT_PRE_TYPE'], preType);
      expect(fw['SCRIPT_PRE_INDEX'], preIndex);
      expect(fw['SCRIPT_PRE_CHAR'], preChar);
      expect(fw['SCRIPT_PRE_MATHOP'], preMathOp);
      expect(fw['SCRIPT_PRE_BOOL'], preBool);
      expect(fw['SCRIPT_PRE_NUMBER'], preNumber);
    });

    test('the instruction-category enum matches', () {
      expect(fw['SCRIPT_CAT_MATH'], catMath);
      expect(fw['SCRIPT_CAT_LOGIC'], catLogic);
      expect(fw['SCRIPT_CAT_FLOW'], catFlow);
      expect(fw['SCRIPT_CAT_TIME'], catTime);
      expect(fw['SCRIPT_CAT_SERVICE'], catService);
      expect(fw['SCRIPT_CAT_COMPOSE'], catCompose);
    });

    test('every inline math operator the editor offers is known to the VM', () {
      // The VM implements one SCRIPT_MATHOP_* name per operator value.
      const expected = {
        0: 'ADD',
        1: 'SUB',
        2: 'MUL',
        3: 'DIV',
        4: 'MOD',
        5: 'POW',
        6: 'AND',
        7: 'OR',
        8: 'XOR',
        9: 'NOT',
        12: 'EQ',
        13: 'NE',
        14: 'LT',
        15: 'LE',
        16: 'GT',
        17: 'GE',
        18: 'OPEN',
        19: 'CLOSE',
        20: 'FN_DOT',
        21: 'FN_CROSS',
        22: 'FN_SIZE',
        23: 'FN_TRANSPOSE',
      };
      for (final entry in expected.entries) {
        expect(
          fw['SCRIPT_MATHOP_${entry.value}'],
          entry.key,
          reason: 'math op ${entry.key} (${entry.value})',
        );
      }
      // And the editor must not offer anything outside that set.
      for (final op in expressionOps) {
        expect(
          expected.containsKey(op),
          isTrue,
          reason: 'editor offers op $op',
        );
      }
    });

    test('the instruction opcodes match the VM', () {
      // (category define prefix, op -> firmware suffix)
      const mathOps = {
        0: 'SET',
        5: 'MOD',
        6: 'MIN',
        7: 'MAX',
        9: 'ABS',
        10: 'LIMIT',
        11: 'TRANSFORM',
      };
      const flowOps = {
        0: 'IF',
        1: 'WHILE',
        2: 'END',
        3: 'JUMP',
        4: 'CALL',
        5: 'RETURN',
        6: 'HALT',
      };
      const timeOps = {0: 'DELAY', 1: 'WAIT', 2: 'GET'};
      const serviceOps = {
        0: 'LOG',
        1: 'REG_READ',
        2: 'REG_WRITE',
        // 3 is reserved: the retired "Script state" instruction, never reused (Script.md).
        4: 'NOP',
        5: 'REG_READ_FOREIGN',
        6: 'REG_WRITE_FOREIGN',
        7: 'SCRIPT_LOAD',
        8: 'SCRIPT_UNLOAD',
        // The state verbs as separate Service instructions (Script.md Stage 5).
        9: 'START',
        10: 'STOP',
        11: 'PAUSE',
        12: 'RESUME',
        13: 'ERROR',
        14: 'REPORT_INFO',
      };
      const composeOps = {0: 'COMPOSE', 1: 'EXTRACT'};

      const tables = {
        catMath: ('SCRIPT_OP_MATH_', mathOps),
        catFlow: ('SCRIPT_OP_FLOW_', flowOps),
        catTime: ('SCRIPT_OP_TIME_', timeOps),
        catService: ('SCRIPT_OP_SERVICE_', serviceOps),
        // The compose pair is not category-infix named in the firmware.
        catCompose: ('SCRIPT_OP_', composeOps),
      };

      for (final def in scriptInstructions) {
        final table = tables[def.category];
        if (table == null) continue; // logic (Select) is checked below
        final (prefix, ops) = table;
        expect(
          ops[def.op],
          isNotNull,
          reason: '${def.label}: op ${def.op} has no firmware define',
        );
        expect(
          fw['$prefix${ops[def.op]}'],
          def.op,
          reason: '${def.label}: $prefix${ops[def.op]}',
        );
      }

      expect(fw['SCRIPT_OP_LOGIC_SELECT'], 12);
    });

    test('the file-format sizes match', () {
      expect(fw['MAX_SCRIPTS'], maxScripts);
      expect(fw['SCRIPT_HEADER_SIZE'], scriptHeaderSize);
      expect(fw['SCRIPT_FUNC_RESERVATION_SIZE'], scriptFunctionReservationSize);
    });

    test('the function-reservation + global-call encoding matches', () {
      // The reservation record layout is the spec's weak point; both sides isolate it. Guard the
      // byte positions the app writes against the record the firmware parses.
      final r = encodeFunctionReservation(
        fileId: 0x1234,
        memOffset: 7,
        instrOffset: 9,
      );
      expect(r[0] | (r[1] << 8), 0x1234, reason: 'FileId at offset 0');
      expect(r[2] | (r[3] << 8), 7, reason: 'MemOffset at offset 2');
      expect(r[4] | (r[5] << 8), 9, reason: 'InstrOffset at offset 4');
      // The global-call operand marks the reservation index with bit 15 (firmware
      // SCRIPT_CALL_GLOBAL_BIT). The low bits hold the index.
      expect(fw['SCRIPT_CALL_GLOBAL_BIT'], scriptCallGlobalBit);
      expect(scriptGlobalCallOperandValue(5), scriptCallGlobalBit | 5);
    });

    test('the max call depth matches', () {
      expect(fw['SCRIPT_MAX_CALL_DEPTH'], scriptMaxCallDepth);
    });

    test(
      'the error-code registry names every SCRIPT_ERR_* the firmware defines',
      () {
        // The app owns the error names (Docs/Services/Script.md "Error Model"), but the numbering is
        // the firmware's: a code whose meaning moved would be read as a different fault.
        final firmwareErrors = {
          for (final entry in fw.entries)
            if (entry.key.startsWith('SCRIPT_ERR_')) entry.key: entry.value,
        };
        expect(firmwareErrors, isNotEmpty);
        for (final entry in firmwareErrors.entries) {
          expect(
            ScriptErrorCode.names[entry.value],
            entry.key,
            reason: 'code ${entry.value}',
          );
        }
        // And the app must not name a code the firmware does not define.
        // (names is code -> name; firmwareErrors is name -> code.)
        for (final entry in ScriptErrorCode.names.entries) {
          expect(
            firmwareErrors[entry.value],
            entry.key,
            reason: 'app names ${entry.value} (code ${entry.key})',
          );
        }
      },
    );

    test('the CID 3 report-record layout matches', () {
      // ScriptClient.readState decodes these offsets; a drift would mis-read the record.
      expect(fw['SCRIPT_REPORT_OFF_STATE'], 0);
      expect(fw['SCRIPT_REPORT_OFF_DEPTH'], 1);
      expect(fw['SCRIPT_REPORT_OFF_LINE'], 2);
      expect(fw['SCRIPT_REPORT_OFF_INSTRUCTION'], 4);
      expect(fw['SCRIPT_REPORT_OFF_ERROR'], 8);
      expect(fw['SCRIPT_REPORT_SIZE'], 4);
      expect(fw['SCRIPT_REPORT_SIZE_ERR'], 12);
    });
  }, skip: skipReason);

  group('core enum contract (app vs firmware)', () {
    Map<String, int> names<T>(List<({String name, int value})> entries) => {
      for (final e in entries) e.name: e.value,
    };

    test('DeviceType matches', () {
      final fw = _firmwareEnum('src/Core/Types/Enums.h', 'DeviceType');
      if (fw.isEmpty) return;
      _expectEnumMatches(
        names([
          for (final t in DeviceType.values) (name: t.name, value: t.value),
        ]),
        fw,
      );
    });

    test('DataType matches', () {
      final fw = _firmwareEnum('src/Core/Types/Enums.h', 'DataType');
      if (fw.isEmpty) return;
      _expectEnumMatches(
        names([
          for (final t in DataType.values) (name: t.name, value: t.value),
        ]),
        fw,
        // The app spells Index/Enum/Bool differently (Dart keywords).
        alias: const {'integer': 'Index'},
        // Firmware aliases with no distinct app entry.
        firmwareOnly: const {'unknownkeyed'},
      );
    });

    test('BlockType matches', () {
      final fw = _firmwareEnum('src/Core/Types/Enums.h', 'BlockType');
      if (fw.isEmpty) return;
      _expectEnumMatches(
        names([
          for (final t in BlockType.values) (name: t.name, value: t.value),
        ]),
        fw,
        alias: const {'vysiDisplay': 'Vysi1Display'},
        firmwareOnly: const {'system'},
        // The banked Script (0x3F4-0x3F7) and Dynamic (0x3F0-0x3F3) ranges are not members of
        // the firmware's BlockType enum (BlockTypeRange owns them), and "render" is the app's
        // pseudo grouping for the 0x101/0x102 dictionary types.
        appOnly: const {'script', 'dynamic', 'render'},
      );
    });

    test('TriggerType matches', () {
      final fw = _firmwareEnum('src/Core/Types/Enums.h', 'TriggerType');
      if (fw.isEmpty) return;
      _expectEnumMatches(
        names([
          for (final t in TriggerType.values) (name: t.name, value: t.value),
        ]),
        fw,
      );
    });

    test('ValueFlags matches', () {
      final fw = _firmwareEnum('src/Core/Types/Enums.h', 'ValueFlags');
      if (fw.isEmpty) return;
      _expectEnumMatches(
        const {
          'readOnly': ValueFlags.readOnly,
          'persistent': ValueFlags.persistent,
          'trigger': ValueFlags.trigger,
        },
        fw,
        firmwareOnly: const {'none'},
      );
      // The ValueInfo flag bits.
      expect(fw['ReadOnly'], ValueFlags.readOnly);
    });

    test('Capability bits match', () {
      final source = File('../firmware/src/Core/Types/Enums.h');
      if (!source.existsSync()) return;
      final fw = <String, int>{};
      final re = RegExp(r'constexpr\s+uint32_t\s+(\w+)\s*=\s*1u\s*<<\s*(\d+)');
      for (final m in re.allMatches(source.readAsStringSync())) {
        fw[m.group(1)!] = 1 << int.parse(m.group(2)!);
      }
      expect(fw, isNotEmpty);
      _expectEnumMatches(
        const {
          'core': Capability.core,
          'router': Capability.router,
          'dynamicMemory': Capability.dynamicMemory,
          'scripts': Capability.scripts,
          'appInterface': Capability.appInterface,
          'subscriptionRequest': Capability.subscriptionRequest,
          'node': Capability.node,
          'subscriptionProvide': Capability.subscriptionProvide,
        },
        fw,
        // The firmware namespace does not define bit 1; the app names it Router from
        // Docs/Services/Router.md (the feature is not implemented).
        appOnly: const {'router'},
      );
    });
  }, skip: skipReason);

  group('block field schema contract (app vs firmware)', () {
    test('the ResistiveMeasure schema and the app registry name the same fields', () {
      final entries = _firmwareBlockEntries(
        'src/Core/Devices/Measuring.h',
        'ResistiveMeasEntries',
      );
      if (entries == null) {
        return; // firmware source not available next to the app
      }
      expect(
        entries,
        isNotEmpty,
        reason:
            'the ResistiveMeasEntries table was not parsed - guard is broken',
      );
      final meas = blockInfoFor(BlockType.resistiveMeasure)!;
      // A firmware field with no app label renders as "Field N" on the register page, the
      // backup view and the subscription picker - the defect this guard exists for.
      expect(
        meas.fields.length,
        entries.length,
        reason: 'app field labels lag the firmware ResistiveMeasEntries table',
      );
      // The label list is indexed by field number, so the entries must run 0..n-1 in order.
      expect(
        [for (final e in entries) e.field],
        [for (var i = 0; i < entries.length; i++) i],
      );
      // The rail flags (app indices 5/6): the app renders them as a Bool with no edit
      // control, which is the firmware's Bool + read-only treatment.
      for (final index in const [5, 6]) {
        final entry = entries.firstWhere((e) => e.field == index);
        expect(entry.type, 'Bool', reason: 'field $index type');
        expect(
          entry.flags,
          contains('ValueReadOnly'),
          reason: 'field $index flags',
        );
      }
    });

    // The WiFi Settings block (type 0x0C) is a compile-in firmware option, so its header only
    // exists in a build that enables it. The guard engages as soon as it lands: the panel reads
    // the fields by number, so a shift in the firmware table would silently read another field.
    test('the WiFi Settings schema and the app registry name the same fields', () {
      final entries = _wifiSettingsEntries();
      if (entries == null) {
        return; // firmware source not available next to the app
      }
      expect(
        entries,
        isNotEmpty,
        reason:
            'the WifiSettings_Entries table was not parsed - guard is broken',
      );
      final wifi = blockInfoFor(BlockType.wifiSettings)!;
      expect(
        wifi.fields.length,
        entries.length,
        reason: 'app field labels lag the firmware WifiSettings table',
      );
      // The label list is indexed by field number, so the entries must run 0..n-1 in order.
      expect(
        [for (final e in entries) e.field],
        [for (var i = 0; i < entries.length; i++) i],
      );
      // The two fields the app treats specially: the secret password (a Text that always reads
      // empty), and the address - 16 raw `Undefined` bytes the APP formats, never Text.
      expect(entries[wifiFieldPassword].type, 'Text');
      expect(entries[wifiFieldIp].type, 'Undefined');
      expect(wifi.field(wifiFieldPassword)!.secret, isTrue);
    });

    // The BLE Settings block (type 0x0B, core only). The panel reads and writes the fields by
    // number and pads text writes to the device's declared width, so both the order and the
    // sizes are part of the contract.
    test('the BLE Settings schema and the app registry name the same fields', () {
      final entries = _bleSettingsEntries();
      if (entries == null) {
        return; // firmware source not available next to the app
      }
      expect(
        entries,
        isNotEmpty,
        reason:
            'the BleSettings_Entries table was not parsed - guard is broken',
      );
      final ble = blockInfoFor(BlockType.bleSettings)!;
      // The table carries one row per key, and the Bond field is keyed three times: 12 fields,
      // 14 rows. The app labels fields by number, so the DISTINCT fields must run 0..11 in order.
      final fields = [for (final e in entries) e.field];
      expect(
        fields.toSet().toList()..sort(),
        [for (var i = 0; i < bleFieldCount; i++) i],
        reason: 'app field labels lag the firmware BleSettings table',
      );
      expect(
        ble.fields.length,
        bleFieldCount,
        reason: 'the registry must name every field of the block',
      );

      ({int field, String type, int size}) at(int field) =>
          entries.firstWhere((e) => e.field == field);

      // The passkey: six ASCII digits, readable and writable, masked in the app.
      expect(at(bleFieldPasskey).type, 'Text');
      expect(at(bleFieldPasskey).size, blePasskeyLength);
      expect(ble.field(bleFieldPasskey)!.secret, isTrue);
      expect(ble.field(bleFieldPasskey)!.maxChars, blePasskeyLength);
      // The bond entries are addresses, not names (Issues.md E60), and the app must read as many
      // keys as the store holds.
      expect(at(bleFieldBond).type, 'Text');
      expect(ble.field(bleFieldBond)!.name, 'Bond address');
      expect(
        entries.where((e) => e.field == bleFieldBond).length,
        bleBondSlots,
      );
      // The window is a 32-bit millisecond count: five minutes does not fit a 16-bit field.
      expect(at(bleFieldWindow).type, 'Uint32');
      expect(at(bleFieldWindow).size, 4);
      expect(at(bleFieldEnabled).type, 'Bool');
      // Both enums carry the block's own numbering, which the panel writes back verbatim.
      expect(at(bleFieldMode).type, 'Enum');
      expect(at(bleFieldAction).type, 'Enum');
      expect(at(bleFieldAction).size, 1);
      expect(ble.field(bleFieldMode)!.enumValues, bleModeLabels);
      expect(ble.field(bleFieldAction)!.enumValues, bleActionLabels);
      expect(ble.field(bleFieldDisplayName)!.maxChars, bleDisplayNameWidth);
      // The E66 read-out (fields 9-11): two addresses at the bond text's width, and the verdict.
      // The app labels that Enum with its own vocabulary, so a re-numbered `BleLinkVerdict` would
      // name the wrong outcome; the values are pinned to the firmware enum here.
      expect(at(bleFieldLastPeer).type, 'Text');
      expect(at(bleFieldLastPeer).size, 20);
      expect(at(bleFieldLastAir).type, 'Text');
      expect(at(bleFieldLastAir).size, 20);
      expect(ble.field(bleFieldLastPeer)!.maxChars, 20);
      expect(at(bleFieldLastLink).type, 'Enum');
      expect(at(bleFieldLastLink).size, 1);
      final verdicts = _firmwareEnum(
        'src/Blocks/BleAdmissionPolicy.h',
        'BleLinkVerdict',
      );
      if (verdicts.isNotEmpty) {
        expect(
          bleVerdictLabels.keys.toList()..sort(),
          verdicts.values.toList()..sort(),
          reason: 'the app labels a BleLinkVerdict value it must not guess at',
        );
        expect(ble.field(bleFieldLastLink)!.enumValues, bleVerdictLabels);
      }
    });

    // The securing budgets the app's failure classifier restates. The card's stale-bond guidance
    // turns on a link that died inside the firmware's deadline, and the app has no other evidence
    // on the drop path: it must not judge a passkey pairing (30 s to type six digits) by the Just
    // Works figure, and it must not call a long-lived session a pairing refusal either.
    test('the BLE securing budgets match BleSecurityPolicy.h', () {
      final budgets = _bleSecureBudgets();
      if (budgets == null) {
        return; // firmware source not available next to the app
      }
      expect(
        budgets,
        isNotEmpty,
        reason:
            'the BleSecureBudget*Ms constants were not parsed - guard is broken',
      );
      expect(budgets['BleSecureBudgetJustWorksMs'], bleSecureBudgetJustWorksMs);
      expect(budgets['BleSecureBudgetPasskeyMs'], bleSecureBudgetPasskeyMs);
      expect(
        bleSecureBudgetPasskeyMs,
        greaterThan(bleSecureBudgetJustWorksMs),
        reason:
            'a passkey budget smaller than Just Works would drop every typed code',
      );
    });
  }, skip: skipReason);

  group('render dictionary contract (app vs firmware)', () {
    test('shape / operation / texture numbering matches Render.h', () {
      final render = _firmwareEnum('src/Blocks/Render.h', 'Geometries');
      if (render.isEmpty) {
        return; // firmware not available; the group below is skipped
      }
      _expectSameEnum(renderShapes, render);
      _expectSameEnum(
        renderOperations,
        _firmwareEnum('src/Blocks/Render.h', 'GeometryOperation'),
      );
      _expectSameEnum(
        renderTextures,
        _firmwareEnum('src/Blocks/Render.h', 'Textures2D'),
      );
    });
  }, skip: skipReason);
}
