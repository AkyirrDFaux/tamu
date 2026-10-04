/// System block schema (type 0, inst 0) shared by the Register view, the backup
/// tool and the memory viewers (Docs/Services/System Block and Device Commands.md).
///
/// Keeping the field/key names and key sets in one place stops the labels drifting
/// between the UI and the semantic backup format.
library;

import 'types.dart';

/// Number of System block fields on a core (the DAS exposes fields 0-6, no NetID/App Active).
const int systemFieldCount = 9;

const Map<int, String> systemFieldNames = {
  0: 'Device Type',
  1: 'Serial Number',
  2: 'Short Address',
  3: 'Time',
  4: 'RAM',
  5: 'Storage',
  6: 'Name',
  7: 'NetID',
  8: 'App Active',
};

/// Field -> (key -> display name). Every System entry is addressed at key 0 (the firmware
/// schema is one BlockEntry per field; a struct's position is not on the wire - the app reads
/// the whole struct and slices the member locally).
const Map<int, Map<int, String>> systemFieldKeys = {
  0: {0: 'Device Type', 1: 'Capability', 2: 'Software version'},
  1: {0: 'Serial Number'},
  2: {0: 'Short Address'},
  3: {
    0: 'Uptime',
    1: 'Current time',
    2: 'Time offset',
    3: 'Loop time',
    4: 'Max Loop time',
  },
  4: {0: 'Used RAM', 1: 'Total RAM'},
  5: {0: 'Used FLASH', 1: 'Total FLASH'},
  6: {0: 'Name'},
  7: {0: 'NetID'},
  8: {0: 'App Active'},
};

/// Field -> (key -> dotted struct member name) for the keyed fields (UI sub-labels).
const Map<int, Map<int, String>> systemStructMembers = {
  0: {0: '.deviceType', 1: '.capabilities', 2: '.softwareVersion'},
  3: {
    0: '.uptime',
    1: '.currentTime',
    2: '.timeOffsetMs',
    3: '.avgLoopTimeMs',
    4: '.maxLoopTimeMs',
  },
  4: {0: '.usedRAM', 1: '.totalRAM'},
  5: {0: '.usedFlash', 1: '.totalFlash'},
  8: {0: '.appActive'},
};

String systemFieldName(int field) => systemFieldNames[field] ?? 'Field $field';

String systemKeyName(int field, int key) => systemFieldKeys[field]?[key] ?? 'Key $key';

String systemStructMemberName(int field, int key) =>
    systemStructMembers[field]?[key] ?? 'Key $key';

/// The keys present in one system field (a scalar field reports its single key).
List<int> systemKeysForField(int field) =>
    systemFieldKeys[field]?.keys.toList() ?? const [0];

/// The System struct fields (0, 3, 4, 5) are read whole (one entry per field, key 0); the app
/// slices the member the key selects. (field, key) -> the member's type and byte size, in
/// struct order (Docs/Services/System Block and Device Commands.md).
const Map<int, Map<int, ({DataType type, int size})>> systemStructFields = {
  0: {
    0: (type: DataType.enum_, size: 4), // Device Type
    1: (type: DataType.integer, size: 4), // Capability
    2: (type: DataType.string, size: 4), // Software version
  },
  3: {
    0: (type: DataType.integer, size: 4), // Uptime
    1: (type: DataType.integer, size: 4), // Current time
    2: (type: DataType.integer, size: 4), // Time offset
    3: (type: DataType.number, size: 4), // Loop time
    4: (type: DataType.number, size: 4), // Max Loop time
  },
  4: {
    0: (type: DataType.integer, size: 4), // Used RAM
    1: (type: DataType.integer, size: 4), // Total RAM
  },
  5: {
    0: (type: DataType.integer, size: 4), // Used FLASH
    1: (type: DataType.integer, size: 4), // Total FLASH
  },
  8: {
    0: (type: DataType.enum_, size: 1), // App Active
  },
};

/// Byte offset of member `key` inside its struct field, or null when `field` is not a struct
/// or `key` is not one of its members.
int? systemStructOffset(int field, int key) {
  final members = systemStructFields[field];
  if (members == null || !members.containsKey(key)) return null;
  var off = 0;
  for (var k = 0; k < key; k++) {
    final m = members[k];
    if (m == null) return null;
    off += m.size;
  }
  return off;
}

/// Whether the device has the System NetID field (a core): its System block reports >= 8
/// fields. It changes the System segment size in the `.SV` space.
bool hasNetIdFor(List<({int type, int inst, ValueInfo meta, String name})?>? blocks) =>
    blocks?.any((b) =>
        b != null && b.type == systemBlockTypeValue && b.meta.size >= 8) ??
    true;
