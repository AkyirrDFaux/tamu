#pragma once

// ===== BLE Settings =====
// The BLE link's settings and state as register fields, so it is visible and settable through the
// register path the app already speaks, instead of a side-channel command set. Block type 0x0B,
// one instance. The radio half (pairing, encrypted attributes, the connectable window, the bond
// store) lives in Devices/Tamu_v2.0A/AppBLE.h; this block is the single source of truth for the
// settings and a read-out of what the radio is doing.
//
//   Passkey      (0, 0)    Text(6), Persistent | Trigger  "000000" = disabled (Just Works)
//   Mode         (1, 0)    Enum, RO                       effective policy, derived from Passkey
//   Bond count   (2, 0)    Index, RO                      entries in the NimBLE bond store
//   Bond         (3, 0..N) Text, RO, keyed                one bonded identity address per key
//   Window       (4, 0)    Uint32, RO                     ms left in the connectable window
//   Action       (5, 0)    Enum, Trigger                  0 open the pairing window, 1 forget bonds
//   Enabled      (6, 0)    Bool, Persistent               false = the radio is not used at all
//   Display name (7, 0)    Text(16), Persistent           advertised name, empty = System.Name
//
// Field numbers run in the order of that table (the owner's table lists the keys, not the field
// indexes; this is the one place they are fixed). Writes are restricted (see the triggers and the
// change hook in Devices/Tamu_v2.0A/BleSettingsGlue.h): the passkey and "forget all bonds" are
// accepted only while the USB link is the live app interface, because a bonded phone that can
// rotate the key would be the hole the passkey exists to close. Opening the window is accepted
// from any link - it grants nothing but the chance to pair, which still needs the code.
//
// Persistence: Passkey, Enabled and Display name are Persistent fields, so the register machinery
// mirrors them to `.SV` with the rest of the static persistent space. There is deliberately no
// second copy in NVS. A firmware reflash keeps `.SV` (it is a data partition), while a full erase,
// a partition-table change or a `Format()` of the storage partition wipes it - together with the
// NimBLE bonds, which live in the `nvs` partition.

#include <cstddef>
#include "Core/Functions/MemoryTypes.h"
#include "Blocks/BleSecurityPolicy.h"

// Bond slots: one per CONFIG_BT_NIMBLE_MAX_BONDS (3). The read-only Bond field exposes that many
// keys; a device with more bonds than slots would silently hide the extras, so the two numbers
// are pinned together in BleSettingsGlue.h.
#define BLE_SETTINGS_BOND_SLOTS 3
// "AA:BB:CC:DD:EE:FF" is 17 characters. The bond store keeps no peer name, so an entry is the
// address; 20 keeps a Text field's 4-byte alignment without wasting the packet budget.
#define BLE_BOND_TEXT_LEN 20
// The advertised name field. BLE advertising carries 31 bytes of data and the name shares them
// with the service UUID, so 16 (the System Name's own width) is the sane ceiling.
#define BLE_DISPLAY_NAME_LEN 16

// Offsets into the two value spaces (see StaticBlockDescriptor::Base).
#define BLE_SET_FIELD_PASSKEY      0
#define BLE_SET_FIELD_MODE         1
#define BLE_SET_FIELD_BONDCOUNT    2
#define BLE_SET_FIELD_BOND         3
#define BLE_SET_FIELD_WINDOW       4
#define BLE_SET_FIELD_ACTION       5
#define BLE_SET_FIELD_ENABLED      6
#define BLE_SET_FIELD_DISPLAYNAME  7

#define BLE_SET_MODE_JUST_WORKS 0
#define BLE_SET_MODE_PASSKEY    1

#define BLE_SET_ACTION_OPEN_WINDOW  0
#define BLE_SET_ACTION_FORGET_BONDS 1

struct BleSettingsPersistent
{
    char    Passkey[BLE_PASSKEY_LEN] = {'0', '0', '0', '0', '0', '0'}; // field 0, offset 0
    // Default on: a device nobody has configured is still reachable over BLE (and the pairing
    // window decides when), so a fresh frame is usable from the phone alone.
    uint8_t Enabled = 1;                                              // field 6, offset 6
    // alignas(4): a value is aligned to its size capped at 4 (Docs/Services/Register.md "Memory
    // with 32-bit alignment"), so the 16-byte name sits at 8 and not at 7.
    alignas(4) char DisplayName[BLE_DISPLAY_NAME_LEN] = {};            // field 7, offset 8
};

struct BleSettingsVolatile
{
    uint8_t  Mode      = BLE_SET_MODE_JUST_WORKS;                   // field 1, offset 0
    uint8_t  BondCount = 0;                                         // field 2, offset 1
    alignas(4) char Bond[BLE_SETTINGS_BOND_SLOTS][BLE_BOND_TEXT_LEN] = {}; // field 3, offsets 4/24/44
    uint32_t WindowMs  = 0;                                         // field 4, offset 64
    uint8_t  Action    = 0;                                         // field 5, offset 68
};

// The entry table addresses the value spaces by byte offset, so the structs and the offsets above
// must agree. Pinned here rather than discovered as a read that returns the wrong field.
static_assert(offsetof(BleSettingsPersistent, Passkey) == 0, "Passkey offset");
static_assert(offsetof(BleSettingsPersistent, Enabled) == 6, "Enabled offset");
static_assert(offsetof(BleSettingsPersistent, DisplayName) == 8, "Display name offset");
static_assert(offsetof(BleSettingsVolatile, Mode) == 0, "Mode offset");
static_assert(offsetof(BleSettingsVolatile, BondCount) == 1, "Bond count offset");
static_assert(offsetof(BleSettingsVolatile, Bond[0]) == 4, "Bond[0] offset");
static_assert(offsetof(BleSettingsVolatile, Bond[1]) == 24, "Bond[1] offset");
static_assert(offsetof(BleSettingsVolatile, Bond[2]) == 44, "Bond[2] offset");
static_assert(offsetof(BleSettingsVolatile, WindowMs) == 64, "Window offset");
static_assert(offsetof(BleSettingsVolatile, Action) == 68, "Action offset");

// Defined in the board glue: the write side of the block (Devices/Tamu_v2.0A/BleSettingsGlue.h).
bool OnBlePasskeyWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length);
bool OnBleActionWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length);
bool OnBleEnabledWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length);

const BlockEntry BleSettings_Entries[] = {
    // Persistent: the passkey text, the single source of truth for the pairing policy.
    { MakeFieldKey(BLE_SET_FIELD_PASSKEY, 0), 0,
      { (uint16_t)DataType::Text, BLE_PASSKEY_LEN, (uint8_t)(ValuePersistent | ValueTrigger) } },
    // Read-only reports of the radio's state.
    { MakeFieldKey(BLE_SET_FIELD_MODE, 0), 0,
      { (uint16_t)DataType::Enum, 1, ValueReadOnly } },
    { MakeFieldKey(BLE_SET_FIELD_BONDCOUNT, 0), 1,
      { (uint16_t)DataType::Index, 1, ValueReadOnly } },
    { MakeFieldKey(BLE_SET_FIELD_BOND, 0), 4,
      { (uint16_t)DataType::Text, BLE_BOND_TEXT_LEN, ValueReadOnly } },
    { MakeFieldKey(BLE_SET_FIELD_BOND, 1), 24,
      { (uint16_t)DataType::Text, BLE_BOND_TEXT_LEN, ValueReadOnly } },
    { MakeFieldKey(BLE_SET_FIELD_BOND, 2), 44,
      { (uint16_t)DataType::Text, BLE_BOND_TEXT_LEN, ValueReadOnly } },
    { MakeFieldKey(BLE_SET_FIELD_WINDOW, 0), 64,
      { (uint16_t)DataType::Uint32, sizeof(uint32_t), ValueReadOnly } },
    // The only command surface. Not read-only: a write is a command, and the trigger decides
    // whether it is accepted; the stored byte stays 0 so a read-back never claims a command is
    // still pending.
    { MakeFieldKey(BLE_SET_FIELD_ACTION, 0), 68,
      { (uint16_t)DataType::Enum, 1, ValueTrigger } },
    // Settable settings. Display name has no trigger: it is a plain copy, and the BLE code notices
    // the change itself (AppBLESettingsApply, called from the main loop), so a write cannot
    // half-apply while the radio is mid-transition. Enabled does have one: switching the radio off
    // from the only radio the device is reachable through would strand the app, so that write obeys
    // the same USB-only rule as the passkey.
    { MakeFieldKey(BLE_SET_FIELD_ENABLED, 0), 6,
      { (uint16_t)DataType::Bool, sizeof(uint8_t), (uint8_t)(ValuePersistent | ValueTrigger) } },
    { MakeFieldKey(BLE_SET_FIELD_DISPLAYNAME, 0), 8,
      { (uint16_t)DataType::Text, BLE_DISPLAY_NAME_LEN, (uint8_t)ValuePersistent } },
};

const BlockTrigger BleSettings_Triggers[] = {
    { MakeFieldKey(BLE_SET_FIELD_PASSKEY, 0), OnBlePasskeyWrite },
    { MakeFieldKey(BLE_SET_FIELD_ACTION, 0), OnBleActionWrite },
    { MakeFieldKey(BLE_SET_FIELD_ENABLED, 0), OnBleEnabledWrite },
};

const BlockSchema BleSettingsSchema = {
    .Entries = BleSettings_Entries,
    .EntryCount = sizeof(BleSettings_Entries) / sizeof(BlockEntry),
    .Triggers = BleSettings_Triggers,
    .TriggerCount = sizeof(BleSettings_Triggers) / sizeof(BlockTrigger),
    .Type = BlockType::BleSettings,
};
