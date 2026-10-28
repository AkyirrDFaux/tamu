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
//   Last peer       (9, 0)    Text, RO               identity the admission decision used (E66)
//   Last air        (10, 0)   Text, RO               the address the peer used on the air (an RPA when resolved)
//   Last link       (11, 0)   Enum, RO               how the last link ended (BleLinkVerdict)
//
// Fields 9-11 are the E66 read-out, and they exist because the log could not carry the answer when
// they were added: DeviceLog is dropped while a host holds the link (Issues.md E67), and a connect
// happens exactly then. Both address fields read "AA:BB:CC:DD:EE:FF/N"; the six bytes are the
// witness and the `/N` suffix is information only.
//   - Last peer is the identity the session gate compared with the bond store (NimBLE's
//     `peer_id_addr`); Last air is the address the peer actually connected from (`peer_ota_addr`).
//     They differ exactly when the controller resolved a rotating private address (RPA) back to this
//     identity, because that is the one case where `peer_ota_addr` holds the RPA instead of the
//     identity. A peer that connects from its own public or static address reads the SAME address
//     twice - including this rig's host, which cannot enable privacy - and equal is the expected
//     reading there, not a failure.
//   - The `/N` suffix does NOT show a resolution: NimBLE's `ble_hs_conn_addrs()` rewrites the
//     controller's resolved-identity types (2/3) to 0/1 on `peer_id_addr`, so the type handed to the
//     host can only read 0 or 1. It is carried because it costs nothing and names the type space.
//   - The verdict names how the link ended, so "refused as a stranger" is readable apart from "not
//     recognised".
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
#include <cstdint>
#include "Core/Functions/MemoryTypes.h"
#include "Blocks/BleSecurityPolicy.h"
#include "Blocks/BleAdmissionPolicy.h" // BleLinkVerdict, the "Last link" field's vocabulary

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
#define BLE_SET_FIELD_LASTPEER     9
#define BLE_SET_FIELD_LASTAIR      10
#define BLE_SET_FIELD_LASTLINK     11

#define BLE_SET_MODE_JUST_WORKS 0
#define BLE_SET_MODE_PASSKEY    1

#define BLE_SET_ACTION_OPEN_WINDOW  0
#define BLE_SET_ACTION_FORGET_BONDS 1

// When the block's Bond texts must be re-read from the bond store (used by the board glue and
// pinned by test/native/ble_settings_test.cpp).
//
// The store's SIZE is not a change signal: when a full store takes a fourth bond, the stack evicts
// the oldest entry and adds the new one inside one callback, so getNumBonds() reads the same
// number before and after while the entries underneath are different. The read-out is therefore
// keyed on the store's CONTENT - a generation counter the radio code bumps on every completed bond
// and on a forget - with the count and a slow re-format-and-compare as the safety net for a store
// that changes without either event.
struct BleBondRefresh
{
    uint32_t Generation = 0; // store generation the texts were last formatted from
    int      Count = 0;      // getNumBonds() at that moment
    uint32_t DueMs = 0;      // TimeFromBoot() at which the slow content check is due
};

static inline bool BleBondRefreshDue(const BleBondRefresh &State, uint32_t Generation, int Count,
                                     uint32_t NowMs)
{
    // DueMs starts at 0, so the first pass after boot always formats; the signed difference keeps
    // the 49-day TimeFromBoot() wrap from stalling the check.
    return Generation != State.Generation || Count != State.Count ||
           (int32_t)(NowMs - State.DueMs) >= 0;
}

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
    // The E66 read-out: the last judged link. Both addresses are "AA:BB:CC:DD:EE:FF/N" (19 chars), or
    // empty while no link has been judged since boot; LastLink is a BleLinkVerdict. The six bytes
    // differ exactly when the controller resolved a rotating private address to the identity, so a
    // read-out with LastPeer = the bonded identity and LastAir = a random address is the resolution
    // itself; equal addresses mean the peer connected from its own address (the expected reading for
    // this rig's host). The `/N` suffix is information only - NimBLE normalises the resolved types
    // 2/3 to 0/1, so it never reads 2 or 3.
    alignas(4) char LastPeer[BLE_BOND_TEXT_LEN] = {};                 // field 9, offset 92
    alignas(4) char LastAir[BLE_BOND_TEXT_LEN] = {};                  // field 10, offset 112
    uint8_t  LastLink = BleLinkNone;                                  // field 11, offset 132
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
static_assert(offsetof(BleSettingsVolatile, LastPeer) == 92, "Last peer offset");
static_assert(offsetof(BleSettingsVolatile, LastAir) == 112, "Last air offset");
static_assert(offsetof(BleSettingsVolatile, LastLink) == 132, "Last link offset");

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
    // The E66 read-out: the last judged link, both addresses and the verdict. Read-only, and copied
    // from RAM (AppBLE.h) once per loop pass; their six-byte addresses differ exactly when the
    // controller resolved a rotating private address to the identity the gate compared with the bond
    // store. The `/N` type suffix is information only (NimBLE normalises 2/3 to 0/1).
    { MakeFieldKey(BLE_SET_FIELD_LASTPEER, 0), 92,
      { (uint16_t)DataType::Text, BLE_BOND_TEXT_LEN, ValueReadOnly } },
    { MakeFieldKey(BLE_SET_FIELD_LASTAIR, 0), 112,
      { (uint16_t)DataType::Text, BLE_BOND_TEXT_LEN, ValueReadOnly } },
    { MakeFieldKey(BLE_SET_FIELD_LASTLINK, 0), 132,
      { (uint16_t)DataType::Enum, 1, ValueReadOnly } },
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
