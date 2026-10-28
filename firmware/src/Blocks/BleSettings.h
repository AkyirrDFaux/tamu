#pragma once

// ===== BLE Settings =====
// The BLE link's settings and state as register fields, so it is visible and settable through the
// register path the app already speaks, instead of a side-channel command set. Block type 0x0B,
// one instance. The radio half (pairing, encrypted attributes, the connectable window, the bond
// store) lives in Devices/Tamu_v2.0A/AppBLE.h; this block is the single source of truth for the
// settings and a read-out of what the radio is doing.
//
//   Passkey         (0, 0)    Text(6), Trigger       the code the phone types; "000000" = Just Works
//   Mode            (1, 0)    Enum, RO               effective policy, derived from Passkey
//   Bond count      (2, 0)    Index, RO              entries in the NimBLE bond store
//   Bond            (3, 0..N) Text, RO, keyed        one bonded identity address per key
//   Window          (4, 0)    Uint32, RO             ms left in the connectable window
//   Action          (5, 0)    Enum, Trigger          0 open the pairing window, 1 forget all bonds
//   Enabled         (6, 0)    Bool, Persistent       false = the radio is not used at all
//   Display name    (7, 0)    Text(16), Persistent   advertised name, empty = System.Name
//   Advertised name (8, 0)    Text(16), RO           the name actually advertised (Display name resolved)
//
// Field numbers run in the order of that table (the owner's table lists the keys, not the field
// indexes; this is the one place they are fixed). Writes are restricted - see the triggers and the
// change hook in Devices/Tamu_v2.0A/BleSettingsGlue.h: every write to this block is refused unless
// the frame arrived over USB (Core/Functions/AppInterface.h publishes the link). A bonded phone
// that can rotate the key, wipe the bonds, rename the device or switch its radio off is exactly
// what the passkey exists to stop, so renaming needs USB.
//
// Persistence, and why the passkey is the exception:
//   - Enabled and Display name are Persistent fields: the register machinery mirrors them, with the
//     rest of the static persistent space, to `.SV` in the `storage` (SPIFFS) partition.
//   - The Passkey is NOT a persistent register field. `.SV` is a file in the storage partition and
//     the Storage service streams files out by name, so a persistent field is readable by anything
//     that can speak the Storage service - not a place for a secret. The value lives in NVS
//     (namespace `ble`, key `passkey`; Devices/Tamu_v2.0A/BlePasskeyStore.h) and the register field
//     is served from there: the read-out is the live RAM value the store loaded at boot and every
//     accepted write goes to NVS before it is applied. It is readable (this guards a local radio
//     link, and the app should be able to show the user their own code) but never persisted here.
//   - The retired first 6 bytes of the persistent struct are kept only so an existing `.SV` keeps
//     its layout; they are scrubbed to `000000` at boot and the mirror rewritten if an older build
//     left a real key there (BlePasskeyInit).
// A firmware reflash keeps both `.SV` and the NVS `ble` namespace; a full erase, a partition-table
// change or a storage `Format()` wipes `.SV` (Enabled, Display name) while NVS keeps the passkey.

#include <cstddef>
#include "Core/Functions/MemoryTypes.h"
#include "Blocks/BleSecurityPolicy.h"

// Bond slots: one per CONFIG_BT_NIMBLE_MAX_BONDS (3). The read-only Bond field exposes that many
// keys; a device with more bonds than slots would silently hide the extras, so the two numbers
// are pinned together in BleSettingsGlue.h.
#define BLE_SETTINGS_BOND_SLOTS 3
// "AA:BB:CC:DD:EE:FF" is 17 characters. The bond store keeps no peer name (Issues.md E60), so an
// entry is the address; 20 keeps a Text field's 4-byte alignment without wasting the packet budget.
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
#define BLE_SET_FIELD_ADVERTISED   8

#define BLE_SET_MODE_JUST_WORKS 0
#define BLE_SET_MODE_PASSKEY    1

#define BLE_SET_ACTION_OPEN_WINDOW  0
#define BLE_SET_ACTION_FORGET_BONDS 1

struct BleSettingsPersistent
{
    // Retired: layout only. This is where the passkey used to be persisted; the layout is kept so
    // an existing `.SV` still decodes (Enabled at 6, Display name at 8) and the bytes are scrubbed
    // to "000000" at boot. Never read as the passkey - see BleSettingsVolatile::Passkey.
    char    ScrubbedPasskey[BLE_PASSKEY_LEN] = {'0', '0', '0', '0', '0', '0'}; // field 0, offset 0
    // Default on: a device nobody has configured is still reachable over BLE (and the pairing
    // window decides when), so a fresh frame is usable from the phone alone.
    uint8_t Enabled = 1;                                              // field 6, offset 6
    // alignas(4): a value is aligned to its size capped at 4 (Docs/Services/Register.md "Memory
    // with 32-bit alignment"), so the 16-byte name sits at 8 and not at 7.
    alignas(4) char DisplayName[BLE_DISPLAY_NAME_LEN] = {};            // field 7, offset 8
};

struct BleSettingsVolatile
{
    // The live passkey text: served from the NVS-backed store, written through to it by the
    // trigger. Not persistent, so it never reaches `.SV` (see the header note).
    char     Passkey[BLE_PASSKEY_LEN] = {'0', '0', '0', '0', '0', '0'}; // field 0, offset 0
    uint8_t  Mode      = BLE_SET_MODE_JUST_WORKS;                   // field 1, offset 6
    uint8_t  BondCount = 0;                                         // field 2, offset 7
    alignas(4) char Bond[BLE_SETTINGS_BOND_SLOTS][BLE_BOND_TEXT_LEN] = {}; // field 3, offsets 8/28/48
    uint32_t WindowMs  = 0;                                         // field 4, offset 68
    uint8_t  Action    = 0;                                         // field 5, offset 72
    // What the advertisement really carries: the Display name, or System.Name when that is empty.
    // Read-only, so the app can show the resolved name without re-implementing the rule.
    alignas(4) char AdvertisedName[BLE_DISPLAY_NAME_LEN] = {};        // field 8, offset 76
};

// The entry table addresses the value spaces by byte offset, so the structs and the offsets above
// must agree. Pinned here rather than discovered as a read that returns the wrong field.
static_assert(offsetof(BleSettingsPersistent, ScrubbedPasskey) == 0, "retired passkey offset");
static_assert(offsetof(BleSettingsPersistent, Enabled) == 6, "Enabled offset");
static_assert(offsetof(BleSettingsPersistent, DisplayName) == 8, "Display name offset");
static_assert(offsetof(BleSettingsVolatile, Passkey) == 0, "Passkey offset");
static_assert(offsetof(BleSettingsVolatile, Mode) == 6, "Mode offset");
static_assert(offsetof(BleSettingsVolatile, BondCount) == 7, "Bond count offset");
static_assert(offsetof(BleSettingsVolatile, Bond[0]) == 8, "Bond[0] offset");
static_assert(offsetof(BleSettingsVolatile, Bond[1]) == 28, "Bond[1] offset");
static_assert(offsetof(BleSettingsVolatile, Bond[2]) == 48, "Bond[2] offset");
static_assert(offsetof(BleSettingsVolatile, WindowMs) == 68, "Window offset");
static_assert(offsetof(BleSettingsVolatile, Action) == 72, "Action offset");
static_assert(offsetof(BleSettingsVolatile, AdvertisedName) == 76, "Advertised name offset");

// Defined in the board glue: the write side of the block (Devices/Tamu_v2.0A/BleSettingsGlue.h).
bool OnBlePasskeyWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length);
bool OnBleActionWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length);
bool OnBleEnabledWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length);
bool OnBleDisplayNameWrite(const StaticBlockDescriptor &Block, uint16_t Index, const void *Data, uint16_t Length);

const BlockEntry BleSettings_Entries[] = {
    // The passkey text: readable (the app may show the user their own code) and writable, but NOT
    // persistent - the value is kept in NVS by the trigger and served from the volatile space.
    { MakeFieldKey(BLE_SET_FIELD_PASSKEY, 0), 0,
      { (uint16_t)DataType::Text, BLE_PASSKEY_LEN, (uint8_t)ValueTrigger } },
    // Read-only reports of the radio's state.
    { MakeFieldKey(BLE_SET_FIELD_MODE, 0), 6,
      { (uint16_t)DataType::Enum, 1, ValueReadOnly } },
    { MakeFieldKey(BLE_SET_FIELD_BONDCOUNT, 0), 7,
      { (uint16_t)DataType::Index, 1, ValueReadOnly } },
    { MakeFieldKey(BLE_SET_FIELD_BOND, 0), 8,
      { (uint16_t)DataType::Text, BLE_BOND_TEXT_LEN, ValueReadOnly } },
    { MakeFieldKey(BLE_SET_FIELD_BOND, 1), 28,
      { (uint16_t)DataType::Text, BLE_BOND_TEXT_LEN, ValueReadOnly } },
    { MakeFieldKey(BLE_SET_FIELD_BOND, 2), 48,
      { (uint16_t)DataType::Text, BLE_BOND_TEXT_LEN, ValueReadOnly } },
    { MakeFieldKey(BLE_SET_FIELD_WINDOW, 0), 68,
      { (uint16_t)DataType::Uint32, sizeof(uint32_t), ValueReadOnly } },
    // The only command surface. Not read-only: a write is a command, and the trigger decides
    // whether it is accepted; the stored byte stays 0 so a read-back never claims a command is
    // still pending.
    { MakeFieldKey(BLE_SET_FIELD_ACTION, 0), 72,
      { (uint16_t)DataType::Enum, 1, ValueTrigger } },
    // Settable settings, both Persistent and both USB-only. Display name has a trigger as well as
    // the copy path because the whole block is closed to the radio, not just its secrets.
    { MakeFieldKey(BLE_SET_FIELD_ENABLED, 0), 6,
      { (uint16_t)DataType::Bool, sizeof(uint8_t), (uint8_t)(ValuePersistent | ValueTrigger) } },
    { MakeFieldKey(BLE_SET_FIELD_DISPLAYNAME, 0), 8,
      { (uint16_t)DataType::Text, BLE_DISPLAY_NAME_LEN, (uint8_t)(ValuePersistent | ValueTrigger) } },
    // The resolved name, kept level with the radio by the main-loop hook.
    { MakeFieldKey(BLE_SET_FIELD_ADVERTISED, 0), 76,
      { (uint16_t)DataType::Text, BLE_DISPLAY_NAME_LEN, ValueReadOnly } },
};

const BlockTrigger BleSettings_Triggers[] = {
    { MakeFieldKey(BLE_SET_FIELD_PASSKEY, 0), OnBlePasskeyWrite },
    { MakeFieldKey(BLE_SET_FIELD_ACTION, 0), OnBleActionWrite },
    { MakeFieldKey(BLE_SET_FIELD_ENABLED, 0), OnBleEnabledWrite },
    { MakeFieldKey(BLE_SET_FIELD_DISPLAYNAME, 0), OnBleDisplayNameWrite },
};

const BlockSchema BleSettingsSchema = {
    .Entries = BleSettings_Entries,
    .EntryCount = sizeof(BleSettings_Entries) / sizeof(BlockEntry),
    .Triggers = BleSettings_Triggers,
    .TriggerCount = sizeof(BleSettings_Triggers) / sizeof(BlockTrigger),
    .Type = BlockType::BleSettings,
};
