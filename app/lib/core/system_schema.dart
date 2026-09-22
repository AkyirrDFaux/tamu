/// System block schema (type 0, inst 0) shared by the Register view, the backup
/// tool and the memory viewers (Docs/Services/System Block and Device Commands.md).
///
/// Keeping the field/key names and key sets in one place stops the labels drifting
/// between the UI and the semantic backup format.
library;

/// Number of System block fields (firmware SYSTEM_FIELD_COUNT).
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
  8: 'App/CLI Active',
};

/// Field -> (key -> display name). Fields 1 (SN) and 6 (Name) are addressed at key
/// 0xFF by the UI; the firmware resolves them regardless of key.
const Map<int, Map<int, String>> systemFieldKeys = {
  0: {0: 'Device Type', 1: 'Capability', 2: 'Software version'},
  1: {0xFF: 'Serial Number'},
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
  6: {0xFF: 'Name'},
  7: {0: 'NetID'},
  8: {0: 'App Active', 1: 'CLI Active'},
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
  8: {0: '.appActive', 1: '.cliActive'},
};

String systemFieldName(int field) => systemFieldNames[field] ?? 'Field $field';

String systemKeyName(int field, int key) => systemFieldKeys[field]?[key] ?? 'Key $key';

String systemStructMemberName(int field, int key) =>
    systemStructMembers[field]?[key] ?? 'Key $key';

/// The keys present in one system field (a scalar field reports its single key).
List<int> systemKeysForField(int field) =>
    systemFieldKeys[field]?.keys.toList() ?? const [0];
